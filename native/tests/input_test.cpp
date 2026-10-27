#include "../host_services.h"
#include "../input.h"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C" [[noreturn]] void ot2_host_die(const char* format,...) {
    char text[256];va_list ap;va_start(ap,format);vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    throw std::runtime_error(text);
}
extern "C" void ot2_host_log(const char*,...) {}
template<class T> uint64_t address(T* p){return reinterpret_cast<uint64_t>(p);}
uint64_t call(const char* name,std::initializer_list<uint64_t> args){
    uint64_t r[8]{};std::copy(args.begin(),args.end(),r);
    for(auto& s:ot2::input_services())if(!strcmp(s.name,name))return s.call(r);
    throw std::runtime_error(std::string("Missing input service: ")+name);
}
void require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
int main(){try{
    std::array<uint32_t,2> handles{0x10203,0x10204};uint32_t id=0x20;
    require(call("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",{address(handles.data()),2,address(&id),2})==0,"no vibration devices");
    require(call("_ZN2nn3hid20IsVibrationPermittedEv",{})==0,"vibration disabled");
    bool permitted=true;
    require(call("_ZN2nn3hid6detail20IsVibrationPermittedEPb",{address(&permitted)})==0&&!permitted,"detail permission is disabled");
    std::array<float,8> values{0.5f,160.f,0.5f,320.f,0.f,160.f,0.f,320.f};
    const char* send="_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi";
    require(call(send,{0,0,0})==0,"empty SDK batch");
    // Real failure: scene transition submits two values even though enumeration
    // returned zero. A disabled actuator must not abort the game at this seam.
    require(call(send,{address(handles.data()),address(values.data()),2})==0,"disabled two-device batch");
    require(handles[0]==0x10203&&values[0]==0.5f,"send does not mutate caller buffers");
    require(call("_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE",{address(handles.data())})==0,"no mounted actuator after send");
    struct Point {int64_t dt;uint32_t attributes,id;int32_t x,y,dx,dy,angle,reserved;};
    struct Touch {int64_t sample;int32_t count,reserved;Point points[16];};
    static_assert(sizeof(Touch)==0x290);
    Touch t{};
    const char* one="_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE";
    const char* many="_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi";
    auto read=[&]{call(one,{address(&t)});};
    using A=ot2::TouchAction;
    auto event=[&](A a,int id,float x,float y){ot2::touch_event(a,id,x,y,1600,1000);};
    require(call(many,{0,0})==0,"zero-capacity touch read does not access output");
    event(A::down,31,800,20);read();require(t.count==0,"letterbox down is ignored");
    event(A::up,31,800,20);
    event(A::down,31,800,500);event(A::up,31,800,500);
    read();require(t.count==1&&t.points[0].attributes==1&&t.points[0].x==640&&t.points[0].y==360,"short tap start and letterbox mapping");
    auto finger=t.points[0].id;
    read();require(t.count==1&&t.points[0].attributes==2&&t.points[0].id==finger,"short tap end");read();require(t.count==0,"tap retired");
    event(A::down,7,400,500);event(A::up,7,400,500);event(A::down,7,1200,500);event(A::up,7,1200,500);
    read();require(t.count==1&&t.points[0].attributes==1&&t.points[0].x==320,"reused pointer first start");
    read();require(t.points[0].attributes==2,"reused pointer first end");
    read();require(t.points[0].attributes==1&&t.points[0].x==960,"reused pointer second start");
    read();require(t.points[0].attributes==2,"reused pointer second end");read();require(t.count==0,"reused pointer retired");
    event(A::down,1,500,500);ot2::touch_cancel();read();require(t.count==0,"cancelled unread down is not a tap");
    event(A::down,1,500,500);event(A::down,9,600,500);read();require(t.count==2&&t.points[0].id!=t.points[1].id,"independent fingers");
    event(A::move,9,2000,-100);read();require(t.points[1].x==1279&&t.points[1].y==0,"drag clamps outside image");
    ot2::touch_cancel();read();require(t.count==2&&t.points[0].attributes==2&&t.points[1].attributes==2,"cancel ends sampled contacts");
    read();require(t.count==0,"cancel leaves no stuck touches");
    for(int i=0;i<17;i++)event(A::down,i,800,500);
    // count is the number of *records*, not the point capacity: asking for one
    // record still reports all sixteen fingers. (It used to be 16 here, which
    // only worked because the old handler ignored count and filled record 0.)
    require(call(many,{address(&t),1})==1&&t.count==16,"at most sixteen fingers, one newest state");
    ot2::touch_cancel();read();read();require(t.count==0,"capacity saturation clears");
    ot2::touch_display_size(4,3);event(A::down,1,100,500);read();require(t.count==0,"pillarbox down is ignored for actual source aspect");
    event(A::down,1,800,500);read();require(t.count==1&&t.points[0].x==640,"non-widescreen image mapping");
    ot2::touch_cancel();read();read();
    // Regression: GetTouchScreenStates<16>(states, 2) must initialise *both*
    // records. Record 1 used to be left as whatever the caller's stack held, and
    // the title reads its count as a loop bound — 0x31c9d0bc in the field crash —
    // then walks 24 entries at 0x28 apiece past the end of its 0x520-byte frame,
    // into the guard page, and faults at main+0x5b74a4 (ldr w20,[x3,#0xc]).
    // Poison the buffer first so an unwritten record cannot pass by luck.
    {
        std::array<Touch,2> two;
        memset(two.data(),0xA5,sizeof(two));
        ot2::touch_cancel();read();read();
        require(call(many,{address(two.data()),2})==1,"two-state read succeeds");
        for(int i=0;i<2;++i){
            const int c=two[size_t(i)].count;
            require(c>=0&&c<=16,"every requested touch record is initialised");
        }
        require(two[1].sample!=int64_t(0xA5A5A5A5A5A5A5A5),"second record was written");
        require(two[1].sample<two[0].sample,"older record carries the earlier sample");
    }
    std::cout<<"Input capability, vibration and touchscreen lifecycle regression passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
