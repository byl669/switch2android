#pragma once
// A mutex that says so before it blocks for a long time.
//
// The host re-enters guest code from inside NVN entries (command memory
// callbacks, fence callbacks) and from SVC handlers (memory transfer
// adapters). A mutex taken on that path can be asked for again by the guest
// while the outer frame still holds it, and a plain std::mutex then deadlocks
// with no output at all: from outside, the title simply stops. Naming the lock
// in the log turns that silence into a diagnosis.
#include <mutex>
#include <unistd.h>
#include "../thread_services.h"

namespace ot2 {
class TracedMutex {
    const char* name;
    std::mutex inner;
public:
    explicit TracedMutex(const char* n):name(n){}
    void lock() {
        // Spin briefly, then back off in microseconds -- not in 100 ms steps.
        //
        // Polling with usleep(100000) meant every contended acquisition cost at
        // least a tenth of a second. A title that takes these locks a few thousand
        // times per frame then spends all of its time asleep, and the log fills
        // with "waited 100ms" lines that look like a hung engine while the engine
        // is in fact only being throttled by this helper. Holding the lock is
        // normally a short operation, so a spin covers most cases with no sleep at
        // all; the slower path stays fine-grained enough that a real re-entry is
        // still reported, just without stalling everything else.
        for (int i = 0; i < 100; i++) {
            if (inner.try_lock()) return;
#if defined(__aarch64__) || defined(__arm__)
            asm volatile("yield" ::: "memory");   // let the holding core run
#endif
        }
        for (int i = 0; i < 3000; i++) {
            if (inner.try_lock()) {
                if (i >= 20) ot2_host_log("LOCK %s waited %dms",name,i);
                return;
            }
            usleep(1000);
        }
        // Print once then wait: the owner may never release, and repeating this
        // would bury the one line that matters.
        ot2_host_log("LOCK %s blocked >3s (re-entered while held, or owner is stuck)",name);
        inner.lock();
    }
    void unlock() { inner.unlock(); }
    TracedMutex(const TracedMutex&)=delete;
    TracedMutex& operator=(const TracedMutex&)=delete;
};
}
