// movie::Decoder ABI adapter. Container parsing and playback stay in the guest;
// Android MediaCodec owns compressed packets, decoding and bounded codec buffers.
#include "host_services.h"
#include "thread_services.h"
#include "graphics/backend.h"
#include "movie_pixels.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <cstdlib>

namespace ot2 {
namespace {
using R=uint64_t*;
constexpr uint32_t Ok=0,Invalid=0x100b,BadState=0x100c,NoInput=0x1011,NoOutput=0x100e,Unsupported=0x2001,Failure=0x1000,Eos=0x2000;
uint64_t call(const char* name,std::initializer_list<uint64_t> values){uint64_t args[8]{};std::copy(values.begin(),values.end(),args);return ot2_call_guest(name,args);}
uint64_t nvn(const char* name,std::initializer_list<uint64_t> values){uint64_t args[8]{};std::copy(values.begin(),values.end(),args);return ot2_call_guest_address(uint64_t(ot2_nvn_lookup(name)),args);}
uint64_t ptr(const void* value){return reinterpret_cast<uint64_t>(value);}
const char* mime(int type){switch(type){case 1:return "video/avc";case 2:return "video/x-vnd.on2.vp8";case 3:return "video/x-vnd.on2.vp9";case 4:return "audio/mp4a-latm";case 5:return "audio/vorbis";default:return nullptr;}}
struct Frame {AMediaCodecBufferInfo info{};movie::YuvLayout layout{};int codec_index{};bool acquired{},copied{},released{};};
struct Decoder {
    std::mutex control,mutex;
    int type{},output{},mode{};AMediaCodec* codec{};AMediaFormat* config{};
    bool configured{},started{},input_eos{};uint32_t error{};
    std::array<uint64_t,4> events{};
    std::deque<int> inputs,outputs,completed;uint32_t next_frame{};std::set<int> leased_inputs;std::map<int,Frame> frames;
    movie::YuvLayout layout{};int channels{},rate{};
    std::vector<uint8_t> pixels;
    std::atomic<bool> quit{false};bool worker_live{};
    alignas(16) std::array<uint8_t,0x400> worker{};void* stack{};
    uint64_t decoded{},packets{},copied{};
    ~Decoder(){if(worker_live)ot2_host_die("Movie destroyed before worker joined");if(codec)AMediaCodec_delete(codec);if(config)AMediaFormat_delete(config);if(stack)call("free",{ptr(stack)});}
};
std::mutex registry_mutex;std::map<uint64_t,std::shared_ptr<Decoder>> registry;
std::shared_ptr<Decoder> decoder(uint64_t id){std::lock_guard lock(registry_mutex);auto it=registry.find(id);if(it==registry.end())ot2_host_die("Unknown movie decoder");return it->second;}
void signal(Decoder& d,unsigned event){if(d.events[event])call("_ZN2nn2os11SignalEventEPNS0_9EventTypeE",{d.events[event]});}
uint32_t error(Decoder& d,media_status_t result,const char* operation){
    if(result==AMEDIA_OK)return Ok;
    d.error=Failure;ot2_host_log("MOVIE_ERROR %s result=%d",operation,int(result));signal(d,3);return d.error;
}
int integer(AMediaFormat* f,const char* key,int fallback){int32_t value;return AMediaFormat_getInt32(f,key,&value)?value:fallback;}
void output_format(Decoder& d){
    auto* f=AMediaCodec_getOutputFormat(d.codec);if(!f)return;
    auto& p=d.layout;p.width=integer(f,"width",p.width);p.height=integer(f,"height",p.height);
    p.stride=integer(f,"stride",p.width);if(p.stride<=0)p.stride=p.width;
    p.slice=integer(f,"slice-height",p.height);if(p.slice<=0)p.slice=p.height;
    p.left=integer(f,"crop-left",0);p.top=integer(f,"crop-top",0);
    p.right=integer(f,"crop-right",p.width-1);p.bottom=integer(f,"crop-bottom",p.height-1);
    int32_t left,top,right,bottom;
    if(AMediaFormat_getRect(f,AMEDIAFORMAT_KEY_DISPLAY_CROP,&left,&top,&right,&bottom)){
        p.left=left;p.top=top;p.right=right;p.bottom=bottom;
    }
    p.format=integer(f,"color-format",0);p.standard=integer(f,"color-standard",p.height>=720?1:2);p.range=integer(f,"color-range",2);
    d.channels=integer(f,"channel-count",d.channels);d.rate=integer(f,"sample-rate",d.rate);
    ot2_host_log("MOVIE_FORMAT %s",AMediaFormat_toString(f));AMediaFormat_delete(f);signal(d,2);
}
uint64_t poll_decoder(Decoder* state){
    auto& d=*state;
    while(!d.quit.load(std::memory_order_acquire)){
        {
            std::lock_guard lock(d.mutex);
            if(d.started&&!d.error){
                bool input_ready=false,output_ready=false;
                if(!d.input_eos)for(int n=0;n<32;++n){
                    auto index=AMediaCodec_dequeueInputBuffer(d.codec,0);if(index==AMEDIACODEC_INFO_TRY_AGAIN_LATER)break;
                    if(index<0){error(d,media_status_t(index),"dequeue input");break;}
                    if(d.inputs.size()+d.leased_inputs.size()>=64)ot2_host_die("Movie input buffer bound exceeded");
                    d.inputs.push_back(int(index));input_ready=true;
                }
                for(int n=0;n<32;++n){
                    AMediaCodecBufferInfo info{};auto index=AMediaCodec_dequeueOutputBuffer(d.codec,&info,0);
                    if(index==AMEDIACODEC_INFO_TRY_AGAIN_LATER)break;
                    if(index==AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED){output_format(d);continue;}
                    if(index==AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)continue;
                    if(index<0){error(d,media_status_t(index),"dequeue output");break;}
                    if(d.frames.size()>=128)ot2_host_die("Movie output buffer ownership violated");
                    int token;do{token=int(++d.next_frame&0x7fffffff);}while(d.frames.contains(token));
                    d.frames.emplace(token,Frame{info,d.layout,int(index)});d.outputs.push_back(token);output_ready=true;
                    if(info.size>0&&++d.decoded==1)ot2_host_log("MOVIE_DECODED first_frame bytes=%d pts=%lld",info.size,(long long)info.presentationTimeUs);
                }
                if(input_ready)signal(d,0);if(output_ready)signal(d,1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}
// Wake every client event the title registered with RegisterClientEvents.
//
// These four events are the whole contract between the decoder and the title's
// own player thread: it parks in WaitSynchronization on them and feeds or drains
// the codec when one is signalled. The decoder is their only producer, and
// Stop/Reset/Flush is exactly where it stops producing -- so without this wake a
// player thread that happened to be parked at that moment is never released. It
// then lives forever, and the teardown that destroys the player joins it with
// nn::os::WaitThread, which never returns: the minigame result screen never
// appears while the title keeps rendering and keeps taking input, which is how
// the stall was reported ("time's up" stays on screen and nothing advances).
// Signalling an event with no waiter is harmless, so this is safe on every path.
void release_waiters(Decoder& d){
    for(unsigned event=0;event<4;++event)signal(d,event);
}
void join_worker(Decoder& d){
    // Release before the join, not after: the point is to let the title's player
    // thread run its stop path, and only then can this thread's exit be observed.
    release_waiters(d);
    d.quit.store(true,std::memory_order_release);
    if(d.worker_live){call("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE",{ptr(d.worker.data())});call("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE",{ptr(d.worker.data())});d.worker_live=false;}
    // Again afterwards, because a waiter woken by the first pass can re-arm
    // between that pass and the thread actually leaving.
    release_waiters(d);
}
uint32_t start_worker(Decoder& d){
    if(!d.stack)d.stack=reinterpret_cast<void*>(call("aligned_alloc",{4096,128*1024}));
    if(!d.stack)return Failure;
    d.quit=false;d.worker.fill(0);
    auto entry=ot2_native_gate("android_movie_worker",reinterpret_cast<void*>(poll_decoder));
    auto rc=call("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi",{ptr(d.worker.data()),ptr(entry),ptr(&d),ptr(d.stack),128*1024,44});
    if(rc){ot2_host_log("MOVIE_ERROR create worker result=%llx",(unsigned long long)rc);return Failure;}
    d.worker_live=true;call("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE",{ptr(d.worker.data())});return Ok;
}
void clear_buffers(Decoder& d){d.inputs.clear();d.outputs.clear();d.leased_inputs.clear();d.frames.clear();d.completed.clear();d.input_eos=false;}
uint32_t stop(Decoder& d){
    // Mark it stopped before anything is woken: a released waiter calls back into
    // this file, and it has to observe BadState (its own stop path) instead of
    // taking a buffer from a codec that is about to be stopped underneath it.
    bool was_started;
    {std::lock_guard lock(d.mutex);was_started=d.started;d.started=false;}
    join_worker(d);
    std::lock_guard lock(d.mutex);uint32_t rc=Ok;
    if(was_started){rc=error(d,AMediaCodec_stop(d.codec),"stop");d.configured=false;}
    clear_buffers(d);return rc;
}
uint64_t construct(R r){
    auto d=std::make_shared<Decoder>();d->type=int(r[1]);d->output=int(r[2]);d->mode=int(r[3]);
    auto table=ot2_guest_symbol_address("_ZTVN5movie7DecoderE");if(!table)ot2_host_die("Missing movie Decoder vtable");
    reinterpret_cast<uint64_t*>(r[0])[0]=table+16;reinterpret_cast<uint64_t*>(r[0])[1]=0;
    {std::lock_guard lock(registry_mutex);if(registry.contains(r[0])||registry.size()>=16)ot2_host_die("Movie decoder lifetime/capacity violation");registry.emplace(r[0],d);}
    ot2_host_log("MOVIE_CREATE type=%d output=%d mode=%d",d->type,d->output,d->mode);return 0;
}
uint64_t destroy(R r,bool deleting){
    auto d=decoder(r[0]);{std::lock_guard control(d->control);stop(*d);}
    {std::lock_guard lock(registry_mutex);registry.erase(r[0]);}
    ot2_host_log("MOVIE_DESTROY packets=%llu decoded=%llu",(unsigned long long)d->packets,(unsigned long long)d->decoded);
    if(deleting)call("_ZdlPv",{r[0]});return 0;
}
bool find_int(uint64_t media,const char* key,int32_t& out){return call("_ZNK5movie9MediaData9FindInt32EPKcPi",{media,ptr(key),ptr(&out)})&1;}
void set_int(uint64_t media,const char* key,int32_t value){call("_ZN5movie9MediaData8SetInt32EPKci",{media,ptr(key),uint64_t(value)});}
uint64_t configure(R r){
    if(!r[1])return Invalid;auto d=decoder(r[0]);std::lock_guard control(d->control);stop(*d);std::lock_guard lock(d->mutex);
    if(d->codec){AMediaCodec_delete(d->codec);d->codec=nullptr;}if(d->config){AMediaFormat_delete(d->config);d->config=nullptr;}
    const auto* type=mime(d->type);if(!type)return Unsupported;
    if(d->type<=3&&!((d->mode==2&&d->output==3)||(d->mode==1&&d->output==2)))return Unsupported;
    if(d->type>3&&(d->mode!=1||d->output!=1))return Unsupported;
    auto* f=AMediaFormat_new();d->config=f;AMediaFormat_setString(f,"mime",type);
    for(const char* key:{"width","height","channel-count","sample-rate","max-input-size","profile","level"}){int32_t value;if(find_int(r[1],key,value))AMediaFormat_setInt32(f,key,value);}
    for(const char* key:{"csd-0","csd-1","csd-2"}){
        uint64_t buffer=0;if(!(call("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE",{r[1],ptr(key),ptr(&buffer)})&1)||!buffer)continue;
        auto data=call("_ZNK5movie6Buffer4DataEv",{buffer});auto size=call("_ZNK5movie6Buffer4SizeEv",{buffer});
        if(!data||size>16*1024*1024)return Invalid;AMediaFormat_setBuffer(f,key,reinterpret_cast<void*>(data),size);
    }
    d->layout.width=integer(f,"width",0);d->layout.height=integer(f,"height",0);d->channels=integer(f,"channel-count",0);d->rate=integer(f,"sample-rate",0);
    if(d->type<=3){
        if(d->layout.width<=0||d->layout.height<=0||d->layout.width>4096||d->layout.height>4096)return Invalid;
        d->layout.stride=d->layout.width;d->layout.slice=d->layout.height;d->layout.right=d->layout.width-1;d->layout.bottom=d->layout.height-1;
        AMediaFormat_setInt32(f,"color-format",19); // Request standard CPU-readable YUV420.
    }
    d->codec=AMediaCodec_createDecoderByType(type);if(!d->codec)return Unsupported;
    d->error=0;auto result=error(*d,AMediaCodec_configure(d->codec,f,nullptr,nullptr,0),"configure");
    d->configured=result==Ok;ot2_host_log("MOVIE_CONFIGURE mime=%s result=%x %dx%d",type,result,d->layout.width,d->layout.height);return result;
}
uint64_t start(R r){
    auto d=decoder(r[0]);std::lock_guard control(d->control);
    {std::lock_guard lock(d->mutex);if(!d->codec||!d->configured||d->started)return BadState;
        auto rc=error(*d,AMediaCodec_start(d->codec),"start");if(rc)return rc;d->started=true;clear_buffers(*d);}
    auto rc=start_worker(*d);if(rc)stop(*d);return rc;
}
uint64_t flush(R r){
    auto d=decoder(r[0]);std::lock_guard control(d->control);join_worker(*d);
    {std::lock_guard lock(d->mutex);if(!d->started)return BadState;auto rc=error(*d,AMediaCodec_flush(d->codec),"flush");if(rc)return rc;clear_buffers(*d);}
    return start_worker(*d);
}
uint64_t acquire_input(R r){
    if(!r[1]||!r[2])return Invalid;*reinterpret_cast<int*>(r[1])=-1;*reinterpret_cast<int*>(r[2])=0;
    auto d=decoder(r[0]);std::lock_guard lock(d->mutex);if(d->error)return d->error;if(!d->started)return BadState;
    if(d->inputs.empty())return NoInput;int index=d->inputs.front();d->inputs.pop_front();d->leased_inputs.insert(index);
    *reinterpret_cast<int*>(r[1])=index;*reinterpret_cast<int*>(r[2])=d->inputs.size();return Ok;
}
uint64_t input_buffer(R r){
    if(!r[2])return Invalid;auto d=decoder(r[0]);std::lock_guard lock(d->mutex);if(!d->started||!d->leased_inputs.contains(int(r[1])))return BadState;
    size_t size=0;auto* data=AMediaCodec_getInputBuffer(d->codec,size_t(r[1]),&size);if(!data)return Failure;
    call("_ZN5movie6Buffer18SetDataAndCapacityEPvm",{r[2],ptr(data),size});call("_ZN5movie6Buffer8SetRangeEii",{r[2],0,0});return Ok;
}
uint64_t send_input(R r){
    auto d=decoder(r[0]);std::lock_guard lock(d->mutex);int index=int(r[1]),offset=int(r[2]),length=int(r[3]);
    if(!d->started||!d->leased_inputs.contains(index))return BadState;
    size_t capacity=0;auto* buffer=AMediaCodec_getInputBuffer(d->codec,index,&capacity);
    if(!buffer||offset<0||length<0||size_t(offset)>capacity||size_t(length)>capacity-size_t(offset))return Invalid;
    auto rc=error(*d,AMediaCodec_queueInputBuffer(d->codec,index,offset,length,r[4],uint32_t(r[5])),"queue input");
    if(!rc){d->leased_inputs.erase(index);++d->packets;if(r[5]&4)d->input_eos=true;}return rc;
}
uint64_t acquire_output(R r){
    if(!r[1]||!r[2]||!r[3]||!r[4])return Invalid;
    *reinterpret_cast<int*>(r[1])=-1;*reinterpret_cast<int64_t*>(r[2])=0;*reinterpret_cast<uint32_t*>(r[3])=0;*reinterpret_cast<int*>(r[4])=0;
    auto d=decoder(r[0]);std::lock_guard lock(d->mutex);if(d->error)return d->error;if(!d->started)return BadState;if(d->outputs.empty())return NoOutput;
    int index=d->outputs.front();d->outputs.pop_front();auto& f=d->frames.at(index);f.acquired=true;
    *reinterpret_cast<int*>(r[1])=index;*reinterpret_cast<int64_t*>(r[2])=f.info.presentationTimeUs;*reinterpret_cast<uint32_t*>(r[3])=f.info.flags;*reinterpret_cast<int*>(r[4])=d->outputs.size();return Ok;
}
uint32_t format(Decoder& d,uint64_t media,const movie::YuvLayout& p){
    if(!media)return Invalid;call("_ZN5movie9MediaData5ClearEv",{media});
    if(d.type<=3){
        int width=p.right>=p.left&&p.width?p.right-p.left+1:d.layout.width,height=p.bottom>=p.top&&p.height?p.bottom-p.top+1:d.layout.height;
        set_int(media,"width",d.mode==2?p.width:width);set_int(media,"height",d.mode==2?p.height:height);set_int(media,"stride",d.mode==2?p.width:width);set_int(media,"slice-height",d.mode==2?p.height:height);
        set_int(media,"color-format",d.output==4?21:0x111);
        call("_ZN5movie9MediaData7SetRectEPKciiii",{media,ptr("crop"),uint64_t(d.mode==2?p.left:0),uint64_t(d.mode==2?p.top:0),uint64_t(d.mode==2?p.right:width-1),uint64_t(d.mode==2?p.bottom:height-1)});
    }else{set_int(media,"channel-count",d.channels);set_int(media,"sample-rate",d.rate);set_int(media,"pcm-encoding",2);}
    return Ok;
}
uint64_t get_format(R r,bool indexed){
    auto d=decoder(r[0]);std::lock_guard lock(d->mutex);
    if(indexed){auto it=d->frames.find(int(r[1]));if(it==d->frames.end()||!it->second.acquired)return NoOutput;if(it->second.info.size==0&&(it->second.info.flags&4))return Eos;return format(*d,r[2],it->second.layout);}
    return format(*d,r[1],d->layout);
}
uint64_t output_buffer(R r,bool with_format){
    if(!r[2])return Invalid;auto d=decoder(r[0]);std::lock_guard lock(d->mutex);
    auto it=d->frames.find(int(r[1]));if(!d->started||it==d->frames.end()||!it->second.acquired)return BadState;
    auto& f=it->second;if(f.copied)return Ok;if(with_format){auto rc=format(*d,r[3],f.layout);if(rc)return rc;}
    size_t capacity=0;auto* data=AMediaCodec_getOutputBuffer(d->codec,f.codec_index,&capacity);
    if(f.info.size==0){call("_ZN5movie6Buffer8SetRangeEii",{r[2],0,0});return (f.info.flags&4)?Eos:Ok;}
    if(!data||f.info.offset<0||f.info.size<0||size_t(f.info.offset)>capacity||size_t(f.info.size)>capacity-f.info.offset)return Failure;
    std::span<const uint8_t> source(data+f.info.offset,capacity-f.info.offset);
    auto base=call("_ZNK5movie6Buffer4BaseEv",{r[2]});if(!base)return Invalid;
    if(d->type>3){
        auto bytes=call("_ZNK5movie6Buffer8CapacityEv",{r[2]});if(bytes<size_t(f.info.size))return Invalid;
        memcpy(reinterpret_cast<void*>(base),source.data(),f.info.size);call("_ZN5movie6Buffer8SetRangeEii",{r[2],0,uint64_t(f.info.size)});
    }else{
        auto p=f.layout;int w=p.right-p.left+1,h=p.bottom-p.top+1;bool bgra=false;
        if(w<=0||h<=0||w>4096||h>4096)return Invalid;
        if(d->mode==2){
            const auto tw=nvn("nvnTextureGetWidth",{base}),th=nvn("nvnTextureGetHeight",{base}),tf=nvn("nvnTextureGetFormat",{base});
            if(tw<uint64_t(p.right+1)||th<uint64_t(p.bottom+1)||(tf!=NVN_FORMAT_RGBA8&&tf!=NVN_FORMAT_RGBA8_SRGB&&tf!=NVN_FORMAT_BGRA8&&tf!=NVN_FORMAT_BGRA8_SRGB))return Unsupported;
            bgra=tf==NVN_FORMAT_BGRA8||tf==NVN_FORMAT_BGRA8_SRGB;
        }
        d->pixels.resize(size_t(w)*h*4);
        if(!movie::rgba(source,p,d->pixels,bgra)){ot2_host_log("MOVIE_ERROR unsupported output layout format=%x stride=%d slice=%d bytes=%zu",p.format,p.stride,p.slice,source.size());return Unsupported;}
        if(d->mode==2){
            // Publish through the same texture transfer/coherence path as NVN
            // CPU writes. The game's next draw imports the updated pixels.
            NVNcopyRegion region{p.left,p.top,0,w,h,1};nvn("nvnTextureWriteTexels",{base,0,ptr(&region),ptr(d->pixels.data())});
            auto sync=call("_ZNK5movie6Buffer10GetNvnSyncEv",{r[2]});if(sync){nvn("nvnSyncInitialize",{sync,0});nvn("nvnQueueFenceSync",{0,sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0});}
        }else{
            auto bytes=call("_ZNK5movie6Buffer8CapacityEv",{r[2]});if(d->output!=2||bytes<d->pixels.size())return Unsupported;
            memcpy(reinterpret_cast<void*>(base),d->pixels.data(),d->pixels.size());call("_ZN5movie6Buffer8SetRangeEii",{r[2],0,d->pixels.size()});
        }
    }
    f.copied=true;
    if(++d->copied==1)ot2_host_log("MOVIE_COPIED first_frame pts=%lld",(long long)f.info.presentationTimeUs);
    // The SDK's texture-output API consumes the codec buffer during copying.
    // Use host tokens so a late explicit release cannot release a reused NDK slot.
    if(d->mode==2){
        auto rc=error(*d,AMediaCodec_releaseOutputBuffer(d->codec,f.codec_index,false),"release copied output");if(rc)return rc;
        f.released=true;d->completed.push_back(int(r[1]));
        while(d->completed.size()>64){d->frames.erase(d->completed.front());d->completed.pop_front();}
    }
    return Ok;
}
uint64_t release_output(R r){
    auto d=decoder(r[0]);std::lock_guard lock(d->mutex);auto it=d->frames.find(int(r[1]));if(!d->started||it==d->frames.end()||!it->second.acquired)return BadState;
    if(it->second.released)return Ok;
    auto rc=error(*d,AMediaCodec_releaseOutputBuffer(d->codec,it->second.codec_index,false),"release output");if(!rc)d->frames.erase(it);return rc;
}
}
template<auto Function> uint64_t host(R r){return ot2_call_on_host_stack(+[](R args)->uint64_t{return Function(args);},r);}
const std::vector<HostService>& movie_services(){static const std::vector<HostService> table={
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi",host<construct>},
    {"_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi",host<construct>},
    {"_ZN5movie7DecoderD0Ev",host<[](R r)->uint64_t{return destroy(r,true);}>},
    {"_ZN5movie7DecoderD1Ev",host<[](R r)->uint64_t{return destroy(r,false);}>},
    {"_ZN5movie7DecoderD2Ev",host<[](R r)->uint64_t{return destroy(r,false);}>},
    {"_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",host<configure>},
    {"_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE",host<[](R r)->uint64_t{auto d=decoder(r[0]);std::lock_guard lock(d->mutex);if(r[1])memcpy(d->events.data(),reinterpret_cast<void*>(r[1]),32);else d->events.fill(0);return 0;}>},
    {"_ZN5movie7Decoder5StartEv",host<start>},
    {"_ZN5movie7Decoder4StopEv",host<[](R r)->uint64_t{auto d=decoder(r[0]);std::lock_guard control(d->control);return stop(*d);}>},
    {"_ZN5movie7Decoder5FlushEv",host<flush>},
    {"_ZN5movie7Decoder5ResetEv",host<[](R r)->uint64_t{auto d=decoder(r[0]);std::lock_guard control(d->control);stop(*d);std::lock_guard lock(d->mutex);if(d->codec){AMediaCodec_delete(d->codec);d->codec=nullptr;}d->configured=false;d->error=0;return 0;}>},
    {"_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_",host<acquire_input>},
    {"_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",host<input_buffer>},
    {"_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj",host<send_input>},
    {"_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",host<acquire_output>},
    {"_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE",host<[](R r)->uint64_t{return output_buffer(r,false);}>},
    {"_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE",host<[](R r)->uint64_t{return output_buffer(r,true);}>},
    {"_ZN5movie7Decoder24ReleaseOutputBufferIndexEi",host<release_output>},
    {"_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",host<[](R r)->uint64_t{return get_format(r,false);}>},
    {"_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE",host<[](R r)->uint64_t{return get_format(r,true);}>},
    {"_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",host<[](R r)->uint64_t{if(!r[1]||!r[2])return Invalid;auto d=decoder(r[0]);std::lock_guard lock(d->mutex);if(d->mode!=1)return Unsupported;*reinterpret_cast<int*>(r[1])=d->type<=3?d->layout.width*d->layout.height*4:8192;*reinterpret_cast<int*>(r[2])=1;return 0;}>},
    {"_ZN5movie7Decoder12GetLastErrorEv",host<[](R r)->uint64_t{auto d=decoder(r[0]);std::lock_guard lock(d->mutex);return d->error;}>}
};return table;}
}
