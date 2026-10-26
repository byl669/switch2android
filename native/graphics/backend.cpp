#include "../input.h"
#include "backend.h"
#include "traced_mutex.h"
#include "nvn_commands.h"
#include "nvn_program.h"
#include "shader_reflection.h"
#include "shader_compat.h"
#include "../third_party/spirv/spirv.hpp"
#include "bc_texture.h"
#include "copy_layout.h"
#include "hint_work.h"
#include "cpu_capacity.h"
#include "serial_queue.h"
#include "completion_timeline.h"
#include "device_limits.h"
#include "nvn_pace.h"
#include "frame_stats.h"
#include "frame_trace.h"
#include "fixed_key.h"
#include "epoch_cache.h"
#include "direct_cache.h"
#include "key_fields.h"
#include "index_summary.h"
#include "write_ranges.h"
#include "cache_budget.h"
#include "clear_shaders.h"
#include "nvn_deswizzle.h"
#include "texture_write.h"
#include "nvn_dirty.h"
#include <set>
#include <cstdarg>
#include <cstdio>
#include <bit>
#include <list>
#include <array>
#include "../thread_services.h"
#include "../third_party/json.hpp"
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <map>
#include <unordered_map>
#include <string_view>
#include <span>
#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <time.h>
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <condition_variable>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <cerrno>
#include <android/performance_hint.h>
extern "C" void ot2_guest_batch_profile(int) __attribute__((weak));
extern "C" void ot2_guest_execution_profile(int) __attribute__((weak));
extern "C" void ot2_guest_register_gates(int) __attribute__((weak));
extern "C" void ot2_guest_permission_profile(int) __attribute__((weak));
extern "C" void ot2_host_stream_profile(int) __attribute__((weak));

namespace {
VkInstance instance;
nvn_host_limits host_limits{};
VkPhysicalDevice physical;
VkDevice device;
VkQueue queue;
uint32_t queue_family;
ANativeWindow* window;
VkSurfaceKHR surface;
VkSwapchainKHR swapchain;
VkFence drawable_acquire_fence{};
VkExtent2D swapchain_extent;
std::vector<VkImage> swapchain_images;
std::atomic<uint64_t> presented_frames{0};
PFN_vkGetPastPresentationTimingGOOGLE get_display_timings{};
uint32_t display_id=0;uint64_t display_previous=0,display_start=0,display_samples=0;
std::mutex frame_stats_mutex;FrameStatsWindow frame_stats;
void record_frame_timing(uint64_t time,bool actual){std::lock_guard lock(frame_stats_mutex);frame_stats.add(time,actual);}
void reset_display_timings(){display_previous=display_start=display_samples=0;std::lock_guard lock(frame_stats_mutex);frame_stats.reset();}
std::map<uintptr_t,uint64_t> pools;
struct TextureDescriptor {tex_t texture;view_t view;bool has_view;VkImageView cached_view{};};
struct SamplerDescriptor {samp_par parameters;VkSampler cached{};};
std::map<uintptr_t,std::map<int,TextureDescriptor>> texture_pools;
std::map<uintptr_t,std::map<int,SamplerDescriptor>> sampler_pools;
uintptr_t active_texture_pool=0,active_sampler_pool=0;
uint32_t active_texture_capacity=0,active_sampler_capacity=0;
bool null_descriptors=false;
bool native_bc=false;
bool robust_uniform_ranges=false;
size_t robust_uniform_alignment=256;
uint32_t dynamic_uniform_limit=0,dynamic_storage_limit=0;
std::set<uint64_t> finalized_textures;
std::map<uint64_t,size_t> texture_references;
auto& active_textures(){return texture_pools[active_texture_pool];}
auto& active_samplers(){return sampler_pools[active_sampler_pool];}
bool cache_texture_lookups=true;
uint64_t texture_lookup_hits=0,image_lookup_hits=0;
struct TextureLookup {uintptr_t pool{};unsigned id{};TextureDescriptor* descriptor{};};
std::array<TextureLookup,1024> texture_lookups{};
void invalidate_texture_lookups(){texture_lookups={};}
TextureDescriptor* texture_descriptor(unsigned id){
    auto& slot=texture_lookups[(id^(active_texture_pool>>8))&(texture_lookups.size()-1)];
    if(cache_texture_lookups&&slot.descriptor&&slot.pool==active_texture_pool&&slot.id==id){++texture_lookup_hits;return slot.descriptor;}
    auto pool=texture_pools.find(active_texture_pool);if(pool==texture_pools.end())return nullptr;
    auto& entries=pool->second;auto it=entries.find(id);if(it==entries.end())return nullptr;
    // Map nodes keep their address on replacement; registration updates the
    // complete descriptor in place. Every erasure invalidates this table first.
    if(cache_texture_lookups)slot={active_texture_pool,id,&it->second};return &it->second;
}
uintptr_t descriptor_pool_key(const dpool_t* p){if(!p || !p->pool || !p->pool->mem)ot2_host_die("Invalid descriptor pool");return uintptr_t(p->pool->mem)+p->off;}
ot2::TracedMutex state_mutex{"state"};std::condition_variable surface_changed;
std::string shader_directory;
std::string shader_binary_directory;
VkShaderModule shared_shader_module(const uint32_t*,size_t);
void start_pipeline_warmup();
bool complete_shader_cache=false;
bool capture_frame=false;
uint64_t capture_frame_number=300,capture_bytes=0;
// The frame a capture is taken from is what makes two runs comparable: the same
// frame twice shows whether a scene is animating, two different frames show
// whether it ever changes. Reading it from a file keeps that reachable without
// a rebuild.
void read_capture_frame_number() {
    std::ifstream in(shader_directory+"/capture-frame-number");
    unsigned long long value=0;
    if(in>>value && value) capture_frame_number=value;
}
bool trace_draw_completion=false;
VkCommandPool command_pool;
VkPipelineCache pipeline_cache;
VkQueryPool occlusion_query,counter_query,synchronous_occlusion_query,synchronous_counter_query;
// Each report owns its logical reset interval. The physical query pool is
// reset once per batch; prefix differences preserve earlier reports even when
// later commands reset the guest counter before the GPU has completed.
struct CounterEpoch {uint64_t samples=0;};
std::shared_ptr<CounterEpoch> counter_epoch=std::make_shared<CounterEpoch>();
struct PendingCounter {uint64_t address;std::shared_ptr<CounterEpoch> epoch;uint32_t type,sample_begin,sample_end;};
struct SampleInterval {std::shared_ptr<CounterEpoch> epoch;uint32_t begin,end;};
std::vector<SampleInterval> sample_intervals;
std::vector<PendingCounter> pending_counters;
WriteRanges pending_write_ranges;
bool dependency_index_enabled=true;
uint64_t dependency_index_checks=0;
bool samples_active=false,precise_occlusion=false,depth_bounds_supported=false;
bool geometry_query_workaround=false;
#ifdef OT2_NVN_TESTING
bool force_software_samples=false;
#endif
constexpr uint32_t query_capacity=256;uint32_t pending_queries=0,sample_reset_index=0;
bool batch_query_resets=true;
#ifdef OT2_NVN_TESTING
VkEvent test_gpu_event{};
bool test_hold_next_gpu=false;
std::atomic<uint64_t> test_gpu_encoded{0},test_gpu_submitted{0};
#endif
bool merge_enabled=true;bool batch_enabled=true;bool batching=false;VkCommandBuffer batch_command{};unsigned batch_operations=0;
bool readonly_stores_enabled=false;
bool precise_barriers=true,clear_planning=true,prune_unused_depth=true;
uint64_t scoped_barriers=0,folded_clears=0,elided_clears=0,pruned_depth=0,empty_draws=0;
// Draws skipped because a shader stage has no translation in the offline cache.
// Counted, not fatal: see shader_for in draw.inc.
uint64_t skipped_missing_draws=0;
// Draws that reached the pipeline with a shader input the title never bound. The
// console supplies a disabled attribute; Vulkan leaves it undefined.
std::atomic<uint64_t> unbound_attribute_draws{0};
// Draws that reached vkCmdDraw / vkCmdDrawIndexed, with the geometry volume they
// described. Zero here with a blank screen means the filter chain, not the GPU.
std::atomic<uint64_t> draws_submitted{0},indices_submitted{0},vertices_submitted{0};
// Conditional-render tallies. "Undecided" means the two sampled counters were both
// still zero, so the comparison could not be trusted and the draw went ahead;
// "skipped" means a decided answer said no. A large undecided count against a small
// decided one means the title's occlusion queries are not being answered at all.
uint64_t undecided_conditional_draws=0,decided_conditional_draws=0,skipped_conditional_draws=0;
void materialize_clears(uint64_t serial=0);
bool consume_clear(const tex_t&,const view_t*,VkExtent2D,uint32_t,VkImageAspectFlags,VkRenderingAttachmentInfo&);
struct AttachmentStores {uint64_t none[3]{},promotions{},fallbacks{};} attachment_stores;
void note_attachment_store(const VkRenderingAttachmentInfo& info,unsigned aspect){if(info.imageView&&info.storeOp==VK_ATTACHMENT_STORE_OP_NONE)++attachment_stores.none[aspect];}
std::vector<std::function<void()>> batch_retire;
VkCommandBuffer rendering_command{};std::string rendering_key;uint32_t rendering_no_store=0;
void gpu_profile_region_end(VkCommandBuffer);
void end_rendering_compatible(VkCommandBuffer);
void end_rendering(){if(rendering_command){end_rendering_compatible(rendering_command);gpu_profile_region_end(rendering_command);VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=precise_barriers?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(rendering_command,precise_barriers?VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);rendering_command={};rendering_key.clear();rendering_no_store=0;}}
void flush_commands(bool wait=true);
void ensure_batch_slot();
void drain_gpu_locked();
void ensure_previous_cpu_writes();
bool previous_cpu_overlap(uintptr_t,size_t);
void ensure_cpu_buffer(uintptr_t,size_t);
void cache_allocation_pressure();
void trim_readbacks();
size_t upload_alignment=256;
uint32_t max_storage_range=0;
VkPhysicalDeviceMemoryProperties memory_properties;
struct Image {uint64_t identity{};FixedKey<128> storage_key;bool shared_storage{};uintptr_t backing{};VkImage image{};VkDeviceMemory memory{};VkFormat format{};VkImageAspectFlags aspects{};uint32_t levels{},layers{};bool is_3d{};std::shared_ptr<BcTexture> bc;uint64_t drawn{},source_stamp{},source_seen{},source_uploads{};bool source_tracked{};VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;};
std::map<uint64_t,std::shared_ptr<Image>> images;
std::unordered_map<FixedKey<128>,std::weak_ptr<Image>,FixedKey<128>::Hash> image_storage;
std::unordered_map<FixedKey<128>,std::set<uint64_t>,FixedKey<128>::Hash> image_storage_owners;
std::map<uint64_t,FixedKey<128>> texture_storage_keys;
bool shared_depth_storage(const tex_t& t){return t.mem&&!t.d.pkg_block_h&&(t.d.format==NVN_FORMAT_DEPTH24_STENCIL8||t.d.format==NVN_FORMAT_DEPTH32F_STENCIL8);}
bool fast_image_identity=true;
FixedKey<128> image_storage_key(const tex_t& t){
    FixedKey<128> k;k.add(uintptr_t(t.mem));k.add(t.L.size);k.add(t.L.layer_size);k.add(t.L.layers);k.add(t.L.tileH);k.add(t.L.tileD);
    k.add(t.d.target);k.add(t.d.format);k.add(t.d.flags);k.add(t.d.samples);k.add(t.d.width);k.add(t.d.height);k.add(t.d.depth);k.add(t.d.levels);k.add(t.d.stride);return k;
}
struct ImageLookup {uint64_t serial{};Image* image{};};
std::array<ImageLookup,1024> image_lookups{};
Image* find_image(uint64_t serial){
    auto& slot=image_lookups[serial&(image_lookups.size()-1)];
    if(cache_texture_lookups&&slot.image&&slot.serial==serial){++image_lookup_hits;return slot.image;}
    auto it=images.find(serial);if(it==images.end())return nullptr;
    if(cache_texture_lookups)slot={serial,it->second.get()};return it->second.get();
}
// Only exact packed depth/stencil storage aliases are shared. CPU textures retain
// their versioned upload semantics; no format/size/offset reinterpretation is inferred.
Image* find_existing_image(const tex_t& t){
    if(auto* image=find_image(t.serial))return image;
    if(!shared_depth_storage(t))return nullptr;
    auto key=image_storage_key(t);auto found=image_storage.find(key);if(found==image_storage.end())return nullptr;
    auto shared=found->second.lock();if(!shared){image_storage.erase(found);return nullptr;}
    auto* result=shared.get();images.emplace(t.serial,std::move(shared));return result;
}
uint64_t image_identity(uint64_t serial){auto* image=find_image(serial);return image?image->identity:serial;}
SerialQueue& translation_queue(){static auto* queue=new SerialQueue;return *queue;}
SerialQueue& presentation_queue(){static auto* queue=new SerialQueue(3);return *queue;}
SerialQueue& completion_queue(){static auto* queue=new SerialQueue;return *queue;}
CompletionTimeline gpu_completion{"gpu_completion"};
constexpr uint64_t present_ticket_bit=uint64_t(1)<<63;
std::atomic<bool> presentation_suspended{false};
std::mutex presentation_wait_mutex;std::condition_variable presentation_changed;
bool verbose_gpu_logs=false,polling_pacer=false;
// Diagnostic switch: swap the title's unit quad for a screen-sized one on 2D
// sprite programs, so the geometry question and the shading question can be
// answered separately. See draw.inc.
bool sprite_pixels_forced=false;
float sprite_pixel_scale=1.0f;
float sprite_instance_scale=1.0f;
float sprite_rect_w=640.f,sprite_rect_h=360.f;
float sprite_scale_probe=0.0f;
bool paint_draws_probe=false;
bool ui_matrix_probe=false;
// Whether the bytes past a bound uniform range are read from the title's memory
// or filled with zero. The console behaviour is to read them: one physical driver
// segment per slot, and titles do write past what they bind -- the bank at 912
// bytes has a rotation and translation starting at exactly 912. Reads are capped
// at the end of the page the range starts in, so this can never cross into
// memory the title has not mapped.
bool uniform_tail_from_memory=true;
// Diagnostic: log every uniform range the title binds and every byte range it
// writes into them. See the OP_BIND_UBO case.
bool trace_uniform_banks=false;
// Diagnostic: dump the constant banks a 2D sprite draw reads, including the bytes
// past each bank's declared size. See the bank dump in the OP_TRACE block.
bool bank_dump_probe=false;
// Per-draw counters cannot tell "the composite of the scene target is missing"
// apart from "the frame really is empty". One traced frame can: it shows every
// target switch, clear, copy and draw in the order the title issued them.
uint64_t trace_ops_frame=0,trace_op_index=0;uint32_t trace_op_hist[64]{};
bool prepare_before_pace=false;
bool present_before_pace=false;
uint64_t present_prepare_ns=0,present_late_ns=0,present_late_max_ns=0;
void drain_queues(){translation_queue().drain();presentation_queue().drain();completion_queue().drain();}
thread_local uint64_t replay_sequence=0;
p_set_rt render_targets{};
tex_t render_target_storage[9]{};
std::vector<uint64_t> render_target_pins;
void bind_render_targets(const p_set_rt& targets);
struct RenderState {
    const prog_t* programs[6]{};
    p_bind_buf uniforms[6][16]{},storage[6][16]{};
    uint64_t texture_handles[6][128]{},sampler_handles[6][128]{},image_handles[6][128]{};
    va_st attributes[16]{};vs_st streams[16]{};NVNbufferRange vertex_buffers[16]{};
    blend_st blends[8]{};color_st color{};chmask_st channel_mask{0xffffffff};ms_st multisample{};poly_st polygon{};ds_st depth_stencil{};
    float viewport[6]{0,0,1280,720,0,1};p_scissor scissor{0,0,1280,720};
    p_depth_bounds depth_bounds{0,0,1};
    uint32_t sample_mask=~0u,raster_discard=0,depth_clamp=0,render_enable=1;float line_width=1;
    // Conditional render state: mode 0 renders when the two sampled counters are
    // equal, mode 1 when they differ. render_cond_active is cleared by a plain
    // SetRenderEnable, which replaces the condition with an unconditional answer.
    int32_t render_cond_mode=-1;uint64_t render_cond_addr=0;
    p_poly_offset depth_bias{};float blend_color[4]{};
    uint32_t stencil_reference[2]{},stencil_compare_mask[2]{255,255},stencil_write_mask[2]{255,255};
} render_state;
std::shared_ptr<const NvnProgramVersion> bound_programs[6];
int64_t copy_row_stride=0,copy_image_stride=0;
float timestamp_period=1;
// Key of a stage as the offline cache names it: the control section at its own
// length followed by the code area. The control section alone is not enough for
// this title -- 613 of its 2125 shaders share a control section and differ only
// in code (the Bezel Engine compiles one control per program family, one code
// area per variant) -- so a control-only key answers every variant with the one
// shader that happened to be translated first. nxnative's host (prog_get) and
// its shader_split.py both key these two pieces together.
std::string shader_key(const void* control,const void* code,size_t code_size) {
    uint64_t hash=14695981039346656037ULL;
    const auto* p=static_cast<const uint8_t*>(control);
    uint32_t n=0x880;
    if(p){uint32_t own=0;memcpy(&own,p+0x20,4);if(own>=0x760&&own<0x880)n=own;
        for(uint32_t j=0;j<n;j++)hash=(hash^p[j])*1099511628211ULL;}
    const auto* c=static_cast<const uint8_t*>(code);
    for(size_t j=0;c&&j<code_size;j++)hash=(hash^c[j])*1099511628211ULL;
    char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)hash);return key;
}
// The control section alone, exactly as the first revision of this host hashed
// it (a fixed 0x880, no own-length adjustment). It stays byte-for-byte as it was
// because it is the name the shipped cache is filed under: the fallback in
// shader_for must be able to reproduce those names precisely.
std::string shader_key(const void* control) {
    uint64_t hash=14695981039346656037ULL;const auto* p=static_cast<const uint8_t*>(control);
    for(uint32_t j=0;p&&j<0x880;j++)hash=(hash^p[j])*1099511628211ULL;
    char key[32];snprintf(key,sizeof(key),"%016llx",(unsigned long long)hash);return key;
}
uint64_t monotonic_ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
struct Perf {uint64_t passes{},submits{},wait_ns{},draw_ns{},buffer_ns{},buffer_bytes{},vertex_bytes{},uniform_bytes{},storage_bytes{},buffer_allocs{},pipeline_ns{},replay_ns{},replay_cpu_ns{};} perf;
// Per-draw clocks are diagnostic work; HUD timing comes from display timestamps.
// Keep the coarse replay timer for low-cost frame comparisons.
bool detail_timing=false,stats_enabled=false;
// Draws every pass opaque, ignoring the title's blend state. A diagnostic for the
// case where the frame presents steadily and geometry is submitted but no pixel
// ever changes: it separates "blending hides it" from "nothing writes here".
bool opaque_blend_forced=false;
// Two independent switches for the winding question (see the rasterisation state
// in draw.inc). `no-cull` forces culling off, which tells a mis-set winding apart
// from a draw that never reaches the rasteriser at all. `keep-winding` restores
// the pre-fix mapping (front-face value passed through unchanged) so the two can
// be compared in one session.
bool cull_none_forced=false;
bool winding_not_flipped=false;
// Diagnostic: drop the depth attachment from every rendering instance. A 2D pass
// that binds a depth image of a different size than its colour target leaves the
// rendering area outside the depth image, which Vulkan does not define; the draw
// then writes nothing and reports no error. Unbinding the depth says whether that
// mismatch is what keeps such a pass off the screen.
bool force_no_depth=false;
void stats_log(const char* format,...) {
    if(!stats_enabled)return;
    char text[2048];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);
    ot2_host_log("%s",text);
}
struct PerfScope {uint64_t& total;uint64_t start;explicit PerfScope(uint64_t& t):total(t),start((detail_timing||&t==&perf.replay_ns)?monotonic_ns():0){}~PerfScope(){if(start)total+=monotonic_ns()-start;}};
uint64_t thread_cpu_ns(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;}
struct ThreadCpuScope {uint64_t& total;uint64_t start=thread_cpu_ns();explicit ThreadCpuScope(uint64_t& t):total(t){}~ThreadCpuScope(){total+=thread_cpu_ns()-start;}};
void check(VkResult r,const char* operation) {if(r!=VK_SUCCESS)ot2_host_die("Vulkan %s failed: %d",operation,r);}
#include "legacy_rendering.inc"
#include "gpu_profile.inc"
#include "performance_hint.inc"
#include "command_state.inc"
#include "command_recycling.inc"
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory_properties.memoryTypeCount;i++)if((bits&(1U<<i)) && (memory_properties.memoryTypes[i].propertyFlags&flags)==flags)return i;
    ot2_host_die("Vulkan memory type unavailable: %x",flags);
}
VkCommandBuffer begin_commands(bool keep_rendering=false) {
    if(batching)ensure_batch_slot();
    if(!keep_rendering)end_rendering();
    if(batch_command)return batch_command;
    VkCommandBuffer cmd=acquire_command();
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd,&b),"begin commands");
    command_state_reset(cmd);
#ifdef OT2_NVN_TESTING
    if(batching&&test_hold_next_gpu){test_hold_next_gpu=false;vkCmdWaitEvents(cmd,1,&test_gpu_event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,0,nullptr);}
