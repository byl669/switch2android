#include "guest_context.h"
#include "thread_services.h"
#include <atomic>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>

extern "C" { __attribute__((visibility("hidden"))) GuestThread ot2_threads[GuestThreadCapacity]{}; }
extern "C" { __attribute__((visibility("hidden"))) int32_t ot2_guest_tids[GuestThreadCapacity]{}; }
namespace {
pthread_key_t key;
pthread_once_t once=PTHREAD_ONCE_INIT;
std::atomic<size_t> live{},mapped_bytes{};
struct Owned {
    GuestThread* context;
    void* tls{};size_t tls_size=16384;
    void* stack{};size_t stack_size{};
    void* signal{};size_t signal_size=2*1024*1024;
    void* callback{};size_t callback_size{};
    // Worker entry stack, separate from the callback stack so a host-to-guest
    // callback inside a worker does not reuse the stack the worker runs on.
    void* worker{};size_t worker_size{};
};
void* allocate(size_t size,int protection){
    auto* memory=mmap(nullptr,size,protection,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory==MAP_FAILED)ot2_host_die("Guest thread mapping failed");
    mapped_bytes.fetch_add(size);return memory;
}
void release(void* memory,size_t size){if(memory){if(munmap(memory,size))ot2_host_die("Guest thread unmap failed");mapped_bytes.fetch_sub(size);}}
void destroy(void* value){
    auto* owned=static_cast<Owned*>(value);auto* c=owned->context;
    // ExitThread first switches to the original pthread stack. Never unmap a
    // signal/guest stack while pthread destructors are still executing on it.
    uintptr_t sp;asm volatile("mov %0,sp":"=r"(sp));
    auto inside=[&](void* p,size_t size){return p&&sp>=uintptr_t(p)&&sp<uintptr_t(p)+size;};
    if(inside(owned->stack,owned->stack_size)||inside(owned->signal,owned->signal_size)||inside(owned->callback,owned->callback_size))ot2_host_die("Guest context destroyed on an owned stack");
    stack_t disabled{};disabled.ss_flags=SS_DISABLE;if(sigaltstack(&disabled,nullptr))ot2_host_die("Guest signal stack disable failed");
    // Free immutable allocation addresses, never guest-modifiable TLS/TLR.
    release(owned->tls,owned->tls_size);release(owned->stack,owned->stack_size);release(owned->signal,owned->signal_size);
    release(owned->callback,owned->callback_size);
    release(owned->worker,owned->worker_size);
    c->host_x18=c->host_frame=c->guest_tls=c->guest_tlr=c->stack=c->stack_size=c->handle=0;
    __atomic_store_n(&c->host_tp,0,__ATOMIC_RELEASE);
    for(unsigned i=0;i<GuestThreadCapacity;i++) if(&ot2_threads[i]==c) ot2_guest_tids[i]=0;
    live.fetch_sub(1);delete owned;
}
void initialize(){if(pthread_key_create(&key,destroy))ot2_host_die("Guest context key creation failed");}
}
GuestThread* ot2_guest_context_current(){
    const auto tp=reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
    for(auto& c:ot2_threads)if(__atomic_load_n(&c.host_tp,__ATOMIC_ACQUIRE)==tp)return &c;
    return nullptr;
}
GuestThread* ot2_guest_context_create(){
    if(auto* current=ot2_guest_context_current())return current;
    pthread_once(&once,initialize);GuestThread* c=nullptr;unsigned index=0;
    for(;index<GuestThreadCapacity;++index){uintptr_t free=0;if(__atomic_compare_exchange_n(&ot2_threads[index].host_tp,&free,UINTPTR_MAX,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){c=&ot2_threads[index];break;}}
    if(!c)ot2_host_die("Guest live thread capacity exhausted");
    // Remember the OS thread behind this context: a guest thread that spins
    // without logging can only be sampled if the watchdog knows where to send
    // its signal, and GuestThread's 64-byte layout is fixed by the ARM64
    // bridges, so the mapping lives beside it.
    ot2_guest_tids[index]=static_cast<int32_t>(syscall(SYS_gettid));
    auto* owned=new Owned{c};owned->tls=allocate(owned->tls_size,PROT_READ|PROT_WRITE);
    c->guest_tls=reinterpret_cast<uintptr_t>(owned->tls);c->guest_tlr=c->guest_tls+8192;
    *reinterpret_cast<uintptr_t*>(c->guest_tlr+0x1f8)=c->guest_tlr+0x200;
    const size_t page=sysconf(_SC_PAGESIZE);c->stack_size=8*1024*1024;owned->stack_size=c->stack_size+2*page;
    owned->stack=allocate(owned->stack_size,PROT_NONE);c->stack=reinterpret_cast<uintptr_t>(owned->stack)+page;
    if(mprotect(reinterpret_cast<void*>(c->stack),c->stack_size,PROT_READ|PROT_WRITE))ot2_host_die("Guest stack protection failed");
    owned->signal=allocate(owned->signal_size,PROT_READ|PROT_WRITE);
    stack_t alternate{};alternate.ss_flags=static_cast<int>(1U<<31); // Linux SS_AUTODISARM, needed for nested guest traps.
    alternate.ss_sp=owned->signal;alternate.ss_size=owned->signal_size;
    if(sigaltstack(&alternate,nullptr))ot2_host_die("Guest signal stack setup failed");
    asm volatile("mov %0,x18":"=r"(c->host_x18));c->handle=0x1001+index;
    if(pthread_setspecific(key,owned))ot2_host_die("Guest context key assignment failed");
    live.fetch_add(1);
    __atomic_store_n(&c->host_tp,reinterpret_cast<uintptr_t>(__builtin_thread_pointer()),__ATOMIC_RELEASE);
    return c;
}
void ot2_guest_context_use_stack(GuestThread* c,uintptr_t base,size_t size){
    auto* owned=static_cast<Owned*>(pthread_getspecific(key));if(!owned||owned->context!=c)ot2_host_die("Guest stack assignment on wrong thread");
    release(owned->stack,owned->stack_size);owned->stack=nullptr;owned->stack_size=0;
    c->stack=base;c->stack_size=size; // SDK-owned mapping; the SDK releases it after thread completion.
}
void ot2_guest_context_callback_stack(GuestThread* c,uintptr_t& base,size_t& size){
    auto* owned=static_cast<Owned*>(pthread_getspecific(key));if(!owned||owned->context!=c)ot2_host_die("Callback stack on wrong thread");
    // Only services using the native-stack boundary allocate this. Reuse it
    // until thread exit, with guards on both ends and a fixed 1 MiB upper bound.
    const size_t page=sysconf(_SC_PAGESIZE);size=1024*1024;
    if(!owned->callback){
        owned->callback_size=size+2*page;owned->callback=allocate(owned->callback_size,PROT_NONE);
        if(mprotect(static_cast<uint8_t*>(owned->callback)+page,size,PROT_READ|PROT_WRITE))ot2_host_die("Callback stack protection failed");
    }
    base=uintptr_t(owned->callback)+page;
}
void ot2_guest_context_worker_stack(GuestThread* c,uintptr_t& base,size_t& size){
    auto* owned=static_cast<Owned*>(pthread_getspecific(key));if(!owned||owned->context!=c)ot2_host_die("Worker stack on wrong thread");
    // The SDK hands its workers a stack carved out of guest memory, and a title
    // that sized it for the console can overflow it here once a host-side call
    // chain is deeper. Give the worker a page-guarded stack of our own instead:
    // the guest only ever uses the stack pointer, never the address, so where it
    // lives is not observable to it.
    const size_t page=sysconf(_SC_PAGESIZE);size=1024*1024;
    if(!owned->worker){
        owned->worker_size=size+2*page;owned->worker=allocate(owned->worker_size,PROT_NONE);
        if(mprotect(static_cast<uint8_t*>(owned->worker)+page,size,PROT_READ|PROT_WRITE))ot2_host_die("Worker stack protection failed");
    }
    base=uintptr_t(owned->worker)+page;
}
size_t ot2_guest_context_live(){return live.load();}
size_t ot2_guest_context_bytes(){return mapped_bytes.load();}
