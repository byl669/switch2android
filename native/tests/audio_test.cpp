#include "../host_services.h"
#include "fake/aaudio/AAudio.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

struct AAudioStreamBuilder {
    int channels=2,rate=48000,perf=0,capacity=0;AAudioStream_dataCallback render{};AAudioStream_errorCallback error{};void* data{};void* error_data{};
};
struct AAudioStream:AAudioStreamBuilder {bool closed=false,disconnected=false,started=false;};
std::mutex api_mutex;std::vector<std::unique_ptr<AAudioStream>> streams;
std::atomic<unsigned> opens{},closes{},bad_calls{},failed_opens{};bool fail_next_open=false;std::atomic<bool> fail_all_opens=false;
// 模拟"这台机型的 HAL 不支持低延迟模式"：降级链必须靠它才测得到。
bool reject_lowlat=false;
thread_local bool in_error_callback=false;
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** b){*b=new AAudioStreamBuilder;return 0;}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*,int){}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*,int){}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* b,int n){b->channels=n;}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* b,int n){b->rate=n;}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*,int){}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* b,int n){b->perf=n;}
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* b,int32_t n){b->capacity=n;}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* b,AAudioStream_dataCallback f,void* p){b->render=f;b->data=p;}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* b,AAudioStream_errorCallback f,void* p){b->error=f;b->error_data=p;}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* b,AAudioStream** s){
    std::lock_guard lock(api_mutex);if(in_error_callback)++bad_calls;
    if(reject_lowlat&&b->perf==AAUDIO_PERFORMANCE_MODE_LOW_LATENCY){++failed_opens;return AAUDIO_ERROR_NO_SERVICE;}
    if(fail_next_open||fail_all_opens){fail_next_open=false;++failed_opens;return AAUDIO_ERROR_NO_SERVICE;}
    auto stream=std::make_unique<AAudioStream>();static_cast<AAudioStreamBuilder&>(*stream)=*b;*s=stream.get();streams.push_back(std::move(stream));++opens;return 0;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* b){delete b;return 0;}
