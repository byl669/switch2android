// SPDX-License-Identifier: MIT
// Adapted from d2r-mobile, Copyright (c) 2026 jhaizhou.
// See ../third_party/d2r-mobile-LICENSE. Android Vulkan adaptation.
#include "../thread_services.h"
#include "nvn_protos.h"
#include "nvn_obj.h"
#include "backend.h"
#include "nvn_commands.h"
#include "nvn_program.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#define LOG ot2_host_log
#define die ot2_host_die
#define OLOG ot2_host_log
#define B(x) ((x)&0xff)
#define OT2_POOL_HOST_BACKED 0x80000000u
typedef struct {const char* name;void* fn;} NativeImplementation;
#ifdef __APPLE__
#define NVN_SECTION "__DATA,ot2_nvn"
#define NVN_RETAIN
extern const NativeImplementation __start_ot2_nvn[] __asm("section$start$__DATA$ot2_nvn");
extern const NativeImplementation __stop_ot2_nvn[] __asm("section$end$__DATA$ot2_nvn");
#else
#define NVN_SECTION "ot2_nvn"
// used preserves emission; retain also protects the registration table from
// ELF section garbage collection in Android release links.
#define NVN_RETAIN __attribute__((retain))
extern const NativeImplementation __start_ot2_nvn[],__stop_ot2_nvn[];
#endif
#define NVN(name,ret,...) \
    static ret h_##name(__VA_ARGS__); \
    static const NVNT_##name check_##name __attribute__((unused))=h_##name; \
    NVN_RETAIN __attribute__((used,section(NVN_SECTION),aligned(8))) static const NativeImplementation entry_##name={#name,(void*)h_##name}; \
    static ret h_##name(__VA_ARGS__)
#define NVN_NOPROTO(name,ret,...) \
    static ret h_##name(__VA_ARGS__); \
    NVN_RETAIN __attribute__((used,section(NVN_SECTION),aligned(8))) static const NativeImplementation entry_##name={#name,(void*)h_##name}; \
    static ret h_##name(__VA_ARGS__)
void* ot2_nvn_lookup(const char* name) {
    for(const NativeImplementation* p=__start_ot2_nvn;p<__stop_ot2_nvn;p++)if(!strcmp(name,p->name))return ot2_native_gate(name,p->fn);
    return ot2_native_gate(name,NULL);
}
#define nvn_lookup ot2_nvn_lookup
uint64_t nvn_new_serial(void){static _Atomic uint64_t serial;return ++serial;}
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// Guest ABI/allocation conventions and compatibility ceilings originate from
// D2R's Tegra table. Optional features reflect this host's actual replay paths;
// hardware-dependent ceilings are reduced by vkhost_device_limit below.
// API/code versions describe the guest translation contract, not Vulkan.
static const int g_devinfo[] = {
    [0x00] = 53, [0x01] = 311,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 0, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 1, [0x27] = 4096, [0x28] = 0, [0x29] = 0, [0x2A] = 1,
    [0x2B] = 0, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 0, [0x33] = 8,
    [0x34] = 0, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 0, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 0, [0x43] = 0, [0x44] = 0,
    [0x45] = 0, [0x46] = 5, [0x47] = 0, [0x48] = 0, [0x49] = 0, [0x4A] = 0, [0x4B] = 1, [0x4C] = 0, [0x4D] = 0,
    [0x4E] = 1024, [0x4F] = 0, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 0, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 128, [0x60] = 0, [0x61] = 1,
};
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { LOG("【红】nvnDeviceGetInteger 未知项 %#x，给 0", w); *out = 0; return; }
    *out = vkhost_device_limit(what,g_devinfo[w]);
    // No per-call log. A title polls device properties for capabilities it checks
    // every frame, and this call ran tens of thousands of times in a single run;
    // logging each one wrote megabytes and, with synchronous file writes, held the
    // frame loop back. Unknown properties are still reported above, which is the
    // only case that needs attention.
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
NVN(nvnDeviceGetTextureHandle,NVNtextureHandle,const NVNdevice* d,int texture,int sampler) {(void)d;return (uint64_t)texture|((uint64_t)sampler<<20);}
// The texel-fetch handle addresses a texture without a sampler, so it carries the texture index
// alone - the same encoding as nvnDeviceGetTextureHandle with the sampler field left zero, and the
// same shape as nvnDeviceGetImageHandle below. The title stores the value in a constant buffer and
// its shaders resolve it through the texture pool, which is how the engine's cbuf texture path
// (XBG_SOURCE_TEXTURE_CBUF) reads it back.
NVN(nvnDeviceGetTexelFetchHandle,NVNtextureHandle,const NVNdevice* d,int texture) {(void)d;return (uint64_t)texture;}
// 分离贴图 / 采样器句柄：值 = 描述符号（采样器号放 20 位以上，与组合句柄的位布局一致）
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; LOG("[nvn] 窗口原点 %s", m ? "左上（同 D3D / Metal）" : "左下（GL）"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; LOG("[nvn] 深度模式 %s", m ? "近=0（同 D3D / Metal）" : "近=-w（GL）"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }
NVN(nvnDeviceGetTimestampInNanoseconds,uint64_t,const NVNdevice* d,const NVNcounterData* counter) {(void)d;return counter->timestamp;}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds,uint64_t,const NVNdevice* d) {(void)d;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
// Optional Tegra fast-clear registration. Normal Vulkan clear commands remain
// authoritative, so report that this hardware-specific optimization is absent.
NVN(nvnDeviceRegisterFastClearColor,NVNboolean,NVNdevice* d,const float* color,NVNformat format) {(void)d;(void)color;(void)format;return 0;}
NVN(nvnDeviceRegisterFastClearColori,NVNboolean,NVNdevice* d,const int* color,NVNformat format) {(void)d;(void)color;(void)format;return 0;}
NVN(nvnDeviceRegisterFastClearColorui,NVNboolean,NVNdevice* d,const uint32_t* color,NVNformat format) {(void)d;(void)color;(void)format;return 0;}
NVN(nvnDeviceRegisterFastClearDepth,NVNboolean,NVNdevice* d,float depth) {(void)d;(void)depth;return 0;}

// ---------- 内存池 ----------
// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size;
    if (o->mem) { vkhost_pool_add(o->mem, o->size);  }
    if (!o->mem && !(o->flags & 0x200)) LOG("【红】MemoryPoolInitialize 非虚拟池没有存储 flags=%#x size=%#llx", o->flags, o->size);
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    pool_t *o = (pool_t *)p;
    if (o->mem) {
        nvn_commands_pool_release(o->mem, o->size);
        vkhost_pool_del(o->mem);
        // A virtual pool was backed by a mapping this host owns; guest-supplied
        // storage belongs to the title and must not be unmapped here.
        if (o->flags & OT2_POOL_HOST_BACKED) munmap(o->mem, o->size);
    }
    o->magic = 0;
}
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (NVNbufferAddress)((const pool_t *)p)->mem; }
NVN(nvnMemoryPoolGetFlags,NVNmemoryPoolFlags,const NVNmemoryPool* p) {return ((const pool_t*)p)->flags;}
// Sparse pools are mapped in one step on NVN. A virtual pool starts with no
// storage, and until this call backs it every buffer and texture carved out of
// it reports address 0 — which the title then uses as a shader code pointer.
// Allocate real pages here so those addresses mean something.
NVN_NOPROTO(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p) {
    pool_t *o = (pool_t *)p;
    if (o->mem) return 1;
    if (!o->size) return 1;
    void *mem = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOG("【红】MemoryPoolMapVirtual 分配失败 size=%#llx", (unsigned long long)o->size);
        return 0;
    }
    o->mem = mem;
    o->flags |= OT2_POOL_HOST_BACKED;
    vkhost_pool_add(o->mem, o->size);
    LOG("MemoryPoolMapVirtual %p -> %p size=%#llx", (void *)p, mem, (unsigned long long)o->size);
    return 1;
}
NVN(nvnMemoryPoolGetSize,size_t,const NVNmemoryPool* p) {return ((const pool_t*)p)->size;}
NVN(nvnMemoryPoolMap,void*,const NVNmemoryPool* p) {return ((const pool_t*)p)->mem;}
NVN(nvnMemoryPoolInvalidateMappedRange,void,const NVNmemoryPool* p,ptrdiff_t offset,size_t size) {(void)p;(void)offset;(void)size;}

