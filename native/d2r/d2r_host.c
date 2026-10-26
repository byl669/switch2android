// Host half of the d2r Vulkan engine integration.
//
// The engine (native/d2r/gfx/vulkan/engine, MIT, from seedds/nxnative and the
// d2r-mobile Vulkan backend) replays the same command stream our recorder emits,
// so it replaces native/graphics/backend.cpp + draw.inc and nothing else: the
// loader, HLE services, guest execution and the NVN entry points stay ours.
//
// Linking the engine against our headers leaves exactly three symbols for this
// file to define -- verified with -Wl,--no-undefined over the whole archive:
//
//   xbg_record_command   the engine copies a command handle into a command buffer
//   nx_vk_depth_zero     which vertex-shader entry the replay picks (z remap)
//   nx_vk_origin_ll      window origin convention for that same selection
//
// Everything else the engine needs (nvn_tex_*, nvn_deswizzle_*, nvn_new_serial)
// already exists in this host.
//
// These two globals are *not* runtime state of the game; they describe the title
// so the replay can pick the matching shader entry point. NVN's defaults are a
// lower-left window origin and a unipolar (GL-style, near = -w) depth range.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "nvn_obj.h"
#include "nvn_commands.h"

// Provided by native/graphics/frontend.c: reserve `size` bytes in the command
// memory the game handed us, growing it through the game's callback if needed.
uint8_t *nvn_command_alloc(cmd_t *command, uint32_t size);

// These two describe what this host has *already* converted to, not what NVN
// itself does. The replay reads them as preconditions (xbg_game.cpp draw():
// "Negative-one depth requires a shader variant", "Lower-left origin requires a
// viewport variant") -- it has no variant shaders for either, so it requires the
// host to have made both conversions itself:
//
//   depth   = 1 : near plane maps to z = 0   (Metal/D3D convention, not GL's -w)
//   origin  = 0 : window origin is the upper left (not NVN's lower left)
//
// The depths this title feeds are the 0..1 form, and the viewport/V position
// handling is ours to get right, which is what these two flags promise.
int nx_vk_depth_zero = 1;
int nx_vk_origin_ll = 0;

// The engine keeps some command records alive across submissions (it copies them
// into its own command buffer when a handle must outlive the guest's memory).
// Appending one is the same operation our recorder performs: reserve room in the
// guest command buffer, copy the record verbatim, then let the command bookkeeping
// take a reference on any object the payload names.
void xbg_record_command(cmd_t *command, const void *record, uint32_t bytes) {
    if (!command || !record || bytes < sizeof(op_hdr)) return;
    uint8_t *destination = nvn_command_alloc(command, bytes);
    memcpy(destination, record, bytes);
    nvn_commands_copy_record(command, (op_hdr *)destination);
}

// Declared with C linkage in graphics/backend.h for the C++ side; this file is C
// so the prototype is repeated here rather than pulling in that header.
uint64_t nvn_new_serial(void);
