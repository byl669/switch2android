#include "host_services.h"
#include "input.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include "thread_services.h"
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
namespace {
std::mutex pad_mutex;uint64_t buttons{},presses{};int32_t lx{},ly{},rx{},ry{};std::atomic<int64_t> sampling{};std::atomic<bool> input_trace{false};
struct PadState {int64_t sample;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,padding;};
static_assert(sizeof(PadState)==40);
using R=uint64_t*;
// Scripted controller input. Driving the on-screen pad from `adb input` cannot
// produce a clean press/release pair: `input swipe` loses its final UP often
// enough that the pad latches a held button, and `input tap` cancels so fast the
// guest never samples it. A marker file sidesteps both -- `<game>/inject-buttons`
// holds a button list ("PLUS", "DOWN A", ...) and the pad reports exactly those
// while the file exists, so a test presses by creating it and releases by
// deleting it. Re-read at most every 150 ms; the file is tiny and this path runs
// once per controller poll.
uint64_t injected_buttons{};
void refresh_injection(){
    using clock=std::chrono::steady_clock;
    static clock::time_point next_check{};
    const auto now=clock::now();
    if(now<next_check) return;
    next_check=now+std::chrono::milliseconds(150);
    const char* directory=ot2_game_directory();
    if(!directory||!*directory) { injected_buttons=0; return; }
    std::ifstream marker(std::string(directory)+"/inject-buttons");
    if(!marker) { if(injected_buttons) ot2_host_log("INJECT release"); injected_buttons=0; return; }
    uint64_t bits=0;
    std::string token;
    static const std::pair<const char*,uint64_t> names[]={
        {"A",1},{"B",2},{"X",4},{"Y",8},{"L3",16},{"R3",32},{"L",64},{"R",128},
        {"ZL",256},{"ZR",512},{"PLUS",1024},{"MINUS",2048},
        {"LEFT",4096},{"UP",8192},{"RIGHT",16384},{"DOWN",32768},
    };
    while(marker>>token) {
        for(const auto& entry:names) {
            if(token==entry.first) { bits|=entry.second; break; }
            // Case-insensitive: a shell test types PLUS, a doc writes plus.
            if(token.size()==strlen(entry.first)) {
                bool same=true;
                for(size_t i=0;i<token.size();i++) if(toupper((unsigned char)token[i])!=entry.first[i]) { same=false; break; }
                if(same) { bits|=entry.second; break; }
            }
        }
    }
    if(bits!=injected_buttons) ot2_host_log("INJECT buttons=%#llx",(unsigned long long)bits);
    injected_buttons=bits;
}
// A bounded trace so a session that never receives input says so plainly
// instead of leaving "nothing happens" to be guessed at.
std::atomic<unsigned> pad_log_budget{40}, touch_log_budget{40};
// Handheld (0x20) and the first full-key controller (0) both report the host
// state. Which one a title reads depends on the operation mode it believes in,
// and answering only one of them is how a controller that looks connected ends
// up reporting "no buttons" forever -- the previous npad_state in
// host_services.cpp wrote nothing but the sampling number.
uint64_t pad(R r){
    if(!r[0]) return 0xcc01;
    auto& out=*reinterpret_cast<PadState*>(r[0]);
    const uint32_t id=r[1]?*reinterpret_cast<const uint32_t*>(r[1]):0;
    out={};
    out.sample=++sampling;
    if(id!=0x20u && id!=0u) return 0;      // no second player is attached
    refresh_injection();
    {
        std::lock_guard lock(pad_mutex);
        out.attributes=3;                   // IsConnected | IsWired
        out.buttons=buttons|presses|injected_buttons;
        presses=0;                          // press edges are latched for one read
        out.lx=lx;out.ly=ly;out.rx=rx;out.ry=ry;
    }
    if(input_trace||(out.buttons&&pad_log_budget.load(std::memory_order_relaxed)>0)){
        if(pad_log_budget.fetch_sub(1,std::memory_order_relaxed)>0)
            ot2_host_log("HID_PAD id=%#x sample=%lld buttons=%llx stick=%d,%d,%d,%d",id,
                         (long long)out.sample,(unsigned long long)out.buttons,out.lx,out.ly,out.rx,out.ry);
    }
    return 0;
}
uint64_t absent_pad(R r){auto& out=*reinterpret_cast<PadState*>(r[0]);out={};out.sample=++sampling;return 0;}
// The plural query is a different symbol from the singular one, and the SDK's
// own implementation is a command on the real hid server -- which this host
// does not provide. A title that samples its frame input through the plural
// form therefore reads nothing at all, however correct the singular answer is.
// Fill every requested slot with the current host state; the press edges belong
// to the newest slot, which is the one a caller diffs against the previous.
uint64_t pads(R r){
    auto* out=reinterpret_cast<PadState*>(r[0]);
    const int32_t count=int32_t(r[1]);
    if(!out||count<=0) return 0;
    const uint32_t id=r[2]?*reinterpret_cast<const uint32_t*>(r[2]):0;
    const bool live=id==0x20u||id==0u;
    refresh_injection();
    {
        std::lock_guard guard(pad_mutex);
        for(int32_t i=0;i<count;++i){
            out[i]={};
            out[i].sample=++sampling;
            if(!live) continue;
            out[i].attributes=3;
            out[i].buttons=buttons|injected_buttons|(i==count-1?presses:0);
            out[i].lx=lx;out[i].ly=ly;out[i].rx=rx;out[i].ry=ry;
        }
        if(live) presses=0;
    }
    if(input_trace||pad_log_budget.load(std::memory_order_relaxed)>0){
        if(pad_log_budget.fetch_sub(1,std::memory_order_relaxed)>0)
            ot2_host_log("HID_PADS id=%#x n=%d buttons=%llx",id,int(count),
                         (unsigned long long)(live?buttons:0));
    }
    return uint64_t(count);
}
// The roster screen paints a player slot with the colour of the controller
// that player used, and the SDK's own getter aborts the title once the hid
// server turns out to be missing -- svcBreak with reason=0 and
// main+0x2d0c14 on the stack. One handheld controller means one answer.
static void write_npad_color(uint64_t address){
    auto* c=reinterpret_cast<uint32_t*>(address);
    if(!c) return;
    c[0]=0x323232u;   // body
    c[1]=0x323232u;   // buttons
}
uint64_t npad_color(R r){write_npad_color(r[0]);return 0;}
uint64_t npad_color2(R r){write_npad_color(r[0]);write_npad_color(r[1]);return 0;}
uint64_t absent_pads(R r){
    auto* out=reinterpret_cast<PadState*>(r[0]);
    const int32_t count=int32_t(r[1]);
    if(!out||count<=0) return 0;
    for(int32_t i=0;i<count;++i){out[i]={};out[i].sample=++sampling;}
    return uint64_t(count);
}
// Fixed capacity: at most 16 guest fingers and four pending gestures per
// finger. Moves coalesce; a quick down/up remains Start then End across reads.
struct TouchPoint {int64_t delta;uint32_t attributes,id;int32_t x,y,dx,dy,angle,reserved;};
struct TouchState {int64_t sample;int32_t count,reserved;TouchPoint points[16];};
static_assert(sizeof(TouchPoint)==0x28 && sizeof(TouchState)==0x290);
struct Gesture {int host_id{},x{},y{};bool held{},reported{};};
struct Finger {std::array<Gesture,4> queue{};int count{};};
std::array<Finger,16> fingers;
std::mutex touch_mutex;
std::atomic<uint64_t> display_size{(uint64_t(1280)<<32)|720};
int64_t touch_sample{},last_touch_time{};
// max is the template parameter of TouchScreenState<max>: the guest's buffer is
// 0x10 + 0x28 * max bytes, so both the clear and the point count are bounded by
// it. Writing a fixed 16 points into a TouchScreenState<1> would overrun the
// caller's stack.
uint64_t read_touch_n(R r,int max) {
    if(!r[0]) return 0xcc01;
    if(max<1) max=1; else if(max>16) max=16;
    auto& out=*reinterpret_cast<TouchState*>(r[0]);
    memset(&out,0,0x10+sizeof(TouchPoint)*size_t(max));
    std::lock_guard lock(touch_mutex);
    auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto delta=last_touch_time?std::max<int64_t>(0,now-last_touch_time):0;last_touch_time=now;
    out.sample=++touch_sample;
    for(unsigned id=0;id<fingers.size()&&out.count<max;++id){
        auto& finger=fingers[id];if(!finger.count)continue;
        auto& g=finger.queue[0];auto& point=out.points[out.count++];
        point={delta,g.reported?(g.held?0u:2u):1u,id,g.x,g.y,30,30,0,0};g.reported=true;
        if(point.attributes==2){for(int i=1;i<finger.count;++i)finger.queue[i-1]=finger.queue[i];--finger.count;}
    }
    if(input_trace&&out.count)ot2_host_log("HID_TOUCH sample=%lld count=%d first=%u,%u,%d,%d",(long long)out.sample,out.count,out.points[0].id,out.points[0].attributes,out.points[0].x,out.points[0].y);
    // Two facts decide why a touch does nothing, and only one of them is about
    // the finger: whether the title polls the touch panel at all, and whether a
    // point reaches this read. Log both, budgeted.
    static std::atomic<unsigned> polls{0};
    const unsigned poll=polls.fetch_add(1,std::memory_order_relaxed)+1;
    if(out.count){
        if(touch_log_budget.fetch_sub(1,std::memory_order_relaxed)>0)
            ot2_host_log("HID_TOUCH sample=%lld count=%d first=id%u attr%u %d,%d",
                         (long long)out.sample,out.count,out.points[0].id,out.points[0].attributes,
                         out.points[0].x,out.points[0].y);
    }else if(poll==1||poll%600==0){
        ot2_host_log("HID_TOUCH poll n=%u sample=%lld count=0 (panel is polled, no finger down)",poll,(long long)out.sample);
    }
    return 0;
}
uint64_t nothing(R){return 0;}
// GetTouchScreenStates<max>(states, count) fills up to `count` *consecutive*
// TouchScreenState<max> records, newest first. We used to write only record 0
// and leave the rest of the caller's buffer untouched, which is whatever the
// caller's stack happened to hold. Titles do use the second record: the one
// that crashed did
//     ldrsw x12,[sp,#0x298]   ; 0x298 = 0x290 (one TouchScreenState<16>) + 8
//     ldr   w20,[x3,#0xc]    ; x3 advances 0x28 per iteration
// so an uninitialised record[1].count (0x31c9d0bc in the 09:17 crash) became a
// 835-million-iteration loop that walked off the 0x520-byte frame, into the
// guard page, and faulted at main+0x5b74a4. Fill every record instead: the same
// instantaneous state with the sample counter stepped back, which is what
// consecutive scans of a finger resting on the panel look like.
static void fill_touch_states(R r,int max,int n) {
    read_touch_n(r,max);
    if(n<=1) return;
    if(n>16) n=16;
    const size_t size=0x10+sizeof(TouchPoint)*size_t(max);
    auto* base=reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(r[0]));
    for(int i=1;i<n;++i) {
        memcpy(base+size*size_t(i),base,size);
        reinterpret_cast<TouchState*>(base+size*size_t(i))->sample-=int64_t(i);
    }
    static std::atomic<unsigned> logged{0};
    if(logged.fetch_add(1,std::memory_order_relaxed)<4)
        ot2_host_log("HID_TOUCH_STATES n=%d max=%d record_size=%#zx",n,max,size);
}
// The touch template N is part of the symbol name, and a title picks whichever
// instantiation its SDK was built with. Answering only one of them leaves every
// other call falling through to the real hid service.
#define OT2_TOUCH(N) \
    uint64_t read_touch_##N(R r){return read_touch_n(r,N);} \
    uint64_t read_touches_##N(R r){const int32_t n=int32_t(r[1]);if(n<=0)return 0;fill_touch_states(r,N,n);return 1;}
