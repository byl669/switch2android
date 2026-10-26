// SPDX-License-Identifier: MIT
// Adapted from d2r-mobile, Copyright (c) 2026 jhaizhou.
// See ../third_party/d2r-mobile-LICENSE. Android Vulkan adaptation.
// NVN 宿主侧对象布局与命令记录格式：hle_nvn.c（接口层，写）与 nvn_mtl.m（Metal 回放层，读）共用。
// 对象就地存在游戏分配的 NVN 对象内存里，大小不超过 nvn.h 的保留大小（FITS 静态断言）。
// 状态对象（Blend / Color / … 4–0x18 字节）是游戏按值拷贝的小结构，布局用位域就地打包。
// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#ifdef __cplusplus
#define _Static_assert static_assert
#endif
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " 超出 " #H " 的保留大小")

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; } queue_t;                 FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; } pool_t;                FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // 贴图 / 采样器描述符池 | Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
typedef struct {
    uint32_t magic; nvn_texdesc d; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off;
    uint64_t serial;                     // 初始化序号：回放层按它在宿主表里找 MTLTexture（游戏可能按值拷贝对象，不能存对象指针） | Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
// All game-visible fields fit the actual 16-byte OT2 view object. A 28-byte
// layout from the reference host overwrites the caller's saved FP/LR here.
typedef struct { uint8_t set_levels, set_layers, set_format, set_target; int16_t base_level, nlevels, min_layer, nlayers; uint8_t format,target; uint16_t swizzle:12,depth_mode:1,set_swizzle:1,set_depth_mode:1,reserved:1; } view_t;
FITS(view_t, NVNtextureView);
_Static_assert(sizeof(view_t)==16,"OT2 texture view ABI");

// 采样器参数（builder 与对象共用一份）
// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func; float lod_min, lod_max, lod_bias, aniso, border[4]; } samp_par;   // 枚举值都 < 256 | Enum values are all < 256
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);

typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial：回放层解析结果的表键 | serial: table key for the replay layer's resolved result

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;                            FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]：交换链第 i 张可再次获取的时刻（ns，单调时钟）= 它的显示时刻 + 一个出图周期；pace：本窗口的出图节拍器目标时刻
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]：第 i 张最近一次出图的票号 | ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// 命令缓冲：命令写进游戏给的命令内存（AddCommandMemory），句柄 = 段首地址；用完回调游戏补内存
// Command buffer: commands are written into the command memory given by the game (AddCommandMemory), handle = segment start address; when it runs out, the game is called back for more memory
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur, *end;                  // 当前命令内存段的写指针 / 段尾 | Write pointer / end of the current command memory segment
    uint8_t *begin;                      // 本次录制的 BEGIN 记录 | BEGIN record of this recording
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint8_t* memory_begin;uint64_t control_size;
    void* snapshot_segment;             // host lease; not part of Texture/View ABI
} cmd_t;                                                                                     FITS(cmd_t, NVNcommandBuffer);

