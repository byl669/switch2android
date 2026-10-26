// Integration shim: the engine reads the host's authoritative NVN object and
// command-record layout. Our recorder is the single source of truth (it is
// validated against the real game), so the engine adapts to it.
#pragma once
#include "../../../graphics/nvn_obj.h"

#ifdef __cplusplus
extern "C" {
#endif
// Object serials are also declared in graphics/backend.h with C linkage; this
// keeps the engine's C and C++ translation units agreeing with that.
uint64_t nvn_new_serial(void);
#ifdef __cplusplus
}
#endif