OT2_TOUCH(1) OT2_TOUCH(2) OT2_TOUCH(3) OT2_TOUCH(4) OT2_TOUCH(5) OT2_TOUCH(6) OT2_TOUCH(7) OT2_TOUCH(8)
OT2_TOUCH(9) OT2_TOUCH(10) OT2_TOUCH(11) OT2_TOUCH(12) OT2_TOUCH(13) OT2_TOUCH(14) OT2_TOUCH(15) OT2_TOUCH(16)
uint64_t read_touch(R r){return read_touch_n(r,16);}
uint64_t read_touches(R r){const int32_t n=int32_t(r[1]);if(n<=0)return 0;fill_touch_states(r,16,n);return 1;}
uint64_t vibration_absent(R r){*reinterpret_cast<bool*>(r[0])=false;return 0;}
uint64_t vibration_batch(R r){
    // Permission is disabled for this host (the same capability as D2R).
    // The original SDK can still enqueue a two-device batch during a scene
    // change, independently of the public handle enumeration result. Apply
    // the disabled-output policy at this final sink as well as SendVibrationValue.
    // Completing a suppressed request must not create a Horizon HID session.
    if(int32_t(r[2])<0)ot2_host_die("Invalid vibration batch count");
    static std::atomic<bool> logged{false};
    if(r[2] && !logged.exchange(true))ot2_host_log("HID_VIBRATION_SUPPRESSED count=%d permitted=0",int32_t(r[2]));
    return 0;
}
}
namespace ot2 {
void touch_display_size(uint32_t w,uint32_t h){if(w&&h)display_size.store((uint64_t(w)<<32)|h,std::memory_order_relaxed);}
void touch_event(TouchAction action,int id,float x,float y,int w,int h){
    static std::atomic<unsigned> budget{40};
    if(budget.fetch_sub(1,std::memory_order_relaxed)>0)
        ot2_host_log("HOST_TOUCH action=%d id=%d at=%.1f,%.1f view=%dx%d",int(action),id,x,y,w,h);
    if(id<0||w<=0||h<=0||!std::isfinite(x)||!std::isfinite(y))return;
    const auto size=display_size.load(std::memory_order_relaxed);
    double sw=size>>32,sh=uint32_t(size),scale=std::min(w/sw,h/sh);
    const int dw=std::max(1,int(sw*scale)),dh=std::max(1,int(sh*scale)),dx=(w-dw)/2,dy=(h-dh)/2;
    if(action==TouchAction::down&&(x<dx||y<dy||x>=dx+dw||y>=dy+dh))return;
    int px=std::min(1279,int(std::clamp((x-dx)/dw,0.0f,1.0f)*1280)),py=std::min(719,int(std::clamp((y-dy)/dh,0.0f,1.0f)*720));
    bool changed=false;
    {
        std::lock_guard lock(touch_mutex);
        Gesture* active=nullptr;
        for(auto& f:fingers)for(int i=0;i<f.count;++i)if(f.queue[i].held&&f.queue[i].host_id==id)active=&f.queue[i];
        if(action==TouchAction::down){
            if(active)return;
            Finger* target=nullptr;
            for(auto& f:fingers)if(f.count&&f.queue[f.count-1].host_id==id){if(f.count==4)return;target=&f;break;}
            if(!target)for(auto& f:fingers)if(!f.count){target=&f;break;}
            if(!target)for(auto& f:fingers)if(f.count<4&&!f.queue[f.count-1].held){target=&f;break;}
            if(target){target->queue[target->count++]={id,px,py,true,false};changed=true;}
        }else if(active){
            active->x=px;active->y=py;
            if(action==TouchAction::up){active->held=false;changed=true;}
        }
    }
    // A finger going down or up is the same "input state changed" condition the
    // pad reports; see the note in setNativePad. Moves are excluded on purpose:
    // they arrive at frame rate and the event is a transition, not a tick.
    if(changed)signal_attached_events();
}
void touch_cancel(){
    std::lock_guard lock(touch_mutex);
    for(auto& f:fingers){
        if(f.count&&f.queue[0].reported){f.count=1;f.queue[0].held=false;}
        else f.count=0;
    }
}
}
#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeTouch(JNIEnv*,jclass,jint action,jint id,jfloat x,jfloat y,jint w,jint h){if(action>=0&&action<=2)ot2::touch_event(ot2::TouchAction(action),id,x,y,w,h);}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_cancelNativeTouch(JNIEnv*,jclass){ot2::touch_cancel();ot2::signal_attached_events();}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeInputTrace(JNIEnv*,jclass,jboolean enabled){input_trace=enabled;}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_clearNativePad(JNIEnv*,jclass){
    {std::lock_guard lock(pad_mutex);buttons=presses=0;lx=ly=rx=ry=0;}
    ot2::touch_cancel();
    ot2::signal_attached_events();
}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativePad(JNIEnv*,jclass,jlong value,jint x,jint y,jint z,jint w) {
    bool changed;
    {
        std::lock_guard lock(pad_mutex);lx=x;ly=y;rx=z;ry=w;
        uint64_t next=uint64_t(value)|(x<-16383?1ULL<<16:0)|(y>16383?1ULL<<17:0)|(x>16383?1ULL<<18:0)|(y<-16383?1ULL<<19:0)
            |(z<-16383?1ULL<<20:0)|(w>16383?1ULL<<21:0)|(z>16383?1ULL<<22:0)|(w<-16383?1ULL<<23:0);
        changed=next!=buttons;
        presses|=next&~buttons;buttons=next;
    }
    // These are the "the controller state changed" events the title builds with
    // AttachReadableHandleToSystemEvent. Nothing else on this host can produce
    // that condition, so a title waiting for one only ever gets past it when the
    // user actually presses something -- which is exactly the hardware
    // behaviour, and is why this is a transition and not a per-frame pulse.
    if(changed) ot2::signal_attached_events();
}
#endif
namespace ot2 {
const std::vector<HostService>& input_services(){static const std::vector<HostService> table={
    // One controller, in the handheld slot: bit1 of NpadStyleTag. Reporting a
    // second (full-key) controller as well is what pushed the title into a
    // controller path this host cannot serve.
    {"_ZN2nn3hid15GetNpadStyleSetERKj",[](R r)->uint64_t{
        const uint32_t id=r[0]?*reinterpret_cast<const uint32_t*>(r[0]):0;
        return id==0x20u?2u:0u;
    }},
    // Only the handheld slot reports a live state. Announcing a full-key
    // controller as well sent the title down its controller-initialisation
    // path, which walks into the real hid server and faults there -- the host
    // does not provide that service. Handheld is the mode this host claims.
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",absent_pad},
    {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",absent_pad},
    // The plural forms: same controller policy, one answer per requested slot.
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",pads},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",absent_pads},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",absent_pads},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",absent_pads},
    {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",absent_pads},
    // Controller colour: the answer is UI data (a player slot is tinted with
    // it), and the SDK's version reaches the hid server we do not have.
    {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj",npad_color},
    {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj",npad_color2},
    {"_ZN2nn3hid6detail22GetNpadControllerColorEPNS0_19NpadControllerColorERKj",npad_color},
    {"_ZN2nn3hid6detail22GetNpadControllerColorEPNS0_19NpadControllerColorES3_RKj",npad_color2},
    // Touch: the panel is virtual, so "initialize" only has to succeed. Left
    // unpatched, the SDK's own version opens a hid session through sm, and a
    // title that treats a failed init as "no touch panel" then never polls.
    {"_ZN2nn3hid21InitializeTouchScreenEv",nothing},
    {"_ZN2nn3hid21InitializeTouchScreenEii",nothing},
    {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",nothing},
    {"_ZN2nn3hid20IsVibrationPermittedEv",nothing},
    {"_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE",nothing},
    {"_ZN2nn3hid6detail20IsVibrationPermittedEPb",vibration_absent},
    {"_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE",vibration_absent},
    {"_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",vibration_batch},
    {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE",nothing},
    {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",nothing},
    {"_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,8);return 0;}},
    {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",nothing},
    {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",nothing},
    {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",nothing},
#define OT2_TOUCH_ENTRY(N) \
    {"_ZN2nn3hid19GetTouchScreenStateILm" #N "EEEvPNS0_16TouchScreenStateIXT_EEE",read_touch_##N}, \
    {"_ZN2nn3hid20GetTouchScreenStatesILm" #N "EEEiPNS0_16TouchScreenStateIXT_EEEi",read_touches_##N},
    OT2_TOUCH_ENTRY(1) OT2_TOUCH_ENTRY(2) OT2_TOUCH_ENTRY(3) OT2_TOUCH_ENTRY(4)
    OT2_TOUCH_ENTRY(5) OT2_TOUCH_ENTRY(6) OT2_TOUCH_ENTRY(7) OT2_TOUCH_ENTRY(8)
    OT2_TOUCH_ENTRY(9) OT2_TOUCH_ENTRY(10) OT2_TOUCH_ENTRY(11) OT2_TOUCH_ENTRY(12)
    OT2_TOUCH_ENTRY(13) OT2_TOUCH_ENTRY(14) OT2_TOUCH_ENTRY(15) OT2_TOUCH_ENTRY(16)
#undef OT2_TOUCH_ENTRY
    {"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,40);*reinterpret_cast<int64_t*>(r[0])=++sampling;return 0;}},
    // This SDK copies three 16-byte pairs (detail function at sdk+0x217d40).
    {"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",[](R r)->uint64_t{memset(reinterpret_cast<void*>(r[0]),0,48);*reinterpret_cast<int64_t*>(r[0])=++sampling;return 0;}},
    {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",[](R r)->uint64_t{auto* out=reinterpret_cast<uint32_t*>(r[0]);if(out){out[0]=1;out[1]=0x20;}return 0;}},
};return table;}
}
