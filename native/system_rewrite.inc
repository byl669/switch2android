// Full-state host gates for elapsed-time reads and SDK service entry points.
// The tiny island never overwrites a second guest instruction. x16/x17/LR are
// saved before entering host code; every other register and FP state is saved
// by ot2_system_entry. Slow BRK sites remain available for negative controls.
alignas(4) uint32_t fast_system_enabled=1;
// `calls` is a per-symbol tally kept only so a stalled title can be asked
// "what are you actually calling?" without another rebuild. Counted with
// __atomic builtins rather than std::atomic so the struct stays trivially
// copyable and the vector can still reallocate.
struct SystemService {const char* name;uint64_t (*call)(uint64_t*);uint64_t calls;};
std::vector<SystemService> system_services;
std::atomic<uint64_t> system_fast_calls{0};
// Top entries by call count. Runs off the periodic profile tick, never from a
// gate, so the sort cannot reenter the dispatch that is being measured.
extern "C" void ot2_system_dump_hot(int top) {
    std::vector<std::pair<uint64_t,const char*>> ranked;
    for(const auto& service:system_services) {
        const uint64_t calls=__atomic_load_n(&service.calls,__ATOMIC_RELAXED);
        if(calls)ranked.push_back({calls,service.name});
    }
    std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    for(int i=0;i<top && i<int(ranked.size());++i)
        log("HLE_HOT %s calls=%llu",ranked[i].second,(unsigned long long)ranked[i].first);
}
// A gate replaces the function's first instruction, so the guest can no longer
// reach the SDK body from the export address. Host code that has to run the
// original -- a takeover that rewrites an argument and then calls through, for
// instance -- needs an entry that replays the instruction the gate took and
// then falls into the untouched body at entry+4.
// Only position-independent first instructions are accepted: an ADRP/ADR or a
// literal load moved to another page would compute from the wrong PC. Audio and
// most SDK service entries open with a plain stack prologue, so this covers the
// cases that need it and refuses the rest loudly instead of corrupting state.
struct OriginalEntry {uintptr_t entry;uint8_t* page;uint8_t* code;};
// Small tables: a few dozen takeovers at most, so a linear scan keeps this
// include-order independent (this file is pulled in mid-translation-unit, and
// pulling <map> in here breaks libc++'s own include order).
std::vector<std::pair<uintptr_t,uint32_t>> patched_first_op;
std::vector<OriginalEntry> original_entries;
static bool pc_relative_instruction(uint32_t op){
    if((op&0x1f000000u)==0x10000000u)return true;   // ADR
    if((op&0x9f000000u)==0x90000000u)return true;   // ADRP
    if((op&0x7c000000u)==0x14000000u)return true;   // B, BL
    if((op&0x7e000000u)==0x34000000u)return true;   // CBZ, CBNZ
    if((op&0x7e000000u)==0x36000000u)return true;   // TBZ, TBNZ
    if((op&0x3b000000u)==0x18000000u)return true;   // LDR literal
    return false;
}
// Returns a callable entry that runs the original body, or 0 when the gate
// cannot be stepped around. An ungated address is returned unchanged, so a
// caller can use this for every symbol and only pay for the ones gated.
extern "C" uint64_t ot2_original_entry(uint64_t entry){
    if(!entry)return 0;
    for(const auto& owned:original_entries)if(owned.entry==entry)return reinterpret_cast<uintptr_t>(owned.code);
    uint32_t op=0;bool gated=false;
    for(const auto& patched:patched_first_op)if(patched.first==uintptr_t(entry)){op=patched.second;gated=true;break;}
    if(!gated)return entry;   // not gated: the export address is the original body
    if(pc_relative_instruction(op)){
        log("ORIGINAL_ENTRY %#llx unavailable: first instruction %#x is PC relative",(unsigned long long)entry,op);
        return 0;
    }
    constexpr size_t page_bytes=4096;
    auto* page=tls_allocate_near(entry,page_bytes);
    if(!page){log("ORIGINAL_ENTRY %#llx unavailable: no island within branch range",(unsigned long long)entry);return 0;}
    uint32_t code[2]={op,tls_branch(reinterpret_cast<uintptr_t>(page)+4,entry+4)};
    memcpy(page,code,sizeof code);
    __builtin___clear_cache(reinterpret_cast<char*>(page),reinterpret_cast<char*>(page)+sizeof code);
    if(mprotect(page,page_bytes,PROT_READ|PROT_EXEC)){
        munmap(page,page_bytes);
        log("ORIGINAL_ENTRY %#llx unavailable: cannot seal the island",(unsigned long long)entry);
        return 0;
    }
    original_entries.push_back({uintptr_t(entry),page,page});
    return reinterpret_cast<uintptr_t>(page);
}
// Builds the step-around trampoline ahead of time, inside the island the gate was
// just installed in. That island is branch-reachable from the entry by
// construction and still writable at this point, so this cannot fail for want of
// range and needs no permission juggling -- which is exactly what the lazy path in
// ot2_original_entry cannot promise.
//
// The lazy path is genuinely fragile: a fresh 4 KiB mapping is only granted where
// a 64 KiB-aligned slot is still free, and the gate islands themselves consume
// those slots as boot proceeds. Measured on one packaged launch, every slot within
// +-128 MB of sdk+0x46c70 was taken by the time the hooks first ran, so
// ot2_original_entry returned 0 and the two hooks standing there answered "nothing
// released, nothing played" to the title for the whole session. The audio graph
// then looked idle, no stream advanced past its first 128 KiB, and the minigame
// result screen never arrived -- a probe that silently changes program behaviour is
// worse than no probe, so reserving here is not an optimisation.
void reserve_original_entry(uintptr_t entry,uint32_t op,TlsIsland& island){
    for(const auto& owned:original_entries)if(owned.entry==entry)return;
    if(pc_relative_instruction(op))return;   // cannot be relocated; see ot2_original_entry
    constexpr size_t trampoline_bytes=16;
    if(island.bytes-island.used<trampoline_bytes)return;
    auto* site=island.memory+island.used;
    if(!tls_branch_reachable(entry,uintptr_t(site)))return;
    if(!tls_branch_reachable(uintptr_t(site)+8,entry+4))return;
    uint32_t code[2]={op,tls_branch(uintptr_t(site)+4,entry+4)};
    memcpy(site,code,sizeof code);
    __builtin___clear_cache(reinterpret_cast<char*>(site),reinterpret_cast<char*>(site)+sizeof code);
    island.used+=trampoline_bytes;
    original_entries.push_back({entry,site,site});
}
extern "C" void ot2_system_entry();
void svc(unsigned,ucontext_t*,GuestThread*);
extern "C" void ot2_system_dispatch(uint64_t selector,uint64_t* registers){
    // Assembly stores its own return link separately at word 31.
    auto* saved=registers+100;
    registers[16]=saved[0];registers[17]=saved[1];registers[30]=saved[2];
    if(selector&0x8000){const auto& service=system_services.at(selector&0x7fff);if(service.call){__atomic_fetch_add(&const_cast<SystemService&>(service).calls,1,__ATOMIC_RELAXED);registers[0]=service.call(registers);}}
    else if(selector&0x4000){
        ucontext_t context{};memcpy(context.uc_mcontext.regs,registers,31*8);
        context.uc_mcontext.sp=reinterpret_cast<uintptr_t>(saved+4);context.uc_mcontext.pc=registers[31];
        auto* previous=active_signal;active_signal=&context;svc(selector&0x3fff,&context,current());active_signal=previous;
        memcpy(registers,context.uc_mcontext.regs,31*8);
    }
    else {const auto target=selector&31;if(target!=31)registers[target]=(selector&32)?19200000:ticks();}
    saved[0]=registers[16];saved[1]=registers[17];saved[2]=registers[30];
    system_fast_calls.fetch_add(1,std::memory_order_relaxed);
}
bool patch_system(uintptr_t pc,uint32_t op,size_t first_island,const char* name=nullptr,uint64_t (*call)(uint64_t*)=nullptr){
    const auto kind=op&~31u;const bool service=name!=nullptr;const bool syscall=(op&0xffe0001f)==0xd4000001;
    if(!service&&kind!=0xd53be020&&kind!=0xd53be000&&!syscall)return false;
    if(syscall&&((op>>5)&0xffff)>=0x4000)return false;
    if(service&&system_services.size()>=0x8000)return false;
    constexpr size_t capacity=112;TlsIsland* island=nullptr;
    for(size_t i=first_island;i<tls_islands.size();++i){auto& c=tls_islands[i];if(c.bytes-c.used>=capacity&&tls_branch_reachable(pc,uintptr_t(c.memory)+c.used)&&tls_branch_reachable(uintptr_t(c.memory)+c.used+capacity,pc+4)){island=&c;break;}}
    if(!island){const size_t bytes=std::max<size_t>(16384,sysconf(_SC_PAGESIZE));auto* memory=tls_allocate_near(pc,bytes);if(!memory)return false;tls_islands.push_back({memory,bytes,0});island=&tls_islands.back();}
    auto* base=island->memory+island->used;
    const uint32_t selector=service?0x8000|uint32_t(system_services.size()):op==0xd40003c1?0u:syscall?0x4000|((op>>5)&0x3fff):(op&31)|(kind==0xd53be000?32u:0u);
    std::vector<uint32_t> code;auto emit=[&](uint32_t word){code.push_back(word);return code.size()-1;};
    auto relative=[&](size_t at,size_t to){code[at]|=(uint32_t(int32_t(to)-int32_t(at))&0x7ffffu)<<5;};
    auto restore=[&]{emit(0xf9400bfe);emit(0xa8c247f0);}; // ldr LR,[sp,16]; ldp x16,x17,[sp],32
    auto resume=[&]{if(service)emit(0xd65f03c0);else emit(tls_branch(uintptr_t(base)+code.size()*4,pc+4));};
    emit(0xa9be47f0);emit(0xf9000bfe); // stp x16,x17,[sp,-32]!; str LR,[sp,16]
    auto flag=emit(0x58000010);emit(0x88dffe11); // ldr x16,literal; ldar w17,[x16]
    auto disabled=emit(0x34000011);
    emit(0x52800010|(selector<<5));auto entry=emit(0x58000011);emit(0xd63f0220);
    restore();resume();relative(disabled,code.size());restore();
    auto trap=emit(0xd4200000|((service?0x703:0x702)<<5));resume();
    if(code.size()&1)emit(0xd503201f);
    auto literal=[&](uintptr_t value){auto at=code.size();emit(uint32_t(value));emit(uint32_t(value>>32));return at;};
    relative(flag,literal(uintptr_t(&fast_system_enabled)));relative(entry,literal(uintptr_t(ot2_system_entry)));
    if(code.size()*4>capacity)fail("System gate island capacity");
    if(service)system_services.push_back({name,call});
    traps.push_back({uintptr_t(base)+trap*4,op,name,call});
    memcpy(base,code.data(),code.size()*4);island->used+=(code.size()*4+15)&~size_t(15);
    reserve_original_entry(pc,op,*island);
    patched_first_op.push_back({pc,op});
    auto branch=tls_branch(pc,uintptr_t(base));memcpy(reinterpret_cast<void*>(pc),&branch,4);return true;
}
