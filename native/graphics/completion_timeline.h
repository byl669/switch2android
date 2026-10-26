#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include "../thread_services.h"

// Translation admission and GPU retirement use the same sequence numbers,
// but advance independently. A completed encode is never a completed fence.
class CompletionTimeline {
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t completed=0;
    const char* name;
public:
    explicit CompletionTimeline(const char* n="timeline"):name(n){}
    void complete(uint64_t sequence) {
        {std::lock_guard lock(mutex);completed=std::max(completed,sequence);}
        changed.notify_all();
    }
    bool wait(uint64_t sequence,uint64_t timeout=UINT64_MAX) {
        std::unique_lock lock(mutex);
        auto ready=[&]{return completed>=sequence;};
        if(ready())return true;
        if(!timeout)return false;
        if(timeout==UINT64_MAX){
            // An unbounded wait on a sequence the host never reaches stops the
            // title with no output at all. Report the gap before sleeping: the
            // numbers say whether a submission is missing or the wait itself is
            // wrong.
            int waited=0;
            while(!changed.wait_for(lock,std::chrono::seconds(2),ready)) {
                waited+=2;
                if(waited>6) {
                    static std::atomic<unsigned> reported{0};
                    if(reported.fetch_add(1,std::memory_order_relaxed)<8)
                        ot2_host_log("TIMELINE %s waiting %ds: want=%llu completed=%llu",
                            name,waited,(unsigned long long)sequence,(unsigned long long)completed);
                }
            }
            return true;
        }
        return changed.wait_for(lock,std::chrono::nanoseconds(std::min<uint64_t>(timeout,INT64_MAX)),ready);
    }
};
