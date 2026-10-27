#include "guest.h"
#include "graphics/traced_mutex.h"
#include "fault_decode.h"
#include "guest_clock.h"
#include "frame_rate_patch.h"
#include "guest_context.h"
#include "native_abi.h"
#include "thread_services.h"
#include "host_services.h"
#include "content_locale.h"
#include "graphics/nvn_dirty.h"
#include <asm/sigcontext.h>
#include "graphics/backend.h"
#include <android/log.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <thread>
#include <vector>

extern "C" {
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_register_gates_enabled=0;
alignas(4) __attribute__((visibility("hidden"))) uint32_t ot2_native_abis[2048]{};
void ot2_signal_entry(int, siginfo_t*, void*);
uint64_t ot2_guest_call(uintptr_t, GuestThread*, const uint64_t*, uintptr_t);
void ot2_host_invoke(void*,uint64_t*,void*,uintptr_t);
void ot2_abi_fixture();
void ot2_native_entry();
// Zero-returning stand-in used when a native API has no implementation.
void ot2_native_zero();
uint64_t ot2_host_stack_call(uint64_t (*)(uint64_t*),uint64_t*,uintptr_t);
}
// Sampling signal for the watchdog; kept away from SIGUSR1, which ART owns.
static const int OT2_SAMPLE_SIGNAL=SIGRTMIN+4;
extern "C" { extern int32_t ot2_guest_tids[GuestThreadCapacity]; }
// 诊断计数：guest 总共调用了多少次 svcGetSystemTick（SVC 0x1e）。
// 必须定义在匿名命名空间之外——匿名命名空间是内部链接，xbg_game.cpp 链接不到
// （已踩过一次：ld.lld undefined symbol ot2_sys_tick_calls）。
// 用途见 ticks()：判定游戏内时间是由真实时间驱动，还是由帧数驱动。
std::atomic<uint64_t> ot2_sys_tick_calls{0};
namespace {
struct Trap { uintptr_t pc; uint32_t instruction; const char* hook; uint64_t (*call)(uint64_t*)=nullptr; uint64_t hits=0; };
std::vector<Trap> traps;
alignas(4) uint32_t fast_tls_enabled=1;
std::atomic<uint64_t> tls_signal_count{0};
std::atomic<uint64_t> svc_signal_count{0},counter_signal_count{0},sdk_signal_count{0};
struct NativeGate {char name[160];void* function;uint64_t hits;bool present;};
NativeGate native_gates[2048]{};
std::atomic<unsigned> native_gate_count{0};
uint8_t* native_gate_code=nullptr;
std::mutex native_gate_mutex;
std::vector<ot2::ModuleView> images;
int log_fd=-1;
uint8_t* address_space{};
constexpr size_t reserve_size=128ULL<<30;
constexpr size_t alias_offset=0x10000000, alias_size=96ULL<<30;
constexpr size_t stack_offset=100ULL<<30, stack_capacity=512ULL<<20;
constexpr size_t heap_offset=101ULL<<30, heap_capacity=4ULL<<30;
size_t heap_size=0;
uint64_t image_size=0;
struct Mapping { uintptr_t base; size_t size; uint32_t state; uintptr_t source; };
std::vector<Mapping> mappings;
ot2::TracedMutex mapping_mutex{"mapping"};
std::mutex setup_mutex;
bool thread_system_ready=false;
// Set from the per-game marker in boot_game(); see the bring-up block below.
bool diagnostic_svc_enabled=false;
ot2::LoadedGame* active_game=nullptr;
thread_local ucontext_t* active_signal=nullptr;
thread_local uintptr_t service_callback_stack=0;
thread_local unsigned host_service_depth=0;

void log(const char* fmt, ...) {
    char buffer[2048]; va_list args; va_start(args,fmt);
    vsnprintf(buffer,sizeof(buffer),fmt,args); va_end(args);
    __android_log_write(ANDROID_LOG_INFO,"OT2Guest",buffer);
    if(log_fd>=0) { write(log_fd,buffer,strlen(buffer)); write(log_fd,"\n",1); }
}
[[noreturn]] void fail(const char* reason, const ucontext_t* uc=nullptr) {
    log("STOP %s",reason);
    if(uc) {
        const auto& c=uc->uc_mcontext;
        log("pc=%llx lr=%llx sp=%llx",c.pc,c.regs[30],c.sp);
        for(const auto& m:images) {
            auto b=reinterpret_cast<uintptr_t>(m.base);
            if(c.pc>=b && c.pc<b+m.size) log("PC %s+0x%llx",m.name.c_str(),c.pc-b);
            if(c.regs[30]>=b && c.regs[30]<b+m.size) log("LR %s+0x%llx",m.name.c_str(),c.regs[30]-b);
        }
        for(unsigned i=0;i<31;i+=3) log("x%u=%llx x%u=%llx x%u=%llx",i,c.regs[i],i+1,i+1<31?c.regs[i+1]:0,i+2,i+2<31?c.regs[i+2]:0);
        uintptr_t fp=c.regs[29];
        for(unsigned depth=0;depth<24 && fp;depth++) {
            bool readable=false;
            for(unsigned n=0;n<GuestThreadCapacity;n++) {
                const auto& t=ot2_threads[n];
                if(fp>=t.stack && fp+16<=t.stack+t.stack_size) readable=true;
            }
            if(!readable || (fp&15)) break;
            const auto* frame=reinterpret_cast<const uintptr_t*>(fp);
            for(const auto& m:images) {
                auto base=reinterpret_cast<uintptr_t>(m.base);
                if(frame[1]>=base && frame[1]<base+m.size) log("FRAME %u %s+0x%llx",depth,m.name.c_str(),(unsigned long long)(frame[1]-base));
            }
            if(frame[0]<=fp) break;
            fp=frame[0];
        }
    }
    if(log_fd>=0) fsync(log_fd);
    _exit(71);
}
GuestThread* current() { return ot2_guest_context_current(); }
ot2::GuestClock guest_clock;
uint64_t monotonic_ns() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000000000 + uint64_t(ts.tv_nsec);
}
uint64_t ticks() {
    const auto ns=guest_clock.read(monotonic_ns);
    return (ns/1000000000)*19200000 + (ns%1000000000)*12/625;
}
GuestThread* create_thread_context() { return ot2_guest_context_create(); }
void install_handlers() {
    struct sigaction action{}; action.sa_sigaction=ot2_signal_entry;
    action.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER; sigemptyset(&action.sa_mask);
    for(int sig:{SIGTRAP,SIGSEGV,SIGBUS,SIGILL,SIGFPE,OT2_SAMPLE_SIGNAL}) if(sigaction(sig,&action,nullptr)) fail("Signal handler setup failed");
}
// Host module ranges, read once by the watchdog thread before sampling. The
// signal handler cannot open files, so the table has to exist beforehand; it is
// what turns a raw return address into "libot2host.so+0x1234", which addr2line
// can resolve offline.
struct HostRange { uintptr_t begin,end; char name[40]; };
std::atomic<bool> dump_stacks{false};
// Sampling without logging: a signal handler that calls log() re-enters the log
// mutex from a thread that may already hold it, which deadlocks the whole title.
// The `trace-services` probe therefore only records the interrupted PC here and
// lets the watchdog thread print it a moment later.
// A single PC says where a thread is, not why: a stalled title usually parks the
// logic thread inside libc or the Vulkan driver, and the interesting part is the
// guest frames underneath. The handler therefore also copies the return-address
// candidates off that thread's stack; resolving them happens later on the
// watchdog, because the handler must not touch the log.
constexpr unsigned PcSampleFrames=16;
// `fp` is kept because a stall is often not the sampled thread: the logic thread
// parks on a join while the real culprit is one of the workers, and only the
// frame chain of *that* thread says which subsystem it died in. The scan above
// only finds addresses that look like code, which is useless for the workers.
struct PcSample {int32_t tid;uintptr_t pc;uintptr_t lr;uintptr_t sp;const char* module;uintptr_t offset;
                 unsigned frames;uintptr_t stack[PcSampleFrames];uintptr_t fp;};
constexpr unsigned PcSampleSlots=64;
PcSample pc_samples[PcSampleSlots];
std::atomic<unsigned> pc_sample_count{0};
std::atomic<bool> pc_sample_store{false};
// ---- Continuous PC histogram (flat profile) ------------------------------
// A single interrupted PC per two seconds cannot tell "which code is burning
// the CPU" from noise, and the stack scan below costs 120 ms per sweep so it can
// never be run at a useful rate. This second mode samples every guest thread at
// ~500 Hz for two seconds and accumulates *exact* PC hits, which ranks the
// functions of a stalled frame instead of guessing at them from one address.
//
// Exact PCs are enough: an AArch64 hot loop re-enters the same handful of
// instructions, so a tight loop shows up as a few thousand hits on two or three
// buckets and the surrounding call chain stays cheap to read by hand. Open
// addressing with linear probe keeps the handler to a few atomics -- it must stay
// async-signal-safe and it must never take the log mutex.
struct PcHit { std::atomic<uintptr_t> pc{0}; std::atomic<int32_t> tid{0}; std::atomic<uint32_t> hits{0}; };
constexpr unsigned PcHitSlots=4096;
PcHit pc_hits[PcHitSlots];
std::atomic<bool> pc_profile_store{false};
std::atomic<uint64_t> pc_profile_samples{0};
// Called from the signal handler: atomics only, no allocation, no logging.
//
// The bucket key is (tid, pc), not pc alone. Keying on pc merges every thread
// that happens to be parked on the same libc instruction -- 25 idle threads
// waiting in futex produced one bogus bucket worth 92% of all samples, which
// reads exactly like a hot loop and is the opposite of one. With tid in the key
// a parked thread shows up as one bucket with a few hundred hits and no CPU time
// behind it, which the CPU table printed next to it exposes immediately.
static inline void pc_hit_record(uintptr_t pc,int32_t tid) {
    // pc>>2 because instructions are 4-byte aligned; mixing in the tid keeps two
    // threads at the same PC in different buckets.
    unsigned i=((unsigned)((pc>>2)*2654435761u)^((unsigned)tid*2246822519u))&(PcHitSlots-1);
    for(unsigned step=0;step<8;step++,i=(i+1)&(PcHitSlots-1)) {
        const uintptr_t slot=pc_hits[i].pc.load(std::memory_order_relaxed);
        if(slot==pc&&pc_hits[i].tid.load(std::memory_order_relaxed)==tid) {
            pc_hits[i].hits.fetch_add(1,std::memory_order_relaxed); return;
        }
        if(!slot) {
            uintptr_t expected=0;
            if(pc_hits[i].pc.compare_exchange_strong(expected,pc,std::memory_order_relaxed)) {
                pc_hits[i].tid.store(tid,std::memory_order_relaxed);
                pc_hits[i].hits.store(1,std::memory_order_relaxed);
                return;
            }
            if(expected==pc&&pc_hits[i].tid.load(std::memory_order_relaxed)==tid) {
                pc_hits[i].hits.fetch_add(1,std::memory_order_relaxed); return;
            }
        }
    }
}
std::atomic<int32_t> pc_sample_target{0};
std::atomic<uint64_t> svc_calls[256];
void dump_svc_hot();
void dump_native_hot();
void dump_service_delta();
void dump_sleep_sites();
static void cpu_profile_burst();
// ---- Sleep-site histogram ---------------------------------------------------
// A title that stops advancing very often sits in a "sleep a few ms, re-check"
// loop, and that loop *is* the bug: the flag it polls is never set. Sleeps carry
// no handle to inspect, so the only thing that names the loop is the guest return
// address that asked for it. Keep that address and how long it asked to sleep.
struct SleepSite { std::atomic<uintptr_t> pc{0}; std::atomic<uint32_t> hits{0}; std::atomic<uint64_t> ns{0};
                   std::atomic<uintptr_t> caller{0}; std::atomic<int32_t> tid{0}; };