#endif
    vkCmdResetQueryPool(cmd,occlusion_query,0,batching?query_capacity:1);
    // Resets must be outside rendering. Allocate timestamp slots from this
    // batch-wide reset so reports need not interrupt an attachment pass.
    if(batching && batch_query_resets)vkCmdResetQueryPool(cmd,counter_query,0,query_capacity);
    gpu_profile_begin(cmd);
    if(batching)batch_command=cmd;return cmd;
}
void finish_commands(VkCommandBuffer cmd) {
    FrameTraceScope trace("OT2 synchronous submit");
    end_rendering();
    VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};visibility.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;visibility.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&visibility,0,nullptr,0,nullptr);
    if(batching){++batch_operations;return;}
    gpu_profile_end(cmd);
    check(vkEndCommandBuffer(cmd),"end commands");
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence;check(vkCreateFence(device,&f,nullptr,&fence),"create fence");
    ++perf.submits;{PerfScope timing(perf.wait_ns);check(vkQueueSubmit(queue,1,&s,fence),"submit");check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"GPU completion");}
    gpu_profile_retire(cmd);retire_legacy_rendering(cmd);vkDestroyFence(device,fence,nullptr);release_command(cmd);
}
VkFormat format(int f) {
    if(f>=NVN_FORMAT_RGBA_ASTC_4x4 && f<=NVN_FORMAT_RGBA_ASTC_12x12)return VkFormat(VK_FORMAT_ASTC_4x4_UNORM_BLOCK+2*(f-NVN_FORMAT_RGBA_ASTC_4x4));
    if(f>=NVN_FORMAT_RGBA_ASTC_4x4_SRGB && f<=NVN_FORMAT_RGBA_ASTC_12x12_SRGB)return VkFormat(VK_FORMAT_ASTC_4x4_SRGB_BLOCK+2*(f-NVN_FORMAT_RGBA_ASTC_4x4_SRGB));
    switch(f) {
#define F(n,v) case NVN_FORMAT_##n:return VK_FORMAT_##v
        F(R8,R8_UNORM);F(R8SN,R8_SNORM);F(R8UI,R8_UINT);F(R8I,R8_SINT);
        F(R16F,R16_SFLOAT);F(R16,R16_UNORM);F(R16SN,R16_SNORM);F(R16UI,R16_UINT);F(R16I,R16_SINT);
        F(R32F,R32_SFLOAT);F(R32UI,R32_UINT);F(R32I,R32_SINT);
        F(RG8,R8G8_UNORM);F(RG8SN,R8G8_SNORM);F(RG8UI,R8G8_UINT);F(RG8I,R8G8_SINT);
        F(RG16F,R16G16_SFLOAT);F(RG16,R16G16_UNORM);F(RG16SN,R16G16_SNORM);F(RG16UI,R16G16_UINT);F(RG16I,R16G16_SINT);
        F(RGB32F,R32G32B32_SFLOAT);F(RGB32UI,R32G32B32_UINT);F(RGB32I,R32G32B32_SINT);
        F(RG32F,R32G32_SFLOAT);F(RG32UI,R32G32_UINT);F(RG32I,R32G32_SINT);
        F(RGBA8,R8G8B8A8_UNORM);F(RGBA8SN,R8G8B8A8_SNORM);F(RGBA8UI,R8G8B8A8_UINT);F(RGBA8I,R8G8B8A8_SINT);
        F(RGBA16F,R16G16B16A16_SFLOAT);F(RGBA16,R16G16B16A16_UNORM);F(RGBA16SN,R16G16B16A16_SNORM);F(RGBA16UI,R16G16B16A16_UINT);F(RGBA16I,R16G16B16A16_SINT);
        F(RGBA32F,R32G32B32A32_SFLOAT);F(RGBA32UI,R32G32B32A32_UINT);F(RGBA32I,R32G32B32A32_SINT);
        F(BGRA8,B8G8R8A8_UNORM);F(BGRX8,B8G8R8A8_UNORM);F(BGRA8_SRGB,B8G8R8A8_SRGB);F(BGRX8_SRGB,B8G8R8A8_SRGB);
        F(RGBA8_SRGB,R8G8B8A8_SRGB);F(RGBX8,R8G8B8A8_UNORM);F(RGBX8_SRGB,R8G8B8A8_SRGB);
        // DEPTH24 is X8_D24_UNORM_PACK32, which Adreno drivers do not offer as a
        // depth attachment (no VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT).
        // The format is then still created and still bound, and the rendering
        // instance that uses it produces nothing at all -- every draw is dropped
        // without an error. D32_SFLOAT carries the same "depth only, no stencil"
        // meaning with full driver support, and a wider depth is never worse.
        F(DEPTH16,D16_UNORM);F(DEPTH24,D32_SFLOAT);F(DEPTH32F,D32_SFLOAT);F(DEPTH24_STENCIL8,D24_UNORM_S8_UINT);F(DEPTH32F_STENCIL8,D32_SFLOAT_S8_UINT);
        F(R11G11B10F,B10G11R11_UFLOAT_PACK32);F(RGB10A2,A2B10G10R10_UNORM_PACK32);F(RGB10A2UI,A2B10G10R10_UINT_PACK32);
        F(RGB_DXT1,BC1_RGB_UNORM_BLOCK);F(RGBA_DXT1,BC1_RGBA_UNORM_BLOCK);F(RGBA_DXT3,BC2_UNORM_BLOCK);F(RGBA_DXT5,BC3_UNORM_BLOCK);
        F(RGB_DXT1_SRGB,BC1_RGB_SRGB_BLOCK);F(RGBA_DXT1_SRGB,BC1_RGBA_SRGB_BLOCK);F(RGBA_DXT3_SRGB,BC2_SRGB_BLOCK);F(RGBA_DXT5_SRGB,BC3_SRGB_BLOCK);
        F(RGTC1_UNORM,BC4_UNORM_BLOCK);F(RGTC1_SNORM,BC4_SNORM_BLOCK);F(RGTC2_UNORM,BC5_UNORM_BLOCK);F(RGTC2_SNORM,BC5_SNORM_BLOCK);
        F(BPTC_UNORM,BC7_UNORM_BLOCK);F(BPTC_UNORM_SRGB,BC7_SRGB_BLOCK);F(BPTC_SFLOAT,BC6H_SFLOAT_BLOCK);F(BPTC_UFLOAT,BC6H_UFLOAT_BLOCK);
#undef F
        default:ot2_host_die("Vulkan texture format pending: 0x%x",f);
    }
}
struct ImageScope {VkPipelineStageFlags stages;VkAccessFlags access;};
ImageScope image_scope(VkImageLayout layout){
    if(!precise_barriers)return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    switch(layout){
    case VK_IMAGE_LAYOUT_UNDEFINED:return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return {VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:return {VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0};
    default:return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    }
}
void barrier(VkCommandBuffer cmd,Image& image,VkImageLayout,VkImageLayout new_layout) {
    const auto old_layout=image.layout;image.layout=new_layout;
    const auto src=image_scope(old_layout),dst=image_scope(new_layout);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=old_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:src.access;
    b.dstAccessMask=dst.access;b.oldLayout=old_layout;b.newLayout=new_layout;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=image.image;b.subresourceRange={image.aspects,0,image.levels,0,image.layers};
    vkCmdPipelineBarrier(cmd,src.stages,dst.stages,0,0,nullptr,0,nullptr,1,&b);if(precise_barriers)++scoped_barriers;
}
Image& image_for(const tex_t& t) {
    if(auto* cached=find_existing_image(t))return *cached;
    if(t.magic!=MAGIC('T','E','X',0) || !t.serial)ot2_host_die("Invalid recorded texture object");
    Image image{};image.identity=t.serial;image.backing=uintptr_t(t.mem);image.format=format(t.d.format);image.is_3d=t.d.target==NVN_TEXTURE_TARGET_3D;
    auto samples=uint32_t(std::max(1,t.d.samples));if(samples>8 || (samples&(samples-1)))ot2_host_die("Invalid NVN multisample count: %u",samples);image.samples=VkSampleCountFlagBits(samples);
    image.levels=std::max(1U,t.d.levels);image.layers=image.is_3d?1:std::max(1U,t.L.layers);
    if(auto info=bc_format(t.d.format);info.block_bytes&&!native_bc){
        if(samples!=1)ot2_host_die("BC multisampling is not supported");
        image.format=format(info.unpacked);
        image.bc=std::make_shared<BcTexture>(t.d.format,t.d.width,std::max(1U,t.d.height),std::max(1U,t.d.depth),image.levels,image.layers,image.is_3d);
    }
    image.aspects=t.d.format>=NVN_FORMAT_DEPTH16 && t.d.format<=NVN_FORMAT_DEPTH32F_STENCIL8?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    if(t.d.format==NVN_FORMAT_DEPTH24_STENCIL8 || t.d.format==NVN_FORMAT_DEPTH32F_STENCIL8)image.aspects|=VK_IMAGE_ASPECT_STENCIL_BIT;
    VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(physical,image.format,&fp);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=image.is_3d?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;ci.format=image.format;
    if(t.d.target==NVN_TEXTURE_TARGET_1D || t.d.target==NVN_TEXTURE_TARGET_1D_ARRAY)ci.imageType=VK_IMAGE_TYPE_1D;
    ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if(image.is_3d)ci.flags|=VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    ci.extent={t.d.width,std::max(1U,t.d.height),image.is_3d?std::max(1U,t.d.depth):1};ci.mipLevels=image.levels;ci.arrayLayers=image.layers;
    ci.samples=image.samples;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(!image.bc&&(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))ci.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)ci.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(!depth_bounds_supported&&(ci.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))ci.usage|=VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if(!image.bc&&samples==1 && (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))ci.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if(t.d.target==NVN_TEXTURE_TARGET_CUBEMAP || t.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageFormatProperties supported{};check(vkGetPhysicalDeviceImageFormatProperties(physical,ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags,&supported),"image capabilities");
    if(!(supported.sampleCounts&samples))ot2_host_die("Unsupported sample count %u for format %u",samples,ci.format);
    check(vkCreateImage(device,&ci,nullptr,&image.image),"texture image");
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image.image,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device,&ai,nullptr,&image.memory),"texture memory");check(vkBindImageMemory(device,image.image,image.memory,0),"bind texture");
    auto cmd=begin_commands();barrier(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);finish_commands(cmd);
    ot2_host_log("GPU_IMAGE serial=%llu %ux%u layers=%u format=%d",(unsigned long long)t.serial,t.d.width,t.d.height,image.layers,image.format);
    auto shared=std::make_shared<Image>(std::move(image));auto* result=shared.get();
    if(shared_depth_storage(t)){result->shared_storage=true;result->storage_key=image_storage_key(t);image_storage[result->storage_key]=shared;
        if(auto owners=image_storage_owners.find(result->storage_key);owners!=image_storage_owners.end())for(auto serial:owners->second)images.try_emplace(serial,shared);
    }
    images.emplace(t.serial,std::move(shared));
    if(cache_texture_lookups)image_lookups[t.serial&(image_lookups.size()-1)]={t.serial,result};
    return *result;
}
// Vulkan buffer/image copies expose depth and stencil as separate planes. NVN
// D24_S8 uses depth in bits 0..23 and stencil in bits 24..31.
// D32F_S8 uses float32 followed by an eight-bit stencil in a uint32 word.
// Keep this conversion shared by command uploads, packaged textures and reads.
bool packed_depth_stencil(const Image& image) {
    if(image.samples!=VK_SAMPLE_COUNT_1_BIT)ot2_host_die("Multisample buffer/image transfer requires resolve");
    return image.format==VK_FORMAT_D24_UNORM_S8_UINT || image.format==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
std::vector<VkBufferImageCopy> transfer_regions(const Image& image,VkBufferImageCopy region,size_t pixels) {
    if(!packed_depth_stencil(image))return {region};
    region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    auto stencil=region;stencil.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;stencil.bufferOffset+=pixels*4;
    return {region,stencil};
}
void split_depth_stencil(VkFormat format,uint8_t* planes,size_t pixels,size_t first,const uint8_t* packed,size_t count) {
    for(size_t x=0;x<count;x++){
        if(format==VK_FORMAT_D24_UNORM_S8_UINT){
            uint32_t word;memcpy(&word,packed+x*4,4);uint32_t depth=word&0xffffff;
            memcpy(planes+(first+x)*4,&depth,4);planes[pixels*4+first+x]=word>>24;
        }else{
            memcpy(planes+(first+x)*4,packed+x*8,4);
            planes[pixels*4+first+x]=packed[x*8+4];
        }
    }
}
void join_depth_stencil(VkFormat format,uint8_t* packed,const uint8_t* planes,size_t pixels,size_t first,size_t count) {
    for(size_t x=0;x<count;x++){
        uint32_t stencil=planes[pixels*4+first+x];
        if(format==VK_FORMAT_D24_UNORM_S8_UINT){
            uint32_t depth;memcpy(&depth,planes+(first+x)*4,4);uint32_t word=(depth&0xffffff)|(stencil<<24);
            memcpy(packed+x*4,&word,4);
        }else{
            memcpy(packed+x*8,planes+(first+x)*4,4);memcpy(packed+x*8+4,&stencil,4);
        }
    }
}
BcRegion bc_region(const Image& image,const view_t* view,const NVNcopyRegion& r){
    if(r.xoffset<0||r.yoffset<0||r.zoffset<0||r.width<=0||r.height<=0||r.depth<=0)ot2_host_die("Invalid BC region");
    return {uint32_t(view&&view->set_levels?view->base_level:0),uint32_t(image.is_3d?0:(view&&view->set_layers?view->min_layer:0)+r.zoffset),uint32_t(r.xoffset),uint32_t(r.yoffset),uint32_t(image.is_3d?r.zoffset:0),uint32_t(r.width),uint32_t(r.height),uint32_t(r.depth)};
}
void copy_to_texture(const p_copy_b2t& copy) {
    int bw,bh;int bytes=nvn_fmt_bpb(copy.tex->d.format,&bw,&bh);
    const auto& region=copy.r;ot2::CopyLayout layout;
    if(!ot2::copy_layout(region.width,region.height,region.depth,bw,bh,bytes,copy_row_stride,copy_image_stride,layout)||
       !ot2::copy_address(copy.src,layout.extent))ot2_host_die("Invalid texture upload layout/address");
    auto& image=image_for(*copy.tex);
    const auto row=layout.row_bytes,rows=layout.rows,stride=layout.row_stride,image_stride=layout.image_stride,pixels=layout.pixels;
    const bool packed=packed_depth_stencil(image);auto bc_info=bc_format(copy.tex->d.format);
    size_t size=layout.tight_bytes;
    if((image.bc||packed)&&!ot2::copy_product(pixels,image.bc?bc_info.pixel_bytes:5,size))ot2_host_die("Texture upload allocation overflow");
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer;check(vkCreateBuffer(device,&bi,nullptr,&buffer),"upload buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;check(vkAllocateMemory(device,&ai,nullptr,&memory),"upload memory");check(vkBindBufferMemory(device,buffer,memory,0),"upload bind");
    void* mapped;check(vkMapMemory(device,memory,0,size,0,&mapped),"map upload");
    ensure_cpu_buffer(copy.src,layout.extent);
    if(image.bc){
        auto r=bc_region(image,copy.has_view?&copy.view:nullptr,region);
        image.bc->write(r,reinterpret_cast<const uint8_t*>(copy.src),stride,image_stride);
        for(unsigned z=0;z<r.depth;z++){
            auto* source=reinterpret_cast<const uint8_t*>(copy.src+z*image_stride);
            auto decoded_row=size_t(r.width)*bc_info.pixel_bytes,decoded_size=decoded_row*r.height;
            bc_decode(copy.tex->d.format,{source,(rows-1)*stride+row},stride,r.width,r.height,{static_cast<uint8_t*>(mapped)+z*decoded_size,decoded_size},decoded_row);
        }
    }else for(uint64_t z=0;z<uint64_t(region.depth);z++)for(uint64_t y=0;y<rows;y++){
        auto* source=reinterpret_cast<const uint8_t*>(copy.src+z*image_stride+y*stride);
        if(packed)split_depth_stencil(image.format,static_cast<uint8_t*>(mapped),pixels,(z*rows+y)*region.width,source,region.width);
        else memcpy(static_cast<uint8_t*>(mapped)+(z*rows+y)*row,source,row);
    }
    vkUnmapMemory(device,memory);
    VkBufferImageCopy bc{};bc.imageSubresource.aspectMask=image.aspects;bc.imageSubresource.mipLevel=copy.has_view && copy.view.set_levels?copy.view.base_level:0;
    bc.imageSubresource.baseArrayLayer=image.is_3d?0:(copy.has_view && copy.view.set_layers?copy.view.min_layer:0)+region.zoffset;
    bc.imageSubresource.layerCount=image.is_3d?1:region.depth;
    bc.imageOffset={region.xoffset,region.yoffset,image.is_3d?region.zoffset:0};bc.imageExtent={uint32_t(region.width),uint32_t(region.height),image.is_3d?uint32_t(region.depth):1};
    auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto regions=transfer_regions(image,bc,pixels);
    vkCmdCopyBufferToImage(cmd,buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,regions.size(),regions.data());finish_commands(cmd);
    auto retire=[buffer,memory]{vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);};if(batch_command)batch_retire.push_back(retire);else retire();
}
void report_counter(uint32_t type,uint64_t address) {
    if(type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP && type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)ot2_host_die("GPU counter type pending: %u",type);
    if(batching){
        if(pending_counters.size()>=query_capacity)flush_commands();
        auto slot=uint32_t(pending_counters.size());
        auto cmd=begin_commands(batch_query_resets);
        if(!batch_query_resets)vkCmdResetQueryPool(cmd,counter_query,slot,1);
        vkCmdWriteTimestamp(cmd,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,counter_query,slot);
        // Timestamp writes are legal inside rendering and already order the
        // requested execution stage; they do not require a memory barrier.
        if(batch_query_resets)++batch_operations;else finish_commands(cmd);
        pending_counters.push_back({address,counter_epoch,type,sample_reset_index,pending_queries});pending_write_ranges.add(address,sizeof(NVNcounterData));return;
    }
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=1;
    VkQueryPool query;check(vkCreateQueryPool(device,&qi,nullptr,&query),"timestamp query");
    auto cmd=begin_commands();vkCmdResetQueryPool(cmd,query,0,1);
    vkCmdWriteTimestamp(cmd,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,query,0);finish_commands(cmd);flush_commands();
    uint64_t value=0;check(vkGetQueryPoolResults(device,query,0,1,8,&value,8,VK_QUERY_RESULT_64_BIT),"GPU timestamp");vkDestroyQueryPool(device,query,nullptr);
    drain_gpu_locked();
    auto* out=reinterpret_cast<NVNcounterData*>(address);out->timestamp=uint64_t(double(value)*timestamp_period);out->counter=type==NVN_COUNTER_TYPE_SAMPLES_PASSED?counter_epoch->samples:out->timestamp;
}
VkImageSubresourceRange subresources(const Image& image,const view_t* view) {
    VkImageSubresourceRange range{image.aspects,0,image.levels,0,image.layers};
    if(view && view->set_levels){range.baseMipLevel=view->base_level;range.levelCount=view->nlevels?view->nlevels:image.levels-range.baseMipLevel;}
    if(view && view->set_layers && !image.is_3d){range.baseArrayLayer=view->min_layer;range.layerCount=view->nlayers?view->nlayers:image.layers-range.baseArrayLayer;}
    return range;
}
void draw_clear(const tex_t&,const view_t*,const NVNcopyRegion&,const void*,int,int,const p_clear_ds*);
// Non-shared storage has exactly the texture's serial as its physical identity.
// Only the packed depth/stencil alias case needs a physical-image lookup.
uint64_t attachment_identity(const tex_t& texture){
    if(fast_image_identity&&!shared_depth_storage(texture))return texture.serial;
    return image_for(texture).identity;
}
struct PendingClear {tex_t texture;VkImageSubresourceRange range;VkClearValue value;};
std::map<uint64_t,PendingClear> pending_clears;
void materialize_clears(uint64_t serial){
    if(fast_image_identity&&pending_clears.empty())return;
    if(serial)serial=image_identity(serial);
    // Remove first: a later CPU dependency may flush while creating resources.
    for(auto it=pending_clears.begin();it!=pending_clears.end();){
        if(serial&&it->first!=serial){++it;continue;}
        auto p=it->second;it=pending_clears.erase(it);auto& image=image_for(p.texture);
        auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if(p.range.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT)vkCmdClearColorImage(cmd,image.image,image.layout,&p.value.color,1,&p.range);
        else vkCmdClearDepthStencilImage(cmd,image.image,image.layout,&p.value.depthStencil,1,&p.range);
        finish_commands(cmd);
    }
}
void defer_clear(const tex_t& texture,VkImageSubresourceRange range,VkClearValue value){
    end_rendering();
    if(auto it=pending_clears.find(image_for(texture).identity);it!=pending_clears.end()){
        auto& previous=it->second;auto a=range,b=previous.range;a.aspectMask=b.aspectMask=0;
        if(memcmp(&a,&b,sizeof(a)))materialize_clears(texture.serial);
        else{
            if(previous.range.aspectMask&range.aspectMask)++elided_clears;
            if(range.aspectMask&VK_IMAGE_ASPECT_COLOR_BIT)previous.value.color=value.color;
            if(range.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT)previous.value.depthStencil.depth=value.depthStencil.depth;
            if(range.aspectMask&VK_IMAGE_ASPECT_STENCIL_BIT)previous.value.depthStencil.stencil=value.depthStencil.stencil;
            previous.range.aspectMask|=range.aspectMask;return;
        }
    }
    pending_clears.emplace(image_for(texture).identity,PendingClear{texture,range,value});
}
bool consume_clear(const tex_t& texture,const view_t* view,VkExtent2D extent,uint32_t layers,VkImageAspectFlags aspect,VkRenderingAttachmentInfo& out){
    auto it=pending_clears.find(image_for(texture).identity);if(it==pending_clears.end())return false;
    const auto& p=it->second;const auto level=view&&view->set_levels?view->base_level:0u;
    const auto layer=view&&view->set_layers?view->min_layer:0u;
    if(texture.d.target==NVN_TEXTURE_TARGET_3D||p.range.baseMipLevel!=level||p.range.levelCount!=1||p.range.baseArrayLayer!=layer||p.range.layerCount!=layers||
       extent.width!=std::max(1U,texture.d.width>>level)||extent.height!=std::max(1U,texture.d.height>>level)||
       (view&&view->set_format&&view->format!=texture.d.format))return false;
    if(!(p.range.aspectMask&aspect))return false;
    out.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;out.clearValue=p.value;++folded_clears;
    it->second.range.aspectMask&=~aspect;if(!it->second.range.aspectMask)pending_clears.erase(it);return true;
}
void clear_texture(const p_clear_tex& clear) {
    auto& image=image_for(*clear.tex);
    if(image.bc)ot2_host_die("Compressed textures cannot be cleared as render targets");
    // Texture clears bypass the render-target clear path entirely, so a frame
    // whose backdrop comes from one shows no CLEAR_COLOR at all and looks like
    // it was painted by a draw. Record which of the two actually happened.
    static std::atomic<uint64_t> clear_tex_probe{0};
    if(clear_tex_probe.fetch_add(1,std::memory_order_relaxed)%32==0)
        ot2_host_log("CLEAR_TEX serial=%llu %ux%u fmt=%u c=%.4g,%.4g,%.4g,%.4g mask=%#x region=%d,%d,%dx%d uint=%d",
            (unsigned long long)clear.tex->serial,clear.tex->d.width,clear.tex->d.height,clear.tex->d.format,
            clear.c.f[0],clear.c.f[1],clear.c.f[2],clear.c.f[3],clear.mask,
            clear.r.xoffset,clear.r.yoffset,clear.r.width,clear.r.height,int(clear.is_uint));
    if(!(clear.mask&15))return;
    auto range=subresources(image,clear.has_view?&clear.view:nullptr);
    const auto w=std::max(1U,clear.tex->d.width>>range.baseMipLevel),h=std::max(1U,clear.tex->d.height>>range.baseMipLevel);
    if((clear.has_view&&clear.view.set_format&&format(clear.view.format)!=image.format)||clear.mask!=15 || image.is_3d || clear.r.xoffset || clear.r.yoffset || uint32_t(clear.r.width)!=w || uint32_t(clear.r.height)!=h){materialize_clears(clear.tex->serial);draw_clear(*clear.tex,clear.has_view?&clear.view:nullptr,clear.r,&clear.c,clear.mask,clear.is_uint,nullptr);return;}
    range.levelCount=1;
    if(!image.is_3d){range.baseArrayLayer+=clear.r.zoffset;range.layerCount=clear.r.depth;}
    if(clear_planning&&batching){VkClearValue value{};memcpy(&value.color,&clear.c,16);defer_clear(*clear.tex,range,value);return;}
    materialize_clears(clear.tex->serial);
    auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue color{};memcpy(&color,&clear.c,16);if(gpu_profile_enabled)gpu_profile_region_begin(cmd,"clear-color");vkCmdClearColorImage(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);gpu_profile_region_end(cmd);finish_commands(cmd);
}
void clear_color(const p_clear_color& clear) {
    if(clear.index<0 || clear.index>=render_targets.n || !render_targets.color[clear.index])return;
    const auto* texture=render_targets.color[clear.index];const auto& view=render_targets.cview[clear.index];
    // Which value ends up as a frame's backdrop is the single question that
    // separates "the game asked for this colour" from "a draw painted it", and
    // the clear colour was invisible in the logs -- so a grey frame could only
    // be argued about, never read off.
    static std::atomic<uint64_t> clear_probe{0};
    if(clear_probe.fetch_add(1,std::memory_order_relaxed)%64==0)
        ot2_host_log("CLEAR_COLOR serial=%llu %ux%u fmt=%u rgba=%.4g,%.4g,%.4g,%.4g mask=%#x scissor=%d,%d,%dx%d uint=%d",
            (unsigned long long)texture->serial,texture->d.width,texture->d.height,texture->d.format,
            clear.rgba[0],clear.rgba[1],clear.rgba[2],clear.rgba[3],clear.mask,
            render_state.scissor.x,render_state.scissor.y,render_state.scissor.w,render_state.scissor.h,int(clear.is_uint));
    p_clear_tex c{};c.tex=texture;c.has_view=render_targets.has_cview[clear.index];c.view=view;c.mask=clear.mask;
    auto level=c.has_view && view.set_levels?view.base_level:0;c.r.width=std::max(1U,texture->d.width>>level);c.r.height=std::max(1U,texture->d.height>>level);c.r.depth=1;
    c.is_uint=clear.is_uint;auto& sc=render_state.scissor;
    c.r.xoffset=std::max(0,sc.x);c.r.yoffset=std::max(0,sc.y);c.r.width=std::max(0,std::min(c.r.width,sc.x+sc.w)-c.r.xoffset);c.r.height=std::max(0,std::min(c.r.height,sc.y+sc.h)-c.r.yoffset);
    if(!c.r.width||!c.r.height)return;
    if(c.has_view&&view.set_layers)c.r.depth=view.nlayers?view.nlayers:texture->L.layers-view.min_layer;
    memcpy(c.c.f,clear.rgba,16);clear_texture(c);
}
void clear_depth(const p_clear_ds& clear) {
    if(!render_targets.depth || (!clear.depth_mask && !clear.stencil_mask))return;
    auto& image=image_for(*render_targets.depth);auto range=subresources(image,render_targets.has_dview?&render_targets.dview:nullptr);
    if(!clear.depth_mask)range.aspectMask&=~VK_IMAGE_ASPECT_DEPTH_BIT;if(!clear.stencil_mask)range.aspectMask&=~VK_IMAGE_ASPECT_STENCIL_BIT;
    if(!range.aspectMask)return;
    const auto* texture=render_targets.depth;auto* view=render_targets.has_dview?&render_targets.dview:nullptr;
    auto& sc=render_state.scissor;int w=std::max(1U,texture->d.width>>range.baseMipLevel),h=std::max(1U,texture->d.height>>range.baseMipLevel);
    if((clear.stencil_mask && clear.stencil_mask!=255) || sc.x>0 || sc.y>0 || sc.x+sc.w<w || sc.y+sc.h<h){
        materialize_clears(texture->serial);NVNcopyRegion region{sc.x,sc.y,0,sc.w,sc.h,int(range.layerCount)};draw_clear(*texture,view,region,nullptr,0,0,&clear);return;}
    range.levelCount=1;
    if(clear_planning&&batching&&!image.is_3d){VkClearValue value{};value.depthStencil={clear.depth,uint32_t(clear.stencil)};defer_clear(*texture,range,value);return;}
    materialize_clears(texture->serial);
    VkClearDepthStencilValue value{clear.depth,uint32_t(clear.stencil)};auto cmd=begin_commands();barrier(cmd,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    if(gpu_profile_enabled)gpu_profile_region_begin(cmd,"clear-depth");vkCmdClearDepthStencilImage(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);gpu_profile_region_end(cmd);finish_commands(cmd);
}
void capture_draw(uint32_t operation,const uint8_t* parameters);
void diagnostic_draw(uint64_t draw,uint32_t operation,const uint8_t* parameters);
static void probe_pixels(const tex_t& texture,uint64_t index,unsigned op);
#include "draw.inc"
#include "cache_maintenance.inc"
#include "gpu_completion.inc"
#include "clear.inc"
#include "presentation.inc"

void capture_draw(uint32_t operation,const uint8_t* parameters) {
    using nlohmann::json;json record;record["operation"]=operation;record["programs"]=json::array();record["targets"]=json::array();record["attributes"]=json::array();
    for(int stage=0;stage<6;stage++)if(auto* p=render_state.programs[stage])for(unsigned i=0;i<p->nstage;i++) {
        // The cache is filed under control section plus code area, so the record
        // has to be named the same way; a control-only key would name a sibling
        // shader (this title has 613 of them).
        uint32_t code_size=0;
        if(p->sd[i].control)memcpy(&code_size,reinterpret_cast<const uint8_t*>(p->sd[i].control)+0x704,4);
        const auto key=shader_key(reinterpret_cast<const void*>(p->sd[i].control),
                                  reinterpret_cast<const void*>(p->sd[i].data),code_size);
        record["programs"].push_back({{"stage",stage},{"key",key}});ot2_host_log("DRAW_SHADER stage=%d key=%s",stage,key.c_str());
    }
    for(int i=0;i<render_targets.n;i++)if(auto* t=render_targets.color[i])record["targets"].push_back({{"width",t->d.width},{"height",t->d.height},{"format",t->d.format},{"serial",t->serial}});
    for(int i=0;i<16;i++){const auto& a=render_state.attributes[i];if(a.fmt)record["attributes"].push_back({{"location",i},{"format",uint32_t(a.fmt)},{"offset",uint32_t(a.offset)},{"stream",uint32_t(a.stream)},{"stride",render_state.streams[a.stream].stride}});}
    if(operation==OP_DRAW){auto& d=*reinterpret_cast<const p_draw*>(parameters);record["draw"]={{"primitive",d.prim},{"first",d.first},{"count",d.count},{"instances",d.instances},{"base_instance",d.base_instance}};}
    if(operation==OP_DRAW_IDX){auto& d=*reinterpret_cast<const p_draw_idx*>(parameters);record["draw"]={{"primitive",d.prim},{"count",d.count},{"instances",d.instances},{"index_type",d.itype}};}
    if(operation==OP_DISPATCH){auto& d=*reinterpret_cast<const p_dispatch*>(parameters);record["dispatch"]={d.x,d.y,d.z};}
    record["resources"]=json::array();
    for(int stage=0;stage<6;stage++)for(int slot=0;slot<128;slot++) {
        uint32_t id=render_state.texture_handles[stage][slot]&0xfffff;
        if(auto it=active_textures().find(id);it!=active_textures().end()){auto& d=it->second;record["resources"].push_back({{"stage",stage},{"slot",slot},{"target",d.has_view && d.view.set_target?d.view.target:d.texture.d.target},{"format",d.has_view && d.view.set_format?d.view.format:d.texture.d.format},{"id",id}});}
    }
    if(trace_draw_completion){
        record["vertex_buffers"]=json::array();
        for(unsigned i=0;i<16;i++){auto& b=render_state.vertex_buffers[i];if(!b.address||!b.size)continue;
            auto size=std::min<uint64_t>(b.size,65536);record["vertex_buffers"].push_back({{"stream",i},{"size",size},{"stride",render_state.streams[i].stride},{"divisor",render_state.streams[i].divisor}});
            std::ofstream(shader_directory+"/trace-v"+std::to_string(i)+".bin",std::ios::binary).write(reinterpret_cast<const char*>(b.address),size);
        }
        for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){auto& sh=stage_shader(stage);
            for(auto& cb:sh.resources->cbufs){auto bank=constant_bank(stage,cb.slot,sh);std::ofstream(shader_directory+"/trace-s"+std::to_string(stage)+"-c"+std::to_string(cb.slot)+".bin",std::ios::binary).write(reinterpret_cast<const char*>(bank.data()),bank.size());}
        }
        record["viewport"]=render_state.viewport;record["scissor"]={render_state.scissor.x,render_state.scissor.y,render_state.scissor.w,render_state.scissor.h};
    }
    std::ofstream(shader_directory+"/first-draw.json")<<record.dump(2);

}
void copy_from_texture(const p_copy_b2t& copy,size_t row_stride,size_t layer_stride) {
    int bw,bh;int bytes=nvn_fmt_bpb(copy.tex->d.format,&bw,&bh);const auto& r=copy.r;ot2::CopyLayout layout;
    if(row_stride>INT64_MAX||layer_stride>INT64_MAX||
       !ot2::copy_layout(r.width,r.height,r.depth,bw,bh,bytes,int64_t(row_stride),int64_t(layer_stride),layout)||
       !ot2::copy_address(copy.src,layout.extent)||(copy.flags&~NVN_COPY_FLAGS_ENGINE_2D_BIT))ot2_host_die("Invalid texture readback layout/address/flags");
    upload_packaged(*copy.tex);auto& image=image_for(*copy.tex);
    const auto row=layout.row_bytes,rows=layout.rows,stride=layout.row_stride,image_stride=layout.image_stride,pixels=layout.pixels;
    const bool packed=packed_depth_stencil(image);size_t size=layout.tight_bytes;
    if(packed&&!ot2::copy_product(pixels,5,size))ot2_host_die("Texture readback allocation overflow");
    if(image.bc){flush_commands();image.bc->read(bc_region(image,copy.has_view?&copy.view:nullptr,r),reinterpret_cast<uint8_t*>(copy.src),stride,image_stride);return;}
    auto buffer=make_buffer(nullptr,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy bc{};bc.bufferOffset=buffer.offset;bc.imageSubresource={image.aspects,uint32_t(copy.has_view&&copy.view.set_levels?copy.view.base_level:0),uint32_t(image.is_3d?0:(copy.has_view&&copy.view.set_layers?copy.view.min_layer:0)+r.zoffset),uint32_t(image.is_3d?1:r.depth)};
    bc.imageOffset={r.xoffset,r.yoffset,image.is_3d?r.zoffset:0};bc.imageExtent={uint32_t(r.width),uint32_t(r.height),uint32_t(image.is_3d?r.depth:1)};
    auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto regions=transfer_regions(image,bc,pixels);
    vkCmdCopyImageToBuffer(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,regions.size(),regions.data());finish_commands(cmd);flush_commands();
    for(size_t z=0;z<size_t(r.depth);z++)for(size_t y=0;y<rows;y++){
        auto* destination=reinterpret_cast<uint8_t*>(copy.src+z*image_stride+y*stride);
        if(packed)join_depth_stencil(image.format,destination,static_cast<const uint8_t*>(buffer.mapped),pixels,(z*rows+y)*r.width,r.width);
        else memcpy(destination,static_cast<const uint8_t*>(buffer.mapped)+(z*rows+y)*row,row);
    }
    destroy_buffer(buffer);static unsigned readbacks=0;if(++readbacks<=3)ot2_host_log("GPU_TEXTURE_READBACK serial=%llu bytes=%zu",(unsigned long long)copy.tex->serial,row*rows*r.depth);
}
void diagnostic_draw(uint64_t draw,uint32_t operation,const uint8_t* parameters) {
    if(!capture_frame)return;
    int target=0;
    if(capture_frame_number!=5){
        if(presented_frames.load()!=capture_frame_number)return;
        static uint64_t frame=0;static std::set<std::string> captured,selected;
        if(frame!=capture_frame_number){frame=capture_frame_number;captured.clear();selected.clear();std::ifstream in(shader_directory+"/capture-select");std::string key;while(in>>key)selected.insert(key);}
        const std::string dir=shader_directory+"/frame"+std::to_string(capture_frame_number);std::filesystem::create_directories(dir);
        Json record={{"draw",draw},{"shaders",Json::array()},{"targets",Json::array()}};std::string fragment;
        for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){auto key=shader_key(stage_data(stage).control);record["shaders"].push_back({{"stage",stage},{"key",key}});if(stage==1)fragment=key;}
        std::ofstream(dir+"/draws.jsonl",std::ios::app)<<record.dump()<<'\n';
        if(render_targets.n>1){if(captured.contains(fragment)||(selected.empty()?captured.size()>=8:!selected.contains(fragment)))return;captured.insert(fragment);}
    }
    const auto* t=render_targets.n?render_targets.color[target]:nullptr;if(!t)return;
    auto& img=image_for(*t);if(img.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || (presented_frames.load()!=capture_frame_number && !(img.is_3d && presented_frames.load()<5 && capture_frame_number==5)))return;
    const auto* view=render_targets.has_cview[target]?&render_targets.cview[target]:nullptr;
    unsigned level=view&&view->set_levels?view->base_level:0,w=std::max(1U,t->d.width>>level),h=std::max(1U,t->d.height>>level);
    unsigned z=img.is_3d?std::max(1U,t->d.depth>>level):1;
    int bw,bh,bytes=nvn_fmt_bpb(t->d.format,&bw,&bh);if(bw!=1 || bh!=1 || size_t(w)*h*z*bytes>16*1024*1024)return;
    const size_t capture_size=size_t(w)*h*z*bytes;if(capture_bytes+capture_size>512*1024*1024)return;capture_bytes+=capture_size;
    auto b=make_buffer(nullptr,capture_size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy c{};c.bufferOffset=b.offset;c.imageSubresource={img.aspects,level,uint32_t(!img.is_3d&&view&&view->set_layers?view->min_layer:0),1};c.imageExtent={w,h,z};
    // Everything recorded so far has to reach the GPU before this image can be
    // copied: under batching the draws above are only recorded, and the image is
    // still a live render attachment. Transitioning an attachment mid-pass and
    // copying it is not legal, and the copy then returns whatever the image held
    // before -- which is why every captured scene target read back as the clear
    // colour and was mistaken for "the draw produced nothing".
    flush_commands();
    auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cmd,img.image,img.layout,b.buffer,1,&c);finish_commands(cmd);flush_commands();
    std::string dir=shader_directory+"/frame"+std::to_string(capture_frame_number);std::filesystem::create_directories(dir);auto path=dir+"/"+std::to_string(draw);
    std::ofstream(path+".raw",std::ios::binary).write(static_cast<const char*>(b.mapped),size_t(w)*h*z*bytes);destroy_buffer(b);
    Json meta={{"draw",draw},{"width",w},{"height",h},{"depth",z},{"format",t->d.format},{"serial",t->serial},{"shaders",Json::array()},{"textures",Json::array()}};
    for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){meta["shaders"].push_back({{"stage",stage},{"key",shader_key(stage_data(stage).control)}});for(int slot=0;slot<128;slot++){unsigned id=render_state.texture_handles[stage][slot]&0xfffff;auto it=active_textures().find(id);if(id&&it!=active_textures().end())meta["textures"].push_back({{"stage",stage},{"slot",slot},{"serial",it->second.texture.serial},{"format",it->second.texture.d.format}});}}
    for(int stage=0;stage<5;stage++)if(render_state.programs[stage]) {
        auto& sd=stage_data(stage);auto& sh=stage_shader(stage);std::string k=shader_key(sd.control);
        for(auto& cb:sh.resources->cbufs){int slot=cb.slot;auto bank=constant_bank(stage,slot,sh);std::ofstream(path+"-"+k+"-c"+std::to_string(slot)+".bin",std::ios::binary).write(reinterpret_cast<const char*>(bank.data()),bank.size());}
        for(auto& r:sh.resources->textures){auto handle=packed_handle(r,stage,sh);auto it=active_textures().find(handle&0xfffff);if(it==active_textures().end())continue;auto& desc=it->second;auto& tex=desc.texture;if(tex.d.target==NVN_TEXTURE_TARGET_BUFFER){int bw,bh,bytes=nvn_fmt_bpb(tex.d.format,&bw,&bh);size_t size=size_t(tex.d.width)*bytes;if(size<=16*1024*1024&&tex.mem){ensure_cpu_buffer(uintptr_t(tex.mem),size);std::ofstream(path+"-s"+std::to_string(stage)+"-t"+std::to_string(r.binding)+".bin",std::ios::binary).write(reinterpret_cast<const char*>(tex.mem),size);}continue;}
            auto f=dir+"/texture-"+std::to_string(tex.serial);if(std::filesystem::exists(f+".raw"))continue;
            auto& image=image_for(tex);int raw_format=image.bc?bc_format(tex.d.format).unpacked:tex.d.format;int bw,bh,bytes=nvn_fmt_bpb(raw_format,&bw,&bh);if(bw!=1 || bh!=1)continue;bool packed=packed_depth_stencil(image);if(packed)bytes=5;
            unsigned z=image.is_3d?tex.d.depth:image.layers;size_t size=size_t(tex.d.width)*tex.d.height*z*bytes;if(size>16*1024*1024||capture_bytes+size>512*1024*1024)continue;capture_bytes+=size;
            auto buffer=make_buffer(nullptr,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy c{};c.bufferOffset=buffer.offset;c.imageSubresource={image.aspects,0,0,image.is_3d?1:image.layers};c.imageExtent={tex.d.width,tex.d.height,image.is_3d?z:1};auto regions=transfer_regions(image,c,size_t(tex.d.width)*tex.d.height*z);auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cmd,image.image,image.layout,buffer.buffer,regions.size(),regions.data());finish_commands(cmd);flush_commands();
            std::ofstream(f+".raw",std::ios::binary).write(static_cast<const char*>(buffer.mapped),size);destroy_buffer(buffer);Json info={{"width",tex.d.width},{"height",tex.d.height},{"depth",z},{"format",raw_format},{"base_format",tex.d.format},{"packed_planes",packed},{"vk_format",int(image.format)},{"serial",tex.serial}};std::ofstream(f+".json")<<info.dump(2);
        }
    }
    meta["attachment_format"]=view&&view->set_format?view->format:t->d.format;
    meta["target_index"]=target;meta["target_count"]=render_targets.n;
    meta["attributes"]=Json::array();for(unsigned i=0;i<16;i++){auto& a=render_state.attributes[i];if(a.fmt)meta["attributes"].push_back({{"location",i},{"format",uint32_t(a.fmt)},{"offset",uint32_t(a.offset)},{"stream",uint32_t(a.stream)},{"stride",render_state.streams[a.stream].stride}});}
    for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){auto& shader=stage_shader(stage);record_shader_facts(stage,stage_data(stage),true);for(auto& ref:shader.resources->textures){auto handle=packed_handle(ref,stage,shader);auto descriptor=active_textures().find(handle&0xfffff);if(descriptor==active_textures().end())continue;auto& tex=descriptor->second.texture;meta["storage"].push_back({{"serial",tex.serial},{"physical",image_identity(tex.serial)},{"backing",uintptr_t(tex.mem)},{"flags",tex.d.flags},{"packaged",tex.d.pkg_block_h},{"levels",tex.d.levels},{"width",tex.d.width},{"height",tex.d.height},{"depth",tex.d.depth}});}}

    if(render_targets.depth)meta["depth_attachment"]={{"serial",render_targets.depth->serial},{"backing",uintptr_t(render_targets.depth->mem)},{"format",render_targets.depth->d.format}};
    meta["viewport"]=render_state.viewport;meta["scissor"]={render_state.scissor.x,render_state.scissor.y,render_state.scissor.w,render_state.scissor.h};
    // The colour write mask decides whether a draw's fragment output reaches the
    // target at all, and it was missing from the record -- so an emulated prepass
    // that left it at zero looked identical to a draw that produced nothing.
    meta["channel_mask"]=render_state.channel_mask.mask;
    meta["depth_test"]=uint32_t(render_state.depth_stencil.dt);
    meta["depth_write"]=uint32_t(render_state.depth_stencil.dw);
    meta["depth_func"]=uint32_t(render_state.depth_stencil.df);
    auto& poly=render_state.polygon;auto& ds=render_state.depth_stencil;auto& db=render_state.depth_bounds;auto& bl=render_state.blends[0];
    meta["raster"]={{"cull",uint32_t(poly.cull)},{"front",uint32_t(poly.front)},{"depth_test",uint32_t(ds.dt)},{"depth_write",uint32_t(ds.dw)},{"depth_func",uint32_t(ds.df)},{"stencil",uint32_t(ds.st)},{"bounds",{db.enable,db.minimum,db.maximum}},{"blend_enable",uint32_t(render_state.color.blend_en)},{"blend",{uint32_t(bl.src_rgb),uint32_t(bl.dst_rgb),uint32_t(bl.eq_rgb)}}};
    if(operation==OP_DRAW){auto& d=*reinterpret_cast<const p_draw*>(parameters);meta["draw_args"]={d.prim,d.first,d.count,d.instances,d.base_instance};}
    if(operation==OP_DRAW_IDX){auto& d=*reinterpret_cast<const p_draw_idx*>(parameters);meta["draw_args"]={d.prim,d.count,d.instances,d.itype};}
    for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){auto& sh=stage_shader(stage);for(auto& r:sh.resources->sbufs){auto b=storage_binding(stage,r,sh);if(b.address&&b.size&&b.size<=16*1024*1024){ensure_cpu_buffer(b.address,b.size);auto f=path+"-s"+std::to_string(stage)+"-b"+std::to_string(r.binding)+".bin";std::ofstream(f,std::ios::binary).write(reinterpret_cast<const char*>(b.address),b.size);}}}

    for(int stage=0;stage<5;stage++)if(render_state.programs[stage]){auto& sh=stage_shader(stage);for(auto& r:sh.resources->textures){auto handle=packed_handle(r,stage,sh);auto it=active_textures().find(handle&0xfffff);if(it!=active_textures().end()){auto& d=it->second;meta["bound_textures"].push_back({{"stage",stage},{"binding",r.binding},{"handle",handle},{"serial",d.texture.serial},{"backing",uintptr_t(d.texture.mem)},{"depth_mode",d.has_view&&d.view.set_depth_mode?d.view.depth_mode:d.texture.d.depth_stencil_mode},{"swizzle",d.has_view&&d.view.set_swizzle?d.view.swizzle:(d.texture.d.swizzle[0]|(d.texture.d.swizzle[1]<<3)|(d.texture.d.swizzle[2]<<6)|(d.texture.d.swizzle[3]<<9))},{"format",d.has_view&&d.view.set_format?d.view.format:d.texture.d.format}});}}}
    std::ofstream(path+".json")<<meta.dump(2);
}
void copy_texture(const p_copy_t2t& c) {
    upload_packaged(*c.src);auto& src=image_for(*c.src);auto& dst=image_for(*c.dst);
    if(src.bc||dst.bc){
        if(!src.bc||!dst.bc||c.src->d.format!=c.dst->d.format||c.sr.width!=c.dr.width||c.sr.height!=c.dr.height||c.sr.depth!=c.dr.depth||(c.flags&~NVN_COPY_FLAGS_ENGINE_2D_BIT))ot2_host_die("Unsupported BC reinterpretation/scaled copy");
        auto info=bc_format(c.src->d.format);size_t row=size_t((c.sr.width+3)/4)*info.block_bytes,rows=(c.sr.height+3)/4;
        std::vector<uint8_t> encoded(row*rows*c.sr.depth);
        src.bc->read(bc_region(src,c.has_sv?&c.sv:nullptr,c.sr),encoded.data());
        p_copy_b2t upload{};upload.tex=c.dst;upload.has_view=c.has_dv;upload.view=c.dv;upload.r=c.dr;upload.src=reinterpret_cast<uint64_t>(encoded.data());
        const auto old_row=copy_row_stride,old_image=copy_image_stride;copy_row_stride=copy_image_stride=0;
        copy_to_texture(upload);copy_row_stride=old_row;copy_image_stride=old_image;dst.drawn=src.drawn;return;
    }
    if(src.aspects!=dst.aspects || src.is_3d!=dst.is_3d)ot2_host_die("Cross dimensional/aspect texture copy pending");
    auto region=[&](const Image& img,const view_t& v,bool has,const NVNcopyRegion& r) {
        return VkImageSubresourceLayers{img.aspects,uint32_t(has&&v.set_levels?v.base_level:0),uint32_t(img.is_3d?0:(has&&v.set_layers?v.min_layer:0)+r.zoffset),uint32_t(img.is_3d?1:r.depth)};
    };
    auto ss=region(src,c.sv,c.has_sv,c.sr),ds=region(dst,c.dv,c.has_dv,c.dr);
    if(!src.is_3d && c.sr.depth!=c.dr.depth)ot2_host_die("Layer count mismatch in texture copy");
    auto cmd=begin_commands();barrier(cmd,src,src.layout,&src==&dst?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(cmd,dst,dst.layout,&src==&dst?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if(src.samples>VK_SAMPLE_COUNT_1_BIT && dst.samples==VK_SAMPLE_COUNT_1_BIT) {
        if(src.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || src.format!=dst.format || c.sr.width!=c.dr.width || c.sr.height!=c.dr.height || c.sr.depth!=c.dr.depth)ot2_host_die("Invalid multisample color resolve");
        VkImageResolve resolve{};resolve.srcSubresource=ss;resolve.dstSubresource=ds;resolve.srcOffset={c.sr.xoffset,c.sr.yoffset,0};resolve.dstOffset={c.dr.xoffset,c.dr.yoffset,0};resolve.extent={uint32_t(c.sr.width),uint32_t(c.sr.height),1};
        vkCmdResolveImage(cmd,src.image,src.layout,dst.image,dst.layout,1,&resolve);
    } else if(src.format==dst.format && c.sr.width==c.dr.width && c.sr.height==c.dr.height && c.sr.depth==c.dr.depth && !(c.flags&0x1d)) {
        VkImageCopy copy{};copy.srcSubresource=ss;copy.dstSubresource=ds;copy.srcOffset={c.sr.xoffset,c.sr.yoffset,src.is_3d?c.sr.zoffset:0};copy.dstOffset={c.dr.xoffset,c.dr.yoffset,dst.is_3d?c.dr.zoffset:0};copy.extent={uint32_t(c.sr.width),uint32_t(c.sr.height),uint32_t(src.is_3d?c.sr.depth:1)};
        vkCmdCopyImage(cmd,src.image,src.layout,dst.image,dst.layout,1,&copy);
    } else {
        VkImageBlit b{};b.srcSubresource=ss;b.dstSubresource=ds;b.srcOffsets[0]={c.sr.xoffset,c.sr.yoffset,src.is_3d?c.sr.zoffset:0};b.srcOffsets[1]={c.sr.xoffset+c.sr.width,c.sr.yoffset+c.sr.height,src.is_3d?c.sr.zoffset+c.sr.depth:1};b.dstOffsets[0]={c.dr.xoffset,c.dr.yoffset,dst.is_3d?c.dr.zoffset:0};b.dstOffsets[1]={c.dr.xoffset+c.dr.width,c.dr.yoffset+c.dr.height,dst.is_3d?c.dr.zoffset+c.dr.depth:1};
        if(c.flags&4)std::swap(b.srcOffsets[0].x,b.srcOffsets[1].x);if(c.flags&8)std::swap(b.srcOffsets[0].y,b.srcOffsets[1].y);if(c.flags&16)std::swap(b.srcOffsets[0].z,b.srcOffsets[1].z);
        vkCmdBlitImage(cmd,src.image,src.layout,dst.image,dst.layout,1,&b,c.flags&1?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    }
    finish_commands(cmd);dst.drawn=src.drawn;
}

// A draw whose state is legal and whose program compiled can still write no
// pixel. Reading back a few texels after each command of one traced frame
// separates "this command painted" from "this command did nothing", which no
// amount of state inspection can decide.
static void probe_pixels(const tex_t& texture,uint64_t index,unsigned op) {
    auto& img=image_for(texture);
    if(img.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)return;
    int bw,bh,bytes=nvn_fmt_bpb(texture.d.format,&bw,&bh);
    const unsigned w=texture.d.width,h=texture.d.height;
    if(bw!=1||bh!=1||w<16||h<16)return;
    // Four points in the top-left eighth cannot tell "wrote nothing" apart from
    // "wrote somewhere else". A coarse 8x6 grid over the whole target can: the
    // map spells out where the content landed, and the legend names the colour.
    constexpr int gx=8,gy=6;constexpr int n=gx*gy;
    auto buffer=make_buffer(nullptr,size_t(n)*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    flush_commands();
    auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    for(int i=0;i<n;i++){
        const unsigned sx=std::min(w-1,unsigned((size_t(w)*(i%gx*2+1))/(2*gx)));
        const unsigned sy=std::min(h-1,unsigned((size_t(h)*(i/gx*2+1))/(2*gy)));
        VkBufferImageCopy copy{};copy.bufferOffset=buffer.offset+size_t(i)*bytes;
        copy.imageSubresource={img.aspects,0,0,1};
        copy.imageOffset={int(sx),int(sy),0};copy.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(cmd,img.image,img.layout,buffer.buffer,1,&copy);
    }
    finish_commands(cmd);flush_commands();
    const auto* p=static_cast<const uint8_t*>(buffer.mapped);
    unsigned legend[16]{};int used=0;char rows[gy][gx+1];memset(rows,0,sizeof rows);
    for(int i=0;i<n;i++){
        unsigned c=0;for(int b=0;b<bytes && b<3;b++)c|=unsigned(p[i*bytes+b])<<(8*b);
        int slot=-1;for(int k=0;k<used;k++)if(legend[k]==c){slot=k;break;}
        if(slot<0){if(used<15){legend[used]=c;slot=used++;}else slot=15;}
        rows[i/gx][i%gx]=char('0'+slot);
    }
    char map[gy*(gx+1)+1]={0};size_t o=0;
    for(int y=0;y<gy;y++){if(y)map[o++]='/';memcpy(map+o,rows[y],gx);o+=gx;}
    char leg[16*9+1]={0};size_t lo=0;
    for(int k=0;k<used;k++)lo+=size_t(snprintf(leg+lo,sizeof leg-lo,"%d=%06x ",k,legend[k]));
    ot2_host_log("PIXMAP n=%llu op=%u rt=%llu %ux%u %s",(unsigned long long)index,op,
        (unsigned long long)texture.serial,w,h,map);
    ot2_host_log("PIXMAP legend %s",leg);
    destroy_buffer(buffer);
}

void replay(const uint8_t* command,unsigned depth) {
    if(depth>32)ot2_host_die("Command call depth exceeded");
    for(unsigned count=0;count<1000000;count++) {
        const auto* h=reinterpret_cast<const op_hdr*>(command);const auto* p=command+8;
        if(h->size<8 || h->size%8 || h->size>16*1024*1024)ot2_host_die("Corrupt graphics command: op=%u size=%u",h->op,h->size);
        if(!indexed_commands.empty()&&h->op!=OP_CALL&&h->op!=OP_JUMP&&h->op!=OP_END){
            if(indexed_cursor<indexed_commands.size()&&indexed_commands[indexed_cursor]==command)++indexed_cursor;
            else {indexed_commands={};++indexed_fallbacks;}
        }
        // Invalidate on memory/layout mutations and execution boundaries. Pure
        // fixed-function state and vertex changes do not alter shader bindings.
        switch(h->op){
        case OP_DRAW:case OP_DRAW_IDX:case OP_DISPATCH:case OP_BIND_PROGRAM:
        case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:
        case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:case OP_VIEWPORTS:
        case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
        case OP_SCISSOR:case OP_DEPTH_BOUNDS:case OP_SAMPLE_MASK:case OP_RASTER_DISCARD:
        case OP_DEPTH_CLAMP:case OP_RENDER_ENABLE:case OP_LINE_WIDTH:case OP_DEPTH_RANGE:
        case OP_POLY_OFFSET:case OP_BLEND_COLOR:case OP_STENCIL:
        case OP_CLEAR_COLOR:case OP_CLEAR_DS:case OP_CLEAR_TEX:case OP_SET_RT:break;
        default:materialize_clears();reset_prepared_bindings();break;
        }
        switch(h->op) {
        case OP_END:return;
        case OP_JUMP:command=*reinterpret_cast<const uint8_t*const*>(p);continue;
        case OP_CALL:replay(*reinterpret_cast<const uint8_t*const*>(p),depth+1);break;
        case OP_BEGIN:break;
        case OP_SET_TEX_POOL:{auto* pool=reinterpret_cast<const p_set_pool*>(p)->pool;auto key=descriptor_pool_key(pool);auto capacity=uint32_t(std::max(0,pool->n));
            if(active_texture_pool!=key||active_texture_capacity!=capacity)reset_prepared_bindings();active_texture_pool=key;active_texture_capacity=capacity;break;}
        case OP_SET_SAMP_POOL:{auto* pool=reinterpret_cast<const p_set_pool*>(p)->pool;auto key=descriptor_pool_key(pool);auto capacity=uint32_t(std::max(0,pool->n));if(active_sampler_pool!=key||active_sampler_capacity!=capacity)reset_prepared_bindings();active_sampler_pool=key;active_sampler_capacity=capacity;break;}
        case OP_BARRIER:{if(batch_command){auto cmd=begin_commands();finish_commands(cmd);}break;}
        case OP_RESET_COUNTER:{auto type=*reinterpret_cast<const uint64_t*>(p);if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)ot2_host_die("Counter reset type pending: %llu",(unsigned long long)type);if(!batch_query_resets)flush_commands();sample_intervals.push_back({counter_epoch,sample_reset_index,pending_queries});counter_epoch=std::make_shared<CounterEpoch>();sample_reset_index=pending_queries;samples_active=true;break;}
        case OP_REPORT_COUNTER:{auto* v=reinterpret_cast<const uint64_t*>(p);report_counter(v[0],v[1]);break;}
        case OP_CLEAR_TEX:reset_prepared_bindings();clear_texture(*reinterpret_cast<const p_clear_tex*>(p));break;
        case OP_SIGNAL_EVENT:{flush_commands();auto* out=*reinterpret_cast<uint32_t*const*>(p);const auto* v=reinterpret_cast<const uint32_t*>(p+8);if(v[0])__atomic_fetch_add(out,v[1],__ATOMIC_RELEASE);else __atomic_store_n(out,v[1],__ATOMIC_RELEASE);break;}
        case OP_COPY_B2T:copy_to_texture(*reinterpret_cast<const p_copy_b2t*>(p));break;
        case OP_COPY_STRIDE:copy_row_stride=reinterpret_cast<const p_copy_stride*>(p)->stride;break;
        case OP_COPY_IMAGE_STRIDE:copy_image_stride=reinterpret_cast<const p_copy_stride*>(p)->stride;break;
        // Draw inputs are immutable snapshots. Only overlapping GPU mirrors
        // need completion/writeback before these CPU reads and mutations.
        // A CPU mutation therefore invalidates future snapshots without waiting
        // for unrelated draws which still own their previous bytes.
        case OP_COPY_B2B:{readonly_snapshots.clear();const auto* v=reinterpret_cast<const uint64_t*>(p);ensure_cpu_buffer(v[0],v[2]);ensure_cpu_buffer(v[1],v[2]);memmove(reinterpret_cast<void*>(v[1]),reinterpret_cast<const void*>(v[0]),v[2]);break;}
        case OP_CLEAR_BUFFER:{readonly_snapshots.clear();const auto* v=reinterpret_cast<const uint64_t*>(p);if((v[0]|v[1])&3)ot2_host_die("Unaligned buffer clear");ensure_cpu_buffer(v[0],v[1]);auto* out=reinterpret_cast<uint32_t*>(v[0]);std::fill_n(out,v[1]/4,uint32_t(v[2]));break;}
        case OP_UPDATE_UB:{readonly_snapshots.clear();const auto* v=reinterpret_cast<const uint64_t*>(p);if(v[1]>h->size-24)ot2_host_die("Uniform command size invalid");
            if(trace_uniform_banks){static std::atomic<uint64_t> seen{0};const uint64_t n=seen.fetch_add(1,std::memory_order_relaxed);
                if(n<400)ot2_host_log("UPDATE_UB addr=%llx size=%llu first=%08x n=%llu",
                    (unsigned long long)v[0],(unsigned long long)v[1],
                    unsigned(p[24])|unsigned(p[25])<<8|unsigned(p[26])<<16|unsigned(p[27])<<24,(unsigned long long)n);}
            ensure_cpu_buffer(v[0],v[1]);memcpy(reinterpret_cast<void*>(v[0]),p+16,v[1]);break;}
        case OP_FENCE:{break;} // Public completion is the enclosing submission's real GPU completion.
        case OP_WAIT_SYNC:{flush_commands();auto* f=*reinterpret_cast<const sync_t*const*>(p);auto ticket=__atomic_load_n(&f->fence,__ATOMIC_ACQUIRE);if(!__atomic_load_n(&f->fenced,__ATOMIC_ACQUIRE))ot2_host_die("Command wait on unsignaled sync");if(!(ticket&present_ticket_bit)&&ticket>replay_sequence)ot2_host_die("Command wait on future sync");break;}
        case OP_COPY_T2B:copy_from_texture(*reinterpret_cast<const p_copy_b2t*>(p),copy_row_stride,copy_image_stride);break;
        case OP_COPY_T2T:copy_texture(*reinterpret_cast<const p_copy_t2t*>(p));break;
        case OP_SET_RT:reset_prepared_bindings();bind_render_targets(*reinterpret_cast<const p_set_rt*>(p));break;
        case OP_CLEAR_COLOR:reset_prepared_bindings();clear_color(*reinterpret_cast<const p_clear_color*>(p));break;
        case OP_CLEAR_DS:reset_prepared_bindings();clear_depth(*reinterpret_cast<const p_clear_ds*>(p));break;
        case OP_VIEWPORTS:{auto* v=reinterpret_cast<const p_viewports*>(p);if(v->first==0 && v->n>0)update_binding_bytes(render_state.viewport,v->v[0],4*sizeof(float));break;}
        case OP_SCISSOR:render_state.scissor=*reinterpret_cast<const p_scissor*>(p);break;
        case OP_DEPTH_BOUNDS:{auto bounds=*reinterpret_cast<const p_depth_bounds*>(p);if(dynamic_depth_bounds){if(render_state.depth_bounds.enable!=bounds.enable)++graphics_state_revision;render_state.depth_bounds=bounds;}else update_pipeline_value(render_state.depth_bounds,bounds);break;}
        case OP_SAMPLE_MASK:update_pipeline_value(render_state.sample_mask,*reinterpret_cast<const uint32_t*>(p));break;
        case OP_RASTER_DISCARD:update_pipeline_value(render_state.raster_discard,*reinterpret_cast<const uint32_t*>(p));break;
        case OP_DEPTH_CLAMP:update_pipeline_value(render_state.depth_clamp,*reinterpret_cast<const uint32_t*>(p));break;
        case OP_RENDER_ENABLE:
            // An unconditional setting supersedes any pending condition, exactly as
            // it would on the device: the last command recorded is the one in force.
            render_state.render_enable=*reinterpret_cast<const uint32_t*>(p);
            render_state.render_cond_mode=-1;break;
        case OP_RENDER_COND:{auto* v=reinterpret_cast<const p_render_cond*>(p);
            render_state.render_cond_mode=v->mode;render_state.render_cond_addr=v->addr;break;}
        case OP_LINE_WIDTH:update_pipeline_value(render_state.line_width,*reinterpret_cast<const float*>(p));break;
        case OP_DEPTH_RANGE:{auto* v=reinterpret_cast<const p_depth_range*>(p);render_state.viewport[4]=v->n;render_state.viewport[5]=v->f;break;}
        case OP_BIND_PROGRAM:{auto* v=reinterpret_cast<const p_bind_program*>(p);
            // The stage argument is a mask, and titles commonly pass
            // NVN_SHADER_STAGE_ALL_GRAPHICS_BITS (31) for a program that provides
            // only a vertex and a fragment shader. That is legal: the mask says
            // which stages this program is being assigned to, and a stage it has
            // no shader for simply ends up empty. Binding the program there
            // instead would leave a stage that reports itself absent at draw
            // time, which drops the whole draw -- the symptom is an indexed draw
            // being skipped on geometry while the program has only two stages.
            for(int i=0;i<6;i++)if(v->stages&(1<<i)){
                auto version=v->prog?nvn_program_get(v->prog->serial):std::shared_ptr<const NvnProgramVersion>();
                const bool provided=version&&version->stages[i]>=0;
                if(provided&&v->prog&&bound_programs[i]&&bound_programs[i]->value.serial==v->prog->serial)continue;
                bound_programs[i]=provided?version:nullptr;
                render_state.programs[i]=provided?&bound_programs[i]->value:nullptr;
                bound_shaders[i]=nullptr;++graphics_state_revision;++resource_generation;change_resource_groups(i,15);
            }break;}
        case OP_BIND_STATE:{auto* v=reinterpret_cast<const p_bind_state*>(p);auto* b=v->bytes;
            switch(v->kind){case 0:{auto& blend=*reinterpret_cast<const blend_st*>(b);update_pipeline_value(render_state.blends[blend.target],blend);break;}case 1:update_pipeline_bytes(&render_state.color,b,sizeof(color_st));break;case 2:update_pipeline_bytes(&render_state.channel_mask,b,sizeof(chmask_st));break;case 3:update_pipeline_bytes(&render_state.multisample,b,sizeof(ms_st));break;case 4:update_pipeline_bytes(&render_state.polygon,b,sizeof(poly_st));break;case 5:update_pipeline_bytes(&render_state.depth_stencil,b,sizeof(ds_st));break;default:ot2_host_die("Unknown pipeline state");}break;}
        case OP_BIND_VATTR:{auto* v=reinterpret_cast<const p_bind_vattr*>(p);update_pipeline_bytes(render_state.attributes+v->first,v->st,v->n*sizeof(va_st));break;}
        case OP_BIND_VSTREAM:{auto* v=reinterpret_cast<const p_bind_vstream*>(p);update_pipeline_bytes(render_state.streams+v->first,v->st,v->n*sizeof(vs_st));break;}
        case OP_BIND_VBUF:{auto* v=reinterpret_cast<const p_bind_vbuf*>(p);memcpy(render_state.vertex_buffers+v->first,v->r,v->n*sizeof(NVNbufferRange));break;}
        case OP_BIND_UBO:case OP_BIND_SSBO:{auto* v=reinterpret_cast<const p_bind_buf*>(p);if(v->stage<0 || v->stage>=6 || v->index<0 || v->index>=16)ot2_host_die("Invalid buffer binding");auto& dst=(h->op==OP_BIND_UBO?render_state.uniforms:render_state.storage)[v->stage][v->index];
            // Every address the title binds for a slot, in order. A bank whose
            // declared size never covers the shader's struct is either bound in
            // pieces or read past its end, and those two need opposite fixes; the
            // sequence of addresses tells which one this is.
            if(trace_uniform_banks&&h->op==OP_BIND_UBO){
                static std::atomic<uint64_t> seen{0};
                const uint64_t n=seen.fetch_add(1,std::memory_order_relaxed);
                if(n<400)ot2_host_log("BIND_UBO st=%d idx=%d addr=%llx size=%llu n=%llu",
                    v->stage,v->index,(unsigned long long)v->addr,(unsigned long long)v->size,(unsigned long long)n);}
            if(update_binding_bytes(&dst,v,sizeof(*v))){change_resource_bank(v->stage,h->op==OP_BIND_UBO?3+v->index:0);if(h->op==OP_BIND_SSBO)++bank_generation[v->stage][0];}break;}
        case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:{auto* v=reinterpret_cast<const p_bind_handles*>(p);if(v->stage<0 || v->stage>=6 || v->first<0 || v->n<0 || v->first+v->n>128)ot2_host_die("Invalid image binding");auto& handles=h->op==OP_BIND_TEX?render_state.texture_handles:h->op==OP_BIND_SAMP?render_state.sampler_handles:render_state.image_handles;if(update_binding_bytes(handles[v->stage]+v->first,v->h,v->n*8)){++bank_generation[v->stage][1];change_resource_bank(v->stage,2);}break;}
        case OP_POLY_OFFSET:update_pipeline_value(render_state.depth_bias,*reinterpret_cast<const p_poly_offset*>(p));break;
        case OP_BLEND_COLOR:update_pipeline_bytes(render_state.blend_color,p,16);break;
        case OP_STENCIL:{auto& v=*reinterpret_cast<const p_stencil*>(p);for(int i=0;i<2;i++)if(v.face==NVN_FACE_FRONT_AND_BACK || v.face==(i?NVN_FACE_BACK:NVN_FACE_FRONT)){if(v.which==0)update_pipeline_value(render_state.stencil_compare_mask[i],uint32_t(v.value_mask));else if(v.which==1)update_pipeline_value(render_state.stencil_write_mask[i],uint32_t(v.mask));else update_pipeline_value(render_state.stencil_reference[i],uint32_t(v.ref));}break;}
        case OP_DRAW:case OP_DRAW_IDX:case OP_DISPATCH:replay_next_command=command+h->size;execute_draw(h->op,p);replay_next_command=nullptr;break;
        default:ot2_host_die("Vulkan replay operation pending: %u",h->op);
        }
        if(trace_ops_frame&&presented_frames.load()+1==trace_ops_frame){
            if(h->op<64)++trace_op_hist[h->op];
            if(h->op==OP_SET_RT||h->op==OP_CLEAR_TEX||h->op==OP_CLEAR_COLOR||h->op==OP_COPY_T2T||
               h->op==OP_DRAW||h->op==OP_DRAW_IDX||h->op==OP_DISPATCH||h->op==OP_END){
                const unsigned long long target=render_targets.n&&render_targets.color[0]?render_targets.color[0]->serial:0;
                if(h->op==OP_SET_RT){
                    // The recorded payload, not the applied state: a draw with no
                    // target can only be told apart from a state-tracking slip by
                    // seeing what the title actually asked for on this command.
                    const auto& rt=*reinterpret_cast<const p_set_rt*>(p);
                    char names[160]={0};size_t used=0;
                    for(int i=0;i<rt.n && i<8;++i)
                        used+=size_t(snprintf(names+used,sizeof names-used," %llu%s",
                            (unsigned long long)(rt.color[i]?rt.color[i]->serial:0),rt.has_cview[i]?"v":""));
                    ot2_host_log("OP_TRACE n=%llu op=8 set_rt n=%d%s depth=%llu applied=%d",
                        (unsigned long long)trace_op_index,rt.n,names,
                        (unsigned long long)(rt.depth?rt.depth->serial:0),render_targets.n);
                } else if(h->op==OP_COPY_T2T){
                    const auto& copy=*reinterpret_cast<const p_copy_t2t*>(p);
                    ot2_host_log("OP_TRACE n=%llu op=36 copy %llu(%ux%u f%u) -> %llu(%ux%u f%u) src=%d,%d,%dx%d dst=%d,%d,%dx%d flags=%#x",
                        (unsigned long long)trace_op_index,
                        (unsigned long long)copy.src->serial,copy.src->d.width,copy.src->d.height,copy.src->d.format,
                        (unsigned long long)copy.dst->serial,copy.dst->d.width,copy.dst->d.height,copy.dst->d.format,
                        copy.sr.xoffset,copy.sr.yoffset,copy.sr.width,copy.sr.height,
                        copy.dr.xoffset,copy.dr.yoffset,copy.dr.width,copy.dr.height,unsigned(copy.flags));
                } else if(h->op==OP_CLEAR_TEX){
                    const auto& clear=*reinterpret_cast<const p_clear_tex*>(p);
                    ot2_host_log("OP_TRACE n=%llu op=38 clear %llu %ux%u f%u c=%.3g,%.3g,%.3g,%.3g region=%d,%d,%dx%d",
                        (unsigned long long)trace_op_index,(unsigned long long)clear.tex->serial,
                        clear.tex->d.width,clear.tex->d.height,clear.tex->d.format,
                        clear.c.f[0],clear.c.f[1],clear.c.f[2],clear.c.f[3],
                        clear.r.xoffset,clear.r.yoffset,clear.r.width,clear.r.height);
                } else if(h->op==OP_DRAW||h->op==OP_DRAW_IDX){
                    // Fixed-function state that can silently void a draw: a stale
                    // discard, a disabled render-enable, or a cull facing that empties
                    // the triangle. Printed next to the counts so an empty frame can be
                    // attributed without another run.
                    const auto* d=reinterpret_cast<const int32_t*>(p);
                    uint32_t declared=0;
                    if(render_state.programs[0])for(auto loc:stage_shader(0).inputs)if(loc<16)declared|=1u<<loc;
                    // Every attribute the title bound, plus which of them the vertex
                    // shader actually declares. A location the shader reads but the
                    // title never bound feeds undefined data; a location bound at an
                    // offset the stream stride cannot reach reads the next vertex.
                    char layout[320]={0};size_t used=0;
                    for(int i=0;i<16;i++){const auto& a=render_state.attributes[i];if(a.fmt||(declared>>i&1)){
                        used+=snprintf(layout+used,sizeof layout-used," %d%s:f%d(v%d)/o%d/s%d",i,
                            (declared>>i&1)?"*":"",unsigned(a.fmt),a.fmt?int(unsigned(format(a.fmt))):-1,
                            uint32_t(a.offset),uint32_t(a.stream));
                        if(used>=sizeof layout-32)break;}}
                    ot2_host_log("OP_TRACE n=%llu op=%u draw rt=%d/%llu vp=%dx%d prim=%d first=%d count=%d base=%d inst=%d discard=%u enable=%u cond=%d cull=%u front=%u mode=%u attr:%s",
                        (unsigned long long)trace_op_index,h->op,render_targets.n,target,
                        int(render_state.viewport[2]),int(render_state.viewport[3]),
                        d[0],h->op==OP_DRAW_IDX?-1:d[1],d[2],d[3],h->op==OP_DRAW_IDX?d[5]:d[4],
                        render_state.raster_discard,render_state.render_enable,render_state.render_cond_mode,
                        unsigned(render_state.polygon.cull),unsigned(render_state.polygon.front),unsigned(render_state.polygon.mode),
                        layout);
                    // A stream whose stride is smaller than the furthest attribute
                    // offset it serves makes every fetch read the neighbouring
                    // vertex; the quad is then assembled from overlapping data and
                    // collapses. Printed for every stream the attributes use.
                    {char streams[192]={0};size_t at=0;
                     for(int s=0;s<16&&at<sizeof streams-24;s++){const auto& st=render_state.streams[s];
                        if(!st.stride&&!st.divisor)continue;
                        at+=snprintf(streams+at,sizeof streams-at," s%d=%u/%u",s,unsigned(st.stride),unsigned(st.divisor));}
                     ot2_host_log("OP_TRACE  streams:%s",streams);}
                    // A 2D sprite program reads one vec4 of vertices and projects
                    // them with a matrix. The bank it reads is the only place that
                    // matrix can come from, and a bank declared shorter than the
                    // shader's struct means either the data is there and unreachable
                    // or it was never written. Dumping the raw range next to the
                    // bank's own contents separates those two: numbers past the
                    // declared size prove the title filled more than it bound.
                    if(bank_dump_probe&&render_state.programs[0]&&stage_shader(0).inputs.size()<=1&&
                       render_state.streams[0].stride==28){
                        for(int bank=0;bank<16;bank++){
                            const auto& u=render_state.uniforms[0][bank];
                            if(!u.addr||!u.size)continue;
                            char vals[512]={0};size_t at=0;
                            const size_t count=std::min<size_t>(u.size/4,32);
                            const float* f=reinterpret_cast<const float*>(u.addr);
                            for(size_t k=0;k<count&&at<sizeof vals-16;k++)
                                at+=size_t(snprintf(vals+at,sizeof vals-at,"%s%g",k?" ":"",f[k]));
                            ot2_host_log("OP_TRACE  bank%d addr=%llx size=%zu %s",bank,
                                (unsigned long long)u.addr,u.size,vals);
                            // Same range continued past the declared size, read
                            // straight from the title's memory.
                            if(u.size<1024){
                                char more[512]={0};size_t at2=0;
                                const size_t extra=std::min<size_t>((1024-u.size)/4,32);
                                for(size_t k=0;k<extra&&at2<sizeof more-16;k++)
                                    at2+=size_t(snprintf(more+at2,sizeof more-at2,"%s%g",k?" ":"",f[count+k]));
                                ot2_host_log("OP_TRACE   past+%zu %s",u.size,more);
                            }
                        }
                    }
                    // A draw that writes nothing is either never rasterised or
                    // thrown away after shading. Blend enable with a zero source
                    // alpha and a zero write mask look identical in the pixels.
                    ot2_host_log("OP_TRACE  state blendEn=%u src=%u/%u dst=%u/%u eq=%u/%u cmask=%#x dt=%u dw=%u df=%u vpf=%.4g..%.4g sc=%d,%d,%dx%d",
                        unsigned(render_state.color.blend_en),
                        unsigned(render_state.blends[0].src_rgb),unsigned(render_state.blends[0].src_a),
                        unsigned(render_state.blends[0].dst_rgb),unsigned(render_state.blends[0].dst_a),
                        unsigned(render_state.blends[0].eq_rgb),unsigned(render_state.blends[0].eq_a),
                        render_state.channel_mask.mask,
                        unsigned(render_state.depth_stencil.dt),unsigned(render_state.depth_stencil.dw),unsigned(render_state.depth_stencil.df),
                        double(render_state.viewport[4]),double(render_state.viewport[5]),
                        render_state.scissor.x,render_state.scissor.y,render_state.scissor.w,render_state.scissor.h);
                    // The composite passes carry no per-vertex transform: the quad
                    // is a unit square and the constant bank places it. If that
                    // bank is zero the quad has no extent and nothing is written,
                    // which from the outside is indistinguishable from a bad shader.
                    // Every bank has to be listed: the transform is not in the same
                    // slot for the scene shader and the composite shader.
                    if(render_state.programs[0]){
                        auto& sh=stage_shader(0);                        char declared_list[96]={0};size_t dl=0;
                        for(auto loc:sh.inputs)dl+=size_t(snprintf(declared_list+dl,sizeof declared_list-dl,"%s%u",dl?",":"",loc));
                        ot2_host_log("OP_TRACE  vs-inputs=[%s] key=%s cbufs=%zu sbufs=%zu support=%d",declared_list,
                            sh.resources->key.c_str(),sh.resources->cbufs.size(),sh.resources->sbufs.size(),
                            sh.needs_support_buffer?1:0);
                        // A sprite program places its quad from data, not from the
                        // vertex buffer: either a constant bank or a storage buffer
                        // whose address the title stores inside one. Both are printed
                        // so an unbound or mis-resolved source shows up as a number
                        // instead of as a silently blank frame.
                        for(auto& sb:sh.resources->sbufs){auto b=storage_binding(0,sb,sh);
                            ot2_host_log("OP_TRACE  sbuf binding=%u bank=%d word=%d addr=%llx size=%llu",
                                sb.binding,sb.cb_slot,sb.cb_offset,(unsigned long long)b.address,(unsigned long long)b.size);
                            // A batched sprite program keeps its per-instance rects
                            // in this buffer, so its contents are the positions. A
                            // rect of zero extent here means the draw is correct and
                            // empty; a sensible rect means the positions are fine and
                            // the vertex stage is what discards them.
                            if(bank_dump_probe&&b.address&&b.size&&b.size<=4096){
                                const auto* f=reinterpret_cast<const float*>(b.address);
                                const size_t count=std::min<size_t>(b.size/4,64);
                                char vals[512]={0};size_t at=0;
                                for(size_t k=0;k<count&&at<sizeof vals-16;k++)
                                    at+=size_t(snprintf(vals+at,sizeof vals-at,"%s%g",k?" ":"",f[k]));
                                ot2_host_log("OP_TRACE   sbufdata %s",vals);}
                        }
                        int slot=0;
                        for(auto& cb:sh.resources->cbufs){                            if(++slot>8)break;
                            auto bank=constant_bank(0,cb.slot,sh);
                            if(bank.size()<16){ot2_host_log("OP_TRACE  vs-c%d size=%zu (short)",cb.slot,bank.size());continue;}
                            const float* f=reinterpret_cast<const float*>(bank.data());
                            const size_t n=std::min<size_t>(bank.size()/4,192);
                            std::string vals;vals.reserve(n*10);
                            char one[32];
                            for(size_t k=0;k<n;k++){snprintf(one,sizeof one,"%s%g",k?" ":"",f[k]);vals+=one;}
                            ot2_host_log("OP_TRACE  vs-c%d size=%zu %s",cb.slot,bank.size(),vals.c_str());
                        }
                    }
                    // A 2D sprite program places each sprite from a per-instance
                    // rectangle rather than from the vertex buffer, so a rect of
                    // zero width or height means the draw is correct and empty at
                    // the same time. The instance records are read straight out of
                    // the bound buffer: whatever is printed here is what the shader
                    // multiplies its unit quad by.
                    const uint32_t instances_seen=h->op==OP_DRAW_IDX?uint32_t(std::max(0,d[5])):uint32_t(std::max(0,d[4]));
                    if(h->op==OP_DRAW_IDX&&instances_seen){
                        const auto& a6=render_state.attributes[6];
                        if(a6.fmt&&a6.stream<16){
                            const auto& vb=render_state.vertex_buffers[a6.stream];const auto& st=render_state.streams[a6.stream];
                            const uint32_t count=instances_seen<8?instances_seen:8;
                            if(vb.address&&st.stride){char rects[320]={0};size_t at=0;
                                for(uint32_t k=0;k<count&&at<sizeof rects-48;k++){
                                    const uint64_t addr=uint64_t(vb.address)+uint64_t(k)*st.stride+a6.offset;
                                    ensure_cpu_buffer(uintptr_t(addr),16);
                                    const float* f=reinterpret_cast<const float*>(addr);
                                    at+=size_t(snprintf(rects+at,sizeof rects-at," [%g %g %g %g]",f[0],f[1],f[2],f[3]));}
                                ot2_host_log("OP_TRACE  rects s%d/%u stride=%u n=%u%s",uint32_t(a6.stream),uint32_t(a6.offset),
                                    uint32_t(st.stride),uint32_t(instances_seen),rects);}
                        }
                    }
                    for(int s=0;s<2;s++){const auto& vb=render_state.vertex_buffers[s];
                        if(!vb.address||!vb.size)continue;
                        const size_t n=size_t(std::min<uint64_t>(vb.size,64));ensure_cpu_buffer(uintptr_t(vb.address),n);
                        const auto* bytes=reinterpret_cast<const uint8_t*>(vb.address);char hex[3*64+1]={0};
                        for(size_t k=0;k<n;k++)snprintf(hex+k*3,4,"%02x ",bytes[k]);
                        ot2_host_log("OP_TRACE  vbuf%d @%llx size=%llu %s",s,(unsigned long long)vb.address,(unsigned long long)vb.size,hex);}
                    // Which images the fragment stage reads is what decides whether a
                    // full-screen "present the scene" quad samples the scene at all.
                    {char sources[128]={0};size_t at=0;
                     for(int slot=0;slot<12&&at<sizeof sources-16;slot++){
                        unsigned id=render_state.texture_handles[1][slot]&0xfffff;if(!id)continue;
                        auto it=active_textures().find(id);if(it==active_textures().end())continue;
                        at+=snprintf(sources+at,sizeof sources-at," %d:%llu/%ux%u",slot,
                            (unsigned long long)it->second.texture.serial,it->second.texture.d.width,it->second.texture.d.height);}
                     ot2_host_log("OP_TRACE  fs_textures:%s",sources[0]?sources:" none");}
                    if(h->op==OP_DRAW_IDX){const auto& di=*reinterpret_cast<const p_draw_idx*>(p);
                        if(di.ibuf){const size_t n=24;ensure_cpu_buffer(uintptr_t(di.ibuf),n);
                            const auto* bytes=reinterpret_cast<const uint8_t*>(di.ibuf);char hex[3*24+1]={0};
                            for(size_t k=0;k<n;k++)snprintf(hex+k*3,4,"%02x ",bytes[k]);
                            ot2_host_log("OP_TRACE  ibuf @%llx itype=%d first_index_bytes=%s",(unsigned long long)di.ibuf,di.itype,hex);}}
                } else
                ot2_host_log("OP_TRACE n=%llu op=%u size=%u rt=%d/%llu vp=%dx%d",
                    (unsigned long long)trace_op_index,h->op,h->size,render_targets.n,target,
                    int(render_state.viewport[2]),int(render_state.viewport[3]));
            }
            ++trace_op_index;
            if((h->op==OP_DRAW||h->op==OP_DRAW_IDX||h->op==OP_CLEAR_TEX||h->op==OP_COPY_T2T)&&
               render_targets.n&&render_targets.color[0])probe_pixels(*render_targets.color[0],trace_op_index,h->op);
        }
        command+=h->size;
    }
    ot2_host_die("Graphics command list has no end");
}
}
extern "C" void vkhost_configure_package(const char* path,const char* offline) {
    shader_directory=path;std::filesystem::create_directories(shader_directory);
    shader_binary_directory=shader_directory;complete_shader_cache=false;
    // Pin one immutable cache for the entire process. Installing a newer set
    // cannot mix its reflection with an already loaded shader's binary.
    std::ifstream active(shader_directory+"/active");std::string id;
    if(active){
        active>>id;
        if(id.size()!=64 || id.find_first_not_of("0123456789abcdef")!=std::string::npos)ot2_host_die("Invalid offline shader cache identifier");
        shader_binary_directory=shader_directory+"/sets/"+id;
        std::ifstream file(shader_binary_directory+"/manifest.json");
        if(!file)ot2_host_die("Offline shader cache manifest missing");
        nlohmann::json manifest;file>>manifest;
        if(manifest.value("format",0)!=1 || manifest.value("id",std::string())!=id || manifest["shaders"].empty())ot2_host_die("Invalid offline shader cache manifest");
        complete_shader_cache=true;ot2_host_log("OFFLINE_SHADERS id=%s shaders=%zu",id.c_str(),manifest["shaders"].size());
    }
    if(offline){
        shader_binary_directory=offline;
        std::ifstream file(shader_binary_directory+"/manifest.json");nlohmann::json manifest;
        if(!file)ot2_host_die("Package shader manifest missing");file>>manifest;
        if(manifest.value("format",0)!=1||manifest["shaders"].empty())ot2_host_die("Invalid package shader manifest");
        complete_shader_cache=true;
    }
    readonly_stores_enabled=false;
    trace_draw_completion=std::filesystem::exists(shader_directory+"/trace-draw-completion");
    capture_frame=std::filesystem::exists(shader_directory+"/capture-frame");
    if(capture_frame){read_capture_frame_number();std::filesystem::remove_all(shader_directory+"/frame"+std::to_string(capture_frame_number));
        ot2_host_log("CAPTURE_FRAME armed for frame %llu",(unsigned long long)capture_frame_number);}
    if(device)start_pipeline_warmup();
}
extern "C" void vkhost_configure(const char* path){vkhost_configure_package(path,nullptr);}
extern "C" void vkhost_program(const prog_t* program) {
    std::lock_guard lock(state_mutex);
    for(unsigned stage=0;stage<program->nstage;stage++) {
        const auto* control=static_cast<const uint8_t*>(program->sd[stage].control);
        const auto* code=reinterpret_cast<const uint8_t*>(program->sd[stage].data);
        if(!control)continue;
        // The code area may still be unmapped at this point: a title is allowed
        // to name an address and fill it in before the first draw. Never read
        // through it unconditionally.
        uint32_t magic=0,code_magic=0,size=0;memcpy(&magic,control,4);if(code)memcpy(&code_magic,code,4);memcpy(&size,control+0x704,4);
        // An unrecognized control section is a stage that is not filled in yet,
        // not a corrupt one: a title may set a placeholder (0x12345678 with a
        // zero length) and provide the real section later. Skip it so the rest
        // of the program can still resolve.
        if(magic!=0x98761234 || size<8 || size>8*1024*1024) {
            ot2_host_log("SHADER skip stage=%u magic=%#x size=%#x code=%s",stage,magic,size,code?"mapped":"unmapped");
            continue;
        }
        // The control section carries its own length at +0x20; hashing a fixed
        // 0x880 over a shorter section would produce a key the cache cannot have.
        // The code area takes part as well, for the reason given at shader_key:
        // this title has 613 shaders that share a control section.
        uint32_t nctl=0x880,own=0;memcpy(&own,control+0x20,4);
        if(own>=0x760&&own<0x880)nctl=own;
        const auto key=shader_key(control,code,size?size:0);
        const auto base=shader_directory+"/"+key;
        // Whether the host's key and the offline cache agree is the single most
        // useful fact when a shader fails to resolve, so report it per stage.
        {
            static std::atomic<unsigned> reported{0};
            const unsigned n=reported.fetch_add(1,std::memory_order_relaxed);
            if(n<24) {
                std::error_code ec;
                // The pipeline loader reads from shader_binary_directory, so the
                // check has to look there too. Probing only shader_directory
                // reported every packaged shader as a miss, which sends the next
                // debugging session after a cache that is in fact present.
                const bool cached=std::filesystem::exists(shader_binary_directory+"/"+key+".spv",ec)
                                 ||std::filesystem::exists(base+".spv",ec);
                ot2_host_log("SHADER %s stage=%u nctl=%#x size=%#x cache=%s dir=%s",key.c_str(),stage,nctl,size,cached?"HIT":"MISS",shader_binary_directory.c_str());
            }
        }
        // A key the packaged cache does not know is a shader this title builds at
        // run time rather than one shipped in RomFS — the offline extractor can
        // never have seen it. Keep the control and code so a later offline pass
        // can translate them and grow the cache; without the bytes the gap is
        // unrecoverable once the process exits.
        const bool captured=std::filesystem::exists(base+".spv");
        {
            // Why a shader is not captured matters more than the miss itself: a
            // missing code pointer and an already-captured key look identical in
            // the cache line, but only the first one loses the bytes forever.
            static std::atomic<unsigned> why{0};
            const unsigned n=why.fetch_add(1,std::memory_order_relaxed);
            if(n<24) ot2_host_log("SHADER_WHY %s stage=%u code=%p captured=%d complete=%d dir=%s",
                key.c_str(),stage,reinterpret_cast<const void*>(code),captured?1:0,complete_shader_cache?1:0,
                shader_directory.c_str());
        }
        if(!captured && code) {
            std::string dir=complete_shader_cache?shader_directory+"/captures":shader_directory;
            std::error_code ec;std::filesystem::create_directories(dir,ec);
            const auto target=dir+"/"+key;
            if(!std::filesystem::exists(target+".control")) {
                std::ofstream(target+".control",std::ios::binary).write(reinterpret_cast<const char*>(control),nctl);
                std::ofstream(target+".code",std::ios::binary).write(reinterpret_cast<const char*>(code),size);
                // The offline prepare step infers texture and attribute facts by
                // reading the assembly text that follows the control section.
                // The console serialises the whole block length just before the
                // control, so that word gives the end of the assembly too.
                uint32_t block=0;memcpy(&block,control-4,4);
                if(block>0x880 && block<=0x40000) {
                    std::ofstream(target+".asm",std::ios::binary)
                        .write(reinterpret_cast<const char*>(control+0x880),block-0x880);
                }
                static std::atomic<unsigned> captured_count{0};
                const unsigned n=captured_count.fetch_add(1,std::memory_order_relaxed);
                if(n<40)ot2_host_log("SHADER_CAPTURE %s stage=%u nctl=%#x size=%#x block=%#x",key.c_str(),stage,nctl,size,block);
            }
        }
    }
}
extern "C" void vkhost_set_window(ANativeWindow* next) {
    drain_queues();
    std::lock_guard lock(state_mutex);
    if(device)vkDeviceWaitIdle(device);
    destroy_swapchain();
    reset_display_timings();
    if(surface){vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;}
    if(window)ANativeWindow_release(window);
    window=next;surface_changed.notify_all();
    ot2_host_log("GRAPHICS Android surface %s",window?"available":"destroyed");
}
extern "C" void vkhost_init() {
    std::lock_guard lock(state_mutex);if(device)return;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="OT2 dedicated host";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
    const char* extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    info.enabledExtensionCount=2;info.ppEnabledExtensionNames=extensions;
    check(vkCreateInstance(&info,nullptr,&instance),"instance");
    uint32_t n=0;check(vkEnumeratePhysicalDevices(instance,&n,nullptr),"enumerate devices");
    if(!n)ot2_host_die("No Vulkan device");std::vector<VkPhysicalDevice> devices(n);check(vkEnumeratePhysicalDevices(instance,&n,devices.data()),"devices");physical=devices[0];
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
    queue_family=UINT32_MAX;for(uint32_t i=0;i<n;i++)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){queue_family=i;break;}
    if(queue_family==UINT32_MAX)ot2_host_die("No graphics queue");
    gpu_timestamp_bits=families[queue_family].timestampValidBits;
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=queue_family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceProperties device_properties{};vkGetPhysicalDeviceProperties(physical,&device_properties);
    // Reproduced on this G710 driver (134221824): any geometry shader inside an
    // occlusion query loses the device, including a three-vertex host fixture.
    geometry_query_workaround=device_properties.vendorID==0x13b5&&device_properties.deviceID==2824994820u&&device_properties.driverVersion==134221824u;
#ifdef OT2_NVN_TESTING
    force_software_samples=getenv("OT2_FORCE_SOFTWARE_SAMPLES")!=nullptr;
#endif
    ot2_host_log("GRAPHICS geometryQueryWorkaround=%u",geometry_query_workaround);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    if(device_properties.apiVersion>=VK_API_VERSION_1_3)f12.pNext=&f13;
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    if(device_properties.apiVersion>=VK_API_VERSION_1_2)draw_parameters.pNext=&f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext=&draw_parameters;
    vkGetPhysicalDeviceFeatures2(physical,&f2);
    ot2_host_log("GRAPHICS Vulkan 1.2 layer=%u viewportIndex=%u",f12.shaderOutputLayer,f12.shaderOutputViewportIndex);
    dynamic_rendering=f13.dynamicRendering&&!getenv("OT2_FORCE_LEGACY_RENDERING")&&!std::filesystem::exists(shader_directory+"/force-legacy-rendering");
    ot2_host_log("GRAPHICS render_path=%s",dynamic_rendering?"dynamic":"legacy");
    precise_occlusion=f2.features.occlusionQueryPrecise;depth_bounds_supported=f2.features.depthBounds&&!getenv("OT2_FORCE_SOFTWARE_DEPTH_BOUNDS");if(!depth_bounds_supported)dynamic_rendering=false;ot2_host_log("GRAPHICS depthBounds=%u",depth_bounds_supported);
    auto& features=f2.features;di.pEnabledFeatures=&features;di.pNext=f2.pNext;
    native_bc=features.textureCompressionBC&&!getenv("OT2_FORCE_SOFTWARE_BC");
    ot2_host_log("GRAPHICS BC=%s",native_bc?"native":"decoded");
    ot2_host_log("GRAPHICS features geometry=%u float64=%u imageWriteWithoutFormat=%u imageReadWithoutFormat=%u",features.geometryShader,features.shaderFloat64,features.shaderStorageImageWriteWithoutFormat,features.shaderStorageImageReadWithoutFormat);
    uint32_t extension_count=0;check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,nullptr),"device extensions");std::vector<VkExtensionProperties> available_extensions(extension_count);check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,available_extensions.data()),"device extensions");
    bool display_timing=false;for(auto& extension:available_extensions)if(!strcmp(extension.extensionName,VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME))display_timing=true;
    std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};if(display_timing)device_extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    VkPhysicalDeviceRobustness2FeaturesEXT null_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    for(const auto& extension:available_extensions)if(!strcmp(extension.extensionName,VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)){
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&null_features;vkGetPhysicalDeviceFeatures2(physical,&query);
        null_descriptors=null_features.nullDescriptor;
        robust_uniform_ranges=null_features.robustBufferAccess2&&features.robustBufferAccess;
        null_features.robustBufferAccess2=robust_uniform_ranges;null_features.robustImageAccess2=VK_FALSE;
        VkPhysicalDeviceRobustness2PropertiesEXT bounds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&bounds;vkGetPhysicalDeviceProperties2(physical,&properties);
        robust_uniform_alignment=bounds.robustUniformBufferAccessSizeAlignment;
        if(null_descriptors||robust_uniform_ranges){device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);null_features.pNext=const_cast<void*>(di.pNext);di.pNext=&null_features;}break;
    }
    ot2_host_log("GRAPHICS nullDescriptor=%u",null_descriptors);
    if(getenv("OT2_FORCE_SOFTWARE_NULL"))null_descriptors=false;
    ot2_host_log("GRAPHICS robustUniformRanges=%u alignment=%zu",robust_uniform_ranges,robust_uniform_alignment);
    di.enabledExtensionCount=device_extensions.size();di.ppEnabledExtensionNames=device_extensions.data();
    check(vkCreateDevice(physical,&di,nullptr,&device),"device");vkGetDeviceQueue(device,queue_family,0,&queue);
    if(display_timing)get_display_timings=reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(vkGetDeviceProcAddr(device,"vkGetPastPresentationTimingGOOGLE"));
    vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=queue_family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device,&pool,nullptr,&command_pool),"command pool");
    VkQueryPoolCreateInfo oq{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};oq.queryType=VK_QUERY_TYPE_OCCLUSION;oq.queryCount=query_capacity;check(vkCreateQueryPool(device,&oq,nullptr,&occlusion_query),"occlusion query");
    VkQueryPoolCreateInfo cq{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};cq.queryType=VK_QUERY_TYPE_TIMESTAMP;cq.queryCount=256;check(vkCreateQueryPool(device,&cq,nullptr,&counter_query),"counter query");
    synchronous_occlusion_query=occlusion_query;synchronous_counter_query=counter_query;
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};check(vkCreatePipelineCache(device,&cache,nullptr,&pipeline_cache),"pipeline cache");
    VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(physical,&p);
    const auto& limits=p.limits;
    dynamic_uniform_limit=limits.maxDescriptorSetUniformBuffersDynamic;dynamic_storage_limit=limits.maxDescriptorSetStorageBuffersDynamic;
    host_limits.uniform_range=limits.maxUniformBufferRange;host_limits.uniform_bindings=limits.maxPerStageDescriptorUniformBuffers;
    host_limits.storage_bindings=limits.maxPerStageDescriptorStorageBuffers;host_limits.sampled_images=limits.maxPerStageDescriptorSampledImages;
    host_limits.samplers=limits.maxPerStageDescriptorSamplers;host_limits.storage_images=limits.maxPerStageDescriptorStorageImages;
    host_limits.color_targets=std::min(limits.maxColorAttachments,limits.maxFragmentOutputAttachments);
    host_limits.vertex_bindings=limits.maxVertexInputBindings;host_limits.vertex_attributes=limits.maxVertexInputAttributes;
    host_limits.texture_size=std::min(limits.maxImageDimension1D,limits.maxImageDimension2D);host_limits.texture_3d=limits.maxImageDimension3D;
    host_limits.texture_cube=limits.maxImageDimensionCube;host_limits.texture_layers=limits.maxImageArrayLayers;host_limits.buffer_texels=limits.maxTexelBufferElements;
    std::copy_n(limits.maxComputeWorkGroupSize,3,host_limits.compute_size);std::copy_n(limits.maxComputeWorkGroupCount,3,host_limits.compute_count);
    host_limits.compute_threads=limits.maxComputeWorkGroupInvocations;host_limits.anisotropy=features.samplerAnisotropy?uint32_t(limits.maxSamplerAnisotropy):1;
    host_limits.astc=features.textureCompressionASTC_LDR;host_limits.subpixel_bits=limits.subPixelPrecisionBits;
    // Shader driver banks currently use a full 64 KiB UBO. Reporting a smaller
    // limit cannot make those translated shaders valid on a smaller device.
    if(limits.maxUniformBufferRange<65536)ot2_host_die("GPU uniform range %u is below the translated shader requirement of 65536",limits.maxUniformBufferRange);
    timestamp_period=limits.timestampPeriod;max_storage_range=limits.maxStorageBufferRange;upload_alignment=std::max<size_t>({256,limits.minUniformBufferOffsetAlignment,limits.minStorageBufferOffsetAlignment,limits.minTexelBufferOffsetAlignment});ot2_host_log("GRAPHICS Vulkan device: %s",p.deviceName);
    ot2_host_log("HOST_LIMITS compute=%u/%u/%u threads=%u texture=%u layers=%u UBO=%u SSBO=%u sampled=%u samplers=%u ASTC=%u",host_limits.compute_size[0],host_limits.compute_size[1],host_limits.compute_size[2],host_limits.compute_threads,host_limits.texture_size,host_limits.texture_layers,host_limits.uniform_bindings,host_limits.storage_bindings,host_limits.sampled_images,host_limits.samplers,host_limits.astc);
    start_pipeline_warmup();
}
extern "C" int vkhost_device_limit(NVNdeviceInfo property,int maximum){vkhost_init();return nvn_limit_for_host(property,maximum,&host_limits);}
extern "C" void vkhost_pool_add(const void* p,uint64_t size) {std::lock_guard lock(state_mutex);pools[uintptr_t(p)]=size;nvn_dirty_add_range(p,size);}
// Resource destruction happens only after the submission fence. Descriptor
// snapshots keep finalized NVN objects alive until the last registration goes.
namespace {
void destroy_image(uint64_t serial){
    if(auto key=texture_storage_keys.find(serial);key!=texture_storage_keys.end()){
        auto owners=image_storage_owners.find(key->second);if(owners!=image_storage_owners.end()){owners->second.erase(serial);if(owners->second.empty())image_storage_owners.erase(owners);}texture_storage_keys.erase(key);
    }
    auto it=images.find(serial);if(it==images.end())return;
    flush_commands();
    auto& lookup=image_lookups[serial&(image_lookups.size()-1)];if(lookup.serial==serial)lookup={};
    if(it->second.use_count()>1){images.erase(it);return;}
    if(it->second->shared_storage)image_storage.erase(it->second->storage_key);
    invalidate_descriptor_contents();
    for(auto& [pool,entries]:texture_pools)for(auto& [id,entry]:entries)if(entry.texture.serial==serial)entry.cached_view={};
    for(auto view=image_views.begin();view!=image_views.end();){VkImage owner{};memcpy(&owner,view->first.bytes.data(),sizeof(owner));
        if(owner==it->second->image){legacy_forget_view(view->second);vkDestroyImageView(device,view->second,nullptr);view=image_views.erase(view);}else ++view;}
    vkDestroyImage(device,it->second->image,nullptr);vkFreeMemory(device,it->second->memory,nullptr);images.erase(it);
}
void drop_texture_reference(uint64_t serial){auto it=texture_references.find(serial);if(it==texture_references.end()||!it->second)ot2_host_die("Texture reference underflow");if(!--it->second)texture_references.erase(it);}
void collect_texture(uint64_t serial){if(serial && finalized_textures.contains(serial) && !texture_references.contains(serial)){destroy_image(serial);finalized_textures.erase(serial);}}
void collect_finalized_textures(){for(auto it=finalized_textures.begin();it!=finalized_textures.end();){if(texture_references.contains(*it)){++it;continue;}destroy_image(*it);it=finalized_textures.erase(it);}}
void bind_render_targets(const p_set_rt& targets){
    auto previous=std::move(render_target_pins);render_target_pins.clear();render_targets=targets;
    auto retain=[&](const tex_t* t,int slot)->const tex_t*{
        if(!t)return nullptr;render_target_storage[slot]=*t;++texture_references[t->serial];render_target_pins.push_back(t->serial);return &render_target_storage[slot];};
    for(int i=0;i<targets.n;++i)render_targets.color[i]=retain(targets.color[i],i);
    render_targets.depth=retain(targets.depth,8);
    for(auto serial:previous){drop_texture_reference(serial);collect_texture(serial);}
}
}
extern "C" void vkhost_tex_retain(uint64_t serial){if(!serial)return;std::lock_guard lock(state_mutex);++texture_references[serial];}
extern "C" void vkhost_tex_unref(uint64_t serial){if(!serial)return;std::lock_guard lock(state_mutex);drop_texture_reference(serial);collect_texture(serial);}
extern "C" void vkhost_pool_del(const void* p) {
    drain_queues();
    std::lock_guard lock(state_mutex);auto pool=pools.find(uintptr_t(p));if(pool==pools.end())return;flush_commands();
    const uintptr_t begin=pool->first,end=begin+pool->second;auto belongs=[&](uintptr_t v){return v>=begin&&v<end;};
    for(auto i=resource_upload_cache.begin();i!=resource_upload_cache.end();)if(belongs(i->first[0]))erase_resource_upload(i++);else ++i;
    for(auto i=persistent_upload_cache.begin();i!=persistent_upload_cache.end();)if(belongs(i->first[0])){persistent_upload_bytes-=i->second.buffer.size;i=persistent_upload_cache.erase(i);}else ++i;
    invalidate_texture_lookups();
    for(auto it=texture_pools.begin();it!=texture_pools.end();){if(belongs(it->first)){for(auto& [id,entry]:it->second)drop_texture_reference(entry.texture.serial);it=texture_pools.erase(it);continue;}
        std::erase_if(it->second,[&](const auto& entry){if(!belongs(uintptr_t(entry.second.texture.mem)))return false;drop_texture_reference(entry.second.texture.serial);return true;});++it;}
    std::erase_if(sampler_pools,[&](const auto& entry){return belongs(entry.first);});
    std::set<uint64_t> removed;for(auto& [serial,img]:images)if(belongs(img->backing))removed.insert(serial);
    for(auto& [serial,key]:texture_storage_keys){uintptr_t backing;memcpy(&backing,key.bytes.data(),sizeof(backing));if(belongs(backing))removed.insert(serial);}
    for(auto serial:removed){destroy_image(serial);finalized_textures.erase(serial);}collect_finalized_textures();nvn_dirty_remove_range(p);pools.erase(pool);
}
extern "C" void vkhost_set_tex_pool_n(int n) {if(n<0)ot2_host_die("Negative texture pool size");}
extern "C" void vkhost_descriptor_pool_del(const dpool_t* p,int sampler) {
    drain_queues();
    std::lock_guard lock(state_mutex);auto key=descriptor_pool_key(p);flush_commands();
    if(!sampler)invalidate_texture_lookups();
    if(sampler)sampler_pools.erase(key);else{auto it=texture_pools.find(key);if(it!=texture_pools.end()){for(auto& [id,entry]:it->second)drop_texture_reference(entry.texture.serial);texture_pools.erase(it);}}collect_finalized_textures();
}
extern "C" void vkhost_reg_tex(const dpool_t* pool,int id,const tex_t* t,const view_t* v) {
    drain_queues();
    std::lock_guard lock(state_mutex);if(id<0||id>=pool->n)ot2_host_die("Texture descriptor index out of range");
    auto& entries=texture_pools[descriptor_pool_key(pool)];auto it=entries.find(id);uint64_t previous=it==entries.end()?0:it->second.texture.serial;
    if(previous!=t->serial){if(previous)drop_texture_reference(previous);++texture_references[t->serial];}
    entries[id]={*t,v?*v:view_t{},v!=nullptr};if(previous!=t->serial)collect_texture(previous);
}
extern "C" void vkhost_reg_samp(const dpool_t* pool,int id,const samp_par* s) {
    drain_queues();
    std::lock_guard lock(state_mutex);if(id<0||id>=pool->n)ot2_host_die("Sampler descriptor index out of range");sampler_pools[descriptor_pool_key(pool)][id]={*s};
}
extern "C" void vkhost_tex_created(const tex_t* texture){
    if(!shared_depth_storage(*texture))return;std::lock_guard lock(state_mutex);auto key=image_storage_key(*texture);
    texture_storage_keys[texture->serial]=key;image_storage_owners[key].insert(texture->serial);find_existing_image(*texture);
}
extern "C" void vkhost_tex_release(tex_t* texture) {
    std::lock_guard lock(state_mutex);if(texture->serial){finalized_textures.insert(texture->serial);collect_texture(texture->serial);}
}
extern "C" void vkhost_prog_release(prog_t*) {}
extern "C" int vkhost_probe_resource_lifecycle() {
    vkhost_init();
    std::array<uint8_t,4096> storage{};pool_t pool{};pool.mem=storage.data();pool.size=storage.size();
    vkhost_pool_add(pool.mem,pool.size);
    dpool_t first{&pool,2048,8},second{&pool,3072,8};
    tex_t a{};a.magic=MAGIC('T','E','X',0);a.serial=nvn_new_serial();a.mem=pool.mem;a.pool=&pool;
    a.d.width=a.d.height=4;a.d.depth=a.d.levels=1;a.d.format=NVN_FORMAT_RGBA8;a.d.target=NVN_TEXTURE_TARGET_2D;
    for(int i=0;i<4;i++)a.d.swizzle[i]=i+2;nvn_tex_layout(&a.d,&a.L);
    tex_t b=a;b.serial=nvn_new_serial();b.mem+=1024;
    vkhost_reg_tex(&first,1,&a,nullptr);vkhost_reg_tex(&second,1,&b,nullptr);vkhost_reg_tex(&second,2,&a,nullptr);
    bool valid;size_t before;
    {std::lock_guard lock(state_mutex);before=image_views.size();make_view(a,nullptr);make_view(b,nullptr);
        auto previous=active_texture_pool;active_texture_pool=descriptor_pool_key(&first);valid=texture_descriptor(1)->texture.serial==a.serial;
        valid&=texture_descriptor(1)->texture.serial==a.serial;
        active_texture_pool=descriptor_pool_key(&second);valid&=texture_descriptor(1)->texture.serial==b.serial;
        valid&=texture_descriptor(2)->texture.serial==a.serial;active_texture_pool=previous;
        valid&=find_image(a.serial)==images.at(a.serial).get()&&find_image(b.serial)==images.at(b.serial).get();}
    vkhost_tex_release(&a);vkhost_tex_release(&b);
    {std::lock_guard lock(state_mutex);valid&=images.contains(a.serial)&&images.contains(b.serial);}
    vkhost_descriptor_pool_del(&first,0);
    {std::lock_guard lock(state_mutex);valid&=images.contains(a.serial)&&images.contains(b.serial);}
    vkhost_reg_tex(&second,2,&b,nullptr);
    {std::lock_guard lock(state_mutex);valid&=!find_image(a.serial)&&find_image(b.serial)&&image_views.size()==before+1;
        auto previous=active_texture_pool;active_texture_pool=descriptor_pool_key(&second);valid&=texture_descriptor(2)->texture.serial==b.serial;active_texture_pool=previous;}
    vkhost_pool_del(pool.mem);
    {std::lock_guard lock(state_mutex);valid&=!find_image(b.serial)&&image_views.size()==before&&!texture_pools.contains(descriptor_pool_key(&second))&&!texture_references.contains(a.serial)&&!texture_references.contains(b.serial);
        auto previous=active_texture_pool;active_texture_pool=descriptor_pool_key(&second);valid&=!texture_descriptor(1)&&!texture_descriptor(2);active_texture_pool=previous;}
    return valid;
}
extern "C" void vkhost_write_texels(const tex_t* texture,const view_t* view,const NVNcopyRegion* region,const void* source,int64_t row,int64_t slice){
    if(!texture||!region||!source)ot2_host_die("Invalid CPU texture write");
    drain_queues();std::lock_guard lock(state_mutex);
    const unsigned level=view&&view->set_levels?view->base_level:0,layer=view&&view->set_layers?view->min_layer:0;
    int bw,bh,bytes=nvn_fmt_bpb(texture->d.format,&bw,&bh);ot2::CopyLayout layout;
    if(!ot2::copy_layout(region->width,region->height,region->depth,bw,bh,bytes,row,slice,layout)||!ot2::copy_address(uintptr_t(source),layout.extent))ot2_host_die("Invalid CPU texture write stride");
    if(view&&view->set_layers&&view->nlayers>0&&(region->zoffset<0||region->depth>view->nlayers-region->zoffset))ot2_host_die("CPU texture write exceeds view");
    // Initialize/preserve untouched pixels before modifying the backing pool.
    reset_prepared_bindings();upload_packaged(*texture);
    {NvnDirtyWrite lease(texture->mem,texture->L.size);
        if(!ot2::write_texture_memory(*texture,level,layer,*region,{static_cast<const uint8_t*>(source),layout.extent},row,slice))ot2_host_die("Invalid CPU texture write region");}
    const auto old_row=copy_row_stride,old_image=copy_image_stride;
    copy_row_stride=row;copy_image_stride=slice;
    p_copy_b2t copy{};copy.tex=texture;copy.src=uintptr_t(source);copy.r=*region;copy.has_view=view!=nullptr;if(view)copy.view=*view;
    copy_to_texture(copy);flush_commands();copy_row_stride=old_row;copy_image_stride=old_image;
    if(texture->d.pkg_block_h){auto& image=image_for(*texture);image.source_tracked=nvn_dirty_protect(texture->mem,texture->L.size);image.source_stamp=image.source_seen=nvn_dirty_now();}
}
extern "C" void vkhost_read_texels(const tex_t* texture,unsigned level,unsigned layer,const NVNcopyRegion* region,uint8_t* destination,size_t row_stride,size_t image_stride) {
    drain_queues();
    std::lock_guard lock(state_mutex);
    p_copy_b2t copy{};copy.tex=texture;copy.src=reinterpret_cast<uintptr_t>(destination);copy.r=*region;copy.has_view=true;
    copy.view.set_levels=copy.view.set_layers=1;copy.view.base_level=level;copy.view.nlevels=1;copy.view.min_layer=layer;
    copy_from_texture(copy,row_stride,image_stride);
}
extern "C" int vkhost_probe_descriptor_reuse() {
    vkhost_init();std::lock_guard lock(state_mutex);
    const bool previous_batching=batching,previous_reuse=reuse_descriptors;
    const bool previous_dynamic=dynamic_buffer_descriptors;dynamic_buffer_descriptors=false;
    flush_commands();batching=true;reuse_descriptors=true;
    static VkShaderModule module{};if(!module)module=clear_module(descriptor_probe_comp);
    auto slots_before=descriptor_slot_creates,reuses_before=descriptor_reuses;
    bool valid=true;std::array<std::array<uint32_t,4>,2> input,output;
    VkDescriptorSet first_set{};
    for(unsigned round=0;round<4;round++){
        for(unsigned i=0;i<2;i++){
            for(unsigned component=0;component<4;component++)input[i][component]=1+round*100+i*10+component;
            output[i].fill(0xdeadbeef);
            auto resources=std::make_unique<DrawResources>();
            resources->buffer(1,input[i].data(),16,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            resources->buffer(2,output[i].data(),16,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,true);
            resources->finish();
            if(i==0)first_set=resources->set;else valid&=first_set!=resources->set;
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=resources->layout;
            auto pipeline=cached_pipeline(ci,resources->bindings);
            auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
            bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources->layout,resources->set,resources->dynamic_offsets);vkCmdDispatch(cmd,1,1,1);finish_commands(cmd);
            auto* retained=resources.release();batch_retire.push_back([retained]{delete retained;});
        }
        flush_commands();
        const uint32_t increment[]={3,5,7,11};
        for(unsigned i=0;i<2;i++)for(unsigned component=0;component<4;component++)valid&=output[i][component]==input[i][component]+increment[component];
    }
    // Pool count is not set count: a slab can create both first-use sets.
    valid&=descriptor_slot_creates-slots_before<=2 && descriptor_reuses-reuses_before>=6
        && (descriptor_slot_creates-slots_before)+(descriptor_reuses-reuses_before)==8;
    // Layout stage visibility is part of the compatibility key, regardless of
    // the order supplied by the caller. Both live leases must remain distinct.
    std::vector<VkDescriptorSetLayoutBinding> bs{{5,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    auto acquire=[&]{PipelineKey key;key.bindings(bs);return acquire_descriptors(bs,nullptr,nullptr,key.data);};
    auto* a=acquire();std::reverse(bs.begin(),bs.end());auto* b=acquire();valid&=a->owner==b->owner && a->set!=b->set;
    bs[0].stageFlags=VK_SHADER_STAGE_ALL;auto* c=acquire();valid&=c->owner!=a->owner;
    auto* d=acquire();valid&=d->owner==c->owner&&d->set!=c->set;
    release_descriptors(a);release_descriptors(b);release_descriptors(c);release_descriptors(d);
    // Pipelines are cached by shader-module handle; keep the tiny fixture module
    // alive with its pipelines, as for game shaders, to avoid handle reuse.
    batching=previous_batching;reuse_descriptors=previous_reuse;dynamic_buffer_descriptors=previous_dynamic;return valid;
}
extern "C" int vkhost_probe_transfers() {
    vkhost_init();
    std::lock_guard lock(state_mutex);
    // Exercise the same two-layer upload/readback path used by NVN, including
    // independent row/image strides, a subrectangle and untouched padding.
    tex_t texture{};texture.magic=MAGIC('T','E','X',0);texture.serial=nvn_new_serial();
    texture.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;texture.d.format=NVN_FORMAT_RGBA8;
    texture.d.width=4;texture.d.height=3;texture.d.depth=2;texture.d.levels=1;nvn_tex_layout(&texture.d,&texture.L);
    std::array<uint8_t,160> input;input.fill(0xcc);
    for(size_t z=0;z<2;z++)for(size_t y=0;y<3;y++)for(size_t x=0;x<16;x++)input[z*80+y*24+x]=uint8_t(1+z*71+y*19+x);
    auto old_row=copy_row_stride,old_image=copy_image_stride;copy_row_stride=24;copy_image_stride=80;
    p_copy_b2t copy{};copy.tex=&texture;copy.src=reinterpret_cast<uintptr_t>(input.data());copy.r={0,0,0,4,3,2};copy_to_texture(copy);
    std::array<uint8_t,64> output;output.fill(0xa5);copy.src=reinterpret_cast<uintptr_t>(output.data());copy.r={1,1,0,2,2,2};copy_from_texture(copy,12,32);
    bool valid=true;
    for(size_t i=0;i<output.size();i++){size_t z=i/32,y=(i%32)/12,x=(i%32)%12;uint8_t expected=y<2&&x<8?input[z*80+(y+1)*24+4+x]:0xa5;valid&=output[i]==expected;}
    // A real four-sample image must resolve before CPU readback. This detects
    // accidentally allocating a single-sample image or using copy instead of resolve.
    tex_t multisample=texture;multisample.serial=nvn_new_serial();multisample.d.samples=4;
    multisample.d.target=NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;nvn_tex_layout(&multisample.d,&multisample.L);
    p_clear_tex clear{};clear.tex=&multisample;clear.mask=15;clear.r={0,0,0,4,3,2};clear.c.f[0]=1;clear.c.f[3]=1;clear_texture(clear);
    clear.mask=2;clear.r={1,1,0,2,2,2};clear.c.f[1]=1;clear_texture(clear);
    p_copy_t2t resolve{};resolve.src=&multisample;resolve.dst=&texture;resolve.sr=resolve.dr={0,0,0,4,3,2};copy_texture(resolve);
    output.fill(0xa5);copy_from_texture(copy,12,32);
    for(size_t i=0;i<output.size();i++){size_t y=(i%32)/12,x=(i%32)%12;uint8_t expected=y<2&&x<8?(x%4==2?0:255):0xa5;valid&=output[i]==expected;}
    tex_t depth=texture;depth.serial=nvn_new_serial();depth.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;depth.d.target=NVN_TEXTURE_TARGET_2D;depth.d.depth=1;nvn_tex_layout(&depth.d,&depth.L);
    auto previous_targets=render_targets;auto previous_scissor=render_state.scissor;
    render_targets={};render_targets.depth=&depth;render_state.scissor={0,0,4,3};clear_depth({0.25f,1,0xa5,255});
    render_state.scissor={1,1,2,1};clear_depth({0.75f,1,0x03,0x0f});
    auto& depth_image=image_for(depth);auto planes=make_buffer(nullptr,60,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy regions[2]{};for(int i=0;i<2;i++){regions[i].bufferOffset=planes.offset+(i?48:0);regions[i].imageSubresource={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};regions[i].imageExtent={4,3,1};}
    auto cmd=begin_commands();barrier(cmd,depth_image,depth_image.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cmd,depth_image.image,depth_image.layout,planes.buffer,2,regions);finish_commands(cmd);flush_commands();
    for(int i=0;i<12;i++){float value;memcpy(&value,static_cast<const uint8_t*>(planes.mapped)+i*4,4);bool changed=i==5||i==6;valid&=value==(changed?0.75f:0.25f);valid&=static_cast<const uint8_t*>(planes.mapped)[48+i]==(changed?0xa3:0xa5);}
    destroy_buffer(planes);
    // Read through the public transfer implementation, including the packed
    // float32 + stencil8 layout and untouched per-row padding.
    std::array<uint8_t,128> packed_depth;packed_depth.fill(0xcc);
    copy.tex=&depth;copy.src=reinterpret_cast<uintptr_t>(packed_depth.data());copy.r={0,0,0,4,3,1};
    copy_from_texture(copy,40,128);
    for(size_t i=0;i<packed_depth.size();i++){
        size_t y=i/40,x=(i%40)/8,component=i%8;
        uint8_t expected=0xcc;
        if(y<3 && x<4){float value=y==1&&(x==1||x==2)?0.75f:0.25f;uint32_t stencil=y==1&&(x==1||x==2)?0xa3:0xa5;uint8_t pixel[8];memcpy(pixel,&value,4);memcpy(pixel+4,&stencil,4);expected=pixel[component];}
        valid&=packed_depth[i]==expected;
    }
    // Distinct depth/stencil values in two layers, poisoned unused stencil bits,
    // independent row/layer strides and an offset texture view.
    tex_t depth_array=depth;depth_array.serial=nvn_new_serial();depth_array.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;depth_array.d.depth=2;nvn_tex_layout(&depth_array.d,&depth_array.L);
    std::array<uint8_t,256> depth_input,depth_output;depth_input.fill(0xee);depth_output.fill(0xcc);
    for(size_t z=0;z<2;z++)for(size_t y=0;y<3;y++)for(size_t x=0;x<4;x++){
        float value=float(1+z*12+y*4+x)/32;uint32_t stencil=0xdead0000|uint32_t(0x40+z*12+y*4+x);
        auto* p=depth_input.data()+z*128+y*40+x*8;memcpy(p,&value,4);memcpy(p+4,&stencil,4);
    }
    copy={};copy.tex=&depth_array;copy.src=reinterpret_cast<uintptr_t>(depth_input.data());copy.r={0,0,0,4,3,2};
    copy_row_stride=40;copy_image_stride=128;copy_to_texture(copy);
    copy.src=reinterpret_cast<uintptr_t>(depth_output.data());copy_from_texture(copy,40,128);
    for(size_t i=0;i<depth_output.size();i++){
        size_t y=(i%128)/40,x=((i%128)%40)/8,component=i%128%40%8;
        uint8_t expected=y<3&&x<4?(component>=5?0:depth_input[i]):0xcc;valid&=depth_output[i]==expected;
    }
    depth_output.fill(0xcc);copy.has_view=true;copy.view.set_layers=1;copy.view.min_layer=1;copy.view.nlayers=1;
    copy.r={1,1,0,2,2,1};copy_from_texture(copy,24,64);
    for(size_t i=0;i<depth_output.size();i++){
        size_t y=i/24,x=(i%24)/8,component=i%8;
        uint8_t expected=y<2&&x<2?(component>=5?0:depth_input[128+(y+1)*40+(x+1)*8+component]):0xcc;valid&=depth_output[i]==expected;
    }
    // Packaged textures use the same plane conversion after linear unpacking.
    tex_t packaged_depth=depth;packaged_depth.serial=nvn_new_serial();packaged_depth.d.flags|=0x10;packaged_depth.d.stride=40;packaged_depth.mem=depth_input.data();nvn_tex_layout(&packaged_depth.d,&packaged_depth.L);
    depth_output.fill(0xcc);copy={};copy.tex=&packaged_depth;copy.src=reinterpret_cast<uintptr_t>(depth_output.data());copy.r={0,0,0,4,3,1};copy_from_texture(copy,40,128);
    for(size_t i=0;i<depth_output.size();i++){
        size_t y=i/40,x=(i%40)/8,component=i%8;
        uint8_t expected=y<3&&x<4?(component>=5?0:depth_input[i]):0xcc;valid&=depth_output[i]==expected;
    }
    // D24S8: independently clear on GPU, then verify the public packed read.
    // Use 0 and 1 for exact UNORM endpoints; masked stencil draws exercise both
    // aspects without relying on upload/readback conversions canceling out.
    tex_t d24=depth;d24.serial=nvn_new_serial();d24.d.format=NVN_FORMAT_DEPTH24_STENCIL8;nvn_tex_layout(&d24.d,&d24.L);
    render_targets={};render_targets.depth=&d24;render_state.scissor={0,0,4,3};
    clear_depth({1.0f,1,0xa5,255});render_state.scissor={1,1,2,1};clear_depth({0.0f,1,3,15});
    std::array<uint8_t,160> d24_output;d24_output.fill(0xcc);
    copy={};copy.tex=&d24;copy.src=reinterpret_cast<uintptr_t>(d24_output.data());copy.r={0,0,0,4,3,1};copy_from_texture(copy,24,80);
    for(size_t i=0;i<d24_output.size();i++){
        size_t y=i/24,x=(i%24)/4,component=i%4;uint8_t expected=0xcc;
        if(y<3&&x<4){bool changed=y==1&&(x==1||x==2);uint32_t word=changed?0xa3000000u:0xa5ffffffu;expected=(word>>(component*8))&255;}
        valid&=d24_output[i]==expected;
    }
    // Two layers, padded rows/images, subrectangle and offset view. Distinct
    // nontrivial depths and stencil values catch bit-order and stride mistakes.
    tex_t d24_array=d24;d24_array.serial=nvn_new_serial();d24_array.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;d24_array.d.depth=2;nvn_tex_layout(&d24_array.d,&d24_array.L);
    std::array<uint8_t,160> d24_input;d24_input.fill(0xee);
    for(size_t z=0;z<2;z++)for(size_t y=0;y<3;y++)for(size_t x=0;x<4;x++){
        uint32_t index=z*12+y*4+x;uint32_t word=((0x40+index)<<24)|((0x123456+index*0x54321)&0xffffff);
        memcpy(d24_input.data()+z*80+y*24+x*4,&word,4);
    }
    copy={};copy.tex=&d24_array;copy.src=reinterpret_cast<uintptr_t>(d24_input.data());copy.r={0,0,0,4,3,2};copy_row_stride=24;copy_image_stride=80;copy_to_texture(copy);
    d24_output.fill(0xcc);copy.src=reinterpret_cast<uintptr_t>(d24_output.data());copy_from_texture(copy,24,80);
    for(size_t i=0;i<d24_output.size();i++){size_t y=(i%80)/24,x=(i%80)%24;valid&=d24_output[i]==(y<3&&x<16?d24_input[i]:0xcc);}
    d24_output.fill(0xcc);copy.has_view=true;copy.view.set_layers=1;copy.view.min_layer=1;copy.view.nlayers=1;copy.r={1,1,0,2,2,1};copy_from_texture(copy,12,32);
    for(size_t i=0;i<d24_output.size();i++){size_t y=i/12,x=i%12;valid&=d24_output[i]==(y<2&&x<8?d24_input[80+(y+1)*24+4+x]:0xcc);}
    tex_t d24_packaged=d24;d24_packaged.serial=nvn_new_serial();d24_packaged.d.flags|=0x10;d24_packaged.d.stride=24;d24_packaged.mem=d24_input.data();nvn_tex_layout(&d24_packaged.d,&d24_packaged.L);
    d24_output.fill(0xcc);copy={};copy.tex=&d24_packaged;copy.src=reinterpret_cast<uintptr_t>(d24_output.data());copy.r={0,0,0,4,3,1};copy_from_texture(copy,24,80);
    for(size_t i=0;i<d24_output.size();i++){size_t y=i/24,x=i%24;valid&=d24_output[i]==(y<3&&x<16?d24_input[i]:0xcc);}
    destroy_image(d24.serial);destroy_image(d24_array.serial);destroy_image(d24_packaged.serial);
    // A zero stride means the layout's default pitch (32 or 128 byte aligned),
    // not width * bytes-per-pixel. Poison the row padding to catch tight copies.
    std::array<tex_t,2> linear_textures;
    for(unsigned mode=0;mode<2;mode++){
        auto& linear=linear_textures[mode];linear=texture;linear.serial=nvn_new_serial();
        linear.d.target=NVN_TEXTURE_TARGET_2D;linear.d.depth=1;linear.d.width=3;linear.d.flags=0x10|(mode?0x40:0);linear.d.stride=0;nvn_tex_layout(&linear.d,&linear.L);
        std::array<uint8_t,384> pixels;pixels.fill(0xa5);size_t pitch=mode?128:32;
        for(size_t y=0;y<3;y++)for(size_t x=0;x<12;x++)pixels[y*pitch+x]=uint8_t(1+y*40+x);
        linear.mem=pixels.data();std::array<uint8_t,64> readback;readback.fill(0xcc);
        copy={};copy.tex=&linear;copy.src=reinterpret_cast<uintptr_t>(readback.data());copy.r={0,0,0,3,3,1};copy_from_texture(copy,20,64);
        for(size_t i=0;i<readback.size();i++){size_t y=i/20,x=i%20;valid&=readback[i]==(y<3&&x<12?uint8_t(1+y*40+x):0xcc);}
    }
    render_targets=previous_targets;render_state.scissor=previous_scissor;
    copy_row_stride=old_row;copy_image_stride=old_image;
    for(auto serial:{texture.serial,multisample.serial,depth.serial,depth_array.serial,packaged_depth.serial})destroy_image(serial);
    for(auto& linear:linear_textures)destroy_image(linear.serial);
    return valid;
}
extern "C" uint64_t vkhost_last_fence() {
    // A queue fence also orders presents already issued by the guest. The
    // present dependency was captured before this barrier, so no cycle exists.
    auto present=presentation_queue().last();
    return translation_queue().push([present](uint64_t sequence){presentation_queue().wait(present);enqueue_gpu_completion(sequence,{});});
}
extern "C" int vkhost_probe_deswizzle(){
    drain_queues();std::lock_guard lock(state_mutex);bool valid=true;unsigned cases=0;
    const int formats[]={NVN_FORMAT_R8,NVN_FORMAT_RG8,NVN_FORMAT_RGBA8,NVN_FORMAT_RGBA16F,NVN_FORMAT_RGBA32F,NVN_FORMAT_RGB_DXT1,NVN_FORMAT_RGBA_DXT5,NVN_FORMAT_RGBA_ASTC_5x4,NVN_FORMAT_RGBA_ASTC_12x12};
    for(int format:formats)for(int target:{NVN_TEXTURE_TARGET_2D,NVN_TEXTURE_TARGET_2D_ARRAY,NVN_TEXTURE_TARGET_3D,NVN_TEXTURE_TARGET_CUBEMAP}){
        tex_t texture{};texture.d.format=format;texture.d.target=target;texture.d.width=67;texture.d.height=35;texture.d.depth=target==NVN_TEXTURE_TARGET_3D?9:target==NVN_TEXTURE_TARGET_2D_ARRAY?3:1;texture.d.levels=5;nvn_tex_layout(&texture.d,&texture.L);
        std::vector<uint8_t> storage(texture.L.size);texture.mem=storage.data();
        for(size_t i=0;i<storage.size();i++)storage[i]=uint8_t((i*37)^(i>>7)^(i>>13));
        bool was_batching=batching;batching=(cases%2)==0;
        for(unsigned level=0;level<texture.d.levels;level++)for(unsigned layer=0;layer<texture.L.layers;layer++){
            uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;valid&=!nvn_deswizzle_geom(&texture.d,level,layer,&row,&rows,&slices,&base,&th,&td,&bpb);
            size_t count=size_t(row)*rows*slices,padded=(count+3)&~size_t(3);std::vector<uint8_t> expected(count);valid&=!nvn_deswizzle(&texture.d,level,layer,storage.data(),expected.data(),row);
            auto output=make_buffer(nullptr,padded+16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);memset(output.mapped,0xcd,output.size);
            valid&=gpu_deswizzle(texture,level,layer,output);flush_commands();valid&=memcmp(output.mapped,expected.data(),count)==0;
            for(size_t i=padded;i<output.size;i++)valid&=static_cast<uint8_t*>(output.mapped)[i]==0xcd;
            destroy_buffer(output);++cases;
        }
        // Two copies of a single address in one batch must retain distinct CPU
        // versions. Checking only independent dispatches cannot catch this.
        batching=true;uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;nvn_deswizzle_geom(&texture.d,0,0,&row,&rows,&slices,&base,&th,&td,&bpb);
        size_t count=size_t(row)*rows*slices;std::vector<uint8_t> expected(count);nvn_deswizzle(&texture.d,0,0,storage.data(),expected.data(),row);
        auto old=make_buffer(nullptr,(count+3)&~size_t(3),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);valid&=gpu_deswizzle(texture,0,0,old);
        for(auto& byte:storage)byte^=0x5a;
        auto next=make_buffer(nullptr,(count+3)&~size_t(3),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);valid&=gpu_deswizzle(texture,0,0,next);flush_commands();
        valid&=memcmp(old.mapped,expected.data(),count)==0;for(auto& byte:expected)byte^=0x5a;valid&=memcmp(next.mapped,expected.data(),count)==0;
        destroy_buffer(old);destroy_buffer(next);batching=was_batching;
    }
    ot2_host_log("GPU_DESWIZZLE_CONTRACT cases=%u valid=%d",cases,valid);return valid;
}
extern "C" int vkhost_probe_texture_coherence(){
    const size_t page=sysconf(_SC_PAGESIZE),storage_size=page*3;auto* storage=static_cast<uint8_t*>(mmap(nullptr,storage_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(storage==MAP_FAILED)ot2_host_die("Texture coherence probe allocation failed");
    pool_t pool{};pool.mem=storage;pool.size=storage_size;vkhost_pool_add(storage,storage_size);
    tex_t texture{};texture.magic=MAGIC('T','E','X',0);texture.serial=nvn_new_serial();texture.pool=&pool;texture.mem=storage;
    texture.d.target=NVN_TEXTURE_TARGET_2D;texture.d.format=NVN_FORMAT_RGBA8;texture.d.width=4;texture.d.height=3;texture.d.depth=texture.d.levels=1;texture.d.pkg_block_h=1;nvn_tex_layout(&texture.d,&texture.L);
    std::array<uint8_t,48> expected{},actual{};for(size_t i=0;i<expected.size();i++)expected[i]=uint8_t(i+1);
    if(nvn_swizzle(&texture.d,0,0,expected.data(),16,storage))ot2_host_die("Texture coherence probe swizzle failed");
    NVNcopyRegion region{0,0,0,4,3,1};vkhost_read_texels(&texture,0,0,&region,actual.data(),0,0);bool valid=expected==actual;
    valid&=nvn_dirty_tracked(storage,texture.L.size);auto faults_before=nvn_dirty_fault_count();
    vkhost_read_texels(&texture,0,0,&region,actual.data(),0,0);
    {std::lock_guard lock(state_mutex);valid&=images.at(texture.serial)->source_uploads==1&&images.at(texture.serial)->source_tracked;}
    // No object recreation or descriptor registration: a streaming write must
    // update the next sample/read of the same NVN texture.
    for(auto& value:expected)value^=0x9a;
    if(nvn_swizzle(&texture.d,0,0,expected.data(),16,storage))ot2_host_die("Texture coherence probe update failed");
    vkhost_read_texels(&texture,0,0,&region,actual.data(),0,0);valid&=expected==actual&&nvn_dirty_fault_count()>faults_before;
    {std::lock_guard lock(state_mutex);valid&=images.at(texture.serial)->source_uploads==2;}
    // One in-flight batch copies the old version, updates the source, then
    // copies the new version. Re-upload barriers must preserve both snapshots.
    auto previous=expected;tex_t snapshots[2]={texture,texture};for(auto& t:snapshots){t.serial=nvn_new_serial();t.d.pkg_block_h=0;t.mem=nullptr;t.pool=nullptr;}
    {
        std::lock_guard lock(state_mutex);bool was_batching=batching;batching=true;
        p_copy_t2t copy{};copy.src=&texture;copy.dst=&snapshots[0];copy.sr=copy.dr=region;copy_texture(copy);
        for(auto& value:expected)value^=0x35;valid&=nvn_swizzle(&texture.d,0,0,expected.data(),16,storage)==0;
        copy.dst=&snapshots[1];copy_texture(copy);flush_commands();batching=was_batching;
    }
    vkhost_read_texels(&snapshots[0],0,0,&region,actual.data(),0,0);valid&=actual==previous;
    vkhost_read_texels(&snapshots[1],0,0,&region,actual.data(),0,0);valid&=actual==expected;
    for(auto& t:snapshots)vkhost_tex_release(&t);
    // Same protected destination, but written by the kernel as in ReadFile.
    for(auto& value:expected)value^=0xa7;std::vector<uint8_t> packaged(texture.L.size);
    valid&=nvn_swizzle(&texture.d,0,0,expected.data(),16,packaged.data())==0;
    int fd=memfd_create("ot2-texture-coherence",MFD_CLOEXEC);if(fd<0)ot2_host_die("Texture coherence file allocation failed");
    valid&=write(fd,packaged.data(),packaged.size())==ssize_t(packaged.size());
    {NvnDirtyWrite lease(storage,packaged.size());valid&=lease.locked;valid&=pread(fd,storage,packaged.size(),0)==ssize_t(packaged.size());}close(fd);
    vkhost_read_texels(&texture,0,0,&region,actual.data(),0,0);valid&=expected==actual;
    // Host threads can fill a guest pool too. Their fault must be handled before
    // requiring a guest execution context; they never enter the ARM64 bridge.
    valid&=nvn_dirty_protect(storage+page,page*2);faults_before=nvn_dirty_fault_count();
    std::atomic<bool> stop{false};std::atomic<unsigned> ready{0};
    auto writer=[&](size_t offset,uint8_t value){volatile uint8_t* p=storage+offset;*p=value;ready.fetch_add(1);while(!stop)*p=value;};
    std::thread writer_one(writer,page,uint8_t(0xa1)),writer_two(writer,page*2,uint8_t(0xb2));
    while(ready<2)std::this_thread::yield();
    for(unsigned i=0;i<100;i++)valid&=nvn_dirty_protect(storage+page,page*2);
    stop=true;writer_one.join();writer_two.join();
    valid&=storage[page]==0xa1&&storage[page*2]==0xb2&&nvn_dirty_fault_count()>faults_before;
    vkhost_tex_release(&texture);vkhost_pool_del(storage);memset(storage,0,storage_size);munmap(storage,storage_size);
    return valid;
}
extern "C" int vkhost_wait(uint64_t fence,uint64_t timeout) {return fence&present_ticket_bit?presentation_queue().wait(fence&~present_ticket_bit,timeout):gpu_completion.wait(fence,timeout);}
extern "C" void vkhost_finish() {drain_queues();{std::lock_guard lock(state_mutex);flush_commands();if(device)check(vkDeviceWaitIdle(device),"idle");checkpoint_driver_cache();}driver_cache_writer().drain();}
extern "C" void vkhost_suspend(int on) {
    {std::lock_guard lock(presentation_wait_mutex);presentation_suspended=on!=0;}
    presentation_changed.notify_all();
    if(on){translation_queue().suspend(true);presentation_queue().suspend(true);completion_queue().drain();std::lock_guard lock(state_mutex);flush_commands();performance_hint_close();if(device){check(vkDeviceWaitIdle(device),"suspend drain");refresh_cache_budget(true);}}
    else {{std::lock_guard lock(state_mutex);if(device)refresh_cache_budget();}presentation_queue().suspend(false);translation_queue().suspend(false);}
}
extern "C" void vkhost_submit(int n,const uint64_t* handles) {
    if(n<0||(n&&!handles))ot2_host_die("Invalid submission handle list");
    auto pinned=std::shared_ptr<void>(nvn_commands_pin(n,handles),nvn_commands_unpin);
    std::vector<uint64_t> owned;if(n)owned.assign(handles,handles+n);
    translation_queue().push([owned,pinned=std::move(pinned)](uint64_t sequence)mutable{
        FrameTraceScope job_trace("OT2 translation job");
        // Present waits must precede the Vulkan state lock; the presenter needs
        // that lock to finish and return the acquired texture.
        {FrameTraceScope trace("OT2 present dependencies");for(auto ticket:nvn_commands_waits(owned.size(),owned.data()))if(ticket&present_ticket_bit)vkhost_wait(ticket,UINT64_MAX);}
        const auto hint_begin=monotonic_ns();
        std::vector<const uint8_t*> index;
        {FrameTraceScope trace("OT2 command index");if(expanded_pass_planning.load())index=nvn_commands_index(pinned.get(),owned.size(),owned.data());}
        std::shared_ptr<GpuBatch> batch;
        {
            std::unique_lock lock(state_mutex,std::defer_lock);
            {FrameTraceScope trace("OT2 translation state lock");lock.lock();}
            FrameTraceScope replay_trace("OT2 replay");replay_sequence=sequence;PerfScope timing(perf.replay_ns);ThreadCpuScope cpu_timing(perf.replay_cpu_ns);
            indexed_commands=std::span<const uint8_t* const>(index);indexed_cursor=0;if(!indexed_commands.empty())++indexed_submissions;
            batching=batch_enabled;for(auto handle:owned)replay(reinterpret_cast<const uint8_t*>(handle),0);indexed_commands={};flush_commands(false);batching=false;batch=last_gpu_batch;
            performance_hint_record(sequence,hint_begin,monotonic_ns());
#ifdef OT2_NVN_TESTING
            ++test_gpu_encoded;
#endif
            if(sequence<=10||(verbose_gpu_logs&&sequence%100==0))ot2_host_log("GPU_SUBMISSION_ENCODED sequence=%llu handles=%zu",(unsigned long long)sequence,owned.size());
        }
        enqueue_gpu_completion(sequence,std::move(batch),std::move(pinned));
    },[&](uint64_t sequence){nvn_commands_publish_fences(n,handles,sequence);});
}
extern "C" uint64_t vkhost_frame_count() {return presented_frames.load();}
extern "C" void vkhost_frame_stats(double out[4]){std::lock_guard lock(frame_stats_mutex);auto s=frame_stats.sample(monotonic_ns());out[0]=s.fps;out[1]=s.mean_ms;out[2]=s.p95_ms;out[3]=s.source;}
// A blank frame can be blamed on the guest or on the translator forever unless
// the exact image the guest hands to present is measured. This reports that
// image's identity plus a quantised colour histogram, so "nothing renders" and
// "one flat colour is presented" stop looking the same from the outside.
static void present_digest(const tex_t& texture) {
    auto& img=image_for(texture);
    if(img.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)return;
    int bw,bh,bytes=nvn_fmt_bpb(texture.d.format,&bw,&bh);
    const unsigned w=texture.d.width,h=texture.d.height;
    if(bw!=1||bh!=1||!w||!h||size_t(w)*h*bytes>32u*1024*1024)return;
    const size_t size=size_t(w)*h*bytes;
    auto buffer=make_buffer(nullptr,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy copy{};copy.bufferOffset=buffer.offset;copy.imageSubresource={img.aspects,0,0,1};copy.imageExtent={w,h,1};
    flush_commands();
    auto cmd=begin_commands();barrier(cmd,img,img.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cmd,img.image,img.layout,buffer.buffer,1,&copy);finish_commands(cmd);flush_commands();
    static uint32_t histogram[65536];
    memset(histogram,0,sizeof histogram);
    const auto* pixels=static_cast<const uint8_t*>(buffer.mapped);
    uint64_t samples=0;unsigned long long sr=0,sg=0,sb=0;
    const unsigned step=std::max(1U,h/128u);
    for(unsigned y=0;y<h;y+=step)for(unsigned x=0;x<w;x+=4){
        const uint8_t* p=pixels+size_t(y)*w*bytes+size_t(x)*bytes;
        const unsigned r=p[bytes>=3?0:0],g=p[bytes>=3?1:0],b=p[bytes>=3?2:0];
        sr+=r;sg+=g;sb+=b;++samples;
        const unsigned bucket=((r>>3)<<11)|((g>>3)<<5)|(b>>3);
        ++histogram[bucket];
    }
    destroy_buffer(buffer);
    unsigned best=0;uint32_t best_count=0,distinct=0;
    for(unsigned i=0;i<65536;i++)if(histogram[i]){++distinct;if(histogram[i]>best_count){best_count=histogram[i];best=i;}}
    ot2_host_log("PRESENT_TEX serial=%llu %ux%u fmt=%u bpp=%d samples=%llu distinct=%u dominant=(%u,%u,%u) share=%.3f mean=(%llu,%llu,%llu)",
        (unsigned long long)texture.serial,w,h,texture.d.format,bytes,(unsigned long long)samples,distinct,
        ((best>>11)&31)<<3,((best>>5)&63)<<2,(best&31)<<3,samples?double(best_count)/samples:0.,
        (unsigned long long)(samples?sr/samples:0),(unsigned long long)(samples?sg/samples:0),(unsigned long long)(samples?sb/samples:0));
}
static void present_frame(const tex_t* texture,int interval,const int* crop,uint64_t dependency) {
    FrameTraceScope trace("OT2 present frame");
    const auto work_end=monotonic_ns();
    {std::lock_guard lock(state_mutex);performance_hint_frame(dependency,interval,work_end,window&&!presentation_suspended);}
    static nvn_pacer pacer{};
    const auto period=uint64_t(std::clamp(interval,0,5))*16666667;
    nvn_pace(&pacer,monotonic_ns(),period);
    const auto deadline=pacer.target;
    auto wait_for_slot=[&]{
        FrameTraceScope pace_trace("OT2 frame pace");
        if(polling_pacer){
            while(!presentation_suspended){auto now=monotonic_ns();if(now>=deadline)break;std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(deadline-now,1000000)));}
        }else{
            std::unique_lock wait_lock(presentation_wait_mutex);
            auto now=monotonic_ns();if(now<deadline)presentation_changed.wait_for(wait_lock,std::chrono::nanoseconds(deadline-now),[]{return presentation_suspended.load();});
        }
    };
    // Keep the same cadence and bounded queue. Complete the final blit during
    // available slack, rather than beginning GPU work at its display deadline.
    const bool queue_early=present_before_pace&&get_display_timings;
    const bool prepare_first=prepare_before_pace||queue_early;
    if(!prepare_first)wait_for_slot();
    const auto prepare_start=monotonic_ns();
    std::unique_lock lock(state_mutex);
    if(!window||presentation_suspended){pacer={};return;} // Every dropped frame still retires its ticket.
    if(!surface) {
        VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};si.window=window;
        check(vkCreateAndroidSurfaceKHR(instance,&si,nullptr,&surface),"Android surface");
        VkBool32 supported=0;check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,queue_family,surface,&supported),"surface support");
        if(!supported)ot2_host_die("Graphics queue cannot present");
    }
    if(!swapchain) {
        VkSurfaceCapabilitiesKHR caps{};check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"surface capabilities");
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))ot2_host_die("Surface cannot receive display rendering");
        uint32_t n=0;check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,nullptr),"surface formats");std::vector<VkSurfaceFormatKHR> formats(n);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,formats.data()),"surface formats");if(!n)ot2_host_die("Surface has no format");
        VkSurfaceFormatKHR format{};for(auto wanted:{VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM})for(const auto& f:formats)if(f.format==wanted&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){format=f;break;}
        if(!format.format)ot2_host_die("Surface lacks a display-encoded UNORM format");swapchain_format=format.format;
        swapchain_extent=caps.currentExtent;
        if(swapchain_extent.width==UINT32_MAX)swapchain_extent={uint32_t(ANativeWindow_getWidth(window)),uint32_t(ANativeWindow_getHeight(window))};
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;
        // 队列深度：+2 张（撑到 5 张）。注意本文件属于 ot2 后端，native/CMakeLists.txt
        // 按 S2A_GRAPHICS_BACKEND 与 graphics/d2r_bridge.cpp 二选一编译；当前交付走 d2r
        // 后端，所以这里的改法对发布包没有任何影响。d2r 后端对应的那份交换链在
        // d2r_bridge.cpp 内，那里 2026-10-26 的 A/B 结论是「+2 无收益、回退默认」。
        // 两处保持一致，避免下次又只改一边。
        uint32_t depth=caps.minImageCount+2;if(caps.maxImageCount)depth=std::min(depth,caps.maxImageCount);
        ci.minImageCount=std::max(2U,depth);
        ci.imageFormat=format.format;ci.imageColorSpace=format.colorSpace;ci.imageExtent=swapchain_extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if(!(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))ot2_host_die("Surface lacks compositor rotation support");
        ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        ot2_host_log("SURFACE_TRANSFORM current=%u supported=%u compositor=identity",caps.currentTransform,caps.supportedTransforms);ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;
        check(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain),"swapchain");reset_display_timings();
        check(vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr),"swapchain images");swapchain_images.resize(n);check(vkGetSwapchainImagesKHR(device,swapchain,&n,swapchain_images.data()),"swapchain images");
        ot2_host_log("SWAPCHAIN images=%u min=%u max=%u extent=%ux%u",n,caps.minImageCount,caps.maxImageCount,swapchain_extent.width,swapchain_extent.height);
        swapchain_views.resize(n);for(unsigned i=0;i<n;i++){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=swapchain_images[i];vi.format=swapchain_format;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(device,&vi,nullptr,&swapchain_views[i]),"display image view");}
    }
    if(!drawable_acquire_fence){VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fi,nullptr,&drawable_acquire_fence),"acquire fence");}
    auto acquired=drawable_acquire_fence;check(vkResetFences(device,1,&acquired),"reset acquire fence");
    lock.unlock(); // Waiting for an Android drawable must not stall translation.
    uint32_t index=0;VkResult acquire_result;
    {FrameTraceScope acquire_trace("OT2 drawable acquire");acquire_result=vkAcquireNextImageKHR(device,swapchain,50000000,VK_NULL_HANDLE,acquired,&index);}
    lock.lock();
    if(acquire_result==VK_TIMEOUT||acquire_result==VK_NOT_READY){return;}
    if(acquire_result==VK_ERROR_OUT_OF_DATE_KHR){check(vkQueueWaitIdle(queue),"resize wait");destroy_swapchain();return;}
    if(acquire_result!=VK_SUBOPTIMAL_KHR)check(acquire_result,"acquire image");
    check(vkWaitForFences(device,1,&acquired,VK_TRUE,UINT64_MAX),"acquire completion");
    static std::atomic<uint64_t> present_probe{0};
    // Sampling every 60th frame aliases against a double-buffered target: 60 is
    // even, so every sample lands on the same buffer and the other one is
    // invisible. The first presents are therefore recorded one by one.
    const uint64_t present_index=present_probe.fetch_add(1,std::memory_order_relaxed);
    if(present_index<240)ot2_host_log("PRESENT_ID n=%llu serial=%llu %ux%u fmt=%u",(unsigned long long)present_index,
        (unsigned long long)texture->serial,texture->d.width,texture->d.height,texture->d.format);
    if(present_index<4||present_index%37==0)present_digest(*texture);
    auto& source=image_for(*texture);Image target{};target.image=swapchain_images[index];target.format=swapchain_format;target.aspects=VK_IMAGE_ASPECT_COLOR_BIT;target.layers=target.levels=1;
    auto cmd=begin_commands();encode_present_pixels(cmd,*texture,target,swapchain_views[index],swapchain_extent,crop);
    barrier(cmd,target,target.layout,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);finish_commands(cmd);
    present_prepare_ns+=monotonic_ns()-prepare_start;
    if(prepare_first&&!queue_early){
        // set_window drains this presentation queue before destroying the
        // swapchain. The completed blit no longer needs its source image.
        lock.unlock();wait_for_slot();lock.lock();
    }
    const auto before_present=monotonic_ns();
    const auto late=before_present>deadline?before_present-deadline:0;
    present_late_ns+=late;present_late_max_ns=std::max(present_late_max_ns,late);
    VkPresentTimeGOOGLE desired{++display_id,pacer.target};VkPresentTimesInfoGOOGLE timing{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};timing.swapchainCount=1;timing.pTimes=&desired;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;if(get_display_timings)pi.pNext=&timing;VkResult present_result;
    {FrameTraceScope present_trace("OT2 queue present");present_result=vkQueuePresentKHR(queue,&pi);}
    if(present_result==VK_ERROR_OUT_OF_DATE_KHR){check(vkQueueWaitIdle(queue),"resize wait");destroy_swapchain();return;}
    if(present_result!=VK_SUBOPTIMAL_KHR)check(present_result,"present");
    if(queue_early){
        // Give Android the completed image before its requested presentation
        // timestamp. Retain the present ticket until the same 30-Hz deadline,
        // so the guest cannot run ahead merely because queueing was earlier.
        lock.unlock();wait_for_slot();lock.lock();
    }
    if(get_display_timings){
        uint32_t count=0;auto timing_status=get_display_timings(device,swapchain,&count,nullptr);
        if(timing_status==VK_SUCCESS&&count){std::vector<VkPastPresentationTimingGOOGLE> timings(count);auto status=get_display_timings(device,swapchain,&count,timings.data());if(status!=VK_SUCCESS&&status!=VK_INCOMPLETE)count=0;timings.resize(count);
            std::sort(timings.begin(),timings.end(),[](const auto& a,const auto& b){return a.actualPresentTime<b.actualPresentTime;});
            for(const auto& t:timings){if(!t.actualPresentTime||t.actualPresentTime<=display_previous)continue;record_frame_timing(t.actualPresentTime,true);if(!display_start)display_start=t.actualPresentTime;else ++display_samples;display_previous=t.actualPresentTime;
                if(display_samples>=60){double seconds=(display_previous-display_start)/1e9;ot2_host_log("DISPLAY_RATE frames=%llu seconds=%.6f fps=%.2f source=VK_GOOGLE_display_timing",(unsigned long long)display_samples,seconds,display_samples/seconds);display_start=display_previous;display_samples=0;}}
        }
    }
    if(!get_display_timings)record_frame_timing(monotonic_ns(),false);
    const auto frame=++presented_frames;
    if(capture_frame&&capture_frame_number!=5&&frame>capture_frame_number){capture_frame=false;ot2_host_log("FRAME_CAPTURE completed=%llu bytes=%llu",(unsigned long long)capture_frame_number,(unsigned long long)capture_bytes);}
    if(frame%60==0&&std::filesystem::exists(shader_directory+"/capture-next-frame")){std::filesystem::remove(shader_directory+"/capture-next-frame");capture_frame=true;capture_frame_number=frame+1;capture_bytes=0;ot2_host_log("FRAME_CAPTURE scheduled=%llu",(unsigned long long)capture_frame_number);}
    if(frame_trace_enabled.load(std::memory_order_relaxed)&&ATrace_isEnabled())ATrace_setCounter("OT2 presented frame",frame);
    if(frame%300==0)checkpoint_driver_cache();
    if(frame%60==0){
        detail_timing=std::filesystem::exists(shader_directory+"/profile-cpu-detail");
        stats_enabled=detail_timing||std::filesystem::exists(shader_directory+"/verbose-stats");
        opaque_blend_forced=std::filesystem::exists(shader_directory+"/opaque-blend");
        sprite_pixels_forced=std::filesystem::exists(shader_directory+"/sprite-pixels");
        {std::ifstream scale(shader_directory+"/sprite-pixels");float v=0;if(scale>>v&&v>0&&v<1024)sprite_pixel_scale=v;}
        sprite_instance_scale=1.f;
        {std::ifstream scale(shader_directory+"/sprite-instance-scale");float v=0;if(scale>>v&&v>0.5f&&v<1e5f)sprite_instance_scale=v;}
        {std::ifstream probe(shader_directory+"/sprite-scale");float v=0;if(probe>>v&&v>1&&v<4096)sprite_scale_probe=v;}
        paint_draws_probe=std::filesystem::exists(shader_directory+"/paint-draws");
        ui_matrix_probe=std::filesystem::exists(shader_directory+"/ui-matrix");
        trace_uniform_banks=std::filesystem::exists(shader_directory+"/trace-uniforms");
        bank_dump_probe=std::filesystem::exists(shader_directory+"/bank-dump");
        cull_none_forced=std::filesystem::exists(shader_directory+"/no-cull");
        winding_not_flipped=std::filesystem::exists(shader_directory+"/keep-winding");
        force_no_depth=std::filesystem::exists(shader_directory+"/no-depth-attach");
        sprite_rect_w=640.f;sprite_rect_h=360.f;
        {std::ifstream rect(shader_directory+"/sprite-rect");float v=0;if(rect>>v&&v>0&&v<8192)sprite_rect_w=v;
         if(rect>>v&&v>0&&v<8192)sprite_rect_h=v;}
        frame_trace_enabled=std::filesystem::exists(shader_directory+"/trace-frame");
        const bool stream_optimized=!std::filesystem::exists(shader_directory+"/no-stream-optimizations");
        if(ot2_host_stream_profile)ot2_host_stream_profile(stream_optimized&&!std::filesystem::exists(shader_directory+"/no-stream-io"));
        const bool use_resource_uploads=stream_optimized&&std::filesystem::exists(shader_directory+"/resource-uploads")&&!std::filesystem::exists(shader_directory+"/no-resource-uploads");
        if(resource_uploads&&!use_resource_uploads){resource_upload_cache.clear();resource_lru.clear();}
        resource_uploads=use_resource_uploads;
        cached_readback=stream_optimized&&!std::filesystem::exists(shader_directory+"/no-cached-readback");
        expanded_pass_planning=stream_optimized&&!std::filesystem::exists(shader_directory+"/no-expanded-pass-plan");
        nvn_dirty_use_summary(stream_optimized&&!std::filesystem::exists(shader_directory+"/no-dirty-summary"));
        uint64_t dirty_pages=0,dirty_blocks=0,dirty_skipped=0;nvn_dirty_query_stats(&dirty_pages,&dirty_blocks,&dirty_skipped);
        stats_log("DIRTY_SUMMARY pages=%llu blocks=%llu skipped=%llu",(unsigned long long)dirty_pages,(unsigned long long)dirty_blocks,(unsigned long long)dirty_skipped);
        stats_log("RESOURCE_UPLOADS enabled=%u hits=%llu misses=%llu partial=%llu uploaded_mb=%.3f saved_mb=%.3f resident_mb=%.3f limit_mb=%zu entries=%zu",resource_uploads,(unsigned long long)resource_upload_hits,(unsigned long long)resource_upload_misses,(unsigned long long)resource_upload_partial,resource_upload_bytes/1048576.,resource_upload_saved/1048576.,resource_resident_bytes/1048576.,cache_budget.resource_bytes/1048576,resource_upload_cache.size());
        resource_upload_hits=resource_upload_misses=resource_upload_partial=resource_upload_bytes=resource_upload_saved=0;
        stats_log("CACHED_READBACK enabled=%u staged_mb=%.3f copied_mb=%.3f cpu_ms=%.3f fallbacks=%llu",cached_readback,readback_staged_bytes/1048576.,readback_copied_bytes/1048576.,readback_cpu_ns/60e6,(unsigned long long)readback_fallbacks);
        readback_staged_bytes=readback_copied_bytes=readback_cpu_ns=readback_fallbacks=0;
        stats_log("COMMAND_INDEX enabled=%u submissions=%llu fallbacks=%llu",expanded_pass_planning.load(),(unsigned long long)indexed_submissions,(unsigned long long)indexed_fallbacks);indexed_submissions=indexed_fallbacks=0;
        stats_log("PASS_PLAN dead_stores=%llu",(unsigned long long)dead_attachment_stores);dead_attachment_stores=0;
        stats_log("DESCRIPTOR_DELTA enabled=%u written=%llu skipped=%llu",incremental_descriptors,(unsigned long long)descriptor_delta_written,(unsigned long long)descriptor_delta_skipped);descriptor_delta_written=descriptor_delta_skipped=0;
        const bool optimized=!std::filesystem::exists(shader_directory+"/no-batch-optimizations");
        stats_log("BATCH_OPTIMIZATIONS enabled=%u groups_hit=%llu groups_miss=%llu pipeline_versions=%llu persistent_hits=%llu saved_mb=%.3f persistent_mb=%.3f clear_fold=%llu clear_elide=%llu depth_prune=%llu scopes=%llu empty=%llu",grouped_resources,(unsigned long long)group_hits,(unsigned long long)group_misses,(unsigned long long)pipeline_version_hits,(unsigned long long)persistent_upload_hits,persistent_upload_saved/1048576.,persistent_upload_bytes/1048576.,(unsigned long long)folded_clears,(unsigned long long)elided_clears,(unsigned long long)pruned_depth,(unsigned long long)scoped_barriers,(unsigned long long)empty_draws);
        if(optimized!=grouped_resources)reset_prepared_bindings();
        grouped_resources=versioned_pipeline_state=persistent_uploads=precise_barriers=clear_planning=prune_unused_depth=incremental_descriptors=optimized;
        if(ot2_guest_batch_profile)ot2_guest_batch_profile(optimized);
        group_hits=group_misses=pipeline_version_hits=persistent_upload_hits=persistent_upload_saved=folded_clears=elided_clears=pruned_depth=scoped_barriers=empty_draws=0;
        stats_log("INDEX_SUMMARIES enabled=%u hits=%llu scans=%llu bytes=%llu entries=%zu",index_summaries.enabled,(unsigned long long)index_summaries.hits,(unsigned long long)index_summaries.scans,(unsigned long long)index_summaries.scanned_bytes,index_summaries.size());
        index_summaries.hits=index_summaries.scans=index_summaries.scanned_bytes=0;
        const bool next_indices=!std::filesystem::exists(shader_directory+"/no-index-summaries");
        if(next_indices!=index_summaries.enabled){index_summaries.clear();index_summaries.enabled=next_indices;}
        stats_log("TEXTURE_LOOKUP enabled=%u descriptors=%llu images=%llu",cache_texture_lookups,(unsigned long long)texture_lookup_hits,(unsigned long long)image_lookup_hits);texture_lookup_hits=image_lookup_hits=0;
        cache_texture_lookups=!std::filesystem::exists(shader_directory+"/no-texture-lookups");
        stats_log("IMAGE_IDENTITY fast=%u INTEGER_SELECTION enabled=%u hits=%llu misses=%llu",fast_image_identity,reuse_integer_selections,(unsigned long long)integer_selection_hits,(unsigned long long)integer_selection_misses);
        integer_selection_hits=integer_selection_misses=0;
        fast_image_identity=!std::filesystem::exists(shader_directory+"/no-identity-fastpath");
        reuse_integer_selections=!std::filesystem::exists(shader_directory+"/no-integer-selections");
        stats_log("DEPENDENCY_LOOKUP bounded=%u checks=%llu skipped=%llu",dependency_bounds_enabled,(unsigned long long)dependency_range_checks,(unsigned long long)dependency_bounds_skips);dependency_range_checks=dependency_bounds_skips=0;
        dependency_bounds_enabled=!std::filesystem::exists(shader_directory+"/no-dependency-bounds");
        stats_log("DEPENDENCY_INDEX enabled=%u checks=%llu",dependency_index_enabled,(unsigned long long)dependency_index_checks);dependency_index_checks=0;
        dependency_index_enabled=!std::filesystem::exists(shader_directory+"/no-dependency-index");
        stats_log("DRAW_DEPENDENCIES precise=%u",precise_draw_dependencies);
        precise_draw_dependencies=!std::filesystem::exists(shader_directory+"/no-precise-draw-dependencies");
        stats_log("DYNAMIC_DEPTH_BOUNDS enabled=%u",dynamic_depth_bounds);
        dynamic_depth_bounds=!std::filesystem::exists(shader_directory+"/no-dynamic-depth-bounds");
        stats_log("DESCRIPTOR_CONTENTS enabled=%u hits=%llu indexed=%zu working_set=%u live=%zu",retain_descriptor_contents,(unsigned long long)descriptor_contents_hits,idle_descriptor_contents.size(),retain_descriptor_working_set,descriptor_live_slots);descriptor_contents_hits=0;
        retain_descriptor_contents=!std::filesystem::exists(shader_directory+"/no-descriptor-contents");
        retain_descriptor_working_set=!std::filesystem::exists(shader_directory+"/no-descriptor-working-set");
        stats_log("DESCRIPTOR_LAYOUT_INPUTS enabled=%u hits=%llu entries=%zu index_nodes=%llu",reuse_layout_inputs,(unsigned long long)descriptor_layout_input_hits,descriptor_layout_inputs.size(),(unsigned long long)descriptor_contents_node_creates);descriptor_layout_input_hits=0;
        reuse_layout_inputs=!std::filesystem::exists(shader_directory+"/no-layout-input-cache");
        stats_log("DESCRIPTOR_TEMPLATES enabled=%u updates=%llu creates=%llu",use_descriptor_templates,(unsigned long long)descriptor_template_updates,(unsigned long long)descriptor_template_creates);
        use_descriptor_templates=std::filesystem::exists(shader_directory+"/descriptor-templates");
        recycle_commands=std::filesystem::exists(shader_directory+"/recycle-commands");
        if(!recycle_commands)trim_commands();
        stats_log("COMMAND_RECYCLING enabled=%u allocated=%llu reused=%llu freed=%llu idle=%zu",recycle_commands,(unsigned long long)command_allocations,(unsigned long long)command_reuses,(unsigned long long)command_frees,idle_commands.size());
        stats_log("SNAPSHOT_LOOKUP flat=%u lookups=%llu capacity=%zu signatures=%u",flat_buffer_snapshots,(unsigned long long)flat_snapshot_lookups,readonly_snapshots.flat.capacity(),reuse_descriptor_signature);flat_snapshot_lookups=0;
        stats_log("FRONT_CACHES snapshots=%u hits=%llu misses=%llu dependencies=%u hits=%llu misses=%llu",snapshot_front_cache,(unsigned long long)snapshot_front_hits,(unsigned long long)snapshot_front_misses,dependency_memo_enabled,(unsigned long long)dependency_memo_hits,(unsigned long long)dependency_memo_misses);
        snapshot_front_hits=snapshot_front_misses=dependency_memo_hits=dependency_memo_misses=0;
        snapshot_front_cache=!std::filesystem::exists(shader_directory+"/no-snapshot-frontcache");
        dependency_memo_enabled=!std::filesystem::exists(shader_directory+"/no-dependency-memo");
        stats_log("CPU_KEY_WORK packed=%u union=%u skips=%llu checks=%llu ranges=%zu",packed_descriptor_keys,dependency_union_enabled,(unsigned long long)dependency_union_skips,(unsigned long long)dependency_union_checks,inflight_write_union.size());
        dependency_union_skips=dependency_union_checks=0;
        packed_descriptor_keys=!std::filesystem::exists(shader_directory+"/no-packed-keys");
        dependency_union_enabled=!std::filesystem::exists(shader_directory+"/no-dependency-union");
        retain_texel_descriptors=!std::filesystem::exists(shader_directory+"/no-texel-descriptor-contents");
        linear_descriptor_delta=!std::filesystem::exists(shader_directory+"/no-linear-descriptor-delta");
        stats_log("CPU_BINDING_WORK linear_delta=%u reflected_preflight=%u uniform_checks=%llu",linear_descriptor_delta,reflected_preflight_banks,(unsigned long long)preflight_uniform_checks);
        preflight_uniform_checks=0;
        reflected_preflight_banks=!std::filesystem::exists(shader_directory+"/no-reflected-preflight");
        const bool next_flat=!std::filesystem::exists(shader_directory+"/no-flat-snapshots");
        if(next_flat!=flat_buffer_snapshots){readonly_snapshots.clear();flat_buffer_snapshots=next_flat;}
        reuse_descriptor_signature=!std::filesystem::exists(shader_directory+"/no-signature-reuse");
        stats_log("DYNAMIC_DESCRIPTORS enabled=%u updates=%llu",dynamic_buffer_descriptors,(unsigned long long)descriptor_set_updates);descriptor_set_updates=0;
        const bool next_dynamic=std::filesystem::exists(shader_directory+"/dynamic-descriptors")&&!std::filesystem::exists(shader_directory+"/no-dynamic-descriptors");
        if(next_dynamic!=dynamic_buffer_descriptors){dynamic_buffer_descriptors=next_dynamic;reset_binding_cache();}
        verbose_gpu_logs=std::filesystem::exists(shader_directory+"/verbose-gpu-logs");
        {std::ifstream traced(shader_directory+"/trace-ops-frame");uint64_t value=0;if(traced>>value&&value){trace_ops_frame=value;trace_op_index=0;memset(trace_op_hist,0,sizeof trace_op_hist);ot2_host_log("OP_TRACE armed for frame %llu",(unsigned long long)value);}}
        polling_pacer=std::filesystem::exists(shader_directory+"/polling-pacer");
        stats_log("PRESENT_SCHEDULE prepare_first=%u queue_early=%u prepare_ms=%.3f late_ms=%.3f max_late_ms=%.3f",prepare_first,queue_early,present_prepare_ns/60e6,present_late_ns/60e6,present_late_max_ns/1e6);
        present_prepare_ns=present_late_ns=present_late_max_ns=0;
        prepare_before_pace=std::filesystem::exists(shader_directory+"/prepare-before-pace")&&!std::filesystem::exists(shader_directory+"/no-prepare-before-pace");
        present_before_pace=std::filesystem::exists(shader_directory+"/present-before-pace");
        refresh_cache_budget();
        stats_log("TEXEL_VIEW_CACHE enabled=%u hits=%llu creates=%llu evictions=%llu entries=%zu limit=%zu",reuse_texel_views,(unsigned long long)texel_view_hits,(unsigned long long)texel_view_creates,(unsigned long long)texel_view_evictions,texel_view_cache.size(),texel_view_limit);
        reuse_texel_views=!std::filesystem::exists(shader_directory+"/no-texel-view-cache");
        if(ot2_guest_permission_profile)ot2_guest_permission_profile(!std::filesystem::exists(shader_directory+"/no-permission-cache"));
        synchronous_capacity_flush=std::filesystem::exists(shader_directory+"/synchronous-capacity-flush");
        stats_log("UNIFORM_UPLOAD compact=%u robust=%u bytes=%llu",compact_uniforms,robust_uniform_ranges,(unsigned long long)uniform_allocation_bytes);uniform_allocation_bytes=0;
        compact_uniforms=!std::filesystem::exists(shader_directory+"/no-compact-uniforms");
        stats_log("DRAW_PREPARATION creates=%llu reuses=%llu idle=%zu",(unsigned long long)draw_object_creates,(unsigned long long)draw_object_reuses,idle_draw_objects.size());
        reuse_draw_objects=!std::filesystem::exists(shader_directory+"/no-draw-object-reuse");
        stats_log("PIPELINE_PREPARATION hits=%llu misses=%llu entries=%zu",(unsigned long long)pipeline_preparation_hits,(unsigned long long)pipeline_preparation_misses,prepared_graphics_pipelines.size());
        reuse_pipeline_preparation=!std::filesystem::exists(shader_directory+"/no-pipeline-preparation");
        reuse_resource_objects=!std::filesystem::exists(shader_directory+"/no-resource-object-reuse");
        stats_log("RESOURCE_PREPARATION hits=%llu misses=%llu",(unsigned long long)resource_preparation_hits,(unsigned long long)resource_preparation_misses);
        reuse_resource_preparation=!std::filesystem::exists(shader_directory+"/no-resource-preparation");
        command_state_report();
        performance_hint_report();
        gpu_profile_report();
        ot2_host_log("REPLAY_CPU frames=60 cpu_ms=%.3f wall_ms=%.3f",perf.replay_cpu_ns/60e6,perf.replay_ns/60e6);
        if(ot2_guest_execution_profile)ot2_guest_execution_profile(!std::filesystem::exists(shader_directory+"/no-fast-tls"));
        const bool register_gates=std::filesystem::exists(shader_directory+"/register-gates");
        if(ot2_guest_register_gates)ot2_guest_register_gates(register_gates);
        stats_log("REGISTER_GATES enabled=%u",register_gates);
        stats_log("PRECISE_PREPARATION enabled=%u hits=%llu checks=%llu fallback=%llu",precise_resource_dependencies,(unsigned long long)precise_dependency_hits,(unsigned long long)precise_dependency_checks,(unsigned long long)precise_dependency_fallbacks);
        const bool precise_preparation=std::filesystem::exists(shader_directory+"/precise-preparation");
        if(precise_resource_dependencies!=precise_preparation)reset_prepared_bindings();
        precise_resource_dependencies=precise_preparation;precise_dependency_hits=precise_dependency_checks=precise_dependency_fallbacks=0;
        stats_log("CONSTANT_DECODE direct=%u expanded_mb=%.3f",direct_storage_decode,constant_bank_expanded_bytes/(60.0*1048576));
        constant_bank_expanded_bytes=0;direct_storage_decode=!std::filesystem::exists(shader_directory+"/no-direct-storage-decode");
        stats_log("ATTACHMENT_STORES enabled=%u color_none=%llu depth_none=%llu stencil_none=%llu promotions=%llu fallbacks=%llu",readonly_stores_enabled,(unsigned long long)attachment_stores.none[0],(unsigned long long)attachment_stores.none[1],(unsigned long long)attachment_stores.none[2],(unsigned long long)attachment_stores.promotions,(unsigned long long)attachment_stores.fallbacks);
        attachment_stores={};readonly_stores_enabled=std::filesystem::exists(shader_directory+"/readonly-stores")&&!std::filesystem::exists(shader_directory+"/no-readonly-stores");
    }
    if(frame%60==0){
        // A frame can present steadily while drawing nothing at all, so the
        // view state is reported next to the geometry counters: a zero-sized
        // scissor or an inverted viewport silently discards every draw without
        // touching any of them.
        ot2_host_log("VIEW_STATE viewport=%.1f,%.1f,%.1fx%.1f,%.2f..%.2f scissor=%d,%d,%dx%d enable=%u depth_test=%u depth_write=%u depth_func=%u color_mask=%#x samples=%u",
            render_state.viewport[0],render_state.viewport[1],render_state.viewport[2],render_state.viewport[3],
            render_state.viewport[4],render_state.viewport[5],
            render_state.scissor.x,render_state.scissor.y,render_state.scissor.w,render_state.scissor.h,
            render_state.render_enable,unsigned(render_state.depth_stencil.dt),unsigned(render_state.depth_stencil.dw),
            unsigned(render_state.depth_stencil.df),render_state.channel_mask.mask,unsigned(render_state.multisample.samples));
        // Blend decides whether a draw changes the pixel at all: factors that
        // multiply the source out to zero leave the cleared colour untouched and
        // the frame presents as if nothing had been drawn.
        ot2_host_log("BLEND_STATE enable=%#x t0_target=%u src_rgb=%u dst_rgb=%u src_a=%u dst_a=%u eq_rgb=%u eq_a=%u forced=%d",
            render_state.color.blend_en,unsigned(render_state.blends[0].target),
            unsigned(render_state.blends[0].src_rgb),unsigned(render_state.blends[0].dst_rgb),
            unsigned(render_state.blends[0].src_a),unsigned(render_state.blends[0].dst_a),
            unsigned(render_state.blends[0].eq_rgb),unsigned(render_state.blends[0].eq_a),int(opaque_blend_forced));
        // How much of the frame's geometry actually reached the GPU. A title can run
        // at a steady frame rate while drawing nothing -- the clear still presents --
        // so the presented rate alone does not say whether rendering works.
        ot2_host_log("DRAW_FILTER skipped_missing=%llu cond_undecided=%llu cond_decided=%llu cond_skipped=%llu unbound_attr=%llu",
            (unsigned long long)skipped_missing_draws,(unsigned long long)undecided_conditional_draws,
            (unsigned long long)decided_conditional_draws,(unsigned long long)skipped_conditional_draws,
            (unsigned long long)unbound_attribute_draws.load(std::memory_order_relaxed));
        ot2_host_log("DRAW_SUBMITTED draws=%llu indices=%llu vertices=%llu",
            (unsigned long long)draws_submitted.load(std::memory_order_relaxed),
            (unsigned long long)indices_submitted.load(std::memory_order_relaxed),
            (unsigned long long)vertices_submitted.load(std::memory_order_relaxed));
        static uint64_t previous_ns=0;timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);uint64_t now=uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec;if(previous_ns)ot2_host_log("PRESENT_RATE frames=60 seconds=%.3f fps=%.2f",double(now-previous_ns)/1e9,60e9/double(now-previous_ns));previous_ns=now;}
    if(frame%60==0&&geometry_query_workaround)stats_log("SOFTWARE_SAMPLES draws=%llu",(unsigned long long)software_sample_draws);
    if(frame%60==0){stats_log("PIPELINE_CACHE foreground=%llu warm=%llu claimed=%llu rejected=%llu running=%u",(unsigned long long)foreground_created.load(),(unsigned long long)warm_created.load(),(unsigned long long)warm_claimed.load(),(unsigned long long)warm_rejected.load(),warm_running.load());asynchronous_gpu=!std::filesystem::exists(shader_directory+"/synchronous-gpu");stats_log("GPU_RING enabled=%u peak=%llu slots=%zu reuses=%llu",asynchronous_gpu,(unsigned long long)gpu_inflight_peak,gpu_ring_size,(unsigned long long)gpu_ring_reuses);batch_query_resets=!std::filesystem::exists(shader_directory+"/synchronous-queries");buffer_mirrors_enabled=!std::filesystem::exists(shader_directory+"/no-buffer-mirrors");reuse_bindings=!std::filesystem::exists(shader_directory+"/uncached-bindings");gpu_deswizzle_enabled=!std::filesystem::exists(shader_directory+"/cpu-deswizzle");stats_log("BUFFER_MIRRORS enabled=%u reuses=%llu writebacks=%llu",buffer_mirrors_enabled,(unsigned long long)mirror_reuses,(unsigned long long)mirror_writebacks);batch_enabled=!std::filesystem::exists(shader_directory+"/synchronous");merge_enabled=!std::filesystem::exists(shader_directory+"/separate-passes");reuse_descriptors=!std::filesystem::exists(shader_directory+"/uncached-descriptors");stats_log("FRAME_COST frames=60 passes=%llu submits=%llu wait_ms=%.3f draw_ms=%.3f buffer_ms=%.3f upload_mb=%.3f vertex_mb=%.3f uniform_mb=%.3f storage_mb=%.3f allocs=%llu pipeline_ms=%.3f replay_ms=%.3f",(unsigned long long)perf.passes,(unsigned long long)perf.submits,perf.wait_ns/60e6,perf.draw_ns/60e6,perf.buffer_ns/60e6,perf.buffer_bytes/(60.0*1048576),perf.vertex_bytes/(60.0*1048576),perf.uniform_bytes/(60.0*1048576),perf.storage_bytes/(60.0*1048576),(unsigned long long)perf.buffer_allocs,perf.pipeline_ns/60e6,perf.replay_ns/60e6);report_descriptors();perf={};}
    if(source.drawn && (frame<=10 || frame%60==0))(frame<=10?ot2_host_log:stats_log)("GAME_FRAME_PRESENTED frame=%llu draw=%llu",(unsigned long long)frame,(unsigned long long)source.drawn);
    if(frame<=3 || frame%60==0)(frame<=3?ot2_host_log:stats_log)("HOST_PRESENTATION frame=%llu source=%ux%u output=%ux%u nvn_format=%u source_vk=%u output_vk=%u scanout=raw-view",(unsigned long long)frame,texture->d.width,texture->d.height,swapchain_extent.width,swapchain_extent.height,texture->d.format,source.format,swapchain_format);
}
extern "C" uint64_t vkhost_present(const tex_t* texture,int interval,const int* crop){
    vkhost_tex_retain(texture->serial);
    auto snapshot=std::shared_ptr<tex_t>(new tex_t(*texture),[](tex_t* t){vkhost_tex_unref(t->serial);delete t;});
    std::array<int,4> region;std::copy_n(crop,4,region.begin());
    const auto dependency=translation_queue().last();
    return present_ticket_bit|presentation_queue().push([snapshot,interval,region,dependency](uint64_t){
        FrameTraceScope trace("OT2 presentation job");
        {FrameTraceScope wait_trace("OT2 presentation GPU dependency");gpu_completion.wait(dependency);}
        present_frame(snapshot.get(),interval,region.data(),dependency);
    });
}
#ifdef OT2_NVN_TESTING
// Kept at this seam so the fixture uses the actual descriptor decoder.
#include "descriptor_capacity_contract.inc"
#include "../tests/nvn_batch_optimization_contract.inc"
#include "../tests/nvn_stream_contract.inc"
#include "../tests/nvn_preparation_contract.inc"
#include "../tests/nvn_layered_contract.inc"
#include "../tests/nvn_format_contract.inc"
extern "C" int vkhost_test_integer_selections(){
    drain_queues();std::lock_guard lock(state_mutex);flush_commands();
    const bool previous=reuse_integer_selections;reuse_integer_selections=true;
    Shader shader;shader.module=clear_module(format_texel_probe_comp);
    shader.resources=std::make_unique<ShaderReflection>(Json{{"key","123456789abcdef0"},{"cbufs",Json::array()},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}});
    shader.resources->textures.push_back(ShaderResource{.binding=1});
    std::vector<ShaderIntegerImage> formats{{1,0,spv::ImageFormatR16ui}};bool valid=true;
    auto check_selection=[&]{
        const auto expected=integer_texel_module(shader.module,formats,*shader.resources);
        const auto misses=integer_selection_misses,hits=integer_selection_hits;
        valid&=integer_texel_module(shader,formats)==expected&&integer_selection_misses==misses+1;
        valid&=integer_texel_module(shader,formats)==expected&&integer_selection_hits==hits+1;
    };
    check_selection();formats[0].image_format=spv::ImageFormatR32ui;check_selection();
    formats[0].signedness=1;formats[0].image_format=spv::ImageFormatR32i;check_selection();
    formats[0].binding=7;check_selection(); // An integer resource of another stage must not specialize this one.
    formats[0]={1,0,spv::ImageFormatR16ui};check_selection();
    valid&=integer_texel_module(shader,{})==shader.module;
    reuse_integer_selections=previous;return valid;
}
#include "../tests/nvn_histogram_contract.inc"
#include "../tests/nvn_pixel_id_contract.inc"
#include "../tests/nvn_constant_contract.inc"
#include "../tests/nvn_bc_contract.inc"
extern "C" uint64_t vkhost_test_gpu_profile(int enabled,int stat){
    drain_queues();std::lock_guard lock(state_mutex);gpu_profile_enabled=enabled!=0;
    if(gpu_tick_delta(250,5,8)!=11||gpu_tick_delta(UINT64_MAX-3,4,64)!=8||gpu_tick_delta(5,10,0)!=0)ot2_host_die("GPU timestamp wrap contract");
    if(stat==0)return gpu_profile_batches.count;if(stat==1)return uint64_t(gpu_profile_batches.ns);if(stat==2)return gpu_profiles.size();if(stat==3)return gpu_profile_dropped;
    uint64_t regions=0;for(auto& [label,value]:gpu_profile_regions)regions+=value.count;return regions;
}
extern "C" void vkhost_test_wait_pipelines(){std::unique_lock lock(warm_done_mutex);if(!warm_done.wait_for(lock,std::chrono::seconds(getenv("OT2_WARM_ONLY")?180:30),[]{return !warm_running;}))ot2_host_die("Pipeline warm-up test timeout");}
extern "C" VkPhysicalDevice vkhost_test_physical_device(){return physical;}
extern "C" int vkhost_test_robust_uniform_ranges(){return robust_uniform_ranges;}
extern "C" int vkhost_test_native_null(){return null_descriptors;}
extern "C" uint64_t vkhost_test_command_recycling(int enabled){drain_queues();std::lock_guard lock(state_mutex);flush_commands();recycle_commands=enabled;if(!enabled)trim_commands();return command_reuses;}
extern "C" uint64_t vkhost_test_pipeline_stat(int which){return which==0?foreground_created.load():which==1?warm_created.load():which==2?warm_claimed.load():warm_rejected.load();}
extern "C" int vkhost_test_pipeline_singleflight(){
    if(pipeline_objects.empty())return 0;auto expected=pipeline_objects.begin()->second;
    std::mutex mutex;std::condition_variable changed;bool entered=false,release=false;std::atomic<unsigned> builds=0,finished=0;
    std::array<VkPipeline,8> answers{};std::vector<std::thread> threads;
    auto build=[&]{++builds;std::unique_lock lock(mutex);entered=true;changed.notify_all();changed.wait(lock,[&]{return release;});return expected;};
    threads.emplace_back([&]{answers[0]=request_pipeline("test-pipeline-singleflight",true,build);++finished;});
    {std::unique_lock lock(mutex);changed.wait(lock,[&]{return entered;});}
    for(size_t i=1;i<answers.size();i++)threads.emplace_back([&,i]{answers[i]=request_pipeline("test-pipeline-singleflight",false,build);++finished;});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));bool valid=builds==1&&finished==0;
    {std::lock_guard lock(mutex);release=true;}changed.notify_all();for(auto& thread:threads)thread.join();
    valid&=builds==1;for(auto p:answers)valid&=p==expected;
    valid&=!request_pipeline("test-pipeline-retry",true,[]{return VkPipeline{};});
    valid&=request_pipeline("test-pipeline-retry",false,[&]{return expected;})==expected;
    unsigned duplicate=0;valid&=request_pipeline("test-pipeline-retry",false,[&]{++duplicate;return expected;})==expected&&duplicate==0;
    bool caught=false;try{request_pipeline("test-pipeline-exception",true,[]()->VkPipeline{throw std::runtime_error("injected optional recipe failure");});}catch(const std::runtime_error&){caught=true;}
    valid&=caught&&request_pipeline("test-pipeline-exception",false,[&]{return expected;})==expected;
    return valid;
}
extern "C" void vkhost_test_hold_gpu(int asynchronous){
    drain_queues();std::lock_guard lock(state_mutex);asynchronous_gpu=asynchronous!=0;
    if(!test_gpu_event){VkEventCreateInfo ci{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};check(vkCreateEvent(device,&ci,nullptr,&test_gpu_event),"test GPU event");}
    check(vkResetEvent(device,test_gpu_event),"reset test GPU event");test_hold_next_gpu=true;
}
extern "C" void vkhost_test_release_gpu(){check(vkSetEvent(device,test_gpu_event),"release test GPU event");}
extern "C" uint64_t vkhost_test_encoded_count(){return test_gpu_encoded.load();}
extern "C" uint64_t vkhost_test_submitted_count(){return test_gpu_submitted.load();}
extern "C" void vkhost_test_query_batching(int enabled){drain_queues();std::lock_guard lock(state_mutex);batch_query_resets=enabled!=0;}
extern "C" uint64_t vkhost_test_gpu_submissions(){drain_queues();std::lock_guard lock(state_mutex);return perf.submits;}
extern "C" uint64_t vkhost_test_render_passes(){drain_queues();std::lock_guard lock(state_mutex);return perf.passes;}
extern "C" uint64_t vkhost_test_attachment_stores(int enabled,int which){drain_queues();std::lock_guard lock(state_mutex);readonly_stores_enabled=enabled!=0;return which<3?attachment_stores.none[which]:which==3?attachment_stores.promotions:attachment_stores.fallbacks;}
extern "C" uint64_t vkhost_test_buffer_mirrors(int enabled){drain_queues();std::lock_guard lock(state_mutex);buffer_mirrors_enabled=enabled!=0;return mirror_reuses;}
extern "C" void vkhost_test_precise_draw_dependencies(int enabled){drain_queues();std::lock_guard lock(state_mutex);precise_draw_dependencies=enabled!=0;}
extern "C" uint64_t vkhost_test_dynamic_depth_bounds(int enabled){drain_queues();std::lock_guard lock(state_mutex);dynamic_depth_bounds=enabled!=0;return foreground_created.load();}
extern "C" uint64_t vkhost_test_texel_cache(int enabled,int stat,size_t limit){drain_queues();std::lock_guard lock(state_mutex);reuse_texel_views=enabled!=0;texel_view_limit=limit;return stat?texel_view_creates:texel_view_hits;}
extern "C" int vkhost_test_texel_cache_lifetime(){
    drain_queues();std::lock_guard lock(state_mutex);bool valid=true;reuse_texel_views=true;
    while(texel_idle_first)erase_idle_texel_view(texel_idle_first);
    const auto old_limit=texel_view_limit;texel_view_limit=2;
    auto b=make_buffer(nullptr,4096,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};info.buffer=b.buffer;info.range=16;info.format=VK_FORMAT_R32_UINT;
    auto first=acquire_texel_view(info,true),second=acquire_texel_view(info,true);
    valid&=first.cached&&second.cached==first.cached&&first.cached->users==2;
    info.offset=256;auto other=acquire_texel_view(info,true);valid&=other.cached&&other.view!=first.view;
    info.offset=512;auto overflow=acquire_texel_view(info,true);valid&=!overflow.cached&&texel_view_cache.size()==2;
    release_texel_view(overflow);release_texel_view(second);valid&=first.cached->users==1;
    release_texel_view(other);
    for(unsigned i=0;i<16;++i){info.offset=i<2?0:256*(i%7+1);info.range=i&1?16:32;info.format=i&1?VK_FORMAT_R32_SINT:VK_FORMAT_R32_UINT;
        auto entry=acquire_texel_view(info,true);valid&=entry.cached&&entry.view!=first.view&&texel_view_cache.size()==2;release_texel_view(entry);}
    // The held lease survives every eviction and diagnostic disable.
    reuse_texel_views=false;info.offset=0;info.range=16;info.format=VK_FORMAT_R32_UINT;
    auto uncached=acquire_texel_view(info,true);valid&=!uncached.cached&&uncached.view!=first.view;release_texel_view(uncached);
    release_texel_view(first);invalidate_buffer_views(b.buffer);valid&=texel_view_cache.empty()&&!texel_idle_first&&!texel_idle_last;
    // Two backing buffers: invalidation must visit only its own views, even
    // with another buffer leased and a larger idle working set beside it.
    reuse_texel_views=true;texel_view_limit=64;
    auto adjacent=make_buffer(nullptr,4096,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    info.buffer=adjacent.buffer;auto held=acquire_texel_view(info,true);
    for(unsigned i=1;i<=8;++i){info.offset=i*256;auto lease=acquire_texel_view(info,true);release_texel_view(lease);}
    info.buffer=b.buffer;
    for(unsigned i=0;i<4;++i){info.offset=i*256;auto lease=acquire_texel_view(info,true);release_texel_view(lease);}
    const auto visits=texel_view_invalidation_visits;invalidate_buffer_views(b.buffer);
    valid&=texel_view_invalidation_visits-visits==4&&texel_view_cache.size()==9&&texel_buffer_heads.size()==1&&held.cached->users==1;
    texel_view_limit=0;trim_texel_views();valid&=texel_view_cache.size()==1&&held.cached->users==1;
    release_texel_view(held);valid&=texel_view_cache.empty()&&texel_buffer_heads.empty()&&!texel_idle_first&&!texel_idle_last;
    destroy_buffer(adjacent);destroy_buffer(b);reuse_texel_views=true;texel_view_limit=old_limit;return valid;
}
extern "C" void vkhost_test_depth_storage_counts(uint64_t* out){
    std::lock_guard lock(state_mutex);out[0]=image_storage.size();out[1]=image_storage_owners.size();out[2]=texture_storage_keys.size();
}
extern "C" void vkhost_test_lighting_mask_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);
    for(unsigned i=0;i<2;i++){
        auto key=shader_key(data[i].control);if(shaders.contains(key))continue;Shader s;s.module=i?clear_module(lighting_mask_probe_frag):clear_module(present_vert);s.facts_recorded=true;
        Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}};
        if(i)meta["textures"].push_back({{"binding",1},{"handle",0x168/4},{"cbuf_slot",-1},{"type",3},{"array",1},{"separate",false}});
        s.reflection=meta;s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
    }
}
extern "C" void vkhost_test_integer_2d_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;
    Shader s;s.module=clear_module(integer_2d_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array({{{"binding",2},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}}})},
        {"textures",Json::array({{{"binding",1},{"handle",0x168/4},{"cbuf_slot",-1},{"type",3},{"array",1},{"separate",false}}})},{"images",Json::array()}};
    s.reflection=meta;s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_cube_program(const NVNshaderData* data,int array){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;
    Shader s;s.module=array?clear_module(cube_array_probe_comp):clear_module(cube_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array({{{"binding",2},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}}})},
        {"textures",Json::array({{{"binding",1},{"handle",0x168/4},{"cbuf_slot",-1},{"type",array?261:5},{"array",1},{"separate",false}}})},{"images",Json::array()}};
    s.reflection=meta;s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_texel_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;
    Shader s;s.module=static_cast<const uint8_t*>(data->control)[0x40]==88?clear_module(format_texel_probe_comp):clear_module(texel_view_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array({{{"binding",2},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}}})},
        {"textures",Json::array({{{"binding",1},{"handle",0x168/4},{"cbuf_slot",-1},{"type",2},{"array",1},{"separate",false}}})},{"images",Json::array()}};
    s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_compute_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;Shader s;s.module=clear_module(mirror_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array({{{"binding",1},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}}})},{"textures",Json::array()},{"images",Json::array()}};s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" uint64_t vkhost_test_binding_cache(int enabled){drain_queues();std::lock_guard lock(state_mutex);reuse_bindings=reuse_resource_preparation=enabled!=0;reset_prepared_bindings();return binding_cache_hits;}