// ---------- 缓冲 ----------
// ---------- Buffers ----------
// A buffer is a window into a memory pool. The guest never sees a host handle:
// nvnBufferMap and nvnBufferGetAddress must return the address inside the pool
// that the title already allocated, or the caller dereferences a null pointer.
typedef struct { const pool_t *pool; int64_t off; uint64_t size; NVNdevice *dev; } buf_builder_t;
typedef struct { uint32_t magic; const pool_t *pool; int64_t off; uint64_t size; } buf_t;
_Static_assert(sizeof(buf_builder_t) <= sizeof(NVNbufferBuilder), "buffer builder must fit");
_Static_assert(sizeof(buf_t) <= sizeof(NVNbuffer), "buffer must fit");
#define BB(b) ((buf_builder_t *)(b))
#define BU(o) ((buf_t *)(o))
static void *buf_data(const buf_t *o) {
    return (o && o->pool && o->pool->mem) ? o->pool->mem + o->off : NULL;
}
NVN_NOPROTO(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) {
    memset(b, 0, sizeof(buf_builder_t));
}
NVN_NOPROTO(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN_NOPROTO(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t size) {
    BB(b)->pool = (const pool_t *)p; BB(b)->off = off; BB(b)->size = size;
}
NVN_NOPROTO(nvnBufferInitialize, NVNboolean, NVNbuffer *o, const NVNbufferBuilder *b) {
    const buf_builder_t *s = BB(b); buf_t *d = BU(o);
    memset(o, 0, sizeof(buf_t));
    d->magic = MAGIC('B', 'U', 'F', 0); d->pool = s->pool; d->off = s->off; d->size = s->size;
    if (!d->pool || !d->pool->mem) {
        // Without backing storage every later Map returns null; say so once
        // instead of letting the title fault somewhere unrelated.
        LOG("【红】BufferInitialize 没有后备存储 pool=%p off=%lld size=%llu",
            (void *)d->pool, (long long)d->off, (unsigned long long)d->size);
        return 0;
    }
    return 1;
}
NVN_NOPROTO(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *o) {
    return (NVNbufferAddress)buf_data(BU(o));
}
NVN_NOPROTO(nvnBufferMap, void *, const NVNbuffer *o) { return buf_data(BU(o)); }
NVN_NOPROTO(nvnBufferFlushMappedRange, void, const NVNbuffer *o, ptrdiff_t off, size_t n) { (void)o; (void)off; (void)n; }
NVN_NOPROTO(nvnBufferInvalidateMappedRange, void, const NVNbuffer *o, ptrdiff_t off, size_t n) { (void)o; (void)off; (void)n; }
NVN_NOPROTO(nvnBufferGetSize, size_t, const NVNbuffer *o) { return BU(o)->size; }
NVN_NOPROTO(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *o) { return BU(o)->off; }
NVN_NOPROTO(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *o) { return (NVNmemoryPool *)BU(o)->pool; }
NVN_NOPROTO(nvnBufferFinalize, void, NVNbuffer *o) { BU(o)->magic = 0; }

// ---------- 描述符池 ----------
// ---------- Descriptor pools ----------
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n); vkhost_set_tex_pool_n(n); return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { vkhost_descriptor_pool_del((const dpool_t*)tp,0); }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) { vkhost_reg_tex((const dpool_t*)tp,id, (const tex_t *)t, (const view_t *)v); }
NVN(nvnTexturePoolRegisterImage,void,const NVNtexturePool* pool,int id,const NVNtexture* t,const NVNtextureView* v) {vkhost_reg_tex((const dpool_t*)pool,id,(const tex_t*)t,(const view_t*)v);}
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { vkhost_descriptor_pool_del((const dpool_t*)sp,1); }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { vkhost_reg_samp((const dpool_t*)sp,id, &((const samp_t *)s)->p); }