constexpr unsigned SleepSiteSlots=48;
SleepSite sleep_sites[SleepSiteSlots];
// One frame up the guest's own frame-pointer chain: the call site of whatever
// contains the sleep. Only read when x29 points inside a known guest stack, so a
// broken frame cannot turn the diagnosis into a fault.
uintptr_t caller_of(uintptr_t fp) {
    if(!fp || (fp&7)) return 0;
    for(unsigned n=0;n<GuestThreadCapacity;n++) {
        const auto& t=ot2_threads[n];
        if(fp>=t.stack && fp+16<=t.stack+t.stack_size) {
            uintptr_t pair[2]{};
            memcpy(pair,reinterpret_cast<const void*>(fp),sizeof pair);
            return pair[1];
        }
    }
    return 0;
}
void record_sleep(uintptr_t pc,uint64_t duration,uintptr_t caller,int32_t tid) {
    for(unsigned i=0;i<SleepSiteSlots;i++) {
        uintptr_t slot=sleep_sites[i].pc.load(std::memory_order_relaxed);
        if(slot==pc) { sleep_sites[i].hits.fetch_add(1,std::memory_order_relaxed); sleep_sites[i].ns.store(duration,std::memory_order_relaxed); return; }
        if(slot) continue;
        if(sleep_sites[i].pc.compare_exchange_strong(slot,pc,std::memory_order_relaxed)) {
            sleep_sites[i].hits.store(1,std::memory_order_relaxed); sleep_sites[i].ns.store(duration,std::memory_order_relaxed);
            sleep_sites[i].caller.store(caller,std::memory_order_relaxed); sleep_sites[i].tid.store(tid,std::memory_order_relaxed); return;
        }
        if(slot==pc) { sleep_sites[i].hits.fetch_add(1,std::memory_order_relaxed); sleep_sites[i].ns.store(duration,std::memory_order_relaxed); return; }
    }
}
// ---- Service-call trace ring -------------------------------------------------
// Marker gate: <game>/trace-services. A title that stalls on one screen keeps
// presenting frames, so the only evidence left is which service calls stop
// happening -- and in what order they happened just before the stop. Every hook
// the trap handler dispatches through is already carrying its own symbol name,
// so recording (name, caller tid, result) is enough to reconstruct the sequence.
// Records are only written while the marker exists; the watchdog prints what
// arrived since its previous tick so the log stays readable.
struct ServiceCallRecord { const char* name; uint32_t result; int32_t tid; };
constexpr unsigned ServiceCallRingSize=1u<<14;
std::array<ServiceCallRecord,ServiceCallRingSize> service_call_ring{};
std::atomic<unsigned> service_call_head{0};
std::atomic<bool> trace_services{false};
unsigned service_call_printed=0;
void dump_service_trace() {
    const unsigned head=service_call_head.load(std::memory_order_relaxed);
    if(head==service_call_printed) { log("TRACE idle calls=%u (no service call since last tick)",head); return; }
    unsigned first=service_call_printed;
    if(head-first>ServiceCallRingSize) first=head-ServiceCallRingSize;
    unsigned lines=0,omitted=0;
    for(unsigned i=first;i<head;i++) {
        const auto& record=service_call_ring[i%ServiceCallRingSize];
        // Collapse a run of identical calls: a spin shows up as a count, and a
        // single call right before the stall stays visible instead of scrolling.
        unsigned repeat=1;
        while(i+repeat<head) {
            const auto& next=service_call_ring[(i+repeat)%ServiceCallRingSize];
            if(next.name!=record.name || next.tid!=record.tid || next.result!=record.result) break;
            ++repeat;
        }
        if(lines<64) log("TRACE #%u x%u tid=%d res=%#x %s",i,repeat,record.tid,record.result,record.name?record.name:"?");
        else omitted+=repeat;
        i+=repeat-1; ++lines;
    }
    if(omitted) log("TRACE omitted %u calls",omitted);
    log("TRACE batch %u..%u",first,head);
    service_call_printed=head;
}
HostRange host_ranges[64];
std::atomic<unsigned> host_range_count{0};
void load_host_ranges() {
    FILE* maps=fopen("/proc/self/maps","re");
    if(!maps) return;
    char line[512]; unsigned count=0;
    while(fgets(line,sizeof line,maps) && count<64) {
        uintptr_t begin=0,end=0; char perms[8]={}; char path[256]={};
        if(sscanf(line,"%lx-%lx %7s %*s %*s %*s %255s",&begin,&end,perms,path)!=4) continue;
        if(!strstr(path,".so") || !strstr(perms,"x")) continue;
        const char* slash=strrchr(path,'/');
        auto& slot=host_ranges[count++];
        slot.begin=begin;slot.end=end;
        snprintf(slot.name,sizeof slot.name,"%s",slash?slash+1:path);
    }
    fclose(maps);
    host_range_count.store(count,std::memory_order_release);
}
// Walks the frame chain from the interrupted frame pointer. The build keeps
// frame pointers, so this reaches host functions that hold the lock the guest is
// waiting on — the lr alone only names the leaf.
void log_host_stack(const char* tag,unsigned sample,uintptr_t fp,uintptr_t lr) {
    const unsigned count=host_range_count.load(std::memory_order_acquire);
    auto resolve=[&](uintptr_t address,char* out,size_t size) {
        for(unsigned i=0;i<count;i++)
            if(address>=host_ranges[i].begin && address<host_ranges[i].end) {
                snprintf(out,size,"%s+0x%llx",host_ranges[i].name,(unsigned long long)(address-host_ranges[i].begin));
                return;
            }
        // Guest frames land in the loaded NSO images, so they have to be named
        // too: a chain that only resolves host modules shows "?" for every level
        // that matters, which is what made earlier stack dumps useless.
        for(const auto& m:images) {
            const auto base=reinterpret_cast<uintptr_t>(m.base);
            if(address>=base && address<base+m.size) {
                snprintf(out,size,"%s+0x%llx",m.name.c_str(),(unsigned long long)(address-base));
                return;
            }
        }
        snprintf(out,size,"%#llx",(unsigned long long)address);
    };
    char text[40];
    resolve(lr,text,sizeof text);
    log("STACK #%u %s lr=%s",sample,tag,text);
    for(unsigned depth=0;depth<14 && fp>=4096;depth++) {
        const auto* frame=reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next=frame[0],ret=frame[1];
        resolve(ret,text,sizeof text);
        log("STACK #%u   [%u] %s",sample,depth,text);
        if(next<=fp || (next-fp)>1024*1024) break;   // not a plausible caller frame
        fp=next;
    }
}
// The frame-pointer walk above stops at the first SDK frame because the SDK is
// built without frame pointers, so every condvar wait reads as the same
// "sdk+0x3511b0". The caller chain still exists as return addresses left on the
// stack; scanning upward from the interrupted sp recovers it and is what turns
// "parked in some SDK wait" into "parked in this game function". Only slots that
// resolve into a loaded image are printed -- a raw scan is full of stale values,
// so the filter is what makes the chain readable.
void log_guest_scan(const char* tag, uintptr_t sp, uintptr_t lr, uintptr_t waddr, uintptr_t wkey) {
    char text[40];
    auto frame_of=[&](uintptr_t address,char* out,size_t size) {
        // Only executable bytes can be a return address. Without this filter the
        // scan is dominated by static-object addresses that happen to sit in a
        // module's bss, which is exactly the noise that hides the real chain.
        for(const auto& m:images) {
            const auto lo=reinterpret_cast<uintptr_t>(m.base)+m.text_offset;
            const auto hi=lo+m.text_size;
            if(address>=lo && address<hi) {
                snprintf(out,size,"%s+0x%llx",m.name.c_str(),(unsigned long long)(address-reinterpret_cast<uintptr_t>(m.base)));
                return true;
            }
        }
        return false;
    };
    auto any_of=[&](uintptr_t address,char* out,size_t size) {
        if(frame_of(address,out,size)) return true;
        const unsigned count=host_range_count.load(std::memory_order_acquire);
        for(unsigned i=0;i<count;i++)
            if(address>=host_ranges[i].begin && address<host_ranges[i].end) {
                snprintf(out,size,"%s+0x%llx",host_ranges[i].name,(unsigned long long)(address-host_ranges[i].begin));
                return true;
            }
        return false;
    };
    // Data words are not code: a message payload legitimately points into a
    // module's bss, which frame_of() rejects by design. Naming it against the
    // whole module keeps the ring dump readable.
    auto region_name=[&](uintptr_t value,char* out,size_t size) {
        for(const auto& m:images) {
            const auto b=reinterpret_cast<uintptr_t>(m.base);
            if(value>=b && value<b+m.size) {
                snprintf(out,size,"%s+0x%llx",m.name.c_str(),(unsigned long long)(value-b));
                return true;
            }
        }
        return any_of(value,out,size);
    };
    if(!any_of(lr,text,sizeof text)) snprintf(text,sizeof text,"%#llx",(unsigned long long)lr);
    log("GSTACK %s lr=%s sp=%#llx waddr=%#llx wkey=%#llx",tag,text,
        (unsigned long long)sp,(unsigned long long)waddr,(unsigned long long)wkey);
    unsigned shown=0;
    // Read through /proc/self/mem instead of dereferencing: the scan walks off the
    // end of the stack by design, and this runs inside the SVC handler where taking
    // the mapping lock to ask for permissions could deadlock.
    static int memfd=-2;
    if(memfd==-2)memfd=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
    if(memfd<0)return;
    // nn::os message queues park on svcWaitProcessWideKeyAtomic with x0 = &lock at
    // object+0x38 and x1 = &condvar at object+0x3c, so the wait address recovers the
    // whole queue. Its ring counters (+0x28 capacity, +0x2c count) and buffer
    // pointer (+0x20) are the only evidence available for "the receiver never
    // drained it": send and receive are pure guest code with no SVC to observe.
    {
        const uintptr_t obj=waddr-0x38;
        if(waddr>0x10000 && waddr!=(uintptr_t)-1 && obj>0x10000) {
            char line[300]; size_t used=0;
            for(unsigned i=0;i<18 && used+20<sizeof line;i++) {
                uint32_t word=0;
                if(pread(memfd,&word,4,off_t(obj+i*4))!=4) break;
                used+=(size_t)snprintf(line+used,sizeof line-used,"%s%08x",i?" ":"",word);
            }
            uint32_t cap=0,cnt=0,buf_lo=0,buf_hi=0;
            pread(memfd,&cap,4,off_t(obj+0x28)); pread(memfd,&cnt,4,off_t(obj+0x2c));
            pread(memfd,&buf_lo,4,off_t(obj+0x20)); pread(memfd,&buf_hi,4,off_t(obj+0x24));
            if(used) log("GSTACK %s   obj[%#llx] cap=%u count=%u buffer=%#x%08x %s",tag,
                (unsigned long long)obj,cap,cnt,buf_hi,buf_lo,line);
            // The ring holds what the blocked producer was trying to hand off.
            // nn::os message queues carry pointer-sized messages, so the words
            // name the consumer that is missing: a run of module addresses is a
            // deferred-work queue, a heap address is a buffer handoff.
            const uintptr_t buf=uintptr_t(buf_lo)|(uintptr_t(buf_hi)<<32);
            if(cap && cap<=64 && buf>0x10000) {
                for(unsigned base=0;base<cap;base+=6) {
                    char dump[360]; size_t n=0;
                    for(unsigned i=base;i<cap && i<base+6;i++) {
                        uint64_t value=0;
                        if(pread(memfd,&value,8,off_t(buf+i*8))!=8) break;
                        char name[48];
                        if(!value) snprintf(name,sizeof name,"0");
                        else if(!region_name(value,name,sizeof name)) snprintf(name,sizeof name,"%#llx",(unsigned long long)value);
                        n+=(size_t)snprintf(dump+n,sizeof dump-n," [%u]%s",i,name);
                    }
                    if(n) log("GSTACK %s   msg%s",tag,dump);
                }
                // A pointer-sized message is opaque, but its target usually
                // starts with a vtable. Naming that vtable slot against the
                // module is what turns "32 anonymous heap objects" into a class:
                // the consumer of the queue is one of those methods.
                for(unsigned i=0;i<cap && i<4;i++) {
                    uint64_t msg=0;
                    if(pread(memfd,&msg,8,off_t(buf+i*8))!=8 || msg<0x10000) continue;
                    char words[300]; size_t n=0;
                    for(unsigned k=0;k<5;k++) {
                        uint64_t value=0;
                        if(pread(memfd,&value,8,off_t(msg+k*8))!=8) break;
                        char name[48];
                        if(!value) snprintf(name,sizeof name,"0");
                        else if(!region_name(value,name,sizeof name)) snprintf(name,sizeof name,"%#llx",(unsigned long long)value);
                        n+=(size_t)snprintf(words+n,sizeof words-n," +%x:%s",k*8,name);
                    }
                    if(n) log("GSTACK %s   body[%u]%s",tag,i,words);
                }
            }
        }
    }
    for(unsigned k=0;k<768 && shown<14;k++) {
        const uintptr_t at=sp+k*8;
        uintptr_t value=0;
        if(pread(memfd,&value,8,off_t(at))!=8) break;
        if(value<0x1000) continue;
        char slot[40];
        if(!frame_of(value,slot,sizeof slot)) continue;
        log("GSTACK %s   [%u] slot=%u %s",tag,shown,k,slot);
        shown++;
    }
    if(!shown) log("GSTACK %s   no code address in 768 slots from sp=%#llx",tag,(unsigned long long)sp);
}
// Samples the guest PC every couple of seconds so a title that spins without
// logging still shows where it is. Every guest context is sampled, not just the
// caller: the thread that hangs is usually a worker, and only its own PC says
// why. SIGRTMIN+4 is chosen because ART owns SIGUSR1 for thread suspension.
// Watchdog-thread memory probe.
//
// Reading guest memory from outside needs the process's own view: a dumped image
// on disk is missing every dynamic result (relocated GOT slots, service handles
// stored into structs), which is exactly what a bring-up bug usually depends on.
// The watcher thread already runs in-process, so it can read any address the
// guest can. The watcher reads a marker file whose first line is a hex address and
// logs 0x40 bytes from there on each tick -- enough to see a struct's fields and
// a PLT slot's resolved target without a debugger.
static void probe_memory_once(const std::string& log_directory) {
    static bool loaded=false; static uintptr_t address=0; static unsigned round=0;
    if(!loaded) {
        loaded=true;
        const auto marker=std::filesystem::path(log_directory)/"probe-memory";
        std::error_code error;
        if(!std::filesystem::exists(marker,error)) return;
        // "<module>+<hex>" is resolved through the live module table, because the
        // base changes on every run: an absolute address written by hand is only
        // valid for the run that produced it, which is how an earlier probe ended
        // up reading an unrelated mapping.
        char text[128]={};
        if(FILE* in=fopen(marker.c_str(),"re")) { if(!fgets(text,sizeof text,in)) text[0]=0; fclose(in); }
        const char* plus=strchr(text,'+');
        if(plus) {
            const std::string name(text,size_t(plus-text));
            unsigned long long offset=0;
            if(sscanf(plus+1,"%llx",&offset)==1) {
                for(const auto& m:images) if(m.name==name) {
                    address=reinterpret_cast<uintptr_t>(m.base)+uintptr_t(offset);
                    break;
                }
            }
        } else sscanf(text,"%llx",(unsigned long long*)&address);
        if(address) log("PROBE_MEMORY watching %#llx",(unsigned long long)address);
    }
    if(!address||round>=8) return;
    // Same address space as the guest: read it directly. Unlike the syscall
    // helpers this is not guarded, so the address must already be proven mapped
    // (it comes from the log, which only prints addresses the guest itself used).
    unsigned char raw[0x40]{};
    memcpy(raw,reinterpret_cast<const void*>(address),sizeof raw);
    // Each 8-byte word that lands inside a loaded module is named here. Without
    // this the caller has to cross-reference a base that changes on every run,
    // and a stale base silently produces a confident wrong answer.
    for(unsigned i=0;i<0x40/8;i++) {
        uint64_t value=0;memcpy(&value,raw+i*8,8);
        const char* where=nullptr;uintptr_t offset=0;
        for(const auto& m:images) {
            const auto base=reinterpret_cast<uintptr_t>(m.base);
            if(value>=base && value<base+m.size) { where=m.name.c_str(); offset=value-base; break; }
        }
        if(where) log("PROBE_MEMORY #%u slot%u -> %s+0x%llx",round,i,where,(unsigned long long)offset);
        else if(value) log("PROBE_MEMORY #%u slot%u = %#llx",round,i,(unsigned long long)value);
        else log("PROBE_MEMORY #%u slot%u = 0",round,i);
    }
    round++;
}
std::string watchdog_log_directory;
// Which thread is actually doing work. A stalled title keeps presenting frames,
// so "everything is idle" and "everything is stuck" look the same from the
// outside; the tick-over-tick delta of utime+stime is the only in-process way to
// tell the driving thread from the parked workers, and the busy one is the
// thread whose stack is worth copying.
//
// USER_HZ is 100 on Android, so one tick is 10 ms and a 2 s window is 200 ticks
// per core. The value is never divided by anything -- it is only compared and
// summed -- so the constant does not have to be discovered at run time.
struct ThreadCpu { int32_t tid; uint64_t cpu; };
static void thread_cpu_snapshot(std::vector<ThreadCpu>& out) {
    out.clear();
    DIR* dir=opendir("/proc/self/task");
    if(!dir) return;
    while(dirent* entry=readdir(dir)) {
        const int32_t tid=atoi(entry->d_name);
        if(tid<=0) continue;
        char path[64]; snprintf(path,sizeof path,"/proc/self/task/%d/stat",tid);
        FILE* file=fopen(path,"re"); if(!file) continue;
        char buffer[512]; const size_t length=fread(buffer,1,sizeof buffer-1,file); fclose(file);
        buffer[length]=0;
        const char* close=strrchr(buffer,')'); if(!close) continue;
        unsigned long long utime=0,stime=0;
        // After the command name the fields are state, ppid, pgrp, session,
        // tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime.
        if(sscanf(close+2,"%*s %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",&utime,&stime)!=2) continue;
        out.push_back({tid,utime+stime});
    }
    closedir(dir);
}
int32_t busiest_thread() {
    static std::vector<ThreadCpu> previous;
    std::vector<ThreadCpu> current;
    thread_cpu_snapshot(current);
    int32_t best=0; uint64_t best_delta=0;
    for(const auto& now:current) {
        for(const auto& before:previous) if(before.tid==now.tid) {
            const uint64_t delta=now.cpu>before.cpu?now.cpu-before.cpu:0;
            if(delta>best_delta) { best_delta=delta; best=now.tid; }
            break;
        }
    }
    previous=std::move(current);
    return best;
}
void start_watchdog(pthread_t target) {
    (void)target;
    pthread_t thread{};
    if(pthread_create(&thread,nullptr,[](void*)->void*{
        load_host_ranges();
        log("WATCHDOG host modules=%u",host_range_count.load());
        off_t previous=-1; int stalled=0;
        // One snapshot per session, then silence. The stall test asks "has the log
        // stopped growing", which a title that simply has nothing left to say also
        // answers yes to -- so it kept re-arming itself and dumping every four
        // seconds for the whole session. Each dump is a few thousand stack lines
        // pushed through logcat and the log file by the threads the picture is
        // waiting on, which is enough to hold a 60-fps target at 27. The first
        // snapshot is the diagnostic value; the rest was cost.
        bool dumped=false;
        for(int i=0;i<1800;i++) {
            struct timespec delay{2,0};
            nanosleep(&delay,nullptr);
            // Diagnostic hold: `trace-services` keeps the sampler armed and prints
            // the service-call ring every tick. Without it the one-shot dump below
            // is all a run gets, which is useless for a stall that starts minutes
            // later -- the interesting moment is the transition, not boot.
            bool tracing=false;
            if(!watchdog_log_directory.empty()) {
                std::error_code trace_error;
                tracing=std::filesystem::exists(std::filesystem::path(watchdog_log_directory)/"trace-services",trace_error);
                trace_services.store(tracing,std::memory_order_relaxed);
            }
            // Idle SDK workers sit in futex waits all the time, so a sample alone
            // proves nothing. Only dump stacks once the log has stopped growing:
            // that is the moment the title is actually stuck.
            const off_t size=log_fd>=0?lseek(log_fd,0,SEEK_END):0;
            if(size==previous&&size>0){ if(++stalled==2){ dump_stacks.store(true,std::memory_order_release); log("WATCHDOG log stalled at %lld bytes; dumping stacks",(long long)size); } }
            else stalled=0;
            // A busy title keeps writing to the log (input polling alone does),
            // so the stall test alone can never fire. Take one unconditional
            // snapshot at 60s: by then the render path is either done or stuck.
            // While tracing, the per-tick PC sample below already answers the same
            // question for the thread that matters, and the full dump costs a
            // few thousand lines, so the two never run together.
            static bool sampled_once=false;
            if(!sampled_once && i>=28 && !tracing) { sampled_once=true; dump_stacks.store(true,std::memory_order_release); log("WATCHDOG timed snapshot at 60s"); }
            previous=size;
            if(!dumped && dump_stacks.load(std::memory_order_acquire)){
                dumped=true;
                dump_svc_hot(); dump_native_hot(); svc_thread_dump_waiters(); svc_thread_dump_addresses(); svc_thread_dump_timeline();
                dump_stacks.store(false,std::memory_order_release);
                stalled=0;
            }
            if(!watchdog_log_directory.empty()) probe_memory_once(watchdog_log_directory);
            // On-demand snapshot: the interesting moment for a stall is minutes
            // into a session, so a fixed timer always misses it. Touching
            // <game>/dump-now from adb re-arms the one-shot dump at the exact
            // frame the title got stuck on.
            if(!watchdog_log_directory.empty()) {
                std::error_code request_error;
                const auto request=std::filesystem::path(watchdog_log_directory)/"dump-now";
                if(std::filesystem::exists(request,request_error)) {
                    std::filesystem::remove(request,request_error);
                    dumped=false;
                    dump_stacks.store(true,std::memory_order_release);
                    log("WATCHDOG dump requested by marker");
                }
            }
            if(trace_services.load(std::memory_order_relaxed)) { dump_service_trace(); dump_service_delta(); dump_sleep_sites(); }
            // On-demand flat profile. Same marker pattern as dump-now: it is
            // useful exactly once per stall, at a moment a timer cannot predict.
            if(!watchdog_log_directory.empty()) {
                std::error_code profile_error;
                const auto request=std::filesystem::path(watchdog_log_directory)/"profile-cpu";
                if(std::filesystem::exists(request,profile_error)) {
                    std::filesystem::remove(request,profile_error);
                    log("WATCHDOG flat profile requested by marker");
                    cpu_profile_burst();
                }
            }
            // Sample every guest thread and print where each one was running. The
            // handler only records, so no guest thread ever waits on the log mutex
            // from inside a signal.
            if(tracing) {
                // Flag the busiest thread before the sweep: only its stack is
                // copied, and the handler needs the tid before it runs.
                const int32_t busy=busiest_thread();
                pc_sample_target.store(busy,std::memory_order_release);
                pc_sample_count.store(0,std::memory_order_release);
                pc_sample_store.store(true,std::memory_order_release);
                for(unsigned slot=0;slot<GuestThreadCapacity;slot++) {
                    const int32_t tid=__atomic_load_n(&ot2_guest_tids[slot],__ATOMIC_ACQUIRE);
                    if(tid>0) syscall(SYS_tgkill,getpid(),tid,OT2_SAMPLE_SIGNAL);
                }
                { struct timespec settle{0,120*1000*1000}; nanosleep(&settle,nullptr); }
                pc_sample_store.store(false,std::memory_order_release);
                const unsigned count=std::min(pc_sample_count.load(std::memory_order_acquire),PcSampleSlots);
                char text[40];
                log("PCSAMPLE busy=%d threads=%u",busy,count);
                for(unsigned n=0;n<count;n++) {
                    const auto& s=pc_samples[n];
                    log("PCSAMPLE tid=%d pc=%#llx %s+0x%llx lr=%#llx sp=%#llx",
                        s.tid,(unsigned long long)s.pc,s.module?s.module:"?",(unsigned long long)s.offset,
                        (unsigned long long)s.lr,(unsigned long long)s.sp);
                    // The copied stack is what turns "parked in libc" into the
                    // guest call chain that led there.
                    if(s.tid!=busy) continue;
                    for(unsigned k=0;k<s.frames;k++) {
                        const char* where="?"; uintptr_t offset=0;
                        for(const auto& m:images) {
                            const auto base=reinterpret_cast<uintptr_t>(m.base);
                            if(s.stack[k]>=base && s.stack[k]<base+m.size) {
                                where=m.name.c_str(); offset=s.stack[k]-base; break;
                            }
                        }
                        snprintf(text,sizeof text,"%s+0x%llx",where,(unsigned long long)offset);
                        log("PCSTACK tid=%d [%u] %s",s.tid,k,text);
                    }
                }
                // Frame-pointer chains for the other threads were tried here and
                // removed: walking all 27 of them wrote ~69k lines per dump and
                // every chain but the busy thread's landed in parking code, so it
                // buried the one line that mattered. The joiner's own chain is
                // already in PCSTACK above, and the thread it joined is named by
                // WAITFOREVER's stack scan.
                if(!count) log("PCSAMPLE no thread answered");
                pc_sample_target.store(0,std::memory_order_release);
            }
            // The sample signal exists only to feed a later dump, so once the one
            // snapshot is taken these interrupts have no reader left.
            if(!dumped) for(unsigned slot=0;slot<GuestThreadCapacity;slot++) {
                const int32_t tid=__atomic_load_n(&ot2_guest_tids[slot],__ATOMIC_ACQUIRE);
                if(tid>0) syscall(SYS_tgkill,getpid(),tid,OT2_SAMPLE_SIGNAL);
            }
        }
        return nullptr;
    },nullptr)) { log("WATCHDOG thread not started"); return; }
    pthread_detach(thread);
}
bool special(uint32_t instruction) {
    auto op=instruction&~31U;
    return (instruction&0xffe0001f)==0xd4000001 || op==0xd53bd040 || op==0xd53bd060 ||
        op==0xd51bd040 || op==0xd53be020 || op==0xd53be000;
}
#include "tls_rewrite.inc"
#include "system_rewrite.inc"
// Diagnostic: which host services the title called since the previous tick.
// The cumulative ranking answers "what is this title built on"; a stall needs
// the opposite question -- what did it still call one tick before it stopped --
// so this prints only the entries whose tally moved.
void dump_service_delta() {
    static std::vector<uint64_t> seen;
    if(seen.size()<system_services.size()) seen.resize(system_services.size(),0);
    unsigned lines=0;
    for(size_t i=0;i<system_services.size();++i) {
        const uint64_t calls=__atomic_load_n(&system_services[i].calls,__ATOMIC_RELAXED);
        if(calls==seen[i]) continue;
        if(lines<48) log("HOP %s +%llu total=%llu",system_services[i].name,(unsigned long long)(calls-seen[i]),(unsigned long long)calls);
        else if(lines==48) log("HOP ... more services called this tick");
        ++lines; seen[i]=calls;
    }
}
// Only the sites whose tally moved since the previous tick, so a stall shows the
// one loop that is still running instead of the whole startup's sleep traffic.
void dump_sleep_sites() {
    static uint32_t printed[SleepSiteSlots]{};
    unsigned lines=0;
    for(unsigned i=0;i<SleepSiteSlots;i++) {
        const uintptr_t pc=sleep_sites[i].pc.load(std::memory_order_relaxed);
        if(!pc) continue;
        const uint32_t hits=sleep_sites[i].hits.load(std::memory_order_relaxed);
        if(hits==printed[i]) continue;
        const char* name="?"; uintptr_t offset=pc;
        for(const auto& m:images) {
            const auto b=reinterpret_cast<uintptr_t>(m.base);
            if(pc>=b && pc<b+m.size) { name=m.name.c_str(); offset=pc-b; break; }
        }
        const uintptr_t caller=sleep_sites[i].caller.load(std::memory_order_relaxed);
        const char* cname="?"; uintptr_t coffset=caller;
        for(const auto& m:images) {
            const auto b=reinterpret_cast<uintptr_t>(m.base);
            if(caller>=b && caller<b+m.size) { cname=m.name.c_str(); coffset=caller-b; break; }
        }
        if(lines<12) log("SLEEP tid=%d %s+0x%llx +%u ns=%llu from %s+0x%llx",
            sleep_sites[i].tid.load(std::memory_order_relaxed), name,(unsigned long long)offset,
            unsigned(hits-printed[i]),(unsigned long long)sleep_sites[i].ns.load(std::memory_order_relaxed),
            cname,(unsigned long long)coffset);
        ++lines; printed[i]=hits;
    }
}
// ---- profile-cpu marker ---------------------------------------------------
// Touch <game>/profile-cpu and the watchdog runs one flat-profile burst, prints
// the ranking and eats the marker. On demand rather than on a timer because the
// only interesting moment is the stalled one: a profile taken at boot ranks the
// loader, a profile taken two minutes in ranks the loop that is still turning.
//
// The burst is deliberately short and its output deliberately small. The title is
// still presenting frames while it is stuck, so a long burst shows up as a
// framerate dip; two seconds at ~500 Hz gives every thread thousands of samples,
// which is plenty to separate a hot loop from incidental traffic.
static void cpu_profile_burst() {
    int32_t tids[GuestThreadCapacity]; unsigned thread_count=0;
    for(unsigned slot=0;slot<GuestThreadCapacity;slot++) {
        const int32_t tid=__atomic_load_n(&ot2_guest_tids[slot],__ATOMIC_ACQUIRE);
        if(tid>0) tids[thread_count++]=tid;
    }
    for(unsigned i=0;i<PcHitSlots;i++) {
        pc_hits[i].pc.store(0,std::memory_order_relaxed);
        pc_hits[i].hits.store(0,std::memory_order_relaxed);
    }
    pc_profile_samples.store(0,std::memory_order_relaxed);
    std::vector<ThreadCpu> cpu_before,cpu_after;
    thread_cpu_snapshot(cpu_before);
    const uint64_t started=monotonic_ns();
    pc_profile_store.store(true,std::memory_order_release);
    log("PROFILE start threads=%u rounds=1000",thread_count);
    for(int round=0;round<1000;round++) {
        for(unsigned i=0;i<thread_count;i++) syscall(SYS_tgkill,getpid(),tids[i],OT2_SAMPLE_SIGNAL);
        struct timespec delay{0,2*1000*1000};
        nanosleep(&delay,nullptr);
    }
    pc_profile_store.store(false,std::memory_order_release);
    const uint64_t window_ms=(monotonic_ns()-started)/1000000;
    thread_cpu_snapshot(cpu_after);
    const uint64_t total=pc_profile_samples.load(std::memory_order_acquire);
    log("PROFILE done samples=%llu window_ms=%llu",(unsigned long long)total,
        (unsigned long long)window_ms);
    if(!total) return;
    // CPU ticks per thread over the sampling window. This is ground truth: a
    // thread parked in futex is sampled at one PC a thousand times yet consumes
    // no CPU at all, so the histogram must never be read without this column.
    struct ThreadRank { int32_t tid; uint64_t ticks; };
    std::vector<ThreadRank> threads;
    for(const auto& now:cpu_after) {
        for(const auto& before:cpu_before) if(before.tid==now.tid) {
            if(now.cpu>before.cpu) threads.push_back({now.tid,now.cpu-before.cpu});
            break;
        }
    }
    std::sort(threads.begin(),threads.end(),[](const ThreadRank& a,const ThreadRank& b){ return a.ticks>b.ticks; });
    for(unsigned i=0;i<threads.size()&&i<8;i++)
        log("PROFILE cpu tid=%d ticks=%llu",threads[i].tid,(unsigned long long)threads[i].ticks);
    // Flat ranking, then the same entries regrouped per thread so the hot thread
    // can be read straight down: "this tid burns 90% and sits in these two
    // instructions" is the whole diagnosis for a spin.
    struct Ranked { const char* name; uintptr_t offset; int32_t tid; uint32_t hits; };
    std::vector<Ranked> ranked;
    for(unsigned i=0;i<PcHitSlots;i++) {
        const uintptr_t pc=pc_hits[i].pc.load(std::memory_order_relaxed);
        if(!pc) continue;
        const char* name="?"; uintptr_t offset=pc;
        for(const auto& m:images) {
            const auto base=reinterpret_cast<uintptr_t>(m.base);
            if(pc>=base && pc<base+m.size) { name=m.name.c_str(); offset=pc-base; break; }
        }
        for(unsigned i=0;i<host_range_count.load(std::memory_order_acquire);i++) {
            if(pc>=host_ranges[i].begin && pc<host_ranges[i].end) {
                name=host_ranges[i].name; offset=pc-host_ranges[i].begin; break;
            }
        }
        ranked.push_back({name,offset,pc_hits[i].tid.load(std::memory_order_relaxed),
                          pc_hits[i].hits.load(std::memory_order_relaxed)});
    }
    std::sort(ranked.begin(),ranked.end(),[](const Ranked& a,const Ranked& b){
        if(a.tid!=b.tid) return a.tid<b.tid;
        return a.hits>b.hits; });
    int32_t current=0; unsigned emitted=0;
    for(const auto& entry:ranked) {
        if(entry.tid!=current) { current=entry.tid; emitted=0; }
        if(emitted>=3) continue;
        log("PROFILE where tid=%d %s+0x%llx hits=%u",entry.tid,entry.name,
            (unsigned long long)entry.offset,entry.hits);
        ++emitted;
    }
}
void patch(uint8_t* base,size_t size) {
    const auto first_island=tls_islands.size();
    size_t tls_native=0,tls_fallback=0;
    for(size_t offset=0;offset+4<=size;offset+=4) {
        uint32_t op; memcpy(&op,base+offset,4);
        if(!special(op)) continue;
        if(patch_tls(reinterpret_cast<uintptr_t>(base+offset),op,first_island)){++tls_native;continue;}
        if(patch_system(reinterpret_cast<uintptr_t>(base+offset),op,first_island))continue;
        auto kind=op&~31u;if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)++tls_fallback;
        traps.push_back({reinterpret_cast<uintptr_t>(base+offset),op,nullptr});
        const uint32_t brk=0xd4200000 | (0x702<<5);
        memcpy(base+offset,&brk,4);
    }
    seal_tls_islands(first_island);
    if(size>4096)log("TLS_PATCH base=%p bytes=%zu native=%zu signal=%zu islands=%zu",base,size,tls_native,tls_fallback,tls_islands.size()-first_island);
}
void install_service_initializers(const ot2::LoadedGame& game) {
    const auto first_island=tls_islands.size();
    // This game's own SDK ships its direct-call audio renderer. Select it at
    // the manager seam instead of opening Horizon's audren IPC service.
    const auto audio_ipc=game.symbol("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv");
    const auto audio_dfc=game.symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if(!audio_ipc || !audio_dfc || (int64_t(audio_dfc)-int64_t(audio_ipc))%4 ||
       std::abs(int64_t(audio_dfc)-int64_t(audio_ipc))>=(1LL<<27))fail("SDK direct audio entry unavailable");
    const uint32_t audio_branch=0x14000000U | (uint32_t((int64_t(audio_dfc)-int64_t(audio_ipc))/4)&0x03ffffffU);
    memcpy(reinterpret_cast<void*>(audio_ipc),&audio_branch,4);
    // Service clients are replaced by host APIs, so their Horizon IPC sessions
    // must not be created. Keep nn::os and libc initialization in the original SDK.
    static constexpr const char* names[]={
        "nndiagStartup", "_ZN2nn2oe10InitializeEv", "_ZN2nn3aoc10InitializeEv",
        "_ZN2nn3err26InitializeContextWriterApiEv", "_ZN2nn2fs24InitializeForApplicationEv",
        "_ZN2nn4pctl10InitializeEv", "_ZN2nn4diag34InitializeApplicationAbortObserverEv"
    };
    // SDK versions differ in which of these exports exist: 5.4.0.0 has no
    // nndiagStartup at all, newer SDKs do. Each name is an IPC client this host
    // replaces, so an absent one is a version difference rather than a missing
    // capability -- gate the ones that are present and name the ones that are
    // not, instead of refusing to start on a title that simply predates a symbol.
    unsigned gated=0;
    for(const auto* name:names) {
        const auto address=game.symbol(name);
        if(!address){log("SERVICE_INIT absent %s",name);continue;}
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,name)){++gated;continue;}
        traps.push_back({address,original,name});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
        ++gated;
    }
    if(!gated)fail("Service initialization export absent");
    // The compatibility set is optional by design: a title whose SDK never
    // reaches the service manager does not import these symbols, and one that
    // does needs every one of them answered. Report which half of that holds so
    // a missing binding is visible in the log instead of only in the crash.
    // It is scoped to diagnostic runs: the shim hands out stand-in sessions,
    // which is the right trade for a bring-up and wrong for a shipped profile.
    if(diagnostic_svc_enabled) {
        const auto& compat=ot2::compatibility_services();
        std::string bound, absent;
        unsigned resolved=0;
        for(const auto& service:compat) {
            const auto address=game.symbol(service.name);
            if(!address) {
                // nn::settings getters are often inlined; an absent entry only
                // matters if the title complains about that service later.
                if(absent.size()<400) { if(!absent.empty()) absent+=' '; absent+=service.name; }
                continue;
            }
            ++resolved;
            std::erase_if(traps,[&](const auto& t){return t.pc==address;});
            uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
            if(!patch_system(address,original,first_island,service.name,service.call)) {
                traps.push_back({address,original,service.name,service.call});
                const uint32_t brk=0xd4200000 | (0x703<<5);
                memcpy(reinterpret_cast<void*>(address),&brk,4);
            }
            if(bound.size()<400) { if(!bound.empty()) bound+=' '; bound+=service.name; }
        }
        log("COMPAT bound=%u/%zu [%s]",resolved,compat.size(),bound.c_str());
        if(!absent.empty()) log("COMPAT absent [%s]",absent.c_str());
    }
    for(const auto* list:{&ot2::host_services(),&ot2::audio_services(),&ot2::audio_renderer_services(),&ot2::shared_font_services(),&ot2::input_services(),&ot2::movie_services()}) for(const auto& service:*list) {
        const auto address=game.symbol(service.name);
        if(!address) continue;
        std::erase_if(traps,[&](const auto& t){return t.pc==address;});
        uint32_t original;memcpy(&original,reinterpret_cast<void*>(address),4);
        if(patch_system(address,original,first_island,service.name,service.call))continue;
        traps.push_back({address,original,service.name,service.call});
        const uint32_t brk=0xd4200000 | (0x703<<5);
        memcpy(reinterpret_cast<void*>(address),&brk,4);
    }
    seal_tls_islands(first_island);
}
#include "guest_memory.inc"
// Writes every loaded module's decompressed image next to the log.
// The packaged .nso files are LZ4-compressed inside an NSO0 container, which
// makes them awkward to disassemble offline (the section table is easy to get
// wrong, and a wrong guess fails silently). The host already has the real
// decompressed bytes in memory the moment the title runs, so dumping them here
// removes the whole problem: what lands on disk is the exact text the guest
// executes, and `addr2line`/`capstone` can use the offsets from a crash log or a
// watchdog sample directly.
//
// Enabled by a marker file so normal boots do not pay for the writes:
//   <game directory>/dump-modules
static void dump_module_images(const std::vector<ot2::ModuleView>& images,const std::string& directory,const std::string& writable) {
    // The marker lives next to the log, not in the game directory: adding a file
    // to the extracted content makes the bundle look modified and the title
    // refuses to start at all, which is a confusing way to lose an afternoon.
    std::error_code error;
    const auto marker=std::filesystem::path(writable)/"dump-modules";
    if(!std::filesystem::exists(marker,error)) return;
    // The game directory lives on shared storage, where this process may not be
    // allowed to create directories. The log's own directory is app-private and
    // always writable, so the dump goes next to the log.
    std::filesystem::path target=std::filesystem::path(writable)/"dumped";
    std::filesystem::create_directories(target,error);
    if(error) { log("DUMP_MODULES cannot create %s: %s",target.c_str(),error.message().c_str()); return; }
    for(const auto& m:images) {
        // A flat image plus a small sidecar keeps offsets comparable to the log:
        // a sample at "<name>+0x1234" is byte 0x1234 of "<name>.bin" only if the
        // file starts at the module base, which is what this writes.
        const auto bin=target/(m.name+".bin");
        FILE* out=fopen(bin.c_str(),"wb");
        if(!out) { log("DUMP_MODULES cannot write %s",bin.c_str()); continue; }
        fwrite(m.base,1,m.size,out);
        fclose(out);
        const auto sidecar=target/(m.name+".txt");
        if(FILE* info=fopen(sidecar.c_str(),"w")) {
            fprintf(info,"name=%s\nbase=%p\nsize=%#zx\ntext_offset=%#zx\ntext_size=%#zx\nro_offset=%#zx\ndata_offset=%#zx\nbuild_id=",
                m.name.c_str(),static_cast<void*>(m.base),m.size,m.text_offset,m.text_size,m.ro_offset,m.data_offset);
            for(uint8_t byte:m.build_id) fprintf(info,"%02x",byte);
            fprintf(info,"\n");
            fclose(info);
        }
        log("DUMP_MODULES wrote %s (%zu bytes)",bin.c_str(),m.size);
    }
}

