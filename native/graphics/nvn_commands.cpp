#include "nvn_commands.h"
#include "backend.h"
#include "nvn_program.h"
#include "../thread_services.h"
#include <map>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <set>
#include <vector>
#include <functional>

namespace {
// Lock order: command ownership -> backend. Backend must pin before taking its
// state lock, and release pins after dropping that lock.
// A recording entry can re-enter the guest: running out of command memory asks
// the title for more, and anything the title touches on the way back in may need
// this lock again. A plain std::mutex deadlocks silently in that case, which from
// the outside looks like the title simply stopping. Report the contention before
// blocking so the cause is visible in the log.
struct TracedMutex {
    std::mutex inner;
    void lock() {
        // Spin briefly, then back off in microseconds.
        //
        // This used to poll with usleep(100000): every contended call then took at
        // least 100 ms, and a title issuing thousands of commands per frame spent
        // all its time sitting in that sleep. The symptom looked like a stalled
        // engine -- frames stopped, memory crept up, every thread parked -- while
        // the real cause was this diagnostics helper throttling the whole command
        // path. A short spin covers the common case (the holder is about to
        // release) and the growing backoff still reports a genuine re-entry.
        for (int i = 0; i < 100; i++) {
            if (inner.try_lock()) return;
#if defined(__aarch64__) || defined(__arm__)
            asm volatile("yield" ::: "memory");   // let the holding core make progress
#endif
        }
        // ~3 s of 1 ms attempts, reporting only once the wait is noticeable.
        for (int i = 0; i < 3000; i++) {
            if (inner.try_lock()) {
                if (i >= 50) ot2_host_log("NVN_CMDS_MUTEX acquired after %dms", i);
                return;
            }
            usleep(1000);
        }
        ot2_host_log("NVN_CMDS_MUTEX blocked >3s: a recording entry re-entered while holding it");
        inner.lock();
    }
    void unlock() { inner.unlock(); }
};
TracedMutex mutex;
struct Texture {
    tex_t value;
    explicit Texture(const tex_t& v):value(v){vkhost_tex_retain(v.serial);}
    ~Texture(){vkhost_tex_unref(value.serial);}
};
struct Program {std::shared_ptr<const NvnProgramVersion> owned;prog_t value;explicit Program(const prog_t& v):owned(nvn_program_get(v.serial)),value(owned->value){};};
struct Pool {pool_t backing;dpool_t value;explicit Pool(const dpool_t& v):backing(v.pool?*v.pool:pool_t{}),value(v){value.pool=&backing;}};
struct Segment {
    uintptr_t begin,end;
    std::map<uint64_t,std::shared_ptr<Texture>> textures;
    std::map<uint64_t,std::shared_ptr<Program>> programs;
    std::vector<std::unique_ptr<Pool>> pools;
    Segment(uintptr_t b,size_t size):begin(b),end(b+size){}
};
using Lease=std::shared_ptr<Segment>;
std::map<uintptr_t,Lease> segments;
std::map<uint64_t,std::weak_ptr<Texture>> textures;
std::map<uint64_t,std::weak_ptr<Program>> programs;
void collect(){std::erase_if(textures,[](const auto& v){return v.second.expired();});std::erase_if(programs,[](const auto& v){return v.second.expired();});}
void remove_overlaps(uintptr_t begin,size_t size){
    if(size>UINTPTR_MAX-begin)ot2_host_die("Command memory interval overflow");
    auto it=segments.lower_bound(begin);
    if(it!=segments.begin()){auto before=std::prev(it);if(before->second->end>begin)it=before;}
    while(it!=segments.end()&&it->first<begin+size)it=segments.erase(it);
}
Segment& owner(cmd_t* command){
    if(!command->snapshot_segment)ot2_host_die("Command object snapshot without command memory");
    return **static_cast<Lease*>(command->snapshot_segment);
}
const tex_t* texture(cmd_t* command,const tex_t* value){
    if(!value)return nullptr;
    if(!value->serial)ot2_host_die("Recording an uninitialized texture");
    auto& owned=owner(command).textures;auto found=owned.find(value->serial);if(found!=owned.end())return &found->second->value;
    auto snapshot=textures[value->serial].lock();if(!snapshot){snapshot=std::make_shared<Texture>(*value);textures[value->serial]=snapshot;}
    owned.emplace(value->serial,snapshot);return &snapshot->value;
}
const prog_t* program(cmd_t* command,const prog_t* value){
    if(!value)return nullptr;
    if(!value->serial)ot2_host_die("Recording an uninitialized program");
    auto& owned=owner(command).programs;auto found=owned.find(value->serial);if(found!=owned.end())return &found->second->value;
    auto snapshot=programs[value->serial].lock();if(!snapshot){snapshot=std::make_shared<Program>(*value);programs[value->serial]=snapshot;}
    owned.emplace(value->serial,snapshot);return &snapshot->value;
}
const dpool_t* pool(cmd_t* command,const dpool_t* value){
    if(!value)return nullptr;
    auto copy=std::make_unique<Pool>(*value);auto* result=&copy->value;owner(command).pools.push_back(std::move(copy));return result;
}
struct Pins {std::vector<Lease> segments;std::set<const Segment*> seen;std::set<uint64_t> traversed;uint64_t fence=0;std::vector<uint64_t> waits;};
void pin_at(Pins& pins,uintptr_t address){
    auto it=segments.upper_bound(address);if(it==segments.begin())return;--it;
    if(address<it->second->end&&pins.seen.insert(it->second.get()).second)pins.segments.push_back(it->second);
}
void walk(Pins& pins,uint64_t handle,unsigned depth){
    if(depth>32)ot2_host_die("Command call recursion limit");
    if(!pins.traversed.insert(handle).second)return;
    pin_at(pins,handle);auto at=reinterpret_cast<const uint8_t*>(handle);
    for(unsigned count=0;count<1000000;++count){
        auto h=reinterpret_cast<const op_hdr*>(at);if(h->size<8||h->size%8||h->size>16*1024*1024)ot2_host_die("Invalid pinned command record");
        if(h->op==OP_END)return;
        if(h->op==OP_JUMP){memcpy(&at,at+8,8);pin_at(pins,reinterpret_cast<uintptr_t>(at));continue;}
        if(h->op==OP_CALL){uint64_t nested;memcpy(&nested,at+8,8);walk(pins,nested,depth+1);}
        if(h->op==OP_WAIT_SYNC){const sync_t* sync;memcpy(&sync,at+8,8);if(__atomic_load_n(&sync->fenced,__ATOMIC_ACQUIRE))pins.waits.push_back(__atomic_load_n(&sync->fence,__ATOMIC_ACQUIRE));}
        if(h->op==OP_FENCE&&pins.fence){sync_t* sync;memcpy(&sync,at+8,8);__atomic_store_n(&sync->fence,pins.fence,__ATOMIC_RELEASE);__atomic_store_n(&sync->fenced,1,__ATOMIC_RELEASE);}
        at+=h->size;
    }
    ot2_host_die("Pinned command list has no end");
}
}
extern "C" void nvn_commands_grant(cmd_t* command,void* memory,size_t size){
    std::lock_guard lock(mutex);remove_overlaps(reinterpret_cast<uintptr_t>(memory),size);
    auto grant=std::make_shared<Segment>(reinterpret_cast<uintptr_t>(memory),size);segments.emplace(grant->begin,grant);
    if(!command->snapshot_segment)command->snapshot_segment=new Lease;
    *static_cast<Lease*>(command->snapshot_segment)=std::move(grant);collect();
}
extern "C" void nvn_commands_finalize(cmd_t* command){std::lock_guard lock(mutex);delete static_cast<Lease*>(command->snapshot_segment);command->snapshot_segment=nullptr;collect();}
extern "C" void nvn_commands_pool_release(const void* memory,size_t size){std::lock_guard lock(mutex);remove_overlaps(reinterpret_cast<uintptr_t>(memory),size);collect();}
extern "C" const tex_t* nvn_command_texture(cmd_t* command,const tex_t* value){std::lock_guard lock(mutex);return texture(command,value);}
extern "C" const prog_t* nvn_command_program(cmd_t* command,const prog_t* value){std::lock_guard lock(mutex);return program(command,value);}
extern "C" const dpool_t* nvn_command_pool(cmd_t* command,const dpool_t* value){std::lock_guard lock(mutex);return pool(command,value);}
extern "C" void nvn_commands_copy_record(cmd_t* command,op_hdr* header){
    std::lock_guard lock(mutex);void* data=header+1;
    switch(header->op){
    case OP_SET_RT:{auto& v=*static_cast<p_set_rt*>(data);for(int i=0;i<v.n;++i)v.color[i]=texture(command,v.color[i]);v.depth=texture(command,v.depth);break;}
    case OP_BIND_PROGRAM:{auto& v=*static_cast<p_bind_program*>(data);v.prog=program(command,v.prog);break;}
    case OP_COPY_B2T:case OP_COPY_T2B:{auto& v=*static_cast<p_copy_b2t*>(data);v.tex=texture(command,v.tex);break;}
    case OP_COPY_T2T:{auto& v=*static_cast<p_copy_t2t*>(data);v.src=texture(command,v.src);v.dst=texture(command,v.dst);break;}
    case OP_CLEAR_TEX:{auto& v=*static_cast<p_clear_tex*>(data);v.tex=texture(command,v.tex);break;}
    case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:{auto& v=*static_cast<p_set_pool*>(data);v.pool=pool(command,v.pool);break;}
    default:break;
    }
}
extern "C" void* nvn_commands_pin(int n,const uint64_t* handles){std::lock_guard lock(mutex);auto pins=std::make_unique<Pins>();for(int i=0;i<n;++i)walk(*pins,handles[i],0);return pins.release();}
extern "C" void nvn_commands_publish_fences(int n,const uint64_t* handles,uint64_t sequence){std::lock_guard lock(mutex);Pins pins;pins.fence=sequence;for(int i=0;i<n;i++)walk(pins,handles[i],0);}
extern "C" void nvn_commands_unpin(void* pins){std::lock_guard lock(mutex);delete static_cast<Pins*>(pins);collect();}
extern "C" void nvn_commands_stats(size_t* s,size_t* t,size_t* p){std::lock_guard lock(mutex);collect();*s=segments.size();*t=textures.size();*p=programs.size();}
std::vector<uint64_t> nvn_commands_waits(int n,const uint64_t* handles){std::lock_guard lock(mutex);Pins pins;for(int i=0;i<n;i++)walk(pins,handles[i],0);return pins.waits;}
std::vector<const uint8_t*> nvn_commands_index(void* value,int count,const uint64_t* handles){
    const auto& pins=*static_cast<const Pins*>(value);std::vector<const uint8_t*> out;out.reserve(4096);
    uintptr_t cached_begin=0,cached_end=0;size_t visited=0;
    auto contains=[&](uintptr_t at,size_t bytes){
        if(bytes>UINTPTR_MAX-at)return false;
        if(at>=cached_begin&&at+bytes<=cached_end)return true;
        for(const auto& s:pins.segments)if(at>=s->begin&&at+bytes<=s->end){cached_begin=s->begin;cached_end=s->end;return true;}return false;
    };
    std::function<bool(uintptr_t,unsigned)> expand=[&](uintptr_t at,unsigned depth){
        if(depth>32)return false;
        while(++visited<=262144){
            if(!contains(at,8))return false;op_hdr h;memcpy(&h,reinterpret_cast<const void*>(at),8);
            if(h.size<8||h.size%8||!contains(at,h.size))return false;
            if(h.op==OP_END)return true;
            if(h.op==OP_CALL||h.op==OP_JUMP){
                if(h.size<16)return false;uintptr_t next;memcpy(&next,reinterpret_cast<const void*>(at+8),8);
                if(h.op==OP_JUMP){at=next;continue;}if(!expand(next,depth+1))return false;
            }else out.push_back(reinterpret_cast<const uint8_t*>(at));
            at+=h.size;
        }return false;
    };
    for(int i=0;i<count;i++)if(!expand(handles[i],0)){out.clear();break;}return out;
}
