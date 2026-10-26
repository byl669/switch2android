#pragma once
#include <cstddef>
#include <cstdint>

// Fixed layout consumed by FIND_THREAD and the ARM64 call/exit bridges.
struct alignas(64) GuestThread {
    uintptr_t host_tp,host_x18,host_frame,guest_tls,guest_tlr,stack,stack_size,handle;
};
static_assert(sizeof(GuestThread)==64);
constexpr unsigned GuestThreadCapacity=256;
extern "C" __attribute__((visibility("hidden"))) GuestThread ot2_threads[GuestThreadCapacity];
GuestThread* ot2_guest_context_current();
GuestThread* ot2_guest_context_create();
void ot2_guest_context_use_stack(GuestThread*,uintptr_t base,size_t size);
void ot2_guest_context_callback_stack(GuestThread*,uintptr_t& base,size_t& size);
// Page-guarded stack for a worker entry, independent of the callback stack.
void ot2_guest_context_worker_stack(GuestThread*,uintptr_t& base,size_t& size);
size_t ot2_guest_context_live();
size_t ot2_guest_context_bytes();