std::atomic<int> requested_frame_rate{30},applied_frame_rate{0};
ot2::FrameRatePatch frame_rate_patch;
void update_frame_rate(){
    struct Memory {
        void* resolve(uintptr_t address,size_t length,bool write){
            if(!length||address>UINTPTR_MAX-length)return nullptr;
            const int need=PROT_READ|(write?PROT_WRITE:0);
            // All fields fit eight bytes; check both 4-KiB guest pages if a
            // pointer straddles the boundary, including borrowed source pages.
            if((memory_permission_at(address)&need)!=need||(memory_permission_at(address+length-1)&need)!=need)return nullptr;
            return reinterpret_cast<void*>(address);
        }
    } memory;
    std::lock_guard lock(mapping_mutex);
    const int target=requested_frame_rate.load(std::memory_order_relaxed);
    auto result=frame_rate_patch.apply(memory,target);
    if(result==ot2::FrameRatePatch::changed||result==ot2::FrameRatePatch::unchanged){
        if(applied_frame_rate.exchange(target)!=target)log("FRAME_RATE_POLICY applied=%d build=0d9649011312f62e clock=unchanged",target);
    }else if(applied_frame_rate.exchange(0)!=0)log("FRAME_RATE_POLICY unavailable result=%d",int(result));
}
// Bring-up diagnostics -------------------------------------------------------
// A title can reach system services this host does not model yet. Stopping at
// the first one hides the rest of the requirement list, so this mode reports
// every unimplemented call, answers it with a plausible result and lets the
// guest continue. One run then enumerates the whole service requirement set.
// Enabled by a marker file inside the game directory; normal boots are byte for
// byte the same path as before.
static uint64_t next_diagnostic_session=0x50000000;
static unsigned diagnostic_svc_budget[256]{};