// ---------- 贴图 ----------
// ---------- Textures ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
    for(int i=0;i<4;i++)TB(b)->d.swizzle[i]=2+i;
}
NVN(nvnTextureBuilderSetSwizzle,void,NVNtextureBuilder* b,NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle blue,NVNtextureSwizzle a) {
    uint8_t v[]={r,g,blue,a};memcpy(TB(b)->d.swizzle,v,sizeof(v));
}
NVN(nvnTextureBuilderSetDepthStencilMode,void,NVNtextureBuilder* b,NVNtextureDepthStencilMode mode) {TB(b)->d.depth_stencil_mode=mode;}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize2D,void,NVNtextureBuilder* b,int w,int h) {TB(b)->d.width=w;TB(b)->d.height=h;TB(b)->d.depth=1;}
NVN(nvnTextureBuilderSetSize3D,void,NVNtextureBuilder* b,int w,int h,int d) {TB(b)->d.width=w;TB(b)->d.height=h;TB(b)->d.depth=d;}
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] 预排布贴图 %ux%ux%u 级%u 格式%#x 目标%d 布局字节 %02x%02x%02x%02x%02x%02x%02x%02x → 块高 log2=%u（取布局第 0 字节）尺寸 %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("【红】TextureBuilderGetStorageSize 未知格式 %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
// NVN_STORAGE_CLASS_TEXTURE. Every builder this host accepts ends up in a memory
// pool and sparse tiling is not implemented, so the answer is never NONE and
// never SPARSE. A caller that gets NONE concludes the builder is unconfigured.
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) {
    (void)b; return 2 /* NVN_STORAGE_CLASS_TEXTURE */;
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) {
    (void)t; return 2 /* NVN_STORAGE_CLASS_TEXTURE */;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial();
    vkhost_tex_created(o);
    if(o->serial<20 || o->serial%1000==0)OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { vkhost_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
// GetWidth is defined below, together with the other atlas geometry probes.
// A 4096x6144 glyph atlas holds about 96 rows of ~128 glyphs, and this title stops filling
// one after roughly two rows and then starts overwriting: the characters it needs later come
// back empty. That means its idea of how much room the atlas has is wrong, and the only three
// answers it can be deriving that from are height, stride and storage size. Log what it asks
// for and what it gets, once per kind, and the arithmetic that goes wrong becomes visible.
#define ATLAS_PROBE(fn,val) do{const tex_t*c=CTX(t);if(c->d.height>=4096){static unsigned n=0;if(n++<40)LOG("【图集查询】%s serial=%llu w=%d h=%d fmt=%#x stride=%lld size=%lld levels=%d -> %lld",fn,(unsigned long long)c->serial,c->d.width,c->d.height,c->d.format,(long long)c->d.stride,(long long)c->L.size,c->d.levels,(long long)(val));}}while(0)
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { ATLAS_PROBE("GetWidth",(int)CTX(t)->d.width); return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { ATLAS_PROBE("GetHeight",(int)CTX(t)->d.height); return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetLevels,int,const NVNtexture* t) {return CTX(t)->d.levels;}
NVN(nvnTextureGetTarget,NVNtextureTarget,const NVNtexture* t) {return CTX(t)->d.target;}
NVN(nvnTextureGetFormat,NVNformat,const NVNtexture* t) {return CTX(t)->d.format;}
NVN(nvnTextureGetSamples,int,const NVNtexture* t) {return CTX(t)->d.samples;}
NVN(nvnTextureGetStride,ptrdiff_t,const NVNtexture* t) { ATLAS_PROBE("GetStride",CTX(t)->d.stride); return CTX(t)->d.stride; }
// The reconstructed header incorrectly declares this opaque object by value.
NVN_NOPROTO(nvnTextureGetMemoryPool,NVNmemoryPool*,const NVNtexture* t) {return (NVNmemoryPool*)CTX(t)->pool;}
NVN(nvnTextureGetMemoryOffset,ptrdiff_t,const NVNtexture* t) {return CTX(t)->off;}
NVN(nvnTextureGetTextureAddress,NVNtextureAddress,const NVNtexture* t) {return (uint64_t)CTX(t)->mem;}
NVN(nvnTextureGetDepthStencilMode,NVNtextureDepthStencilMode,const NVNtexture* t) {return CTX(t)->d.depth_stencil_mode;}
NVN(nvnTextureGetSwizzle,void,const NVNtexture* t,NVNtextureSwizzle* r,NVNtextureSwizzle* g,NVNtextureSwizzle* b,NVNtextureSwizzle* a) {
    *r=CTX(t)->d.swizzle[0];*g=CTX(t)->d.swizzle[1];*b=CTX(t)->d.swizzle[2];*a=CTX(t)->d.swizzle[3];
}
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { ATLAS_PROBE("GetStorageSize",(int)CTX(t)->L.size); return (int)CTX(t)->L.size; }
// Hierarchical depth metadata is allocated internally by the Vulkan driver.
NVN(nvnTextureGetZCullStorageSize,size_t,const NVNtexture* t) {(void)t;return 0;}
NVN(nvnTextureBuilderGetZCullStorageSize,size_t,const NVNtextureBuilder* b) {(void)b;return 0;}
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem;
}
// 头文件无原型；main 0x174899c 一类调用点把返回值与 1 比，不等则置某状态位 0x100。含义未核，先给 1
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewSetSwizzle,void,NVNtextureView* v,NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle b,NVNtextureSwizzle a) {
    VW(v)->swizzle=r|(g<<3)|(b<<6)|(a<<9);VW(v)->set_swizzle=1;
}
NVN(nvnTextureViewSetDepthStencilMode,void,NVNtextureView* v,NVNtextureDepthStencilMode mode){VW(v)->depth_mode=mode;VW(v)->set_depth_mode=1;}
NVN(nvnTextureViewGetLevels,NVNboolean,const NVNtextureView* v,int* base,int* n){const view_t* o=(const view_t*)v;*base=o->base_level;*n=o->nlevels;return o->set_levels;}
NVN(nvnTextureViewGetFormat,NVNboolean,const NVNtextureView* v,NVNformat* f){const view_t* o=(const view_t*)v;*f=o->format;return o->set_format;}
NVN(nvnTextureViewGetTarget,NVNboolean,const NVNtextureView* v,NVNtextureTarget* t){const view_t* o=(const view_t*)v;*t=o->target;return o->set_target;}
NVN(nvnTextureViewGetDepthStencilMode,NVNboolean,const NVNtextureView* v,NVNtextureDepthStencilMode* mode){const view_t* o=(const view_t*)v;*mode=o->depth_mode;return o->set_depth_mode;}
NVN(nvnTextureViewGetSwizzle,NVNboolean,const NVNtextureView* v,NVNtextureSwizzle* r,NVNtextureSwizzle* g,NVNtextureSwizzle* b,NVNtextureSwizzle* a){const view_t* o=(const view_t*)v;*r=o->swizzle&7;*g=(o->swizzle>>3)&7;*b=(o->swizzle>>6)&7;*a=(o->swizzle>>9)&7;return o->set_swizzle;}
NVN(nvnTextureViewCompare,NVNboolean,const NVNtextureView* a,const NVNtextureView* b){return !memcmp(a,b,sizeof(view_t));}
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}
// CPU 读贴图（NVN ReadTexels）：预排布贴图以池内存为真值（解排布直读），GPU 写出的贴图从 MTLTexture 读回（nvn_mtl.m）
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back from the MTLTexture (nvn_mtl.m)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    vkhost_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureWriteTexels,void,const NVNtexture* t,const NVNtextureView* v,const NVNcopyRegion* r,const void* source){
    vkhost_write_texels(CTX(t),(const view_t*)v,r,source,0,0);
}
NVN(nvnTextureWriteTexelsStrided,void,const NVNtexture* t,const NVNtextureView* v,const NVNcopyRegion* r,const void* source,ptrdiff_t row,ptrdiff_t image){
    vkhost_write_texels(CTX(t),(const view_t*)v,r,source,row,image);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }
// Texel writes and reads go straight to the texture's backing memory on this
// host: there is no separate cached copy to flush back or pull from, so both
// directions are already coherent and the calls are no-ops.
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- 采样器 ----------
// ---------- Samplers ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL 默认 | GL defaults
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
// Reduction filter applies to depth comparison samplers; the host samples at
// full rate, which is the NVN default and what a caller asking for WEIGHTED_AVERAGE
// still accepts.
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { (void)b; (void)r; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; (void)v; }   // Metal 无对应，忽略 | No Metal equivalent, ignored
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- 程序 ----------
// ---------- Programs ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); ((prog_t*)p)->serial=nvn_new_serial(); nvn_program_publish((prog_t*)p); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { vkhost_prog_release((prog_t *)p); nvn_program_release(((prog_t*)p)->serial); ((prog_t *)p)->magic = 0; }
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShaders 阶段数 %d", n); return 0; }
    NVNshaderData descriptors[6];if(n)memcpy(descriptors,sd,n*sizeof *sd);
    uint64_t previous=o->serial;vkhost_prog_release(o);o->nstage = n; if(n)memcpy(o->sd, descriptors, n * sizeof *sd); o->serial = nvn_new_serial();
    nvn_program_publish(o);nvn_program_release(previous);
    vkhost_program(o);

    return 1;
}
// The extended form's stages are 0x18 bytes each (the pair above plus one
// 8-byte extension word). Forwarding the caller's array as NVNshaderData read
// every stage after the first at the wrong offset, which made fragment shaders
// arrive with a control section that could never validate. Convert stage by
// stage; the extension word itself is not modelled.
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const NVNshaderDataExt *sd) {
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShadersExt 阶段数 %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) {
        plain[i].data = sd[i].data;
        plain[i].control = sd[i].control;
        static bool warned;
        if (sd[i].extension && !warned) { warned = true; LOG("ProgramSetShadersExt 扩展字段非零 %#llx（host 不使用）", (unsigned long long)sd[i].extension); }
    }
    return h_nvnProgramSetShaders(p, n, plain);
}

NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
// 围栏 = 「此前提交到队列的命令全部完成」：记下最近一次提交的栅栏值
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; __atomic_store_n(&((sync_t*)s)->fence,vkhost_last_fence(),__ATOMIC_RELEASE);__atomic_store_n(&((sync_t*)s)->fenced,1,__ATOMIC_RELEASE); }
NVN(nvnQueueWaitSync,NVNboolean,NVNqueue* q,const NVNsync* sync) {
    (void)q;const sync_t* s=(const sync_t*)sync;return __atomic_load_n(&s->fenced,__ATOMIC_ACQUIRE) && vkhost_wait(__atomic_load_n(&s->fence,__ATOMIC_ACQUIRE),UINT64_MAX);
}
// 没围栏过的同步对象按真机行为等到超时；已围栏的只在 GPU 真正完成到该栅栏值后放行
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    const sync_t *o = (const sync_t *)s;
    if (!__atomic_load_n(&o->fenced,__ATOMIC_ACQUIRE)) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    uint64_t fence=__atomic_load_n(&o->fence,__ATOMIC_ACQUIRE);
    if (vkhost_wait(fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return vkhost_wait(fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("【红】EventInitialize 没有存储"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v; else *e->val = v; }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- 队列 ----------
// ---------- Queue ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // 设备默认值 | Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// 队列内存：命令 + 计算 + 控制三块之和按 4 KiB 取整（宿主不使用，只需游戏按此大小分配）
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), ((const queue_builder_t *)b)->dev, 0};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { ((queue_t *)q)->magic = 0; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; vkhost_finish(); }

// ---------- 命令缓冲：录进游戏的命令内存，提交时执行 CPU 可见副作用 ----------
// ---------- Command buffer: record into the game's command memory, perform CPU-visible side effects at submit ----------
#define CB(c) ((cmd_t *)(c))
#define OP_RESERVE 24                             // 段尾永远留够一条 JUMP / END | Always keep enough room at the segment end for one JUMP / END

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { nvn_commands_finalize(CB(c)); CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
static void put_hdr(uint8_t *p,uint32_t op,uint32_t size) {op_hdr h={op,size};memcpy(p,&h,8);}
static void cmd_new_segment(cmd_t *o,uint8_t *mem,size_t n) {
    uint8_t* start=align8(mem);
    if(o->recording && o->cur){put_hdr(o->cur,OP_JUMP,16);memcpy(o->cur+8,&start,8);}
    o->cur=start;o->end=mem+n;o->memory_begin=start;
}
NVN(nvnCommandBufferAddCommandMemory,void,NVNcommandBuffer* c,const NVNmemoryPool* p,ptrdiff_t off,size_t n) {
    const pool_t* pl=(const pool_t*)p;
    if(!pl || !pl->mem || off<0 || n<64 || (uint64_t)off+n>pl->size)die("Invalid command memory grant");
    // How much the title actually hands over decides whether a frame costs one
    // grant or a thousand; keep the number visible without flooding the log.
    static long grants,total;
    static long extends_count;
    ++grants;total+=(long)n;
    nvn_commands_grant(CB(c),pl->mem+off,n);
    // A title that tops up its command memory polls nvnCommandBufferGetCommandMemoryFree
    // and adds until there is room. Restarting at the new segment on every add throws
    // away the partially filled segment, so free space never grows, the title adds
    // forever -- measured at ~10k guest re-entries per frame, which starves the frame
    // loop -- and the tail of every recording is silently dropped, leaving the submitted
    // command stream shorter than the title believes. When the new range continues
    // exactly where the current segment ends, extend that segment instead.
    cmd_t* o=CB(c);
    uint8_t* before=o->end;
    if(before && pl->mem+off==before) {o->end=before+n; ++extends_count;}
    else cmd_new_segment(o,pl->mem+off,n);
    if(grants<=24||grants%512==0)
        LOG("[nvn] AddCommandMemory cb=%p magic=%x rec=%u cur=%p before=%p mem=%p extends=%ld grants=%ld",(void*)c,o->magic,o->recording,(void*)o->cur,(void*)before,(void*)(pl->mem+off),extends_count,grants);
}
NVN(nvnCommandBufferAddControlMemory,void,NVNcommandBuffer* c,void* memory,size_t n) {(void)memory;CB(c)->control_size=n;}
NVN(nvnCommandBufferGetCommandMemorySize,size_t,const NVNcommandBuffer* c) {const cmd_t* o=(const cmd_t*)c;return o->end-o->memory_begin;}
NVN(nvnCommandBufferGetCommandMemoryUsed,size_t,const NVNcommandBuffer* c) {const cmd_t* o=(const cmd_t*)c;return o->cur-o->memory_begin;}
NVN(nvnCommandBufferGetCommandMemoryFree,size_t,const NVNcommandBuffer* c) {const cmd_t* o=(const cmd_t*)c;return o->end-o->cur;}
NVN(nvnCommandBufferGetControlMemorySize,size_t,const NVNcommandBuffer* c) {return ((const cmd_t*)c)->control_size;}
NVN(nvnCommandBufferGetControlMemoryUsed,size_t,const NVNcommandBuffer* c) {(void)c;return 0;}
NVN(nvnCommandBufferGetControlMemoryFree,size_t,const NVNcommandBuffer* c) {return ((const cmd_t*)c)->control_size;}
// A title's memory callback is told the *minimum* size it must add, so asking
// for exactly one more command makes a title hand back a segment that holds one
// command: the next allocation asks again, and a frame that records a few
// hundred commands becomes a few hundred grants -- each one a re-entry into
// guest code, a segment in the pin map, and a slice of the title's own command
// pool. Ask for a working chunk instead; the second attempt falls back to the
// exact size, so a title that really only has room for one command behaves
// exactly as before.
#define CMD_MIN_GRANT (64u*1024u)
static uint8_t* cmd_alloc(cmd_t* o,uint32_t size) {
    size=(size+7)&~7u;
    for(int tries=0;!o->cur || o->cur+size+OP_RESERVE>o->end;tries++) {
        // Asking the title for more command memory re-enters guest code while a
        // host NVN entry is on the stack. Say so: a hang here looks exactly like
        // a hang in whatever the title was recording. Rate-limited: a title that
        // starves once per command would otherwise bury every other log line.
        static long starved;
        ++starved;
        if(starved<=8||starved%256==0)
            LOG("[nvn] cmd_alloc needs memory total=%ld size=%u tries=%d cur=%p end=%p cb=%p",starved,size,tries,(void*)o->cur,(void*)o->end,(void*)o->cb);
        if(!o->cb || tries==2)die("Command memory exhausted");
        const uint64_t wanted=size+OP_RESERVE;
        const uint64_t request=(tries||wanted>=CMD_MIN_GRANT)?wanted:CMD_MIN_GRANT;
        const uint64_t args[]={(uint64_t)o,NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,request,(uint64_t)o->cb_data,0,0,0,0};
        ot2_call_guest_address((uint64_t)o->cb,args);
    }
    uint8_t* p=o->cur;o->cur+=size;put_hdr(p,0,size);return p;
}
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("【红】BeginRecording 重入");
    o->recording = 1;
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_BEGIN, 8); o->begin = p;
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_END, 8);
    o->recording = 0;
    { static unsigned n; if(n++<8) LOG("[nvn] EndRecording #%u bytes=%llu", n, (unsigned long long)(o->cur-o->memory_begin)); }
    return (NVNcommandHandle)o->begin;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    for (int i = 0; i < n; i++) { uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_CALL, 16); memcpy(p + 8, &h[i], 8); }
}
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *handles) {
    // The caller may reuse the source command memory after this call. Copy
    // records now, following allocation-segment jumps but retaining explicit
    // CallCommands references. A call record here would borrow freed memory.
    for(int i=0;i<n;i++) {
        const uint8_t* source=(const uint8_t*)handles[i];int ended=0;
        for(unsigned count=0;count<1000000;count++) {
            op_hdr h;memcpy(&h,source,8);
            if(h.size<8 || h.size%8 || h.size>16*1024*1024)die("Invalid source command during copy");
            if(h.op==OP_END){ended=1;break;}
            if(h.op==OP_JUMP){memcpy(&source,source+8,8);continue;}
            if(h.op!=OP_BEGIN){uint8_t* destination=cmd_alloc(CB(c),h.size);memcpy(destination,source,h.size);nvn_commands_copy_record(CB(c),(op_hdr*)destination);}
            source+=h.size;
        }
        if(!ended)die("Source command copy has no end");
    }
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform 更新：数据在录制时拷进命令内存（NVN 语义：录制后调用方可改原数据），提交时写进缓冲
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- 渲染命令：参数按 nvn_obj.h 的载荷格式录进命令内存，回放见 nvn_mtl.m ----------
// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h; for replay see nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
// The d2r replay layer appends a finished record it already holds (see
// native/d2r/d2r_host.c); it needs the same reservation the recorder uses,
// including the growth callback into the game's own command memory.
uint8_t* nvn_command_alloc(cmd_t* o,uint32_t size) {return cmd_alloc(o,size);}

#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// 带数组的绑定记录按实际个数分配：前缀字段 + n 个元素（回放端只读前 n 个）。原先按最大个数定长记（绑顶点缓冲 272 字节、绑贴图 / 采样器 280 字节，
// 实际常只有 1–2 个），空尾占每帧命令字节的四到五成；88 级法师的高级属性页每帧录 12.5 MB，把游戏自己的命令内存池（约每帧 105 段 × 128 KB）用空
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the replay side reads only the first n). Previously they were recorded at fixed length for the maximum count (bind vertex buffers 272 bytes, bind textures / samplers 280 bytes,
// while there are usually only 1–2 in practice), and the empty tail took 40–50% of the command bytes per frame; the advanced stats page of the level 88 sorceress recorded 12.5 MB per frame, using up the game's own command memory pool (about 105 segments x 128 KB per frame)
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("【红】SetRenderTargets 数量 %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC(c, OP_SET_RT, p_set_rt); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? nvn_command_texture(CB(c),(const tex_t *)ct[i]) : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = nvn_command_texture(CB(c),(const tex_t *)dt); copy_view(&r->dview, &r->has_dview, dv);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask;r->is_uint=0; memcpy(r->rgba, rgba, 16);
}
NVN(nvnCommandBufferClearColorui,void,NVNcommandBuffer* c,int index,const uint32_t* rgba,int mask){p_clear_color* r=REC(c,OP_CLEAR_COLOR,p_clear_color);r->index=index;r->mask=mask;r->is_uint=1;memcpy(r->rgba,rgba,16);}
NVN(nvnCommandBufferClearColori,void,NVNcommandBuffer* c,int index,const int* rgba,int mask){p_clear_color* r=REC(c,OP_CLEAR_COLOR,p_clear_color);r->index=index;r->mask=mask;r->is_uint=2;memcpy(r->rgba,rgba,16);}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("【红】SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetViewport,void,NVNcommandBuffer* c,int x,int y,int width,int height) {
    p_viewports* v=REC(c,OP_VIEWPORTS,p_viewports);v->first=0;v->n=1;v->v[0][0]=x;v->v[0][1]=y;v->v[0][2]=width;v->v[0][3]=height;v->v[0][5]=1;
}
// Debug labels name objects for a host-side debugger. They carry no rendering
// state and nothing in the pipeline reads them, so implementing them as no-ops is
// the whole of their behaviour here.
//
// They are worth defining even so: a title that labels every buffer and sampler it
// creates calls these tens of thousands of times, and while unimplemented each call
// went through the generic missing-API path and wrote a log line. That log grew to
// megabytes and, with the file written synchronously, took enough time per frame to
// hold the frame loop back.
NVN_NOPROTO(nvnBufferSetDebugLabel, void, NVNbuffer *b, const char *label) { (void)b; (void)label; }
NVN_NOPROTO(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *label) { (void)s; (void)label; }
NVN_NOPROTO(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *label) { (void)p; (void)label; }
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
// The plural form sets a range of scissor rectangles. Every graphics pipeline in
// this backend carries a single dynamic scissor (see command_state.inc), so a
// range wider than one rectangle cannot be expressed exactly: Vulkan would need a
// scissor array and the same count in the pipeline state.
//
// Rather than dropping the call, the union of the requested rectangles is used.
// That errs towards drawing too much instead of clipping content away, which
// keeps the picture complete and only costs overdraw in the regions a title meant
// to cut. The first call that asks for more than one rectangle is reported so the
// real distribution is known instead of assumed.
NVN_NOPROTO(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int num, const NVNrectangle *rects) {
    if (!rects || num <= 0) return;
    static unsigned multi;
    if (num > 1 && !__atomic_fetch_add(&multi, 1u, __ATOMIC_RELAXED))
        LOG("SetScissors: %d rectangles from index %d; using their union (single-scissor backend)", num, first);
    int x0 = rects[0].x, y0 = rects[0].y;
    int x1 = rects[0].x + rects[0].width, y1 = rects[0].y + rects[0].height;
    for (int i = 1; i < num; i++) {
        if (rects[i].x < x0) x0 = rects[i].x;
        if (rects[i].y < y0) y0 = rects[i].y;
        if (rects[i].x + rects[i].width > x1) x1 = rects[i].x + rects[i].width;
        if (rects[i].y + rects[i].height > y1) y1 = rects[i].y + rects[i].height;
    }
    *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x0, y0, x1 - x0, y1 - y0};
}
// The plural depth-range form: ranges holds num pairs of (near, far). Vulkan has
// one depth range for the whole pipeline, so the first pair is the one a
// single-viewport render pass can honour; a wider range is reported once.
NVN_NOPROTO(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int num, const float *ranges) {
    if (!ranges || num <= 0) return;
    static unsigned multi;
    if (num > 1 && !__atomic_fetch_add(&multi, 1u, __ATOMIC_RELAXED))
        LOG("SetDepthRanges: %d ranges from index %d; using the first (one depth range per pipeline)", num, first);
    *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){ranges[0], ranges[1]};
}
NVN(nvnCommandBufferSetDepthBounds,void,NVNcommandBuffer* c,uint32_t enable,float minimum,float maximum) { *REC(c,OP_DEPTH_BOUNDS,p_depth_bounds)=(p_depth_bounds){B(enable),minimum,maximum}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){nvn_command_program(CB(c),(const prog_t *)p), stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("【红】BindVertexAttribState n=%d", n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("【红】BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("【红】BindVertexBuffers first=%d n=%d", first, n); return; }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindVertexBuffer,void,NVNcommandBuffer* c,int index,NVNbufferAddress address,size_t size) {
    if(index<0 || index>=16)die("Invalid vertex buffer slot");
    p_bind_vbuf* r=RECN(c,OP_BIND_VBUF,p_bind_vbuf,r,1);r->first=index;r->n=1;r->r[0]=(NVNbufferRange){address,size};
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
static void bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    if (n < 0 || n > 32) { LOG("【红】绑定句柄数 %d", n); return; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; memcpy(r->h, h, 8 * n);
}
// 图像（可写贴图）：句柄 = 贴图池号（GetImageHandle 原样返回；游戏不调 RegisterImage，图像与贴图共用登记）
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; the game does not call RegisterImage, images and textures share registration)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindImage,void,NVNcommandBuffer* c,NVNshaderStage st,int index,NVNimageHandle h) {bind_handles(c,OP_BIND_IMG,st,index,1,&h);}
NVN(nvnCommandBufferBindTexture,void,NVNcommandBuffer* c,NVNshaderStage st,int index,NVNtextureHandle h) {bind_handles(c,OP_BIND_TEX,st,index,1,&h);}
NVN(nvnCommandBufferBindTextures,void,NVNcommandBuffer* c,NVNshaderStage st,int first,int n,const NVNtextureHandle* h) {bind_handles(c,OP_BIND_TEX,st,first,n,h);}
NVN(nvnCommandBufferBindSeparateTexture,void,NVNcommandBuffer* c,NVNshaderStage st,int index,NVNseparateTextureHandle h) {bind_handles(c,OP_BIND_TEX,st,index,1,&h.value);}
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n,0,1}; }
NVN(nvnCommandBufferDrawArraysInstanced,void,NVNcommandBuffer* c,NVNdrawPrimitive pr,int first,int n,int base,int instances) {
    *REC(c,OP_DRAW,p_draw)=(p_draw){pr,first,n,base,instances};
}
NVN(nvnCommandBufferDrawElements,void,NVNcommandBuffer* c,NVNdrawPrimitive pr,NVNindexType it,int n,NVNbufferAddress ib) {
    *REC(c,OP_DRAW_IDX,p_draw_idx)=(p_draw_idx){pr,it,n,0,0,1,ib};
}
NVN(nvnCommandBufferDrawElementsBaseVertex,void,NVNcommandBuffer* c,NVNdrawPrimitive pr,NVNindexType it,int n,NVNbufferAddress ib,int base) {
    *REC(c,OP_DRAW_IDX,p_draw_idx)=(p_draw_idx){pr,it,n,base,0,1,ib};
}
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, bv, bi, inst, ib};
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // 跳板是尾跳（br），这里的返回地址就是游戏里的调用点
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferClearBuffer,void,NVNcommandBuffer* c,NVNbufferAddress address,size_t size,uint32_t value) {
    uint64_t* p=rec(c,OP_CLEAR_BUFFER,24);p[0]=address;p[1]=size;p[2]=value;
}
// Vulkan owns tiling; every operation currently completes before the next one.
NVN(nvnCommandBufferSetTiledCacheAction,void,NVNcommandBuffer* c,NVNtiledCacheAction action) {
    if(action<1 || action>6)die("Invalid tiled-cache action");
    *(int32_t*)rec(c,OP_BARRIER,8)=0;
}
NVN(nvnCommandBufferSetTiledCacheTileSize,void,NVNcommandBuffer* c,int width,int height) {(void)c;(void)width;(void)height;}
NVN(nvnCommandBufferSetShaderScratchMemory,void,NVNcommandBuffer* c,const NVNmemoryPool* pool,ptrdiff_t offset,size_t size) {
    (void)c;const pool_t* p=(const pool_t*)pool;
    if(offset<0 || (uint64_t)offset>p->size || size>p->size-(uint64_t)offset)die("Invalid shader scratch range");
    // SPIR-V Function/Private variables use Vulkan driver-owned scratch.
}
NVN(nvnCommandBufferSaveZCullData,void,NVNcommandBuffer* c,NVNbufferAddress address,size_t size) {(void)c;(void)address;if(size)die("Unexpected external ZCull storage");}
NVN(nvnCommandBufferRestoreZCullData,void,NVNcommandBuffer* c,NVNbufferAddress address,size_t size) {(void)c;(void)address;if(size)die("Unexpected external ZCull storage");}
NVN(nvnCommandBufferFenceSync,void,NVNcommandBuffer* c,NVNsync* sync,NVNsyncCondition condition,int flags) {
    (void)condition;(void)flags;*(uintptr_t*)rec(c,OP_FENCE,8)=(uintptr_t)sync;
}
NVN(nvnCommandBufferWaitSync,void,NVNcommandBuffer* c,const NVNsync* sync) {
    *(uintptr_t*)rec(c,OP_WAIT_SYNC,8)=(uintptr_t)sync;
}
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = nvn_command_pool(CB(c),(const dpool_t *)p); }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = nvn_command_pool(CB(c),(const dpool_t *)p); }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_B2T, p_copy_b2t); o->src = src; o->tex = nvn_command_texture(CB(c),(const tex_t *)t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_T2B, p_copy_b2t); o->src = dst; o->tex = nvn_command_texture(CB(c),(const tex_t *)t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// 用给定值清贴图区域（浮点 / 无符号整数两版；mask = 通道写掩码）
// Clear a texture region with the given value (float / unsigned integer versions; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = nvn_command_texture(CB(c),(const tex_t *)t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = nvn_command_texture(CB(c),(const tex_t *)t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferClearTexturei,void,NVNcommandBuffer* c,const NVNtexture* t,const NVNtextureView* v,const NVNcopyRegion* r,const int* rgba,int mask){
    p_clear_tex* o=REC(c,OP_CLEAR_TEX,p_clear_tex);o->tex=nvn_command_texture(CB(c),(const tex_t*)t);copy_view(&o->view,&o->has_view,v);o->r=*r;o->mask=mask;o->is_uint=2;memcpy(o->c.u,rgba,16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC(c, OP_COPY_T2T, p_copy_t2t); o->src = nvn_command_texture(CB(c),(const tex_t *)s); o->dst = nvn_command_texture(CB(c),(const tex_t *)d);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { REC(c, OP_COPY_STRIDE, p_copy_stride)->stride = st; }
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { REC(c, OP_COPY_IMAGE_STRIDE, p_copy_stride)->stride = st; }
NVN(nvnCommandBufferSetSampleMask,void,NVNcommandBuffer* c,int mask){*(uint32_t*)rec(c,OP_SAMPLE_MASK,8)=mask;}
NVN(nvnCommandBufferSetRasterizerDiscard,void,NVNcommandBuffer* c,uint32_t enable){*(uint32_t*)rec(c,OP_RASTER_DISCARD,8)=B(enable)!=0;}
NVN(nvnCommandBufferSetDepthClamp,void,NVNcommandBuffer* c,uint32_t enable){*(uint32_t*)rec(c,OP_DEPTH_CLAMP,8)=B(enable)!=0;}
NVN(nvnCommandBufferSetRenderEnable,void,NVNcommandBuffer* c,uint32_t enable){*(uint32_t*)rec(c,OP_RENDER_ENABLE,8)=B(enable)!=0;}
// Conditional render, used for occlusion culling: the title reports samples passed
// twice and asks the GPU whether anything got through in between. The comparison
// happens where the counters are read (see OP_RENDER_COND in backend.cpp), not
// here, because the second sample may not have been written yet when the command
// is recorded.
NVN_NOPROTO(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, int mode, NVNbufferAddress address) {
    if (mode != 0 && mode != 1) { LOG("SetRenderEnableConditional: unknown mode %d, drawing anyway", mode); return; }
    *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){mode, 0, address};
}
NVN(nvnCommandBufferSetLineWidth,void,NVNcommandBuffer* c,float width){*(float*)rec(c,OP_LINE_WIDTH,8)=width;}
NVN(nvnCommandBufferDownsampleTextureView,void,NVNcommandBuffer* c,const NVNtexture* source,const NVNtextureView* sv,const NVNtexture* destination,const NVNtextureView* dv){
    const tex_t* s=CTX(source);const tex_t* d=CTX(destination);const view_t* a=(const view_t*)sv;const view_t* b=(const view_t*)dv;
    unsigned sl=a&&a->set_levels?a->base_level:0,dl=b&&b->set_levels?b->base_level:0;
    int layers=a&&a->set_layers?(a->nlayers?a->nlayers:s->L.layers-a->min_layer):s->L.layers;
    int sw=s->d.width>>sl,sh=s->d.height>>sl,dw=d->d.width>>dl,dh=d->d.height>>dl;
    NVNcopyRegion sr={0,0,0,sw?sw:1,sh?sh:1,layers},dr={0,0,0,dw?dw:1,dh?dh:1,layers};
    h_nvnCommandBufferCopyTextureToTexture(c,source,sv,&sr,destination,dv,&dr,0);
}
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- 状态对象：就地打包（nvn_obj.h） ----------
// ---------- State objects: packed in place (nvn_obj.h) ----------
#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("【红】顶点属性偏移 %td 超出打包范围", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }

#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【红】WindowBuilderSetTextures 数量 %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize 交换链 %d 张 间隔 %d 首张 %dx%d 格式 %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] 游戏设出图间隔 %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; }
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { return ((const win_t *)w)->interval; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}

NVN(nvnQueueSubmitCommands,void,NVNqueue* q,int n,const NVNcommandHandle* handles) {
    vkhost_submit(n,handles);((queue_t*)q)->submits++;
}
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow* w,NVNsync* s,int* index) {
    win_t* o=(win_t*)w;if(o->ntex<=0)die("Empty window texture set");
    *index=o->next;o->next=(o->next+1)%o->ntex;
    // Vsync throttling belongs here, not in present: a title usually calls
    // present while holding one of its own locks, so blocking there can stall a
    // worker that never gets scheduled. deko3d documents the same rule for
    // dkQueueAcquireImage ("blocks if all images are in use"); the real device
    // waits here until the image's previous present has been displayed.
    // The wait is capped: present may never happen during bring-up, and an
    // uncapped wait would turn a spin into a deadlock.
    {
        const uint64_t ticket=o->ticket[*index];
        if(ticket)vkhost_wait(ticket,100000000ULL);
    }
    if(s){__atomic_store_n(&((sync_t*)s)->fence,o->ticket[*index],__ATOMIC_RELEASE);__atomic_store_n(&((sync_t*)s)->fenced,1,__ATOMIC_RELEASE);}
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueuePresentTexture,void,NVNqueue* q,NVNwindow* w,int index) {
    (void)q;win_t* o=(win_t*)w;if(index<0 || index>=o->ntex)die("Invalid window texture index");
    o->ticket[index]=vkhost_present((const tex_t*)o->tex[index],o->interval,o->crop);o->presents++;
}
