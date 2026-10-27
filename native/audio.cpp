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
    // Device route changes reopen the stream; the tier that worked is worth one
    // line per open, because a phone that only accepts a fallback tier is exactly
    // the "no sound on this model" report this log has to answer.
    std::atomic<bool> logged_open{false};
    std::deque<Buffer*> pending,released;size_t position{};
    // All stream API calls are serialized here. The error callback only posts
    // recovery; closing a stream from its own callback can deadlock AAudio.
    std::mutex control;std::counting_semaphore<> recovery_signal{0};std::thread recovery_thread,silent_thread;
    std::condition_variable silent_changed;std::atomic<bool> active{true},available{false};
    // 输出侧诊断：区分"流没跑起来"和"流在跑但游戏混音是静音"。
    // 首次打开无声的现场只有这三条线一起看才定得下来，缺一条就会误判方向。
    std::atomic<uint64_t> hardware_frames{0};std::atomic<unsigned> gate_stopped{0},gate_background{0},gate_unavailable{0};
    // 停滞自愈用：流打开的时刻与已重开次数。硬件回调迟迟不来时只有重开流能解开。
    std::atomic<int64_t> opened_at_ms{0};std::atomic<unsigned> stall_reopens{0};
    std::atomic<bool> recovery_requested{false},closing{false};std::atomic<unsigned> generation{0};
    std::atomic<aaudio_result_t> last_error{0};
    ~Output();
};
std::mutex outputs_mutex;std::map<uint64_t,std::shared_ptr<Output>> outputs;std::atomic<bool> audio_active{true};
int64_t now_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::shared_ptr<Output> output(uint64_t id){std::lock_guard lock(outputs_mutex);auto it=outputs.find(id);if(it==outputs.end())ot2_host_die("Unknown AudioOut object");return it->second;}
void check_audio(aaudio_result_t result,const char* call){if(result<0)ot2_host_die("AAudio %s: %s",call,AAudio_convertResultToText(result));}
void consume(Output& a,void* data,int32_t frames){
    auto* dst=static_cast<uint8_t*>(data);size_t bytes=size_t(frames)*a.channels*2,consumed=0;bool released=false;
    {
        std::lock_guard lock(a.mutex);
        // A late real callback after pause supplies silence. Only the active
        // consumer can advance the queue, so focus changes never replay data.
        if(!a.started || bool(data)!=(a.active.load()&&a.available.load())){
            // 被拦下的原因必须要能分开：stopped = 游戏自己没开始播；
            // background = 我们按焦点闸掉的；unavailable = 流没打开。
            auto& counter=a.started?(a.active.load()?a.gate_unavailable:a.gate_background):a.gate_stopped;
            const unsigned n=counter.fetch_add(1,std::memory_order_relaxed)+1;
            if(n==1 || n%2000==0)
                ot2_host_log("AUDIO_SINK_GATED reason=%s count=%u active=%d available=%d started=%d",
                             a.started?(a.active.load()?"unavailable":"background"):"stopped",n,
                             int(a.active.load()),int(a.available.load()),int(a.started.load()));
            if(dst)memset(dst,0,bytes);return;} 
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
aaudio_data_callback_result_t render(AAudioStream* stream,void* user,void* data,int32_t frames) {
    auto& a=*static_cast<Output*>(user);
    // 只有硬件真的在拉数据时这个计数才涨。它和 AUDREN 侧写进去的帧数是两条
    // 独立的线：前者为 0 说明流没跑起来（输出侧问题），前者正常而游戏混音静音
    // 才是内容问题。首次打开无声的分歧点全在这个区分上。
    const uint64_t pulled=a.hardware_frames.fetch_add(uint64_t(frames),std::memory_order_relaxed)+uint64_t(frames);
    const uint64_t bucket=pulled/(uint64_t(a.rate)*10);
    if(pulled==uint64_t(frames)||bucket!=(pulled-uint64_t(frames))/(uint64_t(a.rate)*10))
        ot2_host_log("AUDIO_HARDWARE_PULL frames=%llu state=%d active=%d available=%d",
                     (unsigned long long)pulled,int(AAudioStream_getState(stream)),int(a.active.load()),int(a.available.load()));
    consume(a,data,frames);
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
// 一档打不开就换更宽松的一档。
//
// LOW_LATENCY + 100 ms capacity 在平板上一次就过，手机上是另一回事：独占音频设备、
// 只肯给小 buffer 的 HAL、或者干脆不支持低延迟模式的机型都会让 open 直接失败。原实现
// 失败后按**同一套参数**每 250 ms 重试，于是"打不开"就永远打不开，从用户那头看就是
// 整局游戏没有声音（进程还活着，画面也正常，最容易误判成引擎没输出）。降级链保证这种
// 机型至少能从第二个能用的配置出声，日志里也留下"被哪一档救回来"的一行证据。
//
// 默认顺序是 **mixer 优先**，不是低延迟优先。原因是一份 A/B 现场：同一台手机、
// 同一个蓝牙耳机，LOW_LATENCY 会让系统建出 DIRECT|MMAP_NOIRQ 直通流
// （audio_flinger 里是 `AudioMmapOut_*` + flags 0x4001）。这条路径在我们自己这边
// 一切正常——`AUDREN sink wrote peak=1767`、pending 里有数据——但硬件回调一帧都
// 不来，`AUDIO_PCM_NONZERO` 从头到尾不出现，于是整局静音；一旦切后台再回前台
// （或蓝牙路由变化）触发流重开，立刻就有声音。换成 NONE 走 AudioFlinger 混音器
// 之后，首次启动就正常出声。原生游戏用 AudioTrack 走的正是混音器，所以"别的游戏
// 有声、只有这个没声"不是设备问题。
//
// 直通不是不能用，只是不该默认用：它对路由变化太脆，而这个标题并不需要低延迟。
// 想试直通的机型放 `audio-lowlatency` 标记即可回到旧顺序。
struct StreamTier{aaudio_performance_mode_t mode;bool capacity;const char* name;};
constexpr StreamTier kMixerFirst[]={
    {AAUDIO_PERFORMANCE_MODE_NONE        ,false,"mixer"},
    {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ,false,"lowlat"},
    {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ,true ,"lowlat+cap"},
};
constexpr StreamTier kLowLatencyFirst[]={
    {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ,true ,"lowlat+cap"},
    {AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ,false,"lowlat"},
    {AAUDIO_PERFORMANCE_MODE_NONE        ,false,"mixer"},
};
aaudio_result_t open_stream(Output& a){
    aaudio_result_t last=AAUDIO_ERROR_INTERNAL;
    const bool want_low_latency=ot2_game_flag("audio-lowlatency")!=0;
    const StreamTier* tiers=want_low_latency?kLowLatencyFirst:kMixerFirst;
    if(want_low_latency)
        ot2_host_log("AUDIO_STREAM_LOWLATENCY_FORCED reason=audio-lowlatency marker");
    for(size_t ti=0;ti<3;ti++){
        const StreamTier& tier=tiers[ti];
        AAudioStreamBuilder* b{};auto rc=AAudio_createStreamBuilder(&b);if(rc<0)return rc;
        AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b,a.channels);AAudioStreamBuilder_setSampleRate(b,a.rate);
        AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);AAudioStreamBuilder_setPerformanceMode(b,tier.mode);
        if(tier.capacity&&a.buffer_frames>0)AAudioStreamBuilder_setBufferCapacityInFrames(b,a.buffer_frames);
        AAudioStreamBuilder_setDataCallback(b,render,&a);AAudioStreamBuilder_setErrorCallback(b,stream_error,&a);
        rc=AAudioStreamBuilder_openStream(b,&a.stream);AAudioStreamBuilder_delete(b);
        if(rc<0){
            last=rc;
            // 限流：一次失败的现场要留，但恢复线程 250 ms 一轮，不能把日志刷满。
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<8)
                ot2_host_log("AUDIO_STREAM_OPEN_FAILED tier=%s rate=%d channels=%d error=%d (%s)",
                             tier.name,a.rate,a.channels,rc,AAudio_convertResultToText(rc));
            continue;
        }
        // Guest buffers keep the original format across route changes. AAudio
        // converts the requested format to the physical device's new format, so a
        // stream that reports another format refused the request. Close it and try
        // the next tier: dying here turned a silent phone into a crash report with
        // nothing in it about audio.
        const int rate=AAudioStream_getSampleRate(a.stream),channels=AAudioStream_getChannelCount(a.stream);
        if(rate!=a.rate||channels!=a.channels){
            last=AAUDIO_ERROR_INVALID_FORMAT;
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<8)
                ot2_host_log("AUDIO_STREAM_FORMAT_REJECTED tier=%s wanted=%d/%d got=%d/%d",
                             tier.name,a.rate,a.channels,rate,channels);
            AAudioStream_close(a.stream);a.stream=nullptr;
            continue;
        }
        static std::atomic<unsigned> reopened{0};
        if(!a.logged_open.exchange(true) || reopened.fetch_add(1,std::memory_order_relaxed)<8)
            ot2_host_log("AUDIO_STREAM_OPENED tier=%s rate=%d channels=%d capacity=%d",tier.name,rate,channels,
                         tier.capacity?a.buffer_frames:0);
        a.generation.fetch_add(1);
        // 每个新流单独计数：硬件拉了多少帧只对"当前这条流"有意义，跨流累计会让
        // 一个新建却没人拉取的流永远看不到停滞。
        a.opened_at_ms=now_ms();a.hardware_frames=0;
        a.available=true;a.silent_changed.notify_all();
        return AAUDIO_OK;
    }
    return last;
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
    if(!a.stream || a.recovery_requested){
        // 静默跳过就是"start 了但什么都没发生"的来源，必须留痕：以前这里返回
        // 得悄无声息，日志上看不出游戏要播、我们却没把流拉起来。
        ot2_host_log("AUDIO_TRANSITION_SKIPPED start=%d stream=%d recovery=%d",
                     int(start),int(a.stream!=nullptr),int(a.recovery_requested.load()));
        return;}
    auto result=start?AAudioStream_requestStart(a.stream):pause?AAudioStream_requestPause(a.stream):AAudioStream_requestStop(a.stream);
    if(result<0 || start)
        ot2_host_log("AUDIO_TRANSITION start=%d rc=%d state=%d",int(start),result,int(AAudioStream_getState(a.stream)));
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
    ot2_host_log("AUDIO_ACTIVE active=%d outputs=%zu",int(active),outputs.size());
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
    ot2_host_log("AUDIO_SINK_OPEN id=%llu rate=%d channels=%d capacity=%d frames active=%d stream=%d",
                 (unsigned long long)id,rate,channels,a->buffer_frames,int(a->active.load()),int(a->stream!=nullptr));
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
    // 队列封顶：硬件不拉的时候这里会一直涨，攒下几十秒的音频再一次性放出来，
    // 比丢帧难听得更明显，也会把蓝牙编码器的缓冲撑爆。丢最旧的。
    while(a.pending.size()>200){
        Buffer* oldest=a.pending.front();a.pending.pop_front();
        if(oldest->capacity>=bytes)s->spare.push_back(oldest);
        else{free(oldest->data);free(oldest);}
        a.position=0; // position 只对队首有意义，丢块之后剩余量必须归零。
    }
    // 停滞自愈：数据在涨、硬件却一帧都没拉过。直通流在蓝牙 A2DP 上会卡在这个
    // 状态——流是 STARTED、我们的 PCM 有峰值、系统里一声不响——只有重开流能解开。
    // 从没拉过一帧才判定，所以正常流里的短促卡顿不会误触发。
    if(a.pending.size()>=60 && a.hardware_frames.load(std::memory_order_relaxed)==0
       && a.active.load() && a.available.load() && a.started.load()){
        const int64_t elapsed=now_ms()-a.opened_at_ms.load();
        const unsigned tries=a.stall_reopens.load();
        if(elapsed>1500 && tries<3 && !a.recovery_requested.load()){
            a.stall_reopens.fetch_add(1,std::memory_order_relaxed);
            ot2_host_log("AUDIO_STALL_REOPEN elapsed_ms=%lld queued=%zu attempt=%u",
                         (long long)elapsed,a.pending.size(),tries+1);
            request_recovery(a,AAUDIO_ERROR_TIMEOUT);
        } else if(tries==0){
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<6)
                ot2_host_log("AUDIO_SINK_BACKLOG_NO_PULL queued=%zu available=%d state=%d",
                             a.pending.size(),int(a.available.load()),a.stream?int(AAudioStream_getState(a.stream)):-1);
        }
    }
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