// Reading guest memory goes through the kernel so an invalid pointer returns an
// error instead of taking down the host with SIGSEGV.
static bool guest_peek(uint64_t address,void* out,size_t length) {
    if(!length||length>0x1000) return false;
    int fds[2];
    if(pipe(fds)!=0) return false;
    bool ok=false;
    if(write(fds[1],reinterpret_cast<const void*>(address),length)==ssize_t(length))
        ok=read(fds[0],out,length)==ssize_t(length);
    close(fds[0]); close(fds[1]);
    return ok;
}
// The write direction uses the same trick: read(2) copies into guest memory
// inside the kernel, so a bad pointer fails with EFAULT instead of faulting.
static bool guest_poke(uint64_t address,const void* source,size_t length) {
    if(!length||length>0x1000) return false;
    int fds[2];
    if(pipe(fds)!=0) return false;
    bool ok=false;
    if(write(fds[1],source,length)==ssize_t(length))
        ok=read(fds[0],reinterpret_cast<void*>(address),length)==ssize_t(length);
    close(fds[0]); close(fds[1]);
    return ok;
}
static uint32_t rd32(const unsigned char* p) {
    return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
}
static void wr32(unsigned char* p,uint32_t v) {
    p[0]=static_cast<unsigned char>(v);p[1]=static_cast<unsigned char>(v>>8);
    p[2]=static_cast<unsigned char>(v>>16);p[3]=static_cast<unsigned char>(v>>24);
}
// Report a CMIF request and answer it with an empty success reply. The reply
// mirrors the caller's HIPC header (same message type, same special-header
// layout) and carries a CmifOutHeader with result 0, which is what the SDK
// checks before it parses any output.
static void diagnostic_reply(uint64_t buffer,const char* source,uint64_t caller) {
    unsigned char request[0x100]{};
    if(!guest_peek(buffer,request,sizeof(request))) {
        log("DIAG %s request buffer %llx is unreadable",source,(unsigned long long)buffer);
        return;
    }
    const uint32_t type=rd32(request)&0xffffu;
    const uint32_t word1=rd32(request+4);
    const bool special=(word1>>31)&1u;
    // HipcHeader packs the descriptor counts into the first word: send statics
    // / send buffers / receive buffers / exchange buffers, four bits each.
    const uint32_t word0=rd32(request);
    const unsigned num_send_buffers=(word0>>20)&0xfu;
    const unsigned num_recv_buffers=(word0>>24)&0xfu;
    const unsigned num_exch_buffers=(word0>>28)&0xfu;
    bool send_pid=false;
    size_t handles_end=8;
    if(special) {
        const uint32_t sp=rd32(request+8);
        send_pid=(sp&1u)!=0;
        handles_end=12+size_t((sp>>1)&0xfu)*4+size_t((sp>>5)&0xfu)*4+(send_pid?8:0);
    }
    // Buffer descriptors sit between the handles and the raw data, 12 bytes
    // each, and the whole block is padded to 16. A title's output buffer is a
    // receive descriptor.
    size_t descriptors=handles_end;
    const size_t descriptor_count=num_send_buffers+num_recv_buffers+num_exch_buffers;
    descriptors+=descriptor_count*12;
    size_t data=(descriptors+15)&~size_t(15);
    if(descriptor_count) {
        log("DIAG %s header type=%u bufs send=%u recv=%u exch=%u descriptors@%#zx",
            source,type,num_send_buffers,num_recv_buffers,num_exch_buffers,descriptors);
        for(size_t i=0;i<descriptor_count&&handles_end+(i+1)*12<=sizeof(request);i++) {
            const size_t at=handles_end+i*12;
            const uint32_t size_low=rd32(request+at);
            const uint32_t addr_low=rd32(request+at+4);
            const uint32_t packed=rd32(request+at+8);
            const uint64_t address=uint64_t(addr_low)|(uint64_t((packed>>6)&0x3fffffu)<<32)|(uint64_t((packed>>26)&0xfu)<<36);
            const uint64_t size=uint64_t(size_low)|(uint64_t((packed>>2)&0xfu)<<32);
            const unsigned mode=packed&3u;
            log("DIAG %s buf[%zu] %s addr=%#llx size=%#llx mode=%u",source,i,
                i<num_send_buffers?"send":"recv",(unsigned long long)address,(unsigned long long)size,mode);
        }
    }
    // A domain message carries a CmifDomainInHeader in front of the CMIF header.
    const bool domain=rd32(request+data)!=0x49434653u;
    const size_t cmif=data+(domain?0x10:0);
    if(cmif+0x20>sizeof(request)) {
        log("DIAG %s message offset %#zx out of range",source,cmif);
        return;
    }
    const uint32_t magic=rd32(request+cmif);
    const uint32_t command=rd32(request+cmif+8);
    const uint32_t token=rd32(request+cmif+12);
    char text[17]{};
    for(size_t i=0;i<16;i++) {
        const unsigned char ch=request[cmif+0x10+i];
        text[i]=(ch>=32&&ch<127)?static_cast<char>(ch):'.';
    }
    const char* service=ot2::compatibility_session_service(caller);
    log("DIAG %s kind=%s domain=%d magic=%08x cmd=%u token=%08x service='%s' arg='%s'",
        source,type==2u?"release":"invoke",domain?1:0,magic,command,token,service?service:"-",text);
    // The first words of the payload say what the command expects: a size, an
    // item key, a pointer into a mapped buffer.
    {
        char words[96]{};
        size_t written=0;
        for(size_t i=0;i<5&&cmif+0x10+(i+1)*4<=sizeof(request);i++)
            written+=size_t(snprintf(words+written,sizeof(words)-written,"%08x ",rd32(request+cmif+0x10+i*4)));
        log("DIAG %s args %s",source,words);
    }

    // GetService has to hand a session back or the SDK retries it forever.
    const bool hands_out_session=type!=2u&&command==1u;
    const uint32_t session=hands_out_session?next_diagnostic_session++:0;
    // hid command 106 is AcquireNpadStyleSetUpdateEventHandle: the service creates
    // an event and returns its handle. Replying "success" with no handle leaves
    // the caller reading whatever the reply happened to leave behind -- 0 on a
    // zeroed buffer, 0x6 on this title's nine binds -- and the SDK then builds a
    // SystemEventType around it.
    //
    // The condition used to require the session's service name to read "hid",
    // which never holds here: this title reaches the SDK's built-in hid path and
    // never calls sm::GetServiceHandle, so the stand-in session table stays empty
    // and the branch was dead. The command number is the reliable discriminator.
    //
    // No pulsing thread any more. The handle this returns is only useful to a
    // caller that waits on it directly; the SDK's own BindNpadStyleSetUpdateEvent
    // hands it to nn::os::AttachReadableHandleToSystemEvent, which builds its own
    // event on this host (see host_services.cpp), so signalling this one would
    // just be a thread per call that nobody waits on.
    const bool hands_out_event=type!=2u&&command==106u;
    uint32_t event_handle=0;
    if(hands_out_event) {
        uint64_t create[8]={0,0,0,0,0,0,0,0};
        if(svc_thread(0x45,create)==1) {
            const uint32_t write_end=uint32_t(create[1]),read_end=uint32_t(create[2]);
            event_handle=read_end;
            log("HID_UPDATE_EVENT read=%#x write=%#x service='%s'",read_end,write_end,service?service:"-");
        } else {
            log("HID_UPDATE_EVENT creation failed");
        }
    }
    const bool gives_handle=hands_out_session||hands_out_event;
    // set: command 5 is GetAvailableLanguageCodes: it returns the number of
    // entries and fills an output buffer. A zero count is what made the caller
    // abort, so report the one language the settings shim answers with.
    const bool reports_count=type!=2u&&command==5u&&service&&std::strcmp(service,"set")==0;

    unsigned char reply[0x100]{};
    wr32(reply,type);
    // The copy handle lives in the special header, not in the raw data, so it
    // must not be counted here: Header1's low 16 bits are the raw word count and
    // the SDK walks that many words to find the CMIF output header.
    const uint32_t payload=uint32_t((domain?4u:0u)+4u+(reports_count?1u:0u));
    const uint32_t special_out=(send_pid?1u:0u)|(gives_handle?2u:0u);
    wr32(reply+4,payload|((special||special_out)?0x80000000u:0));
    size_t at=8;
    if(special||special_out) {
        wr32(reply+at,special_out);
        at+=4;
        if(gives_handle) { wr32(reply+at,hands_out_session?session:event_handle); at+=4; }
        if(send_pid) { wr32(reply+at,0); wr32(reply+at+4,0); at+=8; }
    }
    size_t out=at;
    out=(out+15)&~size_t(15);
    if(domain) {
        wr32(reply+out,hands_out_session?1u:0u);    // objects produced
        wr32(reply+out+4,0); wr32(reply+out+8,0); wr32(reply+out+12,0);
        out+=0x10;
    }
    wr32(reply+out,0x4f434653u);                    // 'SFCO'
    wr32(reply+out+4,1);                            // CMIF version
    wr32(reply+out+8,0);                            // result = success
    wr32(reply+out+12,token);
    if(hands_out_session&&domain) wr32(reply+out+0x10,session);
    if(reports_count) wr32(reply+out+0x10,1);      // one available language
    if(!guest_poke(buffer,reply,sizeof(reply)))
        log("DIAG %s reply buffer %llx is unwritable",source,(unsigned long long)buffer);
    if(hands_out_event) {
        // The SDK reads the handle out of the reply header itself. Dump the words
        // so a wrong offset is visible instead of silent.
        log("HID_UPDATE_REPLY special_out=%#x payload=%u out=%#zx words=%08x %08x %08x %08x %08x %08x",
            special_out,payload,out,
            rd32(reply),rd32(reply+4),rd32(reply+8),rd32(reply+12),rd32(reply+16),rd32(reply+20));
    }
}
static std::string guest_string(uint64_t address,size_t limit) {
    std::string value;
    if(!address) return value;
    char buffer[64]{};
    const size_t length=std::min<size_t>(limit,sizeof(buffer));
    if(!guest_peek(address,buffer,length)) return value;
    for(size_t i=0;i<length;i++) { if(!buffer[i]) break; value.push_back(buffer[i]); }
    return value;
}
static void log_guest_hex(const char* tag,uint64_t address,size_t length) {
    unsigned char buffer[0x200]{};
    if(!length||length>sizeof(buffer)||!guest_peek(address,buffer,length)) {
        log("DIAG %s unreadable at %llx",tag,(unsigned long long)address);
        return;
    }
    std::string line;
    for(size_t i=0;i<length;i++) {
        char byte[4]; snprintf(byte,sizeof(byte),"%02x",buffer[i]);
        line+=byte;
        if((i+1)%32==0) { log("DIAG %s @%llx+%#zx %s",tag,(unsigned long long)address,i+1-32,line.c_str()); line.clear(); }
    }
    if(!line.empty()) log("DIAG %s @%llx+%#zx %s",tag,(unsigned long long)address,length-length%32,line.c_str());
}
// Per-id call counts. A thread that spins inside the SVC bridge is invisible in
// the log because each id only reports its first call, so the watchdog prints
// the hottest ids instead of guessing.
void dump_svc_hot() {
    unsigned top[4]={};uint64_t best[4]{};
    for(unsigned id=0;id<256;id++) {
        const uint64_t n=svc_calls[id].load(std::memory_order_relaxed);
        for(int k=0;k<4;k++) if(n>best[k]) {
            for(int j=3;j>k;j--){best[j]=best[j-1];top[j]=top[j-1];}
            best[k]=n;top[k]=id;break;
        }
    }
    for(int k=0;k<4;k++) if(best[k])
        log("SVC_HOT #%u id=0x%x calls=%llu",k,top[k],(unsigned long long)best[k]);
    // Full histogram, once: whether an id was ever called at all is the fact
    // that decides if an object is missing because the title never asked for it.
    static std::atomic<bool> dumped{false};
    if(!dumped.exchange(true)) {
        char line[900];size_t used=0;
        for(unsigned id=0;id<256;id++) {
            const uint64_t n=svc_calls[id].load(std::memory_order_relaxed);
            if(!n) continue;
            const int wrote=snprintf(line+used,sizeof(line)-used,"%s%x:%llu",used?",":"",id,(unsigned long long)n);
            if(wrote<=0 || used+size_t(wrote)>=sizeof line) break;
            used+=size_t(wrote);
        }
        log("SVC_ALL %s",line);
    }
}
// The NVN gate table already counts every call; the largest counters name the
// function a spinning thread keeps re-entering.
void dump_native_hot() {
    const unsigned count=native_gate_count.load(std::memory_order_acquire);
    unsigned top[6]={};uint64_t best[6]{};
    for(unsigned i=0;i<count;i++) {
        const uint64_t n=__atomic_load_n(&native_gates[i].hits,__ATOMIC_RELAXED);
        for(int k=0;k<6;k++) if(n>best[k]) {
            for(int j=5;j>k;j--){best[j]=best[j-1];top[j]=top[j-1];}
            best[k]=n;top[k]=i;break;
        }
    }
    for(int k=0;k<6;k++) if(best[k])
        log("NATIVE_HOT #%u %s calls=%llu",k,native_gates[top[k]].name,(unsigned long long)best[k]);
}
void svc(unsigned id,ucontext_t* uc,GuestThread* c) {
    auto* r=uc->uc_mcontext.regs;
    static bool seen[256]{};
    if(id<256) svc_calls[id].fetch_add(1,std::memory_order_relaxed);
    if(id==0x0b) record_sleep(uc->uc_mcontext.regs[30],r[0],caller_of(uc->uc_mcontext.regs[29]),int32_t(syscall(SYS_gettid)));
    // A short sleep is the tell-tale of a "poll until ready" loop. Print the few
    // pointers such a loop keeps its state in, so the wait can be named from the
    // data instead of guessed from the call site.
    if(id==0x0b && r[0] && r[0]<=8000000) {
        static std::atomic<unsigned> short_sleeps{0};
        if(short_sleeps.fetch_add(1,std::memory_order_relaxed)<16) {
            // Registers only: this runs inside the trap handler, where touching a
            // possibly-unmapped guest pointer would cost the diagnosis to a fault.
            uintptr_t off=0;const char* mod=ot2_host_module_of(uc->uc_mcontext.regs[30],&off);
            log("SLEEPPEEK %s+0x%llx ns=%llu x19=%#llx x20=%#llx x21=%#llx x22=%#llx",
                mod?mod:"?",(unsigned long long)off,(unsigned long long)r[0],
                (unsigned long long)r[19],(unsigned long long)r[20],
                (unsigned long long)r[21],(unsigned long long)r[22]);
        }
    }
    // A thread yielding in a tight loop is this title's shape for "the screen
    // never advances": the condition the loop spins on never becomes true, and
    // the yield is the only host call it makes, so every other probe stays
    // silent. The call site alone cannot name the subsystem (main exports 30
    // symbols out of thousands of internal functions), but the live guest stack
    // can, because the chain is still on it.
    if(id==0x0b && int64_t(r[0])<0) {
        static thread_local uint64_t yields;
        static thread_local uint64_t next_dump;
        static std::atomic<unsigned> budget{240};
        if(++yields>=200) {
            const uint64_t now=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if(now>=next_dump) {
                next_dump=now+3000; yields=0;
                if(budget.fetch_sub(1,std::memory_order_relaxed)>0) {
                    uintptr_t off=0;const char* mod=ot2_host_module_of(uc->uc_mcontext.regs[30],&off);
                    log("YIELD_SPIN tid=%d at=%s+0x%llx ns=%lld x19=%#llx x20=%#llx x21=%#llx x22=%#llx fp=%#llx",
                        int(syscall(SYS_gettid)),mod?mod:"?",(unsigned long long)off,
                        (long long)r[0],
                        (unsigned long long)r[19],(unsigned long long)r[20],
                        (unsigned long long)r[21],(unsigned long long)r[22],
                        (unsigned long long)uc->uc_mcontext.regs[29]);
                    log_guest_scan("YIELD",uc->uc_mcontext.sp,uc->uc_mcontext.regs[30],
                        uintptr_t(r[19]),uintptr_t(r[20]));
                    // The scan above only lists addresses that happen to look like
                    // code, so a loop reads as a soup of stale return addresses and
                    // the same function repeats at two depths. This title's own code
                    // keeps frame pointers (every prologue is `stp x29,x30`), so the
                    // walk from x29 names the real chain -- including the host frames
                    // when the yield sits under a host call. The first few walks all
                    // land in boot-time worker parking, so the interesting ones are
                    // the later dumps: the stall is minutes into a session.
                    static std::atomic<unsigned> walks{0};
                    const unsigned n=walks.fetch_add(1,std::memory_order_relaxed);
                    if(n<4 || n%6==5)
                        log_host_stack("YIELD",n,uc->uc_mcontext.regs[29],uc->uc_mcontext.regs[30]);
                }
            }
        }
    }
    if(id<256 && !seen[id]) { seen[id]=true; log("SVC 0x%x x0=%llx x1=%llx x2=%llx x3=%llx",id,r[0],r[1],r[2],r[3]); }
    if(id==0x1e) { ot2_sys_tick_calls.fetch_add(1,std::memory_order_relaxed); r[0]=ticks(); return; }
    if(id==0x15 || id==0x51 || id==0x52) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[1];const size_t length=r[2];const uint32_t handle=r[0],permission=r[3];
        uint64_t transfer_serial=0;
        if(auto result=memory_validate(address,length,memory_invalid_current)){r[0]=result;return;}
        if(id!=0x15&&!memory_contains(reinterpret_cast<uintptr_t>(address_space),reserve_size,address,length)){r[0]=memory_invalid_current;return;}
        if(id!=0x52&&permission!=0&&permission!=1&&permission!=3){r[0]=0xd801;return;}
        if(id==0x15){
            if(!memory_source_available(address,length)){r[0]=memory_invalid_current;return;}
            transfer_sources.reserve(transfer_sources.size()+1);
        }else{
            auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
            if(owner==transfer_sources.end()){r[0]=0xe401;return;}
            transfer_serial=owner->serial;
            const auto base=reinterpret_cast<uintptr_t>(address_space);
            if(memory_overlaps(address,length,base+heap_offset,heap_capacity)||memory_overlaps(address,length,base+alias_offset,alias_size)){r[0]=memory_invalid_region;return;}
            if(owner->size!=length){r[0]=memory_invalid_size;return;}
            if(id==0x51){
                if(owner->permission!=permission||std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==transfer_serial;})){r[0]=0xfa01;return;}
                if(!memory_destination_free(address,length)){r[0]=memory_invalid_current;return;}
                mappings.reserve(mappings.size()+1);
            }else{
                auto mapped=std::find_if(mappings.begin(),mappings.end(),[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
                if(mapped==mappings.end()){r[0]=memory_invalid_current;return;}
            }
        }
        if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer memory adapter absent",uc);
        if(!r[0])memory_registry_changed();
        if(!r[0]&&id==0x15){
            transfer_sources.push_back({address,length,uint32_t(r[1]),permission,false,++next_transfer_serial});memory_protect(address,length,permission);
        }
        if(!r[0]&&id==0x51)mappings.push_back({address,length,permission?0xeu:0xdu,transfer_serial});
        if(!r[0]&&id==0x52)std::erase_if(mappings,[&](const auto& m){return m.base==address&&m.size==length&&(m.state==0xd||m.state==0xe)&&m.source==transfer_serial;});
        return;
    }
    if(id==0x16){
        std::lock_guard lock(mapping_mutex);const auto handle=uint32_t(r[0]);
        auto owner=std::find_if(transfer_sources.begin(),transfer_sources.end(),[&](const auto& t){return t.handle==handle&&!t.closed;});
        if(owner!=transfer_sources.end()){
            const bool mapped=std::any_of(mappings.begin(),mappings.end(),[&](const auto& m){return (m.state==0xd||m.state==0xe)&&m.source==owner->serial;});
            if(!svc_thread(id,reinterpret_cast<uint64_t*>(r)))fail("Transfer close adapter absent",uc);
            if(!r[0]){
                if(mapped)owner->closed=true;
                else {const auto address=owner->base,length=owner->size;transfer_sources.erase(owner);memory_protect(address,length,PROT_READ|PROT_WRITE);}
            }
            return;
        }
    }
    // The title's own "wait for a job" loop sleeps 5 ms between polls and never
    // leaves it, which the waiter dump shows as svc 0xb with a fixed return
    // address. Print the object it polls once so the subsystem can be named
    // instead of guessed: the loop keeps the polled pointer in x19.
    if(id==0x0b) {
        static std::atomic<unsigned> polls{0};
        uintptr_t offset=0;
        const char* where=ot2_host_module_of(uc->uc_mcontext.regs[30],&offset);
        if(where && std::strcmp(where,"main")==0 && offset>=0x2ed2a0 && offset<0x2ed2c0) {
            if(polls.fetch_add(1,std::memory_order_relaxed)<6) {
                // The poller passes a container; the state lives one level down:
                // nn::audio::MemoryPoolInfo::GetState() is "return *(u32*)(info+0x10)"
                // and the loop waits for the mapped value 1, which the state table
                // yields only for state 5 (attached).
                const uintptr_t holder=uc->uc_mcontext.regs[19];
                uint64_t info=0;memcpy(&info,reinterpret_cast<const void*>(holder),8);
                uint32_t state=0;memcpy(&state,reinterpret_cast<const void*>(info+0x10),4);
                log("POLL_TARGET holder=%#llx info=%#llx state=%u sleep=%llu",
                    (unsigned long long)holder,(unsigned long long)info,state,
                    (unsigned long long)r[0]);
            }
        }
    }
    // A title that stops making progress is usually blocked in a condition
    // variable, and the wait site alone names only the SDK's own primitive. The
    // guest return addresses on the stack are what say which subsystem is stuck,
    // so dump them on unbounded waits only: a timed wait is a poll loop and would
    // drown the budget long before the stall reproduces, while the wait that
    // actually freezes the title carries no timeout by definition.
    if(id==0x34 || (int64_t(r[3]) < 0)) {
        static std::atomic<unsigned> dumps_static{0}, dumps_other{0};
        // A wait on a module's own static object is a singleton's handshake and
        // always worth a stack -- those are the ones that freeze the title, and
        // the main thread spends its per-thread budget on startup mailboxes long
        // before reaching the queue that actually fills. Everything else gets up
        // to four distinct wait sites per thread, which is enough to name the
        // worker pool without dumping the same parking spot a thousand times.
        const uintptr_t target=uintptr_t(r[1]) ? uintptr_t(r[1]) : uintptr_t(r[0]);
        bool statik=false;
        for(const auto& m:images) {
            const auto lo=reinterpret_cast<uintptr_t>(m.base);
            if(target>=lo && target<lo+m.size) { statik=true; break; }
        }
        static thread_local uintptr_t seen[4]; static thread_local unsigned nseen;
        bool fresh=statik;
        if(!statik) {
            fresh=true;
            for(unsigned i=0;i<nseen;i++) if(seen[i]==target) fresh=false;
            if(nseen>=4) fresh=false;
        }
        const unsigned limit=statik?60u:120u;
        auto& counter=statik?dumps_static:dumps_other;
        if(fresh && counter.fetch_add(1,std::memory_order_relaxed)<limit) {
            if(!statik) seen[nseen++]=target;
            log_guest_scan(id==0x1c?"COND":"ADDR",
                uc->uc_mcontext.sp,uc->uc_mcontext.regs[30],
                uintptr_t(r[0]),uintptr_t(r[1]));
        }
    }
    // A wait with no timeout is the other shape a stall takes: the thread stops
    // calling anything at all, so every counter in this file freezes and only its
    // stack still says which subsystem it is parked in. Throttled per thread and
    // budgeted, because boot takes hundreds of these waits on worker events.
    if(id==0x18 && int64_t(r[3])<0 && r[2]>0) {
        static thread_local uint64_t next;
        static std::atomic<unsigned> budget{60};
        const uint64_t now=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if(now>=next && budget.fetch_sub(1,std::memory_order_relaxed)>0) {
            next=now+10000;
            uint32_t first=0;
            if(r[1]) memcpy(&first,reinterpret_cast<const void*>(r[1]),4);
            log("WAITFOREVER tid=%d n=%llu first=%#x",
                int(syscall(SYS_gettid)),(unsigned long long)r[2],first);
            log_host_stack("WAITFOREVER",0,uc->uc_mcontext.regs[29],uc->uc_mcontext.regs[30]);
            log_guest_scan("WAITFOREVER",uc->uc_mcontext.sp,uc->uc_mcontext.regs[30],first,0);
        }
    }
    if(thread_system_ready) {
        // A spinning WaitSynchronization is host-visible only as a counter here,
        // so record the guest addresses that keep issuing it: they name the loop
        // that never gets what it is waiting for.
        if(id==0x18) {
            static std::atomic<uint64_t> waits{0};
            static bool stacks_dumped=false;
            if(waits.fetch_add(1,std::memory_order_relaxed)<24) {
                const auto guest_pc=uc->uc_mcontext.pc, guest_lr=uc->uc_mcontext.regs[30];
                const char* where="?"; uintptr_t off=0;
                for(const auto& m:images) {
                    const auto b=reinterpret_cast<uintptr_t>(m.base);
                    if(guest_lr>=b && guest_lr<b+m.size) { where=m.name.c_str(); off=guest_lr-b; break; }
                }
                // The handle itself, not just its address: a zero here means the
                // object the title waits on was never created.
                uint32_t first=0;char list[64]={};unsigned count=r[2]<8?unsigned(r[2]):8;
                if(r[1] && count) {
                    memcpy(list,reinterpret_cast<const void*>(r[1]),count*4);
                    memcpy(&first,list,4);
                    for(unsigned i=0;i<count;i++){char t[8];snprintf(t,sizeof t,"%s%02x",i?",":"",(unsigned char)list[i*4]);strcat(list,t);}
                }
                if(!stacks_dumped) {
                    stacks_dumped=true;
                    log_host_stack("WAIT_STACK",waits.load(std::memory_order_relaxed),
                        uc->uc_mcontext.regs[29],uc->uc_mcontext.regs[30]);
                }
                log("WAIT_CALLER tid=%d pc=%llx lr=%s+0x%llx handles@%llx first=%#x n=%llu timeout=%llu list=%s",
                    (int)syscall(SYS_gettid),(unsigned long long)guest_pc,where,(unsigned long long)off,
                    (unsigned long long)r[1],first,
                    (unsigned long long)r[2],(unsigned long long)r[3],list);
                // Approximate call chain by scanning the guest stack for return
                // addresses, which does not depend on frame pointers surviving.
                // The SDK is built without frame pointers, so earlier attempts at
                // walking r29 produced nothing: the chain only exists as leftover
                // return addresses on the stack.
                {
                    std::lock_guard lock(mapping_mutex);
                    const uintptr_t sp=uc->uc_mcontext.sp;
                    unsigned shown=0;
                    for(unsigned k=0;k<512 && shown<12;k++) {
                        const uintptr_t at=sp+k*8;
                        if(!(memory_permission_at(at)&PROT_READ)) break;
                        uintptr_t value=0;memcpy(&value,reinterpret_cast<const void*>(at),8);
                        for(const auto& m:images) {
                            const auto b=reinterpret_cast<uintptr_t>(m.base);
                            if(value>=b && value<b+m.size && value>0x1000) {
                                log("WAIT_FRAME [%u] slot=%u %s+0x%llx",shown,k,m.name.c_str(),(unsigned long long)(value-b));
                                shown++;break;
                            }
                        }
                    }
                    if(!shown) log("WAIT_FRAME none in 512 slots from sp=%#llx",(unsigned long long)sp);
                }
            }
        }
        if(svc_thread(id,reinterpret_cast<uint64_t*>(r))) return;
    }
    // svcMapSharedMemory(Handle src, void* addr, size_t size, u32 perm).
    // There is no second process in this host, so the pages a handle refers to
    // are already reachable and nothing needs mapping. That makes "success"
    // defensible, but it is also how a title can end up reading a block that was
    // never filled: report the range, and whether the handle is one this host
    // handed out. A handle outside the table is a value the title got from a
    // service reply, which names the reply that still has to carry it.
    if(id==0x13) {
        const uint32_t handle=uint32_t(r[0]);
        const uintptr_t at=r[1]; const size_t size=r[2]; const uint32_t perm=r[3];
        const bool known=handle>=0x1000u&&handle<0x1000u+0x1000u;
        char head[3*8+1]={};size_t used=0;
        bool blank=true;
        for(unsigned i=0;i<8;i++) {
            if(!at||i>=size) break;
            uint8_t byte=0;
            if(!guest_peek(at+i,&byte,1)) { used+=size_t(snprintf(head+used,sizeof(head)-used,"??")); continue; }
            if(byte) blank=false;
            used+=size_t(snprintf(head+used,sizeof(head)-used,"%02x",byte));
        }
        log("MAP_SHARED handle=%#x known=%d addr=%#llx size=%#zx perm=%u blank=%d head=%s",
            handle,known?1:0,(unsigned long long)at,size,perm,blank?1:0,head);
        r[0]=0; return;
    }
    if(id==0x10) { r[0]=0; return; }
    if(id==0x25) { r[0]=0;r[1]=c->handle;return; }
    if(id==0x0c) { r[0]=0;r[1]=44;return; }
    if(id==0x0e) { r[0]=0;r[1]=0;r[2]=7;return; }
    if(id==0x27) { log("GUEST %.*s",int(std::min<uint64_t>(r[1],1600)),reinterpret_cast<char*>(r[0]));r[0]=0;return; }
    if(id==0x29) {
        std::lock_guard lock(mapping_mutex);
        auto base=reinterpret_cast<uintptr_t>(address_space); uint64_t value=0;
        switch(r[1]) {
        case 0:value=7;break; case 1:value=0xffffffffffffffffULL;break;
        case 2:value=base+alias_offset;break; case 3:value=alias_size;break;
        case 4:value=base+heap_offset;break; case 5:value=heap_capacity;break;
        case 6:case 21:value=0xcd500000;break;
        case 7:case 22:value=heap_size+image_size;break;
        case 8:value=0;break;
        case 11:if(getrandom(&value,8,0)!=8) fail("Random source failed",uc);break;
        case 12:value=base;break; case 13:value=reserve_size;break;
        case 14:value=base+stack_offset;break; case 15:value=stack_capacity;break;
        case 16:value=0x2000000;break; case 17:value=0x400000;break;
        case 18:value=ot2::configured_program_id();break; case 20:value=0;break;
        case 23:value=1;break;
        case 28:
            if(r[3]){r[0]=0xf001;return;} // Kernel InvalidEnumValue
            if(uint32_t(r[2])!=0xffff8001U){r[0]=0xe401;return;} // Current process handle only
            // The host reserves a fixed alias region (id 3); it does not add
            // the optional extra region selected by newer Horizon processes.
            // Report that actual layout, not an invented per-game size.
            value=0;break;
        default:log("Unhandled GetInfo id=%llu",r[1]);fail("GetInfo unavailable",uc);
        }
        r[0]=0;r[1]=value;return;
    }
    if(id==0x06) {
        std::lock_guard lock(mapping_mutex);
        const uintptr_t address=r[2]; uint64_t b=0,size=0; uint32_t state=0,permission=0,attribute=0;
        if(address>=c->stack && address<c->stack+c->stack_size) { b=c->stack;size=c->stack_size;state=0xb;permission=3; }
        else if(const auto image=memory_image_region(address);image.size){b=image.base;size=image.size;state=image.state;permission=image.permission;}
        if(!size) for(const auto& m:mappings) if(address>=m.base && address<m.base+m.size) {b=m.base;size=m.size;state=m.state;permission=3;break;}
        const auto heap=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
        if(!size && address>=heap && address<heap+heap_size) {b=heap;size=heap_size;state=5;permission=3;}
        memory_query_source(address,b,size,state,attribute,permission);
        if(!size && address>=(1ULL<<39)){b=1ULL<<39;size=0-b;state=0x10;permission=0;}
        if(!size) {
            // Horizon exposes holes as free ranges. SDK startup walks from address zero.
            uint64_t lo=0,hi=1ULL<<39;
            auto boundary=[&](uint64_t begin,uint64_t count) {
                if(begin+count<=address) lo=std::max(lo,begin+count);
                if(begin>address) hi=std::min(hi,begin);
            };
            boundary(c->stack,c->stack_size);
            for(const auto& m:images) boundary(reinterpret_cast<uintptr_t>(m.base),m.size);
            for(const auto& m:mappings) boundary(m.base,m.size);
            if(heap_size) boundary(reinterpret_cast<uintptr_t>(address_space)+heap_offset,heap_size);
            if(address>=hi) fail("QueryMemory exceeds 39-bit address space",uc);
            b=lo;size=hi-lo;
        }
        auto* out=reinterpret_cast<uint64_t*>(r[0]);out[0]=b;out[1]=size;
        auto* p=reinterpret_cast<uint32_t*>(out);p[4]=state;p[5]=attribute;p[6]=permission;p[7]=0;p[8]=0;p[9]=0;
        r[0]=0;r[1]=0;return;
    }
    if(id==0x2c || id==0x2d) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_physical(id==0x2c,r[0],r[1]);return;
    }
    if(id==0x04 || id==0x05) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_stack(id==0x04,r[0],r[1],r[2]);return;
    }
    if(id==0x01) {
        std::lock_guard lock(mapping_mutex);r[0]=memory_resize_heap(r[1]);
        if(!r[0])r[1]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;return;
    }
    // svcBreak: the SDK's own assertion. It reports a service answer it did not
    // like, and on a console the system would then terminate the process.
    //
    // Continuing is the deliberate choice here, not a shortcut. Guests run with
    // assertions enabled, so this fires on differences that do not actually break
    // anything -- a stand-in reply, a service this host models only partially --
    // and stopping at the first one ends the run before the rest of the picture is
    // even attempted. The reason, the module and the offset are logged, because
    // that line is the only record of what the title thought was wrong; a run that
    // ignores thirty of them while still producing frames is more informative than
    // one that stops at the first.
    //
    // The instruction after the break is often a spin (`b .`), so returning to the
    // call site is the only way out; otherwise step past it.
    if(id==0x26) {
        const uintptr_t pc=uc->uc_mcontext.pc,lr=uc->uc_mcontext.regs[30];
        uintptr_t offset=0;
        const char* module=ot2_host_module_of(pc,&offset);
        uint32_t next=0;
        const bool spins=guest_peek(pc+4,&next,4)&&(next&0xfc000000u)==0x14000000u&&(next&0x03ffffffu)==0;
        static std::atomic<unsigned> breaks{0};
        const unsigned n=breaks.fetch_add(1,std::memory_order_relaxed);
        // r[2] points at the nn::Result that failed. Its value is
        // module(9 bits) / description(13 bits), and that pair is the only
        // machine-readable statement of what went wrong -- without it the log says
        // "the SDK aborted" and nothing about why.
        uint32_t result=0;
        const bool have_result=r[2]&&guest_peek(r[2],&result,4);
        if(n<48)
            log("SVC_BREAK #%u reason=%llu at %s+0x%zx%s result=%#x module=%u description=%u raw x1=%#llx x2=%#llx x3=%#llx lr=%#llx sp=%#llx",
                n,(unsigned long long)r[0],
                module?module:"?",offset,spins?" (spin->return)":"",
                have_result?result:0u,
                have_result?(result&0x1ffu):0u,
                have_result?((result>>9)&0x1fffu):0u,
                (unsigned long long)r[1],(unsigned long long)r[2],(unsigned long long)r[3],
                (unsigned long long)uc->uc_mcontext.regs[30],(unsigned long long)uc->uc_mcontext.sp);
        // The abort call itself only uses x0-x3, so anything the deciding frame
        // still needs is in the callee-saved registers. x19 is the usual "this"
        // of the frame that chose to abort, which names the subsystem; the rest
        // are here so a spilled result code is not lost with the log.
        if(n<8) {
            char regs_text[640]; size_t used=0;
            for(unsigned i=0;i<31;i++)
                used+=size_t(snprintf(regs_text+used,sizeof regs_text-used,"%sx%u=%#llx",
                    i?" ":"",i,(unsigned long long)uc->uc_mcontext.regs[i]));
            log("SVC_BREAK_REGS %s",regs_text);
        }
        // svcBreak carries the reason for the abort as a printf format plus its
        // length (x1 = pointer, x2 = length). Without reading it back the log only
        // says "the SDK aborted". It is a guest pointer, so it is read through the
        // same guarded peeker used above.
        if(r[1] && r[2] && r[2]<=256) {
            uint8_t text[257]={0};
            if(guest_peek(r[1],text,(uint32_t)r[2])) {
                char hex[3*64+1]={0},asc[65]={0};unsigned hn=0,an=0;
                for(uint32_t i=0;i<r[2]&&i<64;i++){
                    hn+=snprintf(hex+hn,sizeof(hex)-hn,"%02x",text[i]);
                    asc[an++]=(text[i]>=32&&text[i]<127)?(char)text[i]:'.';
                }
                log("SVC_BREAK_MESSAGE len=%llu hex=%s ascii=\"%s\"",
                    (unsigned long long)r[2],hex,asc);
            } else log("SVC_BREAK_MESSAGE unreadable at %#llx len=%llu",
                       (unsigned long long)r[1],(unsigned long long)r[2]);
        }
        // The SDK's abort path is `mov x1,x0; mov w0,#0; bl svcBreak; b .`, so the
        // return address here names nothing useful -- the frames above it do. The
        // abort helper saved {x29,x30} at the current sp, so the caller is one
        // dereference away; walking the frame chain from there names the function
        // that decided to abort.
        if(n<8) {
            uint64_t fpv=0,lrv=0;
            if(guest_peek(uc->uc_mcontext.sp,&fpv,8) && guest_peek(uc->uc_mcontext.sp+8,&lrv,8)) {
                for(int depth=0;depth<10 && fpv>=4096;depth++) {
                    uint64_t next=0,ret=0;
                    if(!guest_peek(fpv,&next,8) || !guest_peek(fpv+8,&ret,8)) break;
                    const char* mn=nullptr;const char* mo=nullptr;
                    for(const auto& m:images) {
                        const auto b=reinterpret_cast<uintptr_t>(m.base);
                        if(ret>=b && ret<b+m.size) {mn=m.name.c_str();mo=nullptr;
                            log("ABORT_FRAME [%d] %s+0x%zx",depth,mn,(size_t)(ret-b)); break;}
                    }
                    if(next<=fpv || (next-fpv)>1024*1024) break;
                    fpv=next;
                }
            }
        }
        uc->uc_mcontext.pc=spins?lr:pc+4;
        return;
    }
    // Service manager and IPC. These are the two calls the SDK makes before it can
    // reach any service at all, so they cannot be limited to diagnostics: without
    // them a normal boot dies on the first port lookup.
    //
    // Both answers are stand-ins. ConnectToNamedPort hands out a synthetic session
    // and SendSyncRequest replies to it with an empty success message, which is the
    // minimum that lets the SDK's client wrapper finish a call. Services that this
    // host models properly are replaced further up, at their public entry points,
    // so a real session is never needed for them.
    if(id==0x1f) {                       // ConnectToNamedPort(const char* name)
        // The kernel returns out-parameters in registers: x0 = Result and
        // x1 = the new handle. Writing through x0's pointer is not enough —
        // the SDK reads the handle from x1 (confirmed by a trace where the
        // session value passed to SendSyncRequest was a truncated pointer).
        const auto name=guest_string(r[1],32);
        const uint64_t session=next_diagnostic_session++;
        static std::atomic<unsigned> ports{0};
        if(ports.fetch_add(1,std::memory_order_relaxed)<64)
            log("ConnectToNamedPort name='%s' session=%llx",name.c_str(),(unsigned long long)session);
        r[0]=0; r[1]=session; return;
    }
    if(id==0x21) {                       // SendSyncRequest(session,msg)
        if(diagnostic_svc_enabled && id<256 && ++diagnostic_svc_budget[id]<=400)
            log("DIAG SendSyncRequest session=%llx msg=%llx",(unsigned long long)r[0],(unsigned long long)r[1]);
        if(r[1]) diagnostic_reply(r[1],"ipc",r[0]);
        r[0]=0; return;
    }
    if(id==0x22) {                       // ReplyAndReceive
        if(diagnostic_svc_enabled) log("DIAG ReplyAndReceive session=%llx msg=%llx",(unsigned long long)r[0],(unsigned long long)r[1]);
        r[0]=0; return;
    }
    if(diagnostic_svc_enabled) {
        const bool detail=id<256 && ++diagnostic_svc_budget[id]<=400;
        if(detail) log("DIAG unimplemented SVC 0x%x x0=%llx x1=%llx x2=%llx x3=%llx (continuing)",id,(unsigned long long)r[0],(unsigned long long)r[1],(unsigned long long)r[2],(unsigned long long)r[3]);
        r[0]=0; return;
    }
    log("Unimplemented SVC 0x%x",id); fail("Required system call unavailable",uc);
}
}
extern "C" void ot2_host_log(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);log("%s",text);
}
extern "C" void ot2_host_die(const char* format, ...) {
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);fail(text);
}
extern "C" void guest_thread_init() { create_thread_context(); }
#ifdef OT2_EXECUTION_TESTING
extern "C" int ot2_test_memory_permission(uintptr_t address){std::lock_guard lock(mapping_mutex);return memory_permission_at(address);}
extern "C" uint64_t ot2_test_memory_permission_scans(){std::lock_guard lock(mapping_mutex);return memory_permission_scans;}
extern "C" bool ot2_test_memory_init(uint64_t* regions){
    if(address_space)return false;
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED){address_space=nullptr;return false;}
    regions[0]=reinterpret_cast<uintptr_t>(address_space)+heap_offset;
    regions[1]=reinterpret_cast<uintptr_t>(address_space)+stack_offset;
    regions[2]=reinterpret_cast<uintptr_t>(address_space)+alias_offset;
    regions[3]=alias_size;regions[4]=heap_capacity;regions[5]=stack_capacity;
    svc_thread_main_init();thread_system_ready=true;
    return true;
}
extern "C" bool ot2_test_memory_page_size(size_t page){
    if(page<static_cast<size_t>(sysconf(_SC_PAGESIZE))||page<4096||(page&(page-1)))return false;
    memory_host_page=page;return true;
}
extern "C" uintptr_t ot2_test_memory_module(){
    auto* raw=static_cast<uint8_t*>(mmap(nullptr,5*memory_host_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(raw==MAP_FAILED)return 0;
    const auto base=(reinterpret_cast<uintptr_t>(raw)+memory_host_page-1)&~uintptr_t(memory_host_page-1);
    if(mprotect(reinterpret_cast<void*>(base),memory_host_page,PROT_READ|PROT_EXEC)||mprotect(reinterpret_cast<void*>(base+memory_host_page),memory_host_page,PROT_READ))return 0;
    memory_registry_changed();images.push_back({"memory-contract-module",reinterpret_cast<uint8_t*>(base),4*memory_host_page,0,memory_host_page,memory_host_page,2*memory_host_page});
    return base;
}
extern "C" void ot2_test_memory_svc(unsigned id,uint64_t* registers){
    ucontext_t context{};GuestThread thread{};
    memcpy(context.uc_mcontext.regs,registers,31*sizeof(uint64_t));
    svc(id,&context,&thread);
    memcpy(registers,context.uc_mcontext.regs,31*sizeof(uint64_t));
}
extern "C" bool ot2_test_context_fresh(){
    auto* c=create_thread_context();
    uintptr_t callback=0,reused=0;size_t size=0,reused_size=0;
    ot2_guest_context_callback_stack(c,callback,size);
    ot2_guest_context_callback_stack(c,reused,reused_size);
    if(!callback||callback!=reused||size!=1024*1024||size!=reused_size)return false;
    *reinterpret_cast<volatile uint8_t*>(callback)=1;
    *reinterpret_cast<volatile uint8_t*>(callback+size-1)=2;
    bool fresh=c->guest_tls!=0x12340000&&c->guest_tlr!=0x56780000;
    c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
    return fresh;
}
extern "C" size_t ot2_test_context_live(){return ot2_guest_context_live();}
extern "C" size_t ot2_test_context_bytes(){return ot2_guest_context_bytes();}
extern "C" bool ot2_test_context_exit(){
    auto* main=create_thread_context();install_handlers();main->handle=svc_thread_main_init();thread_system_ready=true;
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED)return false;
    const uint32_t instructions[]={0x94000008,0xd4000141,0xd65f03c0},host_break=0xd420e060;
    memcpy(code,instructions,sizeof(instructions));memcpy(code+32,&host_break,4);memcpy(code+64,instructions+1,8);patch(code,4096);
    traps.push_back({reinterpret_cast<uintptr_t>(code+32),host_break,"context_exit_fixture",[](uint64_t* r)->uint64_t{
        auto* c=current();c->guest_tls=0x12340000;c->guest_tlr=0x56780000;
        if(r[0]){const uint64_t args[8]{};ot2_call_guest_address(r[0],args);}
        return 0;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+4096));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC))return false;
    const auto baseline=ot2_guest_context_live(),bytes=ot2_guest_context_bytes();
    for(unsigned i=0;i<400;i++){
        const size_t size=128*1024;auto* stack=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(stack==MAP_FAILED)return false;
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();mappings.push_back({reinterpret_cast<uintptr_t>(stack),size,5,0});}
        uint64_t r[32]{};r[1]=reinterpret_cast<uintptr_t>(code);r[2]=(i&1)?reinterpret_cast<uintptr_t>(code+64):0;
        r[3]=reinterpret_cast<uintptr_t>(stack)+size;r[4]=44;svc_thread(0x08,r);if(r[0])return false;
        uint32_t handle=r[1];r[0]=handle;svc_thread(0x09,r);if(r[0])return false;
        r[1]=reinterpret_cast<uintptr_t>(&handle);r[2]=1;r[3]=2000000000;svc_thread(0x18,r);if(r[0])return false;
        // Completion must already be on the host stack; SDK is free to release this now.
        {std::lock_guard lock(mapping_mutex);memory_registry_changed();std::erase_if(mappings,[&](const Mapping& m){return m.base==reinterpret_cast<uintptr_t>(stack);});}
        munmap(stack,size);r[0]=handle;svc_thread(0x16,r);if(r[0])return false;
        for(unsigned retry=0;retry<10000 && ot2_guest_context_live()!=baseline;retry++)usleep(100);
        if(ot2_guest_context_live()!=baseline||ot2_guest_context_bytes()!=bytes)return false;
    }
    std::erase_if(traps,[&](const Trap& t){return t.pc>=reinterpret_cast<uintptr_t>(code)&&t.pc<reinterpret_cast<uintptr_t>(code+4096);});munmap(code,4096);return true;
}
#endif
extern "C" void crash_thread_init() {}
uintptr_t callback_stack(GuestThread* c){uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));
    if(sp>=c->stack&&sp<c->stack+c->stack_size)return (sp-1024)&~uintptr_t(15);
    if(service_callback_stack)return service_callback_stack;
    return active_signal?(active_signal->uc_mcontext.sp-256)&~uintptr_t(15):0;
}
extern "C" uint64_t ot2_call_on_host_stack(uint64_t (*function)(uint64_t*),uint64_t* args){
    auto* c=current();if(!c||!c->host_frame)fail("Host service outside guest call");
    struct Invocation {uint64_t (*function)(uint64_t*);uint64_t* args;uintptr_t callback;};
    Invocation invocation{function,args,callback_stack(c)};
    auto invoke=+[](uint64_t* raw)->uint64_t{
        auto& call=*reinterpret_cast<Invocation*>(raw);auto* c=current();
        const auto previous=service_callback_stack,base=c->stack,size=c->stack_size;
        if(host_service_depth==0){
            ot2_guest_context_callback_stack(c,c->stack,c->stack_size);
            service_callback_stack=c->stack+c->stack_size;
        }else service_callback_stack=call.callback;
        ++host_service_depth;auto result=call.function(call.args);--host_service_depth;
        c->stack=base;c->stack_size=size;service_callback_stack=previous;return result;
    };
    // host_frame is the suspended ot2_guest_call frame. A nested callback
    // establishes a new frame below the current native service automatically.
    return ot2_host_stack_call(invoke,reinterpret_cast<uint64_t*>(&invocation),(c->host_frame-1024)&~uintptr_t(15));
}
extern "C" uint64_t ot2_call_guest(const char* name,const uint64_t* args) {
    auto* c=current();if(!c || !active_game) fail("Guest callback outside active game");
    auto entry=active_game->symbol(name);if(!entry) fail("Guest callback export absent");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096) fail("Nested guest callback stack exhausted");
    return ot2_guest_call(entry,c,args,top);
}
extern "C" const char* ot2_host_module_of(uint64_t address,uintptr_t* offset) {
    for(const auto& m:images) {
        const auto base=reinterpret_cast<uintptr_t>(m.base);
        if(address>=base && address<base+m.size) {
            if(offset) *offset=uintptr_t(address-base);
            return m.name.c_str();
        }
    }
    // Falls through to the host library so a failure report can name where the
    // host itself was when it decided to fault.
    if(offset) *offset=uintptr_t(address);
    return nullptr;
}
extern "C" uint64_t ot2_guest_symbol_address(const char* name) {
    if(!active_game)fail("Guest symbol outside active game");
    return active_game->symbol(name);
}
extern "C" uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* args) {
    auto* c=current();if(!c || !address)fail("Invalid guest callback");
    uintptr_t top=callback_stack(c);
    if(top && top<c->stack+4096)fail("Nested guest callback stack exhausted");
    return ot2_guest_call(address,c,args,top);
}
extern "C" void* ot2_native_gate(const char* name,void* function) {
    std::lock_guard lock(native_gate_mutex);
    unsigned count=native_gate_count.load();
    for(unsigned i=0;i<count;i++)if(!strcmp(native_gates[i].name,name))return native_gate_code+i*32;
    if(count>=2048 || strlen(name)>=sizeof(native_gates[0].name))fail("Native gate capacity exceeded");
    if(!native_gate_code) {
        native_gate_code=static_cast<uint8_t*>(mmap(nullptr,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(native_gate_code==MAP_FAILED)fail("Native gate allocation failed");
        for(unsigned i=0;i<2048;i++){
            auto* stub=native_gate_code+i*32;uint32_t code[]={0x52800010U|(i<<5),0x58000071,0xd61f0220,0xd503201f};
            memcpy(stub,code,sizeof(code));auto target=reinterpret_cast<uintptr_t>(ot2_native_entry);memcpy(stub+16,&target,8);
        }
        __builtin___clear_cache(reinterpret_cast<char*>(native_gate_code),reinterpret_cast<char*>(native_gate_code+65536));
        if(mprotect(native_gate_code,65536,PROT_READ|PROT_EXEC))fail("Native gate protection failed");
    }
    strcpy(native_gates[count].name,name);native_gates[count].function=function;native_gates[count].present=!strcmp(name,"nvnQueuePresentTexture");
    __atomic_store_n(&ot2_native_abis[count],native_register_abi(name),__ATOMIC_RELEASE);
    native_gate_count.store(count+1,std::memory_order_release);
    return native_gate_code+count*32;
}
extern "C" void* ot2_native_function(unsigned index){
    if(index>=native_gate_count.load(std::memory_order_acquire))fail("Native gate index invalid");
    auto& gate=native_gates[index];if(__atomic_fetch_add(&gate.hits,1,__ATOMIC_RELAXED)==0)log("NATIVE %s",gate.name);
    if(gate.present)update_frame_rate();
    if(!gate.function){
        // A bring-up run wants the full list of what is missing, not just the
        // first entry. Answer with zeros and keep going; a real build still
        // stops here, because silently returning wrong data would be worse than
        // a clear failure once a title is expected to work.
        if(diagnostic_svc_enabled){
            static std::atomic<unsigned> reported{0};
            const unsigned seen=reported.fetch_add(1,std::memory_order_relaxed);
            log("DIAG missing native API #%u: %s (zero stub)",seen+1,gate.name);
            return reinterpret_cast<void*>(ot2_native_zero);
        }
        log("Missing native API: %s",gate.name);fail("Required native API unavailable");
    }
    return gate.function;
}
extern "C" void dm_thread_enter(uint64_t entry,uint64_t argument,uint64_t stack_top) {
    auto* c=current();
    if(!c) fail("Worker thread has no execution context");
    bool found=false;
    {
        std::lock_guard lock(mapping_mutex);
        // A stack grows downward, so the region that holds it starts as low as
        // possible. Picking the first match instead would hand the thread a
        // small overlapping region and it would run off the end of it.
        const auto* best=static_cast<const decltype(mappings)::value_type*>(nullptr);
        unsigned candidates=0;
        for(const auto& m:mappings) {
            if(stack_top<=m.base || stack_top>m.base+m.size) continue;
            ++candidates;
            if(!best || m.base<best->base) best=&m;
        }
        if(best) {
            const size_t derived=stack_top-best->base;
            ot2_guest_context_use_stack(c,best->base,derived);
            found=true;
            // A worker stack this small is a title-sized stack from the console
            // that a host-side call chain can outgrow. Swap in a guarded one and
            // leave the guest's own memory untouched.
            if(derived<0x10000) {
                uintptr_t wbase=0;size_t wsize=0;
                ot2_guest_context_worker_stack(c,wbase,wsize);
                ot2_guest_context_use_stack(c,wbase,wsize);
                log("WORKER_STACK top=%#llx guest=%#zx replaced with %#zx at %#llx",
                    (unsigned long long)stack_top,derived,wsize,(unsigned long long)wbase);
            }
        }
    }
    if(!found) fail("Worker stack has not been mapped");
    const uint64_t args[]={argument,0,0,0,0,0,0,0};
    ot2_guest_call(entry,c,args,0);
    fail("Worker returned without ExitThread");
}
extern "C" void ot2_signal_handler(int sig,siginfo_t* info,void* raw) {
    auto* uc=static_cast<ucontext_t*>(raw);
    if(sig==OT2_SAMPLE_SIGNAL) {
        // Flat-profile mode first: it only wants the PC, so it must not pay for
        // the stack scan below (which reads /proc/self/mem and is the expensive
        // part of a sweep).
        if(pc_profile_store.load(std::memory_order_relaxed)) {
            pc_hit_record(uc->uc_mcontext.pc,int32_t(syscall(SYS_gettid)));
            pc_profile_samples.fetch_add(1,std::memory_order_relaxed);
            return;
        }
        if(pc_sample_store.load(std::memory_order_relaxed)) {
            const auto pc=uc->uc_mcontext.pc;
            const char* where="?";
            uintptr_t offset=0;
            for(const auto& m:images) {
                const auto b=reinterpret_cast<uintptr_t>(m.base);
                if(pc>=b && pc<b+m.size) { where=m.name.c_str(); offset=pc-b; break; }
            }
            const unsigned slot=pc_sample_count.fetch_add(1,std::memory_order_relaxed)%PcSampleSlots;
            auto& record=pc_samples[slot];
            record={int32_t(syscall(SYS_gettid)),pc,(uintptr_t)uc->uc_mcontext.regs[30],(uintptr_t)uc->uc_mcontext.sp,where,offset,0,{},(uintptr_t)uc->uc_mcontext.regs[29]};
            // Stack scan for the one thread the watchdog flagged as busy. Read
            // through /proc/self/mem rather than dereferencing: a thread parked
            // deep in a call chain has its sp close to the guard page and the
            // sweep runs off the end by design, which a raw load turns into a
            // second fault inside the fault-safe path. pread just reports EIO.
            if(record.tid==pc_sample_target.load(std::memory_order_relaxed)) {
                static int mem=-2;
                if(mem==-2) mem=open("/proc/self/mem",O_RDONLY|O_CLOEXEC);
                for(unsigned k=0;k<768 && mem>=0 && record.frames<PcSampleFrames;k++) {
                    uintptr_t value=0;
                    if(pread(mem,&value,8,off_t(record.sp+k*8))!=8) break;
                    if(value<0x1000) continue;
                    for(const auto& m:images) {
                        const auto lo=reinterpret_cast<uintptr_t>(m.base)+m.text_offset;
                        if(value>=lo && value<lo+m.text_size) { record.stack[record.frames++]=value; break; }
                    }
                }
            }
            return;
        }
        // Watchdog sample: the guest pinning a core at 100% with no log output
        // is either a spin on a value the host never writes or a loop the host
        // fed bad data to. Both are only visible as a PC, so print where it was
        // running when the sample arrived.
        static std::atomic<unsigned> samples{0};
        const unsigned n=samples.fetch_add(1,std::memory_order_relaxed);
        // A sample is only worth printing while a dump is being assembled. The
        // handler is otherwise a per-thread log line every two seconds for the whole
        // session -- hundreds of lines a minute saying nothing, on a path that runs
        // on top of every guest thread.
        if(n<12000 && dump_stacks.load(std::memory_order_acquire)) {
            const auto pc=uc->uc_mcontext.pc;
            const char* where="?";
            uintptr_t offset=0;
            for(const auto& m:images) {
                const auto b=reinterpret_cast<uintptr_t>(m.base);
                if(pc>=b && pc<b+m.size) { where=m.name.c_str(); offset=pc-b; break; }
            }
            char tag[32]; snprintf(tag,sizeof tag,"pc=%llx %s+0x%llx",
                (unsigned long long)pc,where,(unsigned long long)offset);
            if(n%12==0)
                log_host_stack(tag,n,uc->uc_mcontext.regs[29],uc->uc_mcontext.regs[30]);
            log("WATCHDOG #%u tid=%d pc=%llx %s+0x%llx sp=%llx lr=%llx x0=%llx x8=%llu x1=%llx x2=%llx",
                n,(int)syscall(SYS_gettid),(unsigned long long)pc,where,(unsigned long long)offset,
                (unsigned long long)uc->uc_mcontext.sp,
                (unsigned long long)uc->uc_mcontext.regs[30],
                (unsigned long long)uc->uc_mcontext.regs[0],
                (unsigned long long)uc->uc_mcontext.regs[8],
                (unsigned long long)uc->uc_mcontext.regs[1],
                (unsigned long long)uc->uc_mcontext.regs[2]);
        }
        return;
    }
    if(sig==SIGSEGV && (info->si_code==SEGV_ACCERR||info->si_code==SEGV_MAPERR)){
        // Linux exposes the ARM64 data-abort ESR in the signal context. Only
        // write faults may be handled by dirty tracking; never swallow execute
        // faults or unrelated guest memory errors.
        auto* records=uc->uc_mcontext.__reserved;size_t offset=0;
        while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(records+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context)){
                const auto esr=reinterpret_cast<const esr_context*>(record)->esr;
                int saved_errno=errno;
                if(dirty_write_fault_candidate(sig,info->si_code,esr)&&nvn_dirty_fault(info->si_addr)){errno=saved_errno;return;}
                break;
            }
            offset+=record->size;
        }
    }
    auto* c=current();
    if(sig!=SIGTRAP) {
        log("Signal %d code=%d address=%p tracked=%d",sig,info->si_code,info->si_addr,nvn_dirty_tracked(info->si_addr,1));
        // A fault whose address equals the stack pointer is a stack problem, not
        // a stray pointer: either the thread ran past its guard page or the page
        // is mapped without the access the caller used. Print what the bookkeeping
        // believes so the two cases can be told apart from one log.
        if(c) {
            const auto sp=uc->uc_mcontext.sp;
            const auto stack=c->stack,size=c->stack_size;
            log("FAULT_STACK sp=%#llx stack=[%#llx,%#llx) inside=%d guard=%d",
                (unsigned long long)sp,(unsigned long long)stack,
                (unsigned long long)(stack+size),
                (sp>=stack&&sp<stack+size)?1:0,(sp<stack+4096)?1:0);
            log("FAULT_STACK perms sp=%#x page=%#x addr=%#x",
                memory_permission_at(sp),memory_permission_at(sp&~uintptr_t(4095)),
                memory_permission_at(reinterpret_cast<uintptr_t>(info->si_addr)));
            // 故障地址落在哪一段 mapping、以及内存里真实的指令编码。
            // 只看模块文件会误导：加载期 TLS 重写会把 mrs/msr 换成 `b island`，
            // 文件里那条指令和真正执行的不是同一条。这里两样都打出来。
            {
                // /proc/self/maps 里的归属。用固定缓冲区、只 read/sscanf，
                // 避免在信号处理里分配内存而撞上 malloc 锁。
                static char maps[256*1024];
                const int fd=open("/proc/self/maps",O_RDONLY);
                if(fd>=0){
                    ssize_t total=0;
                    while(total<int(sizeof(maps))-1){
                        const ssize_t n=read(fd,maps+total,size_t(int(sizeof(maps))-1-total));
                        if(n<=0)break;
                        total+=n;
                    }
                    close(fd);maps[total]=0;
                    const auto report=[&](const char* tag,uintptr_t address){
                        unsigned long long lo=0,hi=0;char perms[8]={};
                        unsigned long long below_lo=0,below_hi=0;char below[192]={};
                        for(const char* at=maps;at&&*at;){
                            const char* line_end=strchr(at,'\n');
                            const size_t length=line_end?size_t(line_end-at):strlen(at);
                            if(sscanf(at,"%llx-%llx %7s",&lo,&hi,perms)==3){
                                if(address>=lo&&address<hi){
                                    char line[192]={};
                                    const auto take=std::min(length,sizeof(line)-1);
                                    memcpy(line,at,take);
                                    log("FAULT_MAP %s=%#llx IN %s",tag,(unsigned long long)address,line);
                                    return;
                                }
                                if(hi<=address&&hi>=below_hi){
                                    below_hi=hi;below_lo=lo;
                                    const auto take=std::min(length,sizeof(below)-1);
                                    memcpy(below,at,take);
                                }
                            }
                            if(!line_end)break;
                            at=line_end+1;
                        }
                        log("FAULT_MAP %s=%#llx NOT_MAPPED below=[%#llx,%#llx) %s",tag,
                            (unsigned long long)address,(unsigned long long)below_lo,
                            (unsigned long long)below_hi,below);
                    };
                    report("fault",reinterpret_cast<uintptr_t>(info->si_addr));
                    report("sp",sp);
                    report("pc",uc->uc_mcontext.pc);
                }
                // 内存里的真实指令。process_vm_readv 读不到会返回错误，
                // 不会在故障处理里再触发一次 SIGSEGV。
                uint64_t words[16]{};
                const auto pc=uc->uc_mcontext.pc;
                struct iovec local{words,sizeof(words)};
                struct iovec remote{reinterpret_cast<void*>(pc-32),sizeof(words)};
                const auto got=process_vm_readv(getpid(),&local,1,&remote,1,0);
                if(got>0){
                    log("FAULT_CODE pc=%#llx words_before_after:",(unsigned long long)pc);
                    for(unsigned i=0;i<got/8;i+=4)
                        log("  pc%+#06x: %08llx %08llx %08llx %08llx",
                            int(i*8)-32,
                            (unsigned long long)(words[i]&0xffffffffu),(unsigned long long)(words[i+1]&0xffffffffu),
                            (unsigned long long)(words[i+2]&0xffffffffu),(unsigned long long)(words[i+3]&0xffffffffu));
                }else{
                    log("FAULT_CODE pc=%#llx unreadable (%lld)",(unsigned long long)pc,(long long)got);
                }
            }
        }
        size_t offset=0;while(offset+sizeof(_aarch64_ctx)<=sizeof(uc->uc_mcontext.__reserved)){
            auto* record=reinterpret_cast<const _aarch64_ctx*>(uc->uc_mcontext.__reserved+offset);
            if(record->size<sizeof(*record)||record->size>sizeof(uc->uc_mcontext.__reserved)-offset)break;
            if(record->magic==ESR_MAGIC&&record->size>=sizeof(esr_context))log("FAULT_ESR value=%llx",(unsigned long long)reinterpret_cast<const esr_context*>(record)->esr);
            offset+=record->size;
        }
        // A null dereference names the pointer that was null but never the object
        // that was supposed to hold it: by the time the faulting `ldr x8,[x0]`
        // runs, the only surviving evidence is the callee-saved registers of the
        // frame that made the call. Dump the qwords around them and around sp so
        // the owning structure can be read out of one log group instead of a
        // debugger session. guest_memory.inc is the only place that may be
        // touched here, because reading an unmapped guest address would fault
        // again inside the handler.
        auto probe=[&](const char* name,uintptr_t base) {
            if(!base || !(memory_permission_at(base)&PROT_READ)) return;
            log("FAULT_OBJ %s=%#llx",name,(unsigned long long)base);
            for(unsigned i=0;i<32;i+=4) {
                const auto at=base+i*8;
                if(!(memory_permission_at(at)&PROT_READ) || !(memory_permission_at(at+24)&PROT_READ)) break;
                const auto* q=reinterpret_cast<const uintptr_t*>(at);
                log("  +%#04x: %#llx %#llx %#llx %#llx",i*8,
                    (unsigned long long)q[0],(unsigned long long)q[1],
                    (unsigned long long)q[2],(unsigned long long)q[3]);
            }
        };
        probe("x1",uc->uc_mcontext.regs[1]);
        probe("x2",uc->uc_mcontext.regs[2]);
        probe("x19",uc->uc_mcontext.regs[19]);
        probe("x20",uc->uc_mcontext.regs[20]);
        probe("x21",uc->uc_mcontext.regs[21]);
        probe("sp",uc->uc_mcontext.sp);
        if(trace_services.load(std::memory_order_relaxed)) dump_service_trace();
        fail(c?"Guest fault":"Signal outside guest thread",uc);
    }
    if(!c) fail("Signal outside guest thread",uc);
    Trap* found=nullptr;
    static std::array<std::atomic<uint32_t>,32768> trap_lookup{};
    auto& cached=trap_lookup[(uc->uc_mcontext.pc>>2)&(trap_lookup.size()-1)];const auto index=cached.load(std::memory_order_relaxed);
    if(index&&index<=traps.size()&&traps[index-1].pc==uc->uc_mcontext.pc)found=&traps[index-1];
    else for(size_t i=0;i<traps.size();++i)if(traps[i].pc==uc->uc_mcontext.pc){found=&traps[i];cached.store(i+1,std::memory_order_relaxed);break;}
    if(!found) fail("Unregistered breakpoint",uc);
    if(found->hook) {
        sdk_signal_count.fetch_add(1,std::memory_order_relaxed);
        if(__atomic_fetch_add(&found->hits,1,__ATOMIC_RELAXED)==0) log("HOST %s",found->hook);
        if(found->call) {
            auto* previous=active_signal;active_signal=uc;
            uc->uc_mcontext.regs[0]=found->call(reinterpret_cast<uint64_t*>(uc->uc_mcontext.regs));
            active_signal=previous;
            if(trace_services.load(std::memory_order_relaxed)) {
                const unsigned slot=service_call_head.fetch_add(1,std::memory_order_relaxed)%ServiceCallRingSize;
                service_call_ring[slot]={found->hook,uint32_t(uc->uc_mcontext.regs[0]),(int32_t)syscall(SYS_gettid)};
            }
        }
        uc->uc_mcontext.pc=uc->uc_mcontext.regs[30];
        return;
    }
    auto op=found->instruction;auto target=op&31;auto kind=op&~31U;
    if(kind==0xd53bd040||kind==0xd53bd060||kind==0xd51bd040)tls_signal_count.fetch_add(1,std::memory_order_relaxed);
    auto* r=uc->uc_mcontext.regs;
    if((op&0xffe0001f)==0xd4000001) {svc_signal_count.fetch_add(1,std::memory_order_relaxed);svc((op>>5)&0xffff,uc,c);}
    else if(kind==0xd51bd040) c->guest_tls=target==31?0:r[target];
    else if(target!=31) {
        switch(kind) {
        case 0xd53bd040:r[target]=c->guest_tls;break;
        case 0xd53bd060:r[target]=c->guest_tlr;break;
        case 0xd53be020:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=ticks();break;
        case 0xd53be000:counter_signal_count.fetch_add(1,std::memory_order_relaxed);r[target]=19200000;break;
        default:fail("Unknown patched instruction",uc);
        }
    }
    uc->uc_mcontext.pc+=4;
}
namespace ot2 {
void freeze_guest_clock(unsigned reason,bool on){guest_clock.freeze(reason,on,monotonic_ns);}
void set_frame_rate_target(int fps){requested_frame_rate.store(fps==60?60:30,std::memory_order_relaxed);}
int frame_rate_applied(){return applied_frame_rate.load(std::memory_order_relaxed);}
static double host_abi_fixture(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,double j,double k) {
    return a+b+c+d+e+f+g+h+i+j+k;
}
std::string execution_probe() {
    std::lock_guard lock(setup_mutex);
    auto* c=create_thread_context();install_handlers();
    // Deliberately overwrite guest x18; cross TLS and SVC gates, then return.
    uint32_t instructions[]={0xd280f692,0xd53bd040,0xd40003c1,0xaa1203e0,0xd65f03c0};
    auto* code=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(code==MAP_FAILED) throw std::runtime_error("Execution probe allocation failed");
    memcpy(code,instructions,sizeof(instructions));patch(code,sizeof(instructions));
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+sizeof(instructions)));
    if(mprotect(code,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Execution probe mprotect failed");
    const uint64_t args[8]{};const auto value=ot2_guest_call(reinterpret_cast<uintptr_t>(code),c,args,0);
    if(value!=1972) throw std::runtime_error("Guest x18 did not survive host trap");
    auto* nested=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(nested==MAP_FAILED) throw std::runtime_error("Nested probe allocation failed");
    const uint32_t outer[]={0xa9bf7bfd,0xd280f692,0x9400000e,0x8b120000,0xa8c17bfd,0xd65f03c0};
    std::vector<uint32_t> inner{0xd11003ff,0xa9007bfd};
    for(unsigned offset=16;offset<1024;offset+=8)inner.push_back(0xf90003ff|((offset/8)<<10));
    for(uint32_t instruction:{0xd280f6b2U,0xd40003c1U,0xaa1203e0U,0xa9407bfdU,0x911003ffU,0xd65f03c0U})inner.push_back(instruction);
    const uint32_t host_break=0xd420e060;
    memcpy(nested,outer,sizeof(outer));memcpy(nested+64,&host_break,4);memcpy(nested+128,inner.data(),inner.size()*4);
    patch(nested+128,inner.size()*4);
    traps.push_back({reinterpret_cast<uintptr_t>(nested+64),host_break,"nested_execution_fixture",[](uint64_t* r)->uint64_t{
        volatile uint64_t sentinels[16];for(unsigned i=0;i<16;++i)sentinels[i]=0x5a5a0000+i;
        const uint64_t arguments[8]{};auto* thread=current();
        const auto top=(active_signal->uc_mcontext.sp-256)&~uintptr_t(15);
        auto result=ot2_guest_call(r[0],thread,arguments,top);
        for(unsigned i=0;i<16;++i)if(sentinels[i]!=0x5a5a0000+i)fail("Nested signal overwrote host frame");
        return result;
    }});
    __builtin___clear_cache(reinterpret_cast<char*>(nested),reinterpret_cast<char*>(nested+4096));
    if(mprotect(nested,4096,PROT_READ|PROT_EXEC)) throw std::runtime_error("Nested probe mprotect failed");
    const uint64_t nested_args[]={reinterpret_cast<uintptr_t>(nested+128),0,0,0,0,0,0,0};
    const auto nested_value=ot2_guest_call(reinterpret_cast<uintptr_t>(nested),c,nested_args,0);
    if(nested_value!=3945) throw std::runtime_error("Nested guest register restoration failed");
    auto* direct=static_cast<uint8_t*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(direct==MAP_FAILED)throw std::runtime_error("Direct callback probe allocation failed");
    memcpy(direct,outer,sizeof(outer));const uint32_t jump[]={0x58000050,0xd61f0200};memcpy(direct+64,jump,sizeof(jump));
    auto callback=+[](uint64_t entry)->uint64_t{volatile uint64_t guard[16];for(unsigned i=0;i<16;i++)guard[i]=0x12340000+i;const uint64_t arguments[8]{};auto value=ot2_call_guest_address(entry,arguments);for(unsigned i=0;i<16;i++)if(guard[i]!=0x12340000+i)fail("Direct callback overwrote native stack");return value;};
    auto gate=reinterpret_cast<uintptr_t>(ot2_native_gate("direct_nested_fixture",reinterpret_cast<void*>(callback)));memcpy(direct+72,&gate,8);
    __builtin___clear_cache(reinterpret_cast<char*>(direct),reinterpret_cast<char*>(direct+4096));if(mprotect(direct,4096,PROT_READ|PROT_EXEC))throw std::runtime_error("Direct callback probe protection failed");
    const auto direct_value=ot2_guest_call(reinterpret_cast<uintptr_t>(direct),c,nested_args,0);if(direct_value!=3945)throw std::runtime_error("Direct native callback register restoration failed");
    const uint64_t abi_args[]={reinterpret_cast<uint64_t>(ot2_native_gate("abi_fixture",reinterpret_cast<void*>(host_abi_fixture))),0,0,0,0,0,0,0};
    auto abi_value=ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_abi_fixture),c,abi_args,0);
    if(abi_value!=49)throw std::runtime_error("Native floating-point/stack ABI failed");
    struct {alignas(16) uint8_t view[16];uint64_t guards[2];} view_fixture{};
    view_fixture.guards[0]=0x12345678abcd0001;view_fixture.guards[1]=0x12345678abcd0002;
    const uint64_t view_args[]={reinterpret_cast<uint64_t>(view_fixture.view),3,2,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetDefaults")),c,view_args,0);
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureViewSetLevels")),c,view_args,0);
    if(view_fixture.guards[0]!=0x12345678abcd0001 || view_fixture.guards[1]!=0x12345678abcd0002)throw std::runtime_error("NVN view exceeds game ABI size");
    struct {alignas(16) uint8_t texture[128];tex_builder_t builder;} texture_fixture{};
    texture_fixture.builder.d.target=NVN_TEXTURE_TARGET_BUFFER;texture_fixture.builder.d.format=NVN_FORMAT_R32UI;
    texture_fixture.builder.d.width=32;texture_fixture.builder.d.height=texture_fixture.builder.d.depth=texture_fixture.builder.d.levels=1;
    const auto saved_builder=texture_fixture.builder;
    const uint64_t texture_args[]={reinterpret_cast<uint64_t>(texture_fixture.texture),reinterpret_cast<uint64_t>(&texture_fixture.builder),0,0,0,0,0,0};
    ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup("nvnTextureInitialize")),c,texture_args,0);
    if(memcmp(&saved_builder,&texture_fixture.builder,sizeof(saved_builder)))throw std::runtime_error("NVN texture writes beyond 128-byte game object");
    alignas(16) std::array<uint8_t,256> source_memory{},copy_memory{};cmd_t source_cmd{},copy_cmd{};
    auto call_nvn=[&](const char* name,std::initializer_list<uint64_t> values){uint64_t a[8]{};std::copy(values.begin(),values.end(),a);return ot2_guest_call(reinterpret_cast<uintptr_t>(ot2_nvn_lookup(name)),c,a,0);};
    for(auto item:{std::pair{&source_cmd,source_memory.data()},std::pair{&copy_cmd,copy_memory.data()}}){
        call_nvn("nvnCommandBufferInitialize",{reinterpret_cast<uint64_t>(item.first),0});
        pool_t pool{MAGIC('P','O','O','L'),0,item.second,256};
        call_nvn("nvnCommandBufferAddCommandMemory",{reinterpret_cast<uint64_t>(item.first),reinterpret_cast<uint64_t>(&pool),0,256});
        call_nvn("nvnCommandBufferBeginRecording",{reinterpret_cast<uint64_t>(item.first)});
    }
    call_nvn("nvnCommandBufferSetCopyRowStride",{reinterpret_cast<uint64_t>(&source_cmd),1234});
    uint64_t handle=call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&source_cmd)});
    call_nvn("nvnCommandBufferCopyCommands",{reinterpret_cast<uint64_t>(&copy_cmd),1,reinterpret_cast<uint64_t>(&handle)});
    call_nvn("nvnCommandBufferEndRecording",{reinterpret_cast<uint64_t>(&copy_cmd)});
    source_memory.fill(0xdd);op_hdr copied{};int64_t stride=0;memcpy(&copied,copy_memory.data()+8,8);memcpy(&stride,copy_memory.data()+16,8);
    if(copied.op!=OP_COPY_STRIDE || stride!=1234)throw std::runtime_error("NVN CopyCommands borrowed reusable source memory");
    alignas(16) std::array<uint8_t,64> keyboard;keyboard.fill(0xa5);uint64_t keyboard_args[]={reinterpret_cast<uint64_t>(keyboard.data()),0,0,0,0,0,0,0};
    bool keyboard_called=false;for(auto& service:input_services())if(!strcmp(service.name,"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE")){service.call(keyboard_args);keyboard_called=true;}
    if(!keyboard_called)throw std::runtime_error("Keyboard state service absent");
    for(size_t i=8;i<keyboard.size();i++)if(keyboard[i]!=(i<48?0:0xa5))throw std::runtime_error("Keyboard state ABI size mismatch");
    return "{\"direct_native_callback\":true,\"guest_x18\":"+std::to_string(value)+",\"tls_and_svc_traps\":true,\"nested_return\":"+std::to_string(nested_value)+",\"native_abi_return\":"+std::to_string(abi_value)+",\"nvn_view_16_bytes\":true,\"nvn_texture_128_bytes\":true,\"nvn_command_copy_owned\":true,\"keyboard_state_48_bytes\":true}";
}
extern "C" void ot2_guest_batch_profile(int enabled){
    ot2::set_io_prefetch(enabled!=0);__atomic_store_n(&fast_system_enabled,enabled!=0,__ATOMIC_RELEASE);
    ot2_host_log("SYSTEM_GATES enabled=%d calls=%llu",enabled,(unsigned long long)system_fast_calls.exchange(0));
    // The profile tick arrives about once a second. A per-symbol tally every
    // half minute is enough to spot a title spinning on one service, and it
    // costs nothing on the frames in between.
    static int profile_ticks=0;
    if(++profile_ticks>=30){profile_ticks=0;ot2_system_dump_hot(28);}
}
extern "C" void ot2_guest_register_gates(int enabled){
    __atomic_store_n(&ot2_register_gates_enabled,enabled!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_execution_profile(int enable_tls){
    log("EXECUTION_BOUNDARIES tls_fast=%u tls_traps=%llu svc_traps=%llu counter_traps=%llu sdk_traps=%llu",
        __atomic_load_n(&fast_tls_enabled,__ATOMIC_ACQUIRE),(unsigned long long)tls_signal_count.exchange(0),
        (unsigned long long)svc_signal_count.exchange(0),(unsigned long long)counter_signal_count.exchange(0),(unsigned long long)sdk_signal_count.exchange(0));
    __atomic_store_n(&fast_tls_enabled,enable_tls!=0,__ATOMIC_RELEASE);
}
extern "C" void ot2_guest_permission_profile(int enabled){
    std::lock_guard lock(mapping_mutex);
    log("MEMORY_PERMISSION enabled=%u scans=%llu hits=%llu mappings=%zu",memory_permission_cache_enabled,(unsigned long long)memory_permission_scans,(unsigned long long)memory_permission_hits,mappings.size());
    memory_permission_cache_enabled=enabled!=0;memory_permission_scans=memory_permission_hits=0;
}
// 给底层服务（thread_services.c 等）读同一个标记目录用：路径只在这里算一次，
// 免得每个模块自己拼一遍 /data/user/0/... 的绝对路径。
// Lets lower layers (thread_services.c and friends) read the same marker directory:
// the path is computed once here so no module has to rebuild the absolute path itself.
extern "C" int ot2_game_flag(const char* name) {
    if(watchdog_log_directory.empty()||!name||!*name) return 0;
    std::error_code error;
    return std::filesystem::exists(std::filesystem::path(watchdog_log_directory)/name,error)?1:0;
}
extern "C" const char* ot2_game_directory(void) {
    return watchdog_log_directory.c_str();
}
void boot_game(const std::string& directory,const std::string& log_path,uint64_t program_id) {
    log_fd=open(log_path.c_str(),O_CREAT|O_WRONLY|O_TRUNC|O_CLOEXEC,0600);
    log("Dedicated native host: ProgramId=%016llx",(unsigned long long)program_id);
    diagnostic_svc_enabled=std::filesystem::exists(std::filesystem::path(log_path).parent_path()/"diagnostic-svc");
    if(diagnostic_svc_enabled)log("DIAG diagnostic SVC mode: unimplemented calls are reported and continued");
    watchdog_log_directory=std::filesystem::path(log_path).parent_path().string();
    // The service-call ring is normally armed by the watchdog, which first looks
    // for the `trace-services` marker two seconds in. A title that faults while
    // booting never reaches that tick, and a boot fault is exactly when the last
    // few host calls decide everything -- so honour the marker here too.
    if(std::filesystem::exists(std::filesystem::path(watchdog_log_directory)/"trace-services"))
        trace_services.store(true,std::memory_order_relaxed);
    auto user_path=(std::filesystem::path(log_path).parent_path()/"userdata").string();
    std::filesystem::create_directories(user_path);
    vkhost_configure_package((std::filesystem::path(log_path).parent_path()/"shaders").c_str(),(std::filesystem::path(directory).parent_path().parent_path()/"shaders").c_str());
    // The engine reads its culture out of RomFS/boot.lua before any service can
    // answer a language query, so this has to run before the guest starts -- and
    // on every boot, because the import writes the file back with the value the
    // title shipped with. Marker: <game>/content-language (missing = zhCN).
    const auto romfs_path=(std::filesystem::path(directory).parent_path()/"RomFS").string();
    const ot2::CultureRewrite boot_culture=ot2::rewrite_boot_culture(romfs_path,ot2::content_language(watchdog_log_directory));
    if(boot_culture.status!=ot2::CultureRewrite::kNoBootScript)
        log("CONTENT_LANG %s %s -> %s",ot2::culture_status_name(boot_culture),boot_culture.from.c_str(),boot_culture.to.c_str());
    configure_services(romfs_path,user_path,program_id);
    auto* c=create_thread_context();install_handlers();
    address_space=static_cast<uint8_t*>(mmap(nullptr,reserve_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(address_space==MAP_FAILED) fail("Address space reserve failed");
    c->handle=svc_thread_main_init();thread_system_ready=true;
    LoadedGame game(directory);active_game=&game;memory_registry_changed();images=game.modules();
    // The shared-font handoff depends on which side deobfuscates it, and only the
    // title knows that. Asked here, before any service can be reached, because the
    // font is requested during the title's own boot.
    set_shared_font_decodes(game.imports("main","_ZN2nn6fontll24ScalableFontEngineHelper6Decode"));
    ot2_host_log("MAIN_IMPORTS total=%zu font_engine=%d decode_helper=%d",
        game.imported_symbols("main").size(),
        game.imports("main","_ZN2nn6fontll18ScalableFontEngine")?1:0,
        game.imports("main","_ZN2nn6fontll24ScalableFontEngineHelper6Decode")?1:0);
    // Dumped before make_executable(): relocations are already applied by the
    // loader, and the text has not picked up this host's BRK patches yet, so the
    // file holds the instructions the compiler emitted at the offsets the log
    // reports as "<name>+0x...".
    dump_module_images(images,directory,std::filesystem::path(log_path).parent_path().string());
    for(const auto& m:images)if(m.name=="main")frame_rate_patch.configure(reinterpret_cast<uintptr_t>(m.base),m.size,m.build_id);
    for(auto& m:images) { image_size+=m.size;patch(m.base+m.text_offset,m.text_size);log("Module %s base=%p size=%zu",m.name.c_str(),m.base,m.size); }
    // Whether the SDK carries the in-process renderer is the one fact that
    // decides if audio can be driven without an audren server (the approach the
    // nxnative line uses: switch the parameter to CPU + manual and call the
    // SDK's own ExecuteAudioRendererRendering). A symbol that is missing here
    // means that path does not exist in this build and the plan has to change.
    { static const char* probes[]={
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm",
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm",
        "_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE",
        "_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE",
        "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE",
        "_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm",
        "_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv",
        "_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv",
        // Input and language: the two host paths that decide whether the title
        // reacts to a finger and speaks Chinese. A MISSING line here means the
        // patch site does not exist in this build, so the handler list can be as
        // correct as it likes and the guest still calls the real service.
        "_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",
        "_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi",
        "_ZN2nn3hid21InitializeTouchScreenEv",
        "_ZN2nn3hid21InitializeTouchScreenEii",
        "_ZN2nn3hid14InitializeNpadEv",
        "_ZN2nn3hid15GetNpadStyleSetERKj",
        "_ZN2nn3hid24GetSupportedNpadStyleSetEv",
        "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",
        "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",
        "_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",
        "_ZN2nn2oe18GetDesiredLanguageEv",
        "_ZN2nn2oe16GetOperationModeEv",
        "_ZN2nn8settings15GetLanguageCodeEPNS0_12LanguageCodeE",
        "_ZN2nn8settings29GetAvailableLanguageCodeCountEv",
        "_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi",
        nullptr };
      for(int i=0;probes[i];i++){const auto a=game.symbol(probes[i]);
        log("SYMPROBE %s = %s%#llx",probes[i],a?"":"MISSING ",(unsigned long long)a);} }
    install_service_initializers(game);
    // Diagnostic only, off unless the title's marker directory asks for it.
    //
    // The sdk checks one byte of a caller's object (offset 0x28) against the two
    // kinds it knows and aborts on anything else, with the message "Unsupported
    // color conversion request". The byte arrives from a display-capability query
    // the host answers with an empty reply, so it is zero here. Forcing the check
    // to its accepted path answers a different question from "make the query
    // right": it says whether this rejection is the only thing between this build
    // and the title's own code, or the first of several. Both outcomes are worth
    // knowing before spending more time on the reply's contents.
    if(ot2_game_flag("force-color-conversion")) {
        for(const auto& m:images) if(m.name=="sdk") {
            // sdk+0x346f38: ldrb w8,[x0,#0x28] -> mov w8,#2 (the accepted kind)
            auto* at=reinterpret_cast<uint32_t*>(m.base+0x346f38);
            const uint32_t was=*at;
            if(was==0x3940a008u) { *at=0x52800048u; log("SDK_PATCH sdk+0x346f38 ldrb w8,[x0,#0x28] -> mov w8,#2"); }
            else log("SDK_PATCH skipped: sdk+0x346f38 holds %#x",was);
        }
    }
    game.make_executable();log("Patched %zu system boundaries; running ARM64 directly",traps.size());
    start_watchdog(pthread_self());
    auto call=[&](const char* name,uint64_t arg0=0,uint64_t arg1=0) {
        auto address=game.symbol(name);if(!address) {log("Missing %s",name);fail("Required entry absent");}
        const uint64_t args[]={arg0,arg1,0,0,0,0,0,0};
        log("ENTER %s",name);auto result=ot2_guest_call(address,c,args,0);log("RETURN %s = %llx",name,(unsigned long long)result);
    };
    call("_init_libc0");call("nnosInitialize",c->handle,0);call("_init_libc1");
    call("nninitInitializeSdkModule");call("nninitStartup");call("_init_libc2");
    for(auto address:game.initializers()) {const uint64_t args[8]{};log("DT_INIT %p",reinterpret_cast<void*>(address));ot2_guest_call(address,c,args,0);}
    call("nnMain");
}
#ifdef OT2_EXECUTION_TESTING
#include "tests/guest_sdk_contract.inc"
#include "tests/register_contract.inc"
#include "tests/tls_contract.inc"
#include "tests/native_gate_contract.inc"
#include "tests/host_stack_contract.inc"
#endif
}