extern "C" uint64_t vkhost_test_snapshot_cache(int enabled){drain_queues();std::lock_guard lock(state_mutex);readonly_snapshots.clear();flat_buffer_snapshots=reuse_descriptor_signature=enabled!=0;return flat_snapshot_lookups;}
extern "C" uint64_t vkhost_test_index_cache(int enabled){drain_queues();std::lock_guard lock(state_mutex);if(index_summaries.enabled!=(enabled!=0))index_summaries.clear();index_summaries.enabled=enabled!=0;return index_summaries.hits;}
extern "C" uint64_t vkhost_test_draw_reuse(int enabled){drain_queues();std::lock_guard lock(state_mutex);reuse_draw_objects=enabled!=0;return draw_object_reuses;}
extern "C" uint64_t vkhost_test_prepare_reuse(int enabled){drain_queues();std::lock_guard lock(state_mutex);reuse_pipeline_preparation=reuse_resource_objects=enabled!=0;return pipeline_preparation_hits;}
extern "C" uint64_t vkhost_test_resource_preparation(){drain_queues();std::lock_guard lock(state_mutex);return resource_preparation_hits;}
extern "C" uint64_t vkhost_test_command_state(int enabled,int kind){drain_queues();std::lock_guard lock(state_mutex);command_state_enabled=enabled!=0;return command_state_stats.skipped[kind];}
extern "C" uint64_t vkhost_test_dynamic_descriptors(int enabled){drain_queues();std::lock_guard lock(state_mutex);dynamic_buffer_descriptors=enabled!=0;reset_binding_cache();return descriptor_set_updates;}
extern "C" uint64_t vkhost_test_descriptor_contents(int enabled){drain_queues();std::lock_guard lock(state_mutex);retain_descriptor_contents=enabled!=0;invalidate_descriptor_contents();return descriptor_contents_hits;}
extern "C" uint64_t vkhost_test_layout_inputs(int enabled){drain_queues();std::lock_guard lock(state_mutex);reuse_layout_inputs=enabled!=0;return descriptor_layout_input_hits;}
extern "C" uint64_t vkhost_test_descriptor_templates(int enabled){drain_queues();std::lock_guard lock(state_mutex);use_descriptor_templates=enabled!=0;return descriptor_template_updates;}
extern "C" uint64_t vkhost_test_texel_descriptors(int enabled){drain_queues();std::lock_guard lock(state_mutex);retain_texel_descriptors=enabled!=0;return descriptor_delta_skipped+descriptor_contents_hits;}
extern "C" void vkhost_test_reflected_preflight(int enabled){drain_queues();std::lock_guard lock(state_mutex);reflected_preflight_banks=enabled!=0;}
extern "C" uint64_t vkhost_test_dependency_lookup(uintptr_t address,size_t bytes,int enabled,int* result){std::lock_guard lock(state_mutex);const bool use_union=dependency_union_enabled;dependency_union_enabled=false;dependency_index_enabled=false;dependency_bounds_enabled=enabled!=0;const bool memo=dependency_memo_enabled;dependency_memo_enabled=false;*result=previous_cpu_overlap(address,bytes);dependency_union_enabled=use_union;dependency_memo_enabled=memo;return dependency_range_checks;}
extern "C" uint64_t vkhost_test_dependency_index(uintptr_t address,size_t bytes,int enabled,int* result){std::lock_guard lock(state_mutex);const bool use_union=dependency_union_enabled;dependency_union_enabled=false;dependency_index_enabled=enabled!=0;const bool memo=dependency_memo_enabled;dependency_memo_enabled=false;*result=previous_cpu_overlap(address,bytes);dependency_union_enabled=use_union;dependency_memo_enabled=memo;return dependency_index_checks;}
extern "C" uint64_t vkhost_test_dependency_union(uintptr_t address,size_t bytes,int* result){std::lock_guard lock(state_mutex);dependency_union_enabled=true;*result=previous_cpu_overlap(address,bytes);return dependency_union_skips;}
extern "C" uint64_t vkhost_test_dependency_memo(uintptr_t address,size_t bytes,int* result){std::lock_guard lock(state_mutex);const bool use_union=dependency_union_enabled;dependency_union_enabled=false;dependency_memo_enabled=true;*result=previous_cpu_overlap(address,bytes);dependency_union_enabled=use_union;return dependency_memo_hits;}
extern "C" uint64_t vkhost_test_compact_uniforms(int enabled){drain_queues();std::lock_guard lock(state_mutex);compact_uniforms=enabled!=0;return uniform_allocation_bytes;}
extern "C" void vkhost_test_uniform_range_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;
    Shader s;s.module=clear_module(uniform_range_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array({{{"binding",1},{"slot",3}}})},{"sbufs",Json::array({{{"binding",2},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}}})},{"textures",Json::array()},{"images",Json::array()}};
    s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_null_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;
    Shader s;s.module=clear_module(null_resource_probe_comp);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array({{{"binding",3},{"slot",4}}})},{"sbufs",Json::array({{{"binding",1},{"cb_slot",0},{"cb_offset",0x310/4},{"flags",1}},{{"binding",2},{"cb_slot",0},{"cb_offset",0x320/4},{"flags",1}}})},{"textures",Json::array()},{"images",Json::array()}};
    unsigned types[]={3,4,5,259,261,515,1027,2,3,2,4,259};
    for(unsigned i=0;i<std::size(types);i++)meta[i<8?"textures":"images"].push_back({{"binding",i+4},{"handle",0x168/4},{"cbuf_slot",-1},{"type",types[i]},{"array",1},{"separate",false},{"flags",i<8?0:4}});
    s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_vertex_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);
    for(int i=0;i<2;i++){
        auto key=shader_key(data[i].control);if(shaders.contains(key))continue;Shader s;s.module=i?clear_module(state_probe_frag):clear_module(vertex_binding_probe_vert);s.facts_recorded=true;
        if(!i)s.inputs={0};
        Json meta={{"key",key},{"cbufs",i?Json::array({{{"binding",2},{"slot",3}}}):Json::array()},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}};
        s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
    }
}
extern "C" void vkhost_test_attachment_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);auto key=shader_key(data->control);if(shaders.contains(key))return;Shader s;s.module=clear_module(attachment_probe_frag);s.facts_recorded=true;
    Json meta={{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array()},{"textures",Json::array({{{"binding",3},{"handle",0x40000001},{"cbuf_slot",-1},{"type",3},{"array",1},{"separate",false}}})},{"images",Json::array()}};
    s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
}
extern "C" void vkhost_test_state_program(const NVNshaderData* data){
    std::lock_guard lock(state_mutex);
    for(int i=0;i<2;i++){
        auto key=shader_key(data[i].control);if(shaders.contains(key))continue;Shader s;s.module=i?clear_module(state_probe_frag):clear_module(state_probe_vert);s.facts_recorded=true;
        Json meta={{"key",key},{"cbufs",Json::array({{{"binding",i+1},{"slot",3}}})},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}};
        s.resources=std::make_unique<ShaderReflection>(meta);shaders.emplace(key,std::move(s));
    }
}
namespace {std::mutex test_gate_mutex;std::condition_variable test_gate_changed;bool test_gate_entered=false,test_gate_open=false;}
namespace {std::mutex present_gate_mutex;std::condition_variable present_gate_changed;bool present_gate_entered=false,present_gate_open=false;}
extern "C" void vkhost_test_present_gate(int hold){
    if(hold){
        {std::lock_guard lock(present_gate_mutex);present_gate_entered=present_gate_open=false;}
        presentation_queue().push([](uint64_t){std::unique_lock lock(present_gate_mutex);present_gate_entered=true;present_gate_changed.notify_all();present_gate_changed.wait(lock,[]{return present_gate_open;});});
        std::unique_lock lock(present_gate_mutex);present_gate_changed.wait(lock,[]{return present_gate_entered;});
    }else{{std::lock_guard lock(present_gate_mutex);present_gate_open=true;}present_gate_changed.notify_all();}
}
extern "C" void vkhost_test_queue_gate(int hold){
    if(hold){
        {std::lock_guard lock(test_gate_mutex);test_gate_entered=test_gate_open=false;}
        translation_queue().push([](uint64_t sequence){{std::unique_lock lock(test_gate_mutex);test_gate_entered=true;test_gate_changed.notify_all();test_gate_changed.wait(lock,[]{return test_gate_open;});}enqueue_gpu_completion(sequence,{});});
        std::unique_lock lock(test_gate_mutex);test_gate_changed.wait(lock,[]{return test_gate_entered;});
    }else{{std::lock_guard lock(test_gate_mutex);test_gate_open=true;}test_gate_changed.notify_all();}
}
extern "C" bool vkhost_test_is_suspended(){return translation_queue().is_paused();}
#endif