int32_t AAudioStream_getSampleRate(AAudioStream* s){return s->rate;}
int32_t AAudioStream_getChannelCount(AAudioStream* s){return s->channels;}
aaudio_result_t request(AAudioStream* s,bool start){std::lock_guard lock(api_mutex);if(s->closed||in_error_callback)++bad_calls;if(s->disconnected)return AAUDIO_ERROR_DISCONNECTED;s->started=start;return 0;}
aaudio_result_t AAudioStream_requestStart(AAudioStream* s){return request(s,true);}
aaudio_result_t AAudioStream_requestStop(AAudioStream* s){return request(s,false);}
aaudio_result_t AAudioStream_requestPause(AAudioStream* s){return request(s,false);}
aaudio_result_t AAudioStream_close(AAudioStream* s){std::lock_guard lock(api_mutex);if(s->closed||in_error_callback)++bad_calls;s->closed=true;s->started=false;++closes;return 0;}
const char* AAudio_convertResultToText(aaudio_result_t){return "test audio error";}
extern "C" [[noreturn]] void ot2_host_die(const char* format,...){char text[256];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);throw std::runtime_error(text);}
extern "C" void ot2_host_log(const char*,...){}
extern "C" int ot2_signal_event(uint32_t){return 0;}
extern "C" uint64_t ot2_call_guest(const char*,const uint64_t*){throw std::runtime_error("Unexpected guest call in AudioOut test");}
uint64_t call(const char* name,std::initializer_list<uint64_t> args){uint64_t r[8]{};std::copy(args.begin(),args.end(),r);for(auto& s:ot2::audio_services())if(!strcmp(s.name,name))return s.call(r);throw std::runtime_error("Missing audio service");}
template<class T> uint64_t address(T* p){return reinterpret_cast<uint64_t>(p);}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
AAudioStream* latest(){std::lock_guard lock(api_mutex);return streams.back().get();}
bool playing(){std::lock_guard lock(api_mutex);return streams.back()->started;}
void disconnect(bool fail_reopen=false){
    std::lock_guard lock(api_mutex);auto* s=streams.back().get();s->disconnected=true;s->started=false;fail_next_open=fail_reopen;
    require(s->error,"Disconnected AudioOut did not register a recovery callback");
    in_error_callback=true;s->error(s,s->error_data,AAUDIO_ERROR_DISCONNECTED);in_error_callback=false;
}
void wait_open(unsigned n){for(int i=0;i<300&&opens<n;i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));require(opens>=n,"Disconnected AudioOut was not reopened");}
std::vector<int16_t> consume(int frames){std::lock_guard lock(api_mutex);auto* s=streams.back().get();require(s->started&&!s->closed,"Render requires live started stream");std::vector<int16_t> pcm(frames*s->channels);s->render(s,s->data,pcm.data(),frames);return pcm;}
int main(){try{
    uint64_t object=0;struct {int32_t rate;uint16_t channels,padding;} params{48000,2,0};
    const char* open="_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE";
    const char* start="_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE";
    const char* stop="_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE";
    const char* close="_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE";
    const char* state="_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE";
    const char* count="_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE";
    const char* contains="_ZN2nn5audio22ContainsAudioOutBufferEPKNS0_8AudioOutEPKNS0_14AudioOutBufferE";
    std::array<char,272> devices;devices.fill(char(0x5a));
    require(call("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi",{address(devices.data()),1})==1&&!strcmp(devices.data(),"DeviceOut")&&devices[256]==0x5a,"Device enumeration ABI guard");
    // 降级链：不支持低延迟模式的机型（两档 low-latency 都被拒）必须落到 NONE 档打开，
    // 而不是拿同一套参数一直重试——那就是"画面正常、整局没有声音"的现场。
    {
        reject_lowlat=true;
        const unsigned before=failed_opens;
        call(open,{address(&object),address(&params)});
        require(failed_opens>=before+2,"Tries the low-latency tiers before falling back");
        {std::lock_guard lock(api_mutex);require(streams.back()->perf==AAUDIO_PERFORMANCE_MODE_NONE,"Falls back to the default performance mode instead of failing the open");}
        reject_lowlat=false;
        call(close,{address(&object)});
    }
    call(open,{address(&object),address(&params)});require(call(state,{address(&object)})==1,"SDK stopped state is one");call(start,{address(&object)});require(call(state,{address(&object)})==0,"SDK started state is zero");
    std::array<int16_t,16> pcm;for(int i=0;i<16;i++)pcm[i]=100+i;
    std::array<uint64_t,4> buffer{};
    call("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm",{address(buffer.data()),address(pcm.data()),sizeof(pcm),sizeof(pcm)});
    require(call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(buffer.data())})==1,"SDK AppendAudioOutBuffer returns bool true on success");
    require(call(count,{address(&object)})==1&&call(contains,{address(&object),address(buffer.data())})==1,"Pending buffer queries");
    require(consume(3)==std::vector<int16_t>(pcm.begin(),pcm.begin()+6),"Initial PCM");
    const unsigned resumed=opens+1;
    disconnect(true);wait_open(resumed);
    for(int i=0;i<100&&!playing();i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // 降级链让重开几乎立刻发生（旧实现要等满 250 ms 的重试周期），于是这里有两种合法结果：
    // pending 已被实时消费掉（全 0），或者把剩下的 10 个样本接着播完。关键是只消费一次、
    // 不重复渲染同一段 PCM。
    const auto tail=consume(5);
    require(tail==std::vector<int16_t>(10,0)||tail==std::vector<int16_t>(pcm.begin()+6,pcm.end()),
            "Resumed output consumes the leftover PCM exactly once");
    require(call(count,{address(&object)})==0&&call(contains,{address(&object),address(buffer.data())})==1,"Released buffer remains owned until retrieved");
    require(call("_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE",{address(&object)})==8,"Recovery preserves playback count through the actual SDK export");
    require(call("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",{address(&object)})==address(buffer.data()),"Recovery releases original buffer once");
    require(call(contains,{address(&object),address(buffer.data())})==0,"Returned buffer no longer owned");
    call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(buffer.data())});consume(2);
    require(call("_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE",{address(&object)})==1&&call(count,{address(&object)})==0,"Flush releases pending data");
    require(call("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",{address(&object)})==address(buffer.data()),"Flushed buffer retrievable exactly once");
    require(consume(2)==std::vector<int16_t>(4,0),"Flushed data is not replayed");
    ot2::set_audio_active(false);
    call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(buffer.data())});
    for(int i=0;i<100&&call(count,{address(&object)});i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(call(count,{address(&object)})==0,"Background silent sink must release PCM and keep SDK audio pacing alive");
    require(call("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",{address(&object)})==address(buffer.data()),"Background returns the same buffer");
    const unsigned background=opens+1;
    disconnect();wait_open(background);require(!playing(),"Background recovery must stay paused");
    call(stop,{address(&object)});ot2::set_audio_active(true);require(!playing(),"Focus cannot restart explicitly stopped output");
    call(start,{address(&object)});require(playing(),"Explicit restart after recovery");
    call(close,{address(&object)});
    call(open,{address(&object),address(&params)});
    std::array<std::array<uint64_t,5>,65> queued{};
    for(size_t i=0;i<queued.size();i++){
        queued[i][3]=0x12345678;
        call("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm",{address(queued[i].data()),address(pcm.data()),sizeof(pcm),sizeof(pcm)});
        require(queued[i][3]==0x12345678,"Buffer metadata does not overwrite SDK-owned tail");
        require(call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(queued[i].data())})==(i<64),"Bounded queue reports full using bool false");
    }
    call("_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE",{address(&object)});
    require(call(count,{address(&object)})==0,"Flush removes pending count");
    require(call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(queued[64].data())})==0,"Unretrieved releases still consume queue capacity");
    for(size_t i=0;i<64;i++)require(call("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",{address(&object)})==address(queued[i].data()),"Flush preserves release order");
    require(call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(queued[64].data())})==1,"Retrieved buffers free capacity");
    call(close,{address(&object)});
    const unsigned failures=failed_opens;
    call(open,{address(&object),address(&params)});disconnect(true);
    for(int i=0;i<100&&failed_opens==failures;i++)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(failed_opens>failures,"Exercise close while waiting to retry");call(close,{address(&object)});
    for(int i=0;i<30;i++){call(open,{address(&object),address(&params)});call(start,{address(&object)});disconnect();call(close,{address(&object)});}
    fail_all_opens=true;call(open,{address(&object),address(&params)});call(start,{address(&object)});
    call("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE",{address(&object),address(buffer.data())});
    for(int i=0;i<100&&call(count,{address(&object)});i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(call(count,{address(&object)})==0,"Initial device failure still drains PCM at real-time cadence");
    require(call("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE",{address(&object)})==address(buffer.data()),"Initial failure releases buffer exactly once");
    auto expected=opens+1;fail_all_opens=false;wait_open(expected);
    for(int i=0;i<100&&!playing();i++)std::this_thread::sleep_for(std::chrono::milliseconds(5));require(playing(),"Initial failure recovers without reopening guest AudioOut");call(close,{address(&object)});
    require(opens==closes&&bad_calls==0,"All streams close exactly once outside callbacks, without stale handle access");
    std::cout<<"AudioOut disconnect/retry/PCM/focus/close regression passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