// ---------- 状态对象（默认值按 NVN / GL 默认） ----------
// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 每目标 4 位：r=1 g=2 b=4 a=8，置位 = 写 | 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// ---------- 命令记录：{op, size（含头，8 对齐）} + 载荷 ----------
// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
//
// 这些编号是**跨宿主的线上格式**：d2r 的 Vulkan 回放（native/d2r）按同一张表
// 解我们写出的记录。所以 0–48 与上游 d2r-mobile 逐个对齐，本宿主独有的语义
// （FENCE / WAIT_SYNC / DEPTH_BOUNDS / SAMPLE_MASK / RENDER_ENABLE / LINE_WIDTH）
// 一律排在 49 之后——回放遇到它们按无操作跳过，绝不与上游的 op 混号。
// 上游有而本宿主不写的 42–44、46 保留占位，防止将来错位。
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU 可见副作用 | CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX,
    // ---- 以下与上游 d2r-mobile 同号，回放直接消费 ----
    OP_CLEAR_BUFFER, OP_DEPTH_CLAMP, OP_RENDER_COND,                              // 39 40 41
    OP_FENCE_SYNC, OP_TF_BIND, OP_TF_STATE,                                       // 42 43 44 上游写，本宿主不写 | upstream only; this host never records these
    OP_RASTER_DISCARD,                                                            // 45
    OP_VP_SWIZZLE,                                                                // 46 上游写，本宿主不写 | upstream only
    OP_BIND_TEX1, OP_COPY_IMAGE_STRIDE,                                           // 47 48
    // ---- 扩展段：回放按 OP_COUNT_+n 认领（见 xbg_game.cpp 的 switch） ----
    // 回放的 OP_COUNT_ 是 49，所以 +1..+9 就是 50..58；我们只写 +5（深度边界），
    // 载荷与回放的 xbg_depth_bounds 同形。其余留空，别占。
    // The replay's OP_COUNT_ is 49, so its +n slots are 50..58; only +5 (depth
    // bounds) is recorded here, with a payload identical to xbg_depth_bounds.
    OP_EXT_RESERVED_50 = 50, OP_EXT_RESERVED_51, OP_EXT_RESERVED_52, OP_EXT_RESERVED_53,
    OP_DEPTH_BOUNDS,                                                              // 54 = OP_COUNT_+5
    OP_EXT_RESERVED_55, OP_EXT_RESERVED_56, OP_EXT_RESERVED_57, OP_EXT_RESERVED_58,
    // ---- 本宿主独有：回放没有 case，按无操作跳过（见 d2r 白名单） ----
    OP_FENCE, OP_WAIT_SYNC, OP_SAMPLE_MASK, OP_RENDER_ENABLE, OP_LINE_WIDTH,       // 59..63
    OP_COUNT_
};
// 载荷（紧跟 op_hdr）
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4];int32_t is_uint; } p_clear_color;
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, 近, 远（近远由 DEPTH_RANGE 覆盖） | x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
// Conditional render: mode 0 renders when the two counters are equal, mode 1 when
// they differ (NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL / _RENDER_IF_NOT_EQUAL).
// The counters are the `counter` field of the NVNcounterData at addr and addr+16,
// so a title brackets work with two samples-passed reports and asks whether any
// pixel got through in between.
typedef struct { int32_t mode, pad; uint64_t addr; } p_render_cond;
typedef struct { uint32_t enable;float minimum,maximum; } p_depth_bounds;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind：0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
// 分离贴图 / 采样器句柄。defer / src 是「句柄写在客体内存、记录只给地址」的形态：
// 本宿主直接按值写 h（defer = 0），这两个字段放在 h 之后，既不改 h 的偏移，也不进
// 记录长度（RECN 只写 offsetof(h) + 8n 字节，回放读到 0 = 不延后）。
// Separate texture / sampler handles. defer / src are the "handles live in guest
// memory, the record only names the address" form; this host writes the handles
// by value (defer = 0). They sit after h so neither h's offset nor the record
// length changes -- RECN writes offsetof(h) + 8n bytes, so the replay reads 0.
typedef struct { int32_t stage, first, n; uint64_t h[32]; int32_t defer, pad; uint64_t src; } p_bind_handles;
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which：0 值掩码 1 写掩码 2 参考值 | which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
// The trailing indirect / multi-draw fields are what the d2r Vulkan replay reads
// (draw_rec / draw_idx_rec). Our recorder keeps writing the short form -- the
// record stops at `ind` -- because the game sizes its command memory by what the
// real driver would have used, and a longer plain draw would eat into per-mesh
// reservations. The replay zero-fills whatever the record does not carry.
typedef struct { int32_t prim, first, count, base_instance, instances; uint64_t ind, mdi_cnt; int32_t mdi_i, pad; } p_draw;
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf, ind, mdi_cnt; int32_t mdi_i, pad; } p_draw_idx;
typedef struct { int32_t x, y, z; uint64_t caller, ind; } p_dispatch;   // caller：游戏调用点返回地址（定位派发者） | caller: return address of the game call site (to locate the dispatcher)
// A single combined texture+sampler handle as the game gives it (texture id |
// sampler id << 20), with the numbering bases of the descriptor pools bound to
// the command buffer. bind_rec() widens it to a p_bind_handles record. We do not
// record this form today; the replay supports it so a title that patches its own
// command memory in place cannot be misread as a huge count.
typedef struct { int32_t stage, slot; uint64_t h; int32_t tbase, sbase; } p_bind_tex1;
static inline const p_bind_handles *bind_rec(uint32_t op, const void *a, p_bind_handles *t) {
    if (op != OP_BIND_TEX1) return (const p_bind_handles *)a;
    const p_bind_tex1 *b = (const p_bind_tex1 *)a; uint64_t tid = b->h & 0xfffff, sid = (uint32_t)b->h >> 20;
    t->stage = b->stage; t->first = b->slot; t->n = 1; t->h[0] = (tid ? tid + (uint64_t)b->tbase : 0) | (sid ? sid + (uint64_t)b->sbase : 0) << 20; return t;
}
// 记录可能只写了前段（本宿主就是如此）：缺的字段按 0 补，绝不越界读。
// A record may stop before the trailing fields (our host always does): the
// missing ones read as 0, never past the end of the record.
static inline const p_draw *draw_rec(const void *a, uint32_t size, p_draw *t) { if (size - 8 >= sizeof *t) return (const p_draw *)a; memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
static inline const p_draw_idx *draw_idx_rec(const void *a, uint32_t size, p_draw_idx *t) { if (size - 8 >= sizeof *t) return (const p_draw_idx *)a; memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // T2B 复用：src 存目标缓冲地址 | Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; } p_copy_stride;
typedef struct { float rgba[4]; } p_blend_color;

// 初始化序号：贴图 / 程序对象每次 Initialize 取一个新号，回放层按它把对象
// 认到宿主表里（游戏可能按值拷贝对象，不能存对象指针）。实现见 frontend.c，
// 声明见 backend.h（extern "C"，d2r_host.c 也用它）。
// Initialization serial: a texture or program object takes a new number on every
// Initialize and the replay layer keys its host tables by it (the game may copy
// the object by value, so object pointers cannot be stored). Implemented in
// frontend.c, declared in backend.h with C linkage (d2r_host.c uses it too).
// 合并句柄记录（OP_BIND_TEX1）由游戏自己就地改写命令内存时才出现，本宿主不写。
// The combined-handle record (OP_BIND_TEX1) only appears when a game rewrites its
// own command memory in place; this host never writes one.
