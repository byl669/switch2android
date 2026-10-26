// Android output adapter for the original nn::audio AudioOut API.
#include "host_services.h"
#include "thread_services.h"
#include <aaudio/AAudio.h>
#include <mutex>
#include <map>
#include <memory>
#include <deque>
#include <cstring>
#include <atomic>
#include <thread>
#include <semaphore>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <array>
#include <cstdlib>
namespace ot2 {
namespace {
using R=uint64_t*;
struct Buffer {void* data;uint64_t capacity,size;};
struct Output {
    std::mutex mutex;AAudioStream* stream{};uint32_t event{};int channels{2},rate{48000};bool logged_pcm{};std::atomic<bool> started{false};std::atomic<uint64_t> played_frames{0};
    // 0 = let AAudio pick. The audren sink sets a capacity in frames so a
    // producer that paces itself on a 5 ms timer can absorb its own jitter
    // instead of underrunning the stream.
    int buffer_frames{};
    std::deque<Buffer*> pending,released;size_t position{};
    // All stream API calls are serialized here. The error callback only posts
    // recovery; closing a stream from its own callback can deadlock AAudio.
    std::mutex control;std::counting_semaphore<> recovery_signal{0};std::thread recovery_thread,silent_thread;
    std::condition_variable silent_changed;std::atomic<bool> active{true},available{false};
    std::atomic<bool> recovery_requested{false},closing{false};std::atomic<unsigned> generation{0};
    std::atomic<aaudio_result_t> last_error{0};
    ~Output();
};
std::mutex outputs_mutex;std::map<uint64_t,std::shared_ptr<Output>> outputs;std::atomic<bool> audio_active{true};
std::shared_ptr<Output> output(uint64_t id){std::lock_guard lock(outputs_mutex);auto it=outputs.find(id);if(it==outputs.end())ot2_host_die("Unknown AudioOut object");return it->second;}
void check_audio(aaudio_result_t result,const char* call){if(result<0)ot2_host_die("AAudio %s: %s",call,AAudio_convertResultToText(result));}
void consume(Output& a,void* data,int32_t frames){
    auto* dst=static_cast<uint8_t*>(data);size_t bytes=size_t(frames)*a.channels*2,consumed=0;bool released=false;
    {
        std::lock_guard lock(a.mutex);
        // A late real callback after pause supplies silence. Only the active
        // consumer can advance the queue, so focus changes never replay data.
        if(!a.started || bool(data)!=(a.active.load()&&a.available.load())){if(dst)memset(dst,0,bytes);return;}
        while(bytes){
            if(a.pending.empty()){if(dst)memset(dst,0,bytes);break;}
            auto* b=a.pending.front();size_t n=std::min(bytes,size_t(b->size-a.position));
            if(dst){memcpy(dst,static_cast<uint8_t*>(b->data)+a.position,n);dst+=n;}
            a.position+=n;bytes-=n;consumed+=n;
            if(a.position==b->size){a.released.push_back(b);a.pending.pop_front();a.position=0;released=true;}
        }
    }
    a.played_frames.fetch_add(consumed/(a.channels*2),std::memory_order_relaxed);
    if(released && a.event)ot2_signal_event(a.event);
}
aaudio_data_callback_result_t render(AAudioStream*,void* user,void* data,int32_t frames) {
    auto& a=*static_cast<Output*>(user);consume(a,data,frames);
    if(!a.logged_pcm){auto* pcm=static_cast<const int16_t*>(data);int peak=0;for(int i=0;i<frames*a.channels;i++)peak=std::max(peak,std::abs(int(pcm[i])));if(peak){a.logged_pcm=true;ot2_host_log("AUDIO_PCM_NONZERO peak=%d channels=%d rate=%d",peak,a.channels,a.rate);}}
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
void silent_output(Output& a){
    std::unique_lock lock(a.control);
    while(!a.closing){
        a.silent_changed.wait(lock,[&]{return a.closing || (a.started&&(!a.active||!a.available));});
        auto next=std::chrono::steady_clock::now();uint64_t fractional_frames=0;
        while(!a.closing && a.started && (!a.active||!a.available)){
            next+=std::chrono::milliseconds(10);
            if(a.silent_changed.wait_until(lock,next,[&]{return a.closing || !a.started || (a.active&&a.available);} ))break;
            fractional_frames+=a.rate;int frames=fractional_frames/100;fractional_frames%=100;
            lock.unlock();consume(a,nullptr,frames);lock.lock();
            // Do not burst-consume seconds of audio after device suspension.
            if(std::chrono::steady_clock::now()-next>std::chrono::milliseconds(100))next=std::chrono::steady_clock::now();
        }
    }
}
void request_recovery(Output& a,aaudio_result_t error){
    a.last_error=error;a.available=false;a.silent_changed.notify_all();
    if(!a.recovery_requested.exchange(true))a.recovery_signal.release();
}
void stream_error(AAudioStream*,void* user,aaudio_result_t error){request_recovery(*static_cast<Output*>(user),error);}
aaudio_result_t open_stream(Output& a){
    AAudioStreamBuilder* b{};auto rc=AAudio_createStreamBuilder(&b);if(rc<0)return rc;
    AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b,a.channels);AAudioStreamBuilder_setSampleRate(b,a.rate);
    AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);AAudioStreamBuilder_setPerformanceMode(b,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if(a.buffer_frames>0)AAudioStreamBuilder_setBufferCapacityInFrames(b,a.buffer_frames);
    AAudioStreamBuilder_setDataCallback(b,render,&a);AAudioStreamBuilder_setErrorCallback(b,stream_error,&a);
    rc=AAudioStreamBuilder_openStream(b,&a.stream);AAudioStreamBuilder_delete(b);
    if(rc>=0){
        // Guest buffers keep the original format across route changes. AAudio
        // converts the requested format to the physical device's new format.
        if(AAudioStream_getSampleRate(a.stream)!=a.rate || AAudioStream_getChannelCount(a.stream)!=a.channels)ot2_host_die("AAudio changed the requested PCM format");
        a.generation.fetch_add(1);
        a.available=true;a.silent_changed.notify_all();
    }
    return rc;
}
void close_stream(Output& a){a.available=false;a.silent_changed.notify_all();if(a.stream){AAudioStream_requestStop(a.stream);AAudioStream_close(a.stream);a.stream=nullptr;}}
void recover_output(Output& a){
    for(;;){
        a.recovery_signal.acquire();
        std::unique_lock lock(a.control);
        if(a.closing)break;
        if(!a.recovery_requested)continue;
        const auto error=a.last_error.load();close_stream(a);a.recovery_requested=false;
        unsigned attempts=0;
        while(!a.closing){
            a.recovery_requested=false;
            auto rc=open_stream(a);
            if(rc>=0 && a.started && a.active)rc=AAudioStream_requestStart(a.stream);
            if(rc>=0){ot2_host_log("AUDIO_OUTPUT_RECOVERED generation=%u error=%d",a.generation.load(),error);break;}
            close_stream(a);
            if(++attempts==1 || attempts%20==0)ot2_host_log("AUDIO_OUTPUT_RETRY attempt=%u error=%d",attempts,rc);
            lock.unlock();(void)a.recovery_signal.try_acquire_for(std::chrono::milliseconds(250));lock.lock();
        }
        if(a.closing)break;
        // An error from the newly opened stream posts its own recovery request.
    }
    std::lock_guard lock(a.control);close_stream(a);
}
Output::~Output(){
    {std::lock_guard lock(control);closing=true;}recovery_signal.release();silent_changed.notify_all();
    if(silent_thread.joinable())silent_thread.join();
    if(recovery_thread.joinable())recovery_thread.join();
    else close_stream(*this);
}
void transition(Output& a,bool start,bool pause=false){
    std::lock_guard lock(a.control);
    if(!a.stream || a.recovery_requested)return;
    auto result=start?AAudioStream_requestStart(a.stream):pause?AAudioStream_requestPause(a.stream):AAudioStream_requestStop(a.stream);
    if(result<0)request_recovery(a,result);
}
uint64_t open_output(uint64_t object,uint64_t event,uint64_t params) {
    auto a=std::make_shared<Output>();auto* p=reinterpret_cast<const uint8_t*>(params);int32_t rate;uint16_t ch;memcpy(&rate,p,4);memcpy(&ch,p+4,2);if(rate)a->rate=rate;if(ch)a->channels=ch;
    if(a->channels<1 || a->channels>8)ot2_host_die("Invalid output channel count");
    if(event){const uint64_t args[]={event,1,1,0,0,0,0,0};auto rc=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);if(rc)return rc;a->event=ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",args);}
    auto rc=open_stream(*a);if(rc<0){close_stream(*a);request_recovery(*a,rc);ot2_host_log("AUDIO_OUTPUT_UNAVAILABLE error=%d; paced silent output until recovery",rc);}
    a->recovery_thread=std::thread([state=a.get()]{recover_output(*state);});a->silent_thread=std::thread([state=a.get()]{silent_output(*state);});
    {std::lock_guard lock(outputs_mutex);if(outputs.contains(object))ot2_host_die("AudioOut already open");a->active=audio_active.load();outputs.emplace(object,a);}
    ot2_host_log("AUDIO_OUTPUT_OPEN rate=%d channels=%d",a->rate,a->channels);return 0;
}
uint64_t start_output(uint64_t object,bool start){
    std::lock_guard lock(outputs_mutex);auto it=outputs.find(object);if(it==outputs.end())ot2_host_die("Unknown AudioOut object");
    auto& a=*it->second;{std::lock_guard control(a.control);a.started=start;}a.silent_changed.notify_all();
    if(!start)transition(a,false);
    else if(a.active)transition(a,true);
    return 0;
}
}
void set_audio_active(bool active){
    std::lock_guard lock(outputs_mutex);audio_active=active;
    for(auto& [id,a]:outputs){
        {std::lock_guard control(a->control);a->active=active;}a->silent_changed.notify_all();
        if(a->started)transition(*a,active,!active);
    }
}
bool probe_audio_recovery(){
    // Silent real-device probe: inject the registered error path while stopped.
    // It proves close/reopen on AAudio, not a physical Bluetooth route change.
    auto a=std::make_shared<Output>();a->channels=6;
    auto result=open_stream(*a);if(result<0)return false;
    a->recovery_thread=std::thread([state=a.get()]{recover_output(*state);});
    {std::lock_guard lock(a->control);stream_error(a->stream,a.get(),AAUDIO_ERROR_DISCONNECTED);}
    for(int i=0;i<300&&a->generation<2;i++)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool recovered;{std::lock_guard lock(a->control);recovered=a->generation>=2 && a->stream && !a->started && AAudioStream_getChannelCount(a->stream)==6 && AAudioStream_getSampleRate(a->stream)==48000;}
    std::array<int16_t,288> silence{};Buffer buffer{silence.data(),sizeof(silence),sizeof(silence)};
    {std::lock_guard lock(a->mutex);a->pending.push_back(&buffer);}
    a->active=false;a->started=true;a->silent_thread=std::thread([state=a.get()]{silent_output(*state);});
    for(int i=0;i<100&&!a->played_frames;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bool paced;{std::lock_guard lock(a->mutex);paced=a->pending.empty()&&a->released.size()==1&&a->released.front()==&buffer&&a->played_frames==48;}
    a.reset(); // Join all consumers before the stack-owned PCM goes away.
    return recovered&&paced;
}
// ---- AudioRenderer (audren) sink ----------------------------------------
// The audren path hands out complete interleaved frames, so unlike AudioOut
// there is nothing to describe back to the guest: the renderer owns the blocks
// and the sink owns a heap copy. Blocks are recycled instead of freed, because
// the renderer produces one per 5 ms frame and an allocation per frame would be
// the only per-frame heap traffic in the audio path.
//
// `spare` is guarded by the Output's own `mutex`, not by audren_mutex: every
// access touches `released` in the same breath, and one lock for both keeps the
// order trivial.
namespace {
struct AudrenSink {std::shared_ptr<Output> out;std::deque<Buffer*> spare;};
std::mutex audren_mutex;std::map<uint64_t,std::shared_ptr<AudrenSink>> audren_sinks;
uint64_t audren_next_id=1;
}
uint64_t audren_sink_open(int rate,int channels){
    if(rate<=0||channels<1||channels>8)return 0;
    auto sink=std::make_shared<AudrenSink>();
    auto a=std::make_shared<Output>();a->rate=rate;a->channels=channels;
    // 100 ms of buffer: the producer is a 5 ms timer, and timer jitter that the
    // stream cannot absorb shows up as dropouts rather than as a slow clock.
    a->buffer_frames=rate/10;
    a->active=audio_active.load();
    auto rc=open_stream(*a);
    if(rc<0){close_stream(*a);request_recovery(*a,rc);ot2_host_log("AUDIO_SINK_UNAVAILABLE error=%d; paced silent output until recovery",rc);}
    a->recovery_thread=std::thread([state=a.get()]{recover_output(*state);});
    a->silent_thread=std::thread([state=a.get()]{silent_output(*state);});
    {std::lock_guard lock(a->control);a->started=true;}
    a->silent_changed.notify_all();
    if(a->active)transition(*a,true);
    sink->out=a;
    uint64_t id;
    {std::lock_guard lock(audren_mutex);id=audren_next_id++;}
    // Registered in `outputs` so set_audio_active drives it with everything else:
    // a sink that keeps pulling while the app is in the background is exactly the
    // case that path exists to prevent.
    {std::lock_guard lock(outputs_mutex);outputs.emplace(id,a);}
    {std::lock_guard lock(audren_mutex);audren_sinks.emplace(id,sink);}
    ot2_host_log("AUDIO_SINK_OPEN id=%llu rate=%d channels=%d capacity=%d frames",
                 (unsigned long long)id,rate,channels,a->buffer_frames);
    return id;
}
void audren_sink_write(uint64_t sink,const int16_t* interleaved,int frames){
    if(!sink||!interleaved||frames<=0)return;
    std::shared_ptr<AudrenSink> s;
    {std::lock_guard lock(audren_mutex);auto it=audren_sinks.find(sink);if(it==audren_sinks.end())return;s=it->second;}
    Output& a=*s->out;
    const size_t bytes=size_t(frames)*size_t(a.channels)*sizeof(int16_t);
    std::lock_guard lock(a.mutex);
    while(!a.released.empty()){s->spare.push_back(a.released.front());a.released.pop_front();}
    Buffer* block=nullptr;
    for(auto it=s->spare.begin();it!=s->spare.end();++it)
        if((*it)->capacity>=bytes){block=*it;s->spare.erase(it);break;}
    if(!block){
        block=static_cast<Buffer*>(calloc(1,sizeof(Buffer)));
        if(!block)return;
        block->data=malloc(bytes);
        if(!block->data){free(block);return;}
        block->capacity=bytes;
    }
    block->size=bytes;
    memcpy(block->data,interleaved,bytes);
    a.pending.push_back(block);
}
void audren_sink_close(uint64_t sink){
    std::shared_ptr<AudrenSink> s;
    {std::lock_guard lock(audren_mutex);auto it=audren_sinks.find(sink);if(it==audren_sinks.end())return;s=it->second;audren_sinks.erase(it);}
    {std::lock_guard lock(outputs_mutex);outputs.erase(sink);}
    Output& a=*s->out;
    {std::lock_guard lock(a.mutex);
     for(Buffer* b:a.pending){free(b->data);free(b);}a.pending.clear();
     for(Buffer* b:a.released){free(b->data);free(b);}a.released.clear();
     for(Buffer* b:s->spare){free(b->data);free(b);}s->spare.clear();}
    // Last reference: the destructor signals closing and joins the recovery and
    // paced-silent threads before the stream is torn down.
    s->out.reset();
    ot2_host_log("AUDIO_SINK_CLOSE id=%llu",(unsigned long long)sink);
}
const std::vector<HostService>& audio_services(){static const std::vector<HostService> table={
    // Both original SDK classes implement the same IOpusDecoder vtable (10
    // slots, checked against this game's relocation table). Software uses
    // 0x30 bytes within the hardware object's 0x40-byte allocation.
    {"_ZN2nn5codec19HardwareOpusDecoderC1Ei",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec11OpusDecoderC1Ev",r);}},
    {"_ZN2nn5codec19HardwareOpusDecoderC2Ei",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec11OpusDecoderC2Ev",r);}},
    // Multi-stream shares the twelve-slot interface too. Its software object
    // occupies 0x38 bytes, within the hardware allocation of 0x48 bytes.
    {"_ZN2nn5codec30HardwareOpusMultiStreamDecoderC1Ei",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec22OpusMultiStreamDecoderC1Ev",r);}},
    {"_ZN2nn5codec30HardwareOpusMultiStreamDecoderC2Ei",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec22OpusMultiStreamDecoderC2Ev",r);}},
    {"_ZN2nn5codec30HardwareOpusMultiStreamDecoderD1Ev",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec22OpusMultiStreamDecoderD1Ev",r);}},
    {"_ZN2nn5codec30HardwareOpusMultiStreamDecoderD2Ev",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec22OpusMultiStreamDecoderD2Ev",r);}},
    {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii",r);}},
    {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii",[](R r)->uint64_t{return ot2_call_guest("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii",r);}},
    // No hardware decoder is scheduled; this API measures DSP processing time,
    // not total CPU time. Never read a hardware field from the software object.
    {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderProcessingTimeEPKNS0_34HardwareOpusMultiStreamDecoderTypeE",[](R)->uint64_t{return 0;}},
    {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,8);return 0;}},
    {"_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi",[](R r)->uint64_t{if(int32_t(r[1])<1)return 0;auto* info=reinterpret_cast<char*>(r[0]);memset(info,0,256);memcpy(info,"DeviceOut",10);return 1;}},
    {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE",[](R r)->uint64_t{auto* name=reinterpret_cast<const char*>(r[2]);if(name&&*name&&strcmp(name,"DeviceOut"))ot2_host_die("Unknown AudioOut device");auto res=open_output(r[0],r[1],r[3]);ot2_host_log("AUDIO_OPEN event=%#llx res=%#llx",(unsigned long long)r[1],(unsigned long long)res);return res;}},
    {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE",[](R r)->uint64_t{auto* name=reinterpret_cast<const char*>(r[1]);if(name&&*name&&strcmp(name,"DeviceOut"))ot2_host_die("Unknown AudioOut device");auto res=open_output(r[0],0,r[2]);ot2_host_log("AUDIO_OPEN_DEFAULT res=%#llx",(unsigned long long)res);return res;}},
    {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",[](R r)->uint64_t{auto res=open_output(r[0],r[1],r[2]);ot2_host_log("AUDIO_OPEN_DEFAULT event=%#llx res=%#llx",(unsigned long long)r[1],(unsigned long long)res);return res;}},
    {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE",[](R r)->uint64_t{return open_output(r[0],0,r[1]);}},
    {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE",[](R r)->uint64_t{auto result=start_output(r[0],true);ot2_host_log("AUDIO_OUTPUT_STARTED");return result;}},
    {"_ZN2nn5audio21AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE__TRACE",[](R)->uint64_t{return 0;}},
    {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE",[](R r)->uint64_t{return start_output(r[0],false);}},
    {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE",[](R r)->uint64_t{std::shared_ptr<Output> removed;{std::lock_guard lock(outputs_mutex);auto it=outputs.find(r[0]);if(it!=outputs.end()){removed=it->second;outputs.erase(it);}}return 0;}},
    {"_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE",[](R)->uint64_t{return reinterpret_cast<uint64_t>("DeviceOut");}},
    {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE",[](R r)->uint64_t{return output(r[0])->started?0:1;}},
    {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE",[](R r)->uint64_t{return output(r[0])->rate;}},
    {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE",[](R r)->uint64_t{return output(r[0])->channels;}},
    {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE",[](R)->uint64_t{return 2;}},
    {"_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE",[](R r)->uint64_t{return output(r[0])->played_frames.load(std::memory_order_relaxed);}},
    {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm",[](R r)->uint64_t{if(r[3]>r[2])ot2_host_die("Audio buffer exceeds capacity");*reinterpret_cast<Buffer*>(r[0])={reinterpret_cast<void*>(r[1]),r[2],r[3]};return 0;}},
    {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE",[](R r)->uint64_t{return reinterpret_cast<uint64_t>(reinterpret_cast<Buffer*>(r[0])->data);}},
    {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE",[](R r)->uint64_t{return reinterpret_cast<Buffer*>(r[0])->size;}},
    {"_ZN2nn5audio27GetAudioOutBufferBufferSizeEPKNS0_14AudioOutBufferE",[](R r)->uint64_t{return reinterpret_cast<Buffer*>(r[0])->capacity;}},
    {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",[](R r)->uint64_t{static std::atomic<unsigned> n{0};if(n.fetch_add(1,std::memory_order_relaxed)<8)ot2_host_log("AUDIO_APPEND size=%llu",(unsigned long long)(r[1]?reinterpret_cast<Buffer*>(r[1])->size:0));auto a=output(r[0]);auto* b=reinterpret_cast<Buffer*>(r[1]);if(!b->data || !b->size || b->size>b->capacity || b->size%(2*a->channels))ot2_host_die("Invalid PCM output buffer");std::lock_guard lock(a->mutex);if(a->pending.size()+a->released.size()>=64)return 0;if(std::find(a->pending.begin(),a->pending.end(),b)!=a->pending.end()||std::find(a->released.begin(),a->released.end(),b)!=a->released.end())ot2_host_die("Audio buffer appended before release");a->pending.push_back(b);return 1;}},
    {"_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE",[](R r)->uint64_t{auto a=output(r[0]);std::lock_guard lock(a->mutex);return a->pending.size();}},
    {"_ZN2nn5audio22ContainsAudioOutBufferEPKNS0_8AudioOutEPKNS0_14AudioOutBufferE",[](R r)->uint64_t{auto a=output(r[0]);auto* b=reinterpret_cast<Buffer*>(r[1]);std::lock_guard lock(a->mutex);return std::find(a->pending.begin(),a->pending.end(),b)!=a->pending.end()||std::find(a->released.begin(),a->released.end(),b)!=a->released.end();}},
    {"_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE",[](R r)->uint64_t{auto a=output(r[0]);bool released=false;{std::lock_guard lock(a->mutex);released=!a->pending.empty();while(!a->pending.empty()){a->released.push_back(a->pending.front());a->pending.pop_front();}a->position=0;}if(released&&a->event)ot2_signal_event(a->event);return 1;}},
    {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",[](R r)->uint64_t{auto a=output(r[0]);std::lock_guard lock(a->mutex);if(a->released.empty())return 0;auto* b=a->released.front();a->released.pop_front();return reinterpret_cast<uint64_t>(b);}},
};return table;}
}
