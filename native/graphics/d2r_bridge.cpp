// d2r Vulkan backend: platform half.
//
// This file owns everything the d2r replay engine does not: the VkInstance,
// the VkDevice, the Android surface and swapchain, the blit that puts the
// engine's finished image on screen, and the small vkhost_* surface that
// native/graphics/frontend.c calls. The command stream itself is replayed by
// ../d2r/gfx/vulkan/engine (unmodified upstream, MIT).
//
// It is a drop-in replacement for graphics/backend.cpp: same vkhost_* entry
// points, same declarations in graphics/nvn_obj.h, so frontend.c and the HLE
// layer do not change at all. Which of the two is compiled is a build option
// (S2A_GRAPHICS_BACKEND), never both.
//
// Threading: frontend.c may call in from the guest thread while the engine
// runs its own graphics thread. Every entry point here therefore takes
// state_mutex, and the engine calls back into present() with that same mutex
// already held by us (the engine's present runs on our submit thread), so
// present() must not take it again.
// VK_USE_PLATFORM_ANDROID_KHR must precede the loader so the ANativeWindow
// surface entry points are declared; the NDK's vulkan.h only offers them then.
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

#include "nvn_obj.h"
#include "nvn_commands.h"
#include "device_limits.h"
// Copying a host-linear region into NVN's block-linear storage: the same code
// the ot2 backend uses, so both backends agree byte for byte on where a texel
// lands in pool memory.
#include "texture_write.h"
#include "../thread_services.h"

extern "C" {
#include "../d2r/gfx/vulkan/nvn_vk.h"
// The engine's host-facing entry points (pool / register / submit / present /
// read_texels / device queries). nvn_vk.h only carries the configuration type.
#include "../d2r/gfx/vulkan/engine/xbg_game_internal.h"
}

// Provided by frontend.c: the serial counter that keys texture and program
// objects (the engine uses the same number to find them in its own tables).
uint64_t nvn_new_serial(void);

namespace {

std::recursive_mutex state_mutex;

VkInstance instance{};
VkPhysicalDevice physical{};
VkDevice device{};
VkQueue queue{};
uint32_t queue_family{};
VkSurfaceKHR surface{};
ANativeWindow *window{};
VkSwapchainKHR swapchain{};
VkFormat swapchain_format{};
VkExtent2D swapchain_extent{};
std::vector<VkImage> swapchain_images;
std::vector<VkImageView> swapchain_views;
VkCommandPool present_pool{};
VkCommandBuffer present_command{};
VkSemaphore present_done{};
VkFence present_fence{};

bool engine_ready{};
bool suspend_present{};
// The picture is turned by this many degrees clockwise on its way to the
// swapchain, because this title renders a 1920x1080 landscape frame while the
// display surface is a portrait surface that the compositor rotates again
// (mDisplayRotation=ROTATION_90 on the test tablet). vkCmdBlitImage cannot
// rotate, so a turned picture goes through a one-quad render pass instead.
int present_rotation{};
// Vertical mirror applied to the blit in the 0-degree path. Off matches the
// upstream Android frontend, whose blit is a straight copy: the engine's own
// viewport already applies `y + height, -height`.
bool flip_present{};

// Defined further down. The present path rebuilds a lost chain itself rather
// than reporting the frame as failed, so it needs these up front.
bool create_surface_and_swapchain();
void destroy_swapchain();
void destroy_surface();

// Render-pass path for a rotated present: one triangle, sampling the finished
// image. Compiled from the two shaders kept beside this file.
static const uint32_t k_rotate_vertex[] = {
    0x07230203,0x00010300,0x000d000a,0x00000075,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0008000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x00000016,0x00000021,0x00000063,
    0x00040047,0x00000016,0x0000000b,0x0000002a,0x00050048,0x0000001f,0x00000000,0x0000000b,
    0x00000000,0x00050048,0x0000001f,0x00000001,0x0000000b,0x00000001,0x00050048,0x0000001f,
    0x00000002,0x0000000b,0x00000003,0x00050048,0x0000001f,0x00000003,0x0000000b,0x00000004,
    0x00030047,0x0000001f,0x00000002,0x00050048,0x00000028,0x00000000,0x00000023,0x00000000,
    0x00050048,0x00000028,0x00000001,0x00000023,0x00000008,0x00050048,0x00000028,0x00000002,
    0x00000023,0x00000010,0x00050048,0x00000028,0x00000003,0x00000023,0x00000020,0x00030047,
    0x00000028,0x00000002,0x00040047,0x00000063,0x0000001e,0x00000000,0x00020013,0x00000002,
    0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,
    0x00000020,0x00000000,0x0004002b,0x0000000a,0x0000000b,0x00000006,0x0004001c,0x0000000c,
    0x00000007,0x0000000b,0x0004002b,0x00000006,0x0000000d,0x00000000,0x0005002c,0x00000007,
    0x0000000e,0x0000000d,0x0000000d,0x0004002b,0x00000006,0x0000000f,0x3f800000,0x0005002c,
    0x00000007,0x00000010,0x0000000f,0x0000000d,0x0005002c,0x00000007,0x00000011,0x0000000d,
    0x0000000f,0x0005002c,0x00000007,0x00000012,0x0000000f,0x0000000f,0x0009002c,0x0000000c,
    0x00000013,0x0000000e,0x00000010,0x00000011,0x00000010,0x00000012,0x00000011,0x00040015,
    0x00000014,0x00000020,0x00000001,0x00040020,0x00000015,0x00000001,0x00000014,0x0004003b,
    0x00000015,0x00000016,0x00000001,0x00040020,0x00000018,0x00000007,0x0000000c,0x00040017,
    0x0000001c,0x00000006,0x00000004,0x0004002b,0x0000000a,0x0000001d,0x00000001,0x0004001c,
    0x0000001e,0x00000006,0x0000001d,0x0006001e,0x0000001f,0x0000001c,0x00000006,0x0000001e,
    0x0000001e,0x00040020,0x00000020,0x00000003,0x0000001f,0x0004003b,0x00000020,0x00000021,
    0x00000003,0x0004002b,0x00000014,0x00000022,0x00000000,0x0004002b,0x00000006,0x00000024,
    0x40000000,0x0006001e,0x00000028,0x00000007,0x00000007,0x0000001c,0x0000000a,0x00040020,
    0x00000029,0x00000009,0x00000028,0x0004003b,0x00000029,0x0000002a,0x00000009,0x00040020,
    0x0000002b,0x00000009,0x00000007,0x0004002b,0x00000014,0x0000002f,0x00000001,0x00040020,
    0x00000036,0x00000003,0x0000001c,0x0004002b,0x00000014,0x0000003a,0x00000003,0x00040020,
    0x0000003b,0x00000009,0x0000000a,0x0004002b,0x0000000a,0x0000003e,0x0000005a,0x00020014,
    0x0000003f,0x0004002b,0x0000000a,0x0000004e,0x000000b4,0x0004002b,0x0000000a,0x00000058,
    0x0000010e,0x00040020,0x00000062,0x00000003,0x00000007,0x0004003b,0x00000062,0x00000063,
    0x00000003,0x0004002b,0x00000014,0x00000064,0x00000002,0x00040020,0x00000065,0x00000009,
    0x0000001c,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,
    0x0004003b,0x00000018,0x00000019,0x00000007,0x0004003d,0x00000014,0x00000017,0x00000016,
    0x0003003e,0x00000019,0x00000013,0x00050041,0x00000008,0x0000001a,0x00000019,0x00000017,
    0x0004003d,0x00000007,0x0000001b,0x0000001a,0x0005008e,0x00000007,0x00000025,0x0000001b,
    0x00000024,0x00050083,0x00000007,0x00000027,0x00000025,0x00000012,0x00050041,0x0000002b,
    0x0000002c,0x0000002a,0x00000022,0x0004003d,0x00000007,0x0000002d,0x0000002c,0x00050041,
    0x0000002b,0x00000030,0x0000002a,0x0000002f,0x0004003d,0x00000007,0x00000031,0x00000030,
    0x0008000c,0x00000007,0x00000032,0x00000001,0x00000032,0x00000027,0x0000002d,0x00000031,
    0x00050051,0x00000006,0x00000033,0x00000032,0x00000000,0x00050051,0x00000006,0x00000034,
    0x00000032,0x00000001,0x00070050,0x0000001c,0x00000035,0x00000033,0x00000034,0x0000000d,
    0x0000000f,0x00050041,0x00000036,0x00000037,0x00000021,0x00000022,0x0003003e,0x00000037,
    0x00000035,0x00050041,0x0000003b,0x0000003c,0x0000002a,0x0000003a,0x0004003d,0x0000000a,
    0x0000003d,0x0000003c,0x000500aa,0x0000003f,0x00000040,0x0000003d,0x0000003e,0x000300f7,
    0x00000042,0x00000000,0x000400fa,0x00000040,0x00000041,0x0000004b,0x000200f8,0x00000041,
    0x00050051,0x00000006,0x00000045,0x0000001b,0x00000001,0x00050051,0x00000006,0x00000048,
    0x0000001b,0x00000000,0x00050083,0x00000006,0x00000049,0x0000000f,0x00000048,0x00050050,
    0x00000007,0x0000004a,0x00000045,0x00000049,0x000200f9,0x00000042,0x000200f8,0x0000004b,
    0x000500aa,0x0000003f,0x0000004f,0x0000003d,0x0000004e,0x000300f7,0x00000051,0x00000000,
    0x000400fa,0x0000004f,0x00000050,0x00000055,0x000200f8,0x00000050,0x00050083,0x00000007,
    0x00000054,0x00000012,0x0000001b,0x000200f9,0x00000051,0x000200f8,0x00000055,0x000500aa,
    0x0000003f,0x00000059,0x0000003d,0x00000058,0x000300f7,0x0000005b,0x00000000,0x000400fa,
    0x00000059,0x0000005a,0x0000005b,0x000200f8,0x0000005a,0x00050051,0x00000006,0x0000005d,
    0x0000001b,0x00000001,0x00050083,0x00000006,0x0000005e,0x0000000f,0x0000005d,0x00050051,
    0x00000006,0x00000060,0x0000001b,0x00000000,0x00050050,0x00000007,0x00000061,0x0000005e,
    0x00000060,0x000200f9,0x0000005b,0x000200f8,0x0000005b,0x000700f5,0x00000007,0x00000074,
    0x0000001b,0x00000055,0x00000061,0x0000005a,0x000200f9,0x00000051,0x000200f8,0x00000051,
    0x000700f5,0x00000007,0x00000073,0x00000054,0x00000050,0x00000074,0x0000005b,0x000200f9,
    0x00000042,0x000200f8,0x00000042,0x000700f5,0x00000007,0x00000072,0x0000004a,0x00000041,
    0x00000073,0x00000051,0x00050041,0x00000065,0x00000066,0x0000002a,0x00000064,0x0004003d,
    0x0000001c,0x00000067,0x00000066,0x0007004f,0x00000007,0x00000068,0x00000067,0x00000067,
    0x00000000,0x00000001,0x0007004f,0x00000007,0x0000006b,0x00000067,0x00000067,0x00000002,
    0x00000003,0x0008000c,0x00000007,0x0000006d,0x00000001,0x0000002e,0x00000068,0x0000006b,
    0x00000072,0x0003003e,0x00000063,0x0000006d,0x000100fd,0x00010038,
};
static const uint32_t k_rotate_fragment[] = {
    0x07230203,0x00010300,0x000d000a,0x00000014,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00000011,0x00030010,
    0x00000004,0x00000007,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,0x0000000d,
    0x00000022,0x00000000,0x00040047,0x0000000d,0x00000021,0x00000000,0x00040047,0x00000011,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,
    0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00090019,0x0000000a,
    0x00000006,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,
    0x0000000b,0x0000000a,0x00040020,0x0000000c,0x00000000,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000000,0x00040017,0x0000000f,0x00000006,0x00000002,0x00040020,0x00000010,
    0x00000001,0x0000000f,0x0004003b,0x00000010,0x00000011,0x00000001,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x0000000b,0x0000000e,
    0x0000000d,0x0004003d,0x0000000f,0x00000012,0x00000011,0x00050057,0x00000007,0x00000013,
    0x0000000e,0x00000012,0x0003003e,0x00000009,0x00000013,0x000100fd,0x00010038,
};

VkRenderPass rotate_pass{};
VkPipelineLayout rotate_layout{};
VkPipeline rotate_pipeline{};
VkDescriptorSetLayout rotate_set_layout{};
VkDescriptorPool rotate_pool{};
VkDescriptorSet rotate_set{};
VkSampler rotate_sampler{};
VkImageView rotate_view{};          // view of the engine's image, rebuilt per format
VkFormat rotate_view_format{};
std::vector<VkImageView> rotate_view_cache;    // per swapchain image
std::vector<VkFramebuffer> rotate_framebuffers;
std::string shader_directory;      // <files>/games/<id>/shaders
std::string bundle_directory;      // <shader_directory>/vk : the .xgs bundles
std::atomic<uint64_t> presented{0};
std::atomic<uint64_t> last_fence{0};

// Frame pacing for the HUD.
//
// vkhost_frame_stats used to answer zero and the overlay therefore had nothing
// to show. The only place a frame actually lands on the panel is the present
// below, so the timestamps are recorded there: a fixed window of present-to-
// present intervals, which is what the number on screen should mean. A ring
// keeps the cost per frame to one store and never allocates.
constexpr int kFrameWindow = 120;
int64_t frame_ns[kFrameWindow]{};
int frame_pos = 0, frame_fill = 0;
int64_t frame_last_ns = 0;
std::mutex frame_mutex;

int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 呈现分段计时（诊断用，常开）。
//
// 引擎的 NVN PERF 把整段 present 记成一个数（present_cpu_ms），但一次 present 里有五件
// 完全不同的事：等一个空闲交换链缓冲、等上一次提交真的做完、录制 blit、提交、以及
// vkQueuePresentKHR 里等合成器收走。不拆开就分不清那 1~13ms 是在等面板（正常节拍）还是
// 在等 GPU（真瓶颈）。全部只在本线程累加，不加重试锁；每 kSplitReport 帧打一行。
struct PresentSplitStats {
    uint64_t acquire_ns = 0;   // vkAcquireNextImageKHR
    uint64_t fence_ns = 0;     // vkWaitForFences(present_fence)
    uint64_t record_ns = 0;    // 录制 blit 到 vkEndCommandBuffer
    uint64_t submit_ns = 0;    // vkQueueSubmit
    uint64_t queue_ns = 0;     // vkQueuePresentKHR
    uint64_t frames = 0;
    int64_t record_start = 0;  // 本帧录制起点，submit_present 用它结算 record 段
};
PresentSplitStats present_split;
constexpr uint64_t kSplitReport = 120;

void report_present_split() {
    if (++present_split.frames < kSplitReport) return;
    const double f = double(present_split.frames) * 1e6;   // ns / f = 毫秒每帧
    ot2_host_log("D2R PRESENT split_ms/frame acquire=%.2f fence=%.2f record=%.2f submit=%.2f queue=%.2f "
                 "frames=%llu",
                 double(present_split.acquire_ns) / f, double(present_split.fence_ns) / f,
                 double(present_split.record_ns) / f, double(present_split.submit_ns) / f,
                 double(present_split.queue_ns) / f, (unsigned long long)present_split.frames);
    present_split = PresentSplitStats{};
}

void record_present_timing() {
    const int64_t now = monotonic_ns();
    std::lock_guard lock(frame_mutex);
    if (frame_last_ns) {
        frame_ns[frame_pos] = now - frame_last_ns;
        frame_pos = (frame_pos + 1) % kFrameWindow;
        if (frame_fill < kFrameWindow) ++frame_fill;
    }
    frame_last_ns = now;
}

// What the device actually offers, in the shape graphics/device_limits.h maps
// NVN queries onto. The title sizes its own buffers from these answers, so they
// are taken from the driver rather than from the engine: this host's query
// policy is the one the game has already been brought up against, and the
// engine's own answers only cover a subset of the properties.
nvn_host_limits host_limits{};
VkPhysicalDeviceFeatures device_features{};

void check(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return;
    char message[256];
    snprintf(message, sizeof message, "Vulkan %s failed: %d", what, int(result));
    ot2_host_die(message);
}

// Surface and swapchain calls are the one family allowed to fail without
// killing the process. The compositor destroys the native window every time the
// app leaves the foreground, and the VkSurfaceKHR built from it dies with it:
// the next creation answers VK_ERROR_SURFACE_LOST_KHR (-1000000000). That is a
// recoverable state, not a broken device -- the next present simply retries
// against whatever window the compositor hands over. Everything else (device,
// queues, command pools) keeps using check().
bool soft_check(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return true;
    ot2_host_log("D2R %s failed: %d (surface/swapchain left unbuilt, retrying)", what, int(result));
    return false;
}

// ---------------------------------------------------------------------------
// The engine's presentation callback.
//
// xbg_present_texture() has already moved its image to TRANSFER_SRC and
// finished the submission; all that is left is the copy onto a swapchain image
// and the queue present.
//
// The crop rectangle is a region *of the source image*, not a destination
// rectangle: NVN uses it to say which part of the window texture is live, and
// the compositor scales that to the display. Treating it as a destination is
// what puts the picture in one corner at its native size instead of filling the
// screen. So: crop the source, then scale it to fill the swapchain with the
// aspect ratio preserved and letterbox the remainder -- the same rule the
// upstream Android frontend follows.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Rotated present: one triangle sampling the finished image through a render
// pass, because vkCmdBlitImage cannot turn a picture. Built once, reused for
// every frame; the source view is rebuilt only when the engine's image format
// changes.
// ---------------------------------------------------------------------------
VkShaderModule make_module(const uint32_t *words, size_t count) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = count * sizeof(uint32_t);
    info.pCode = words;
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &info, nullptr, &module), "rotate module");
    return module;
}

bool build_rotate_pipeline(VkFormat target_format) {
    if (rotate_pipeline) return true;

    VkAttachmentDescription attachment{};
    attachment.format = target_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // bars are black
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    check(vkCreateRenderPass(device, &pass_info, nullptr, &rotate_pass), "rotate render pass");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &rotate_set_layout), "rotate set layout");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(float) * 8 + sizeof(uint32_t);   // scale, offset, uv rect, rotation
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &rotate_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device, &layout_info, nullptr, &rotate_layout), "rotate layout");

    VkShaderModule vertex = make_module(k_rotate_vertex, sizeof k_rotate_vertex / sizeof k_rotate_vertex[0]);
    VkShaderModule fragment = make_module(k_rotate_fragment, sizeof k_rotate_fragment / sizeof k_rotate_fragment[0]);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend_info{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_info.attachmentCount = 1;
    blend_info.pAttachments = &blend;
    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend_info;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = rotate_layout;
    pipeline.renderPass = rotate_pass;
    VkResult created = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &rotate_pipeline);
    vkDestroyShaderModule(device, vertex, nullptr);
    vkDestroyShaderModule(device, fragment, nullptr);
    if (created != VK_SUCCESS) { rotate_pipeline = VK_NULL_HANDLE; return false; }

    if (!rotate_sampler) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0;
        check(vkCreateSampler(device, &sampler, nullptr, &rotate_sampler), "rotate sampler");
    }
    if (!rotate_pool) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device, &pool, nullptr, &rotate_pool), "rotate descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = rotate_pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &rotate_set_layout;
        check(vkAllocateDescriptorSets(device, &allocate, &rotate_set), "rotate descriptor set");
    }
    ot2_host_log("D2R rotate pipeline ready target_format=%d", int(target_format));
    return true;
}

// Crop rectangle of the source, clamped into the image, as a UV rectangle.
struct UvRect { float u0, v0, u1, v1; };
UvRect source_uv(uint32_t width, uint32_t height, const int32_t crop[4]) {
    int32_t x = 0, y = 0, w = int32_t(width), h = int32_t(height);
    if (crop && crop[2] > 0 && crop[3] > 0) {
        x = std::clamp(crop[0], 0, w);
        y = std::clamp(crop[1], 0, h);
        w = std::min(crop[2], int32_t(width) - x);
        h = std::min(crop[3], int32_t(height) - y);
    }
    if (w <= 0 || h <= 0) { x = y = 0; w = int32_t(width); h = int32_t(height); }
    return {float(x) / width, float(y) / height, float(x + w) / width, float(y + h) / height};
}

// Records the finished command buffer, hands the image to the compositor and
// retires the frame. Shared by the blit and the rotated-render-pass paths.
int submit_present(uint32_t index, uint32_t width, uint32_t height,
                   const int32_t crop[4], int32_t sx, int32_t sy, int32_t sw, int32_t sh);

// 实验开关 gfx-1to1 的开关值（见 present_to_surface 内的说明）。默认关闭：
// 标记文件不存在时保持 false，行为与未打补丁时完全一致。在 vkhost_configure_package
// 里求值，因为 swapchain 可能在引擎启动之前就建好了（那时 shader_directory 还是空串）。
bool present_1to1 = false;
// [diag] 见 present_to_surface 里的用法：把上屏放大 blit 摘掉，只用于归因测量。
// [diag] See its use in present_to_surface: drops the upscale blit for attribution only.
bool no_present_blit = false;

int present_to_surface(void *userdata, void *native_window, VkImage image, VkFormat format,
                       uint32_t width, uint32_t height, const int32_t crop[4], int32_t interval) {
    (void)userdata;
    (void)native_window;
    (void)format;
    (void)interval;
    if (suspend_present || !window) return 0;   // Dropped frame, not an error.
    std::lock_guard lock(state_mutex);

    // 先把 crop 算出来：下面建 swapchain 之前就要用到它的大小。
    // Compute the crop first: the swapchain size depends on it in the 1:1 mode.
    int32_t sx = 0, sy = 0;
    int32_t sw = int32_t(width), sh = int32_t(height);
    if (crop && crop[2] > 0 && crop[3] > 0) {
        sx = std::clamp(crop[0], 0, sw);
        sy = std::clamp(crop[1], 0, sh);
        sw = std::min(crop[2], int32_t(width) - sx);
        sh = std::min(crop[3], int32_t(height) - sy);
    }
    if (sw <= 0 || sh <= 0) return 0;

    // 实验开关 gfx-1to1（默认关闭）：把上屏缓冲改成与 crop 同尺寸，让全屏放大交给
    // 合成器做。现在每帧是 guest 出 1280x720 → 我们先 GPU 放大到 2560x1440 → 合成器
    // 再放大到 2560x1600，同一批源像素被重采样两次；1:1 上屏可以整块省掉第一次放大
    // 的填充率。chain 在引擎启动前就按窗口尺寸建好了（vkhost_init），那时还不知道
    // crop，所以要在这里做一次：改缓冲尺寸 → 丢掉旧 chain → 下面按新尺寸重建。
    // 标记缺失时 present_1to1 恒为 false，行为与未打补丁时完全一致。
    static bool one_to_one_applied = false;
    if (present_1to1 && !one_to_one_applied) {
        one_to_one_applied = true;
        const int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
        const int rc = (sw < ww && sh < wh) ? ANativeWindow_setBuffersGeometry(window, sw, sh, 0) : -1;
        ot2_host_log("D2R PRESENT_1TO1 surface %dx%d crop %dx%d rc=%d -> buffer %dx%d",
                     ww, wh, sw, sh, rc, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
        if (rc == 0) { destroy_swapchain(); destroy_surface(); }
    }

    if (!swapchain) {
        // The window is back but the chain is not. Rebuild here instead of
        // reporting failure: the engine treats a non-zero return as fatal
        // ("Game presentation failed"), which is exactly how a resume used to
        // kill the process. Rate-limited so a surface the driver keeps refusing
        // cannot turn every frame into a creation attempt.
        const int64_t now = monotonic_ns();
        static int64_t next_attempt_ns = 0;
        if (now < next_attempt_ns) return 0;
        next_attempt_ns = now + 250000000LL;   // 250 ms
        if (!create_surface_and_swapchain() || !swapchain) return 0;
    }

    uint32_t index = 0;
    const int64_t split_acquire = monotonic_ns();
    VkResult acquired = vkAcquireNextImageKHR(device, swapchain, 100000000ULL, VK_NULL_HANDLE, present_fence, &index);
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return 0;
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) return 0;   // Rebuilt on the next present.
    if (acquired == VK_ERROR_SURFACE_LOST_KHR) {
        // The window behind the surface went away. Both are dead; rebuild from
        // whatever window the compositor gives us next.
        destroy_swapchain();
        destroy_surface();
        return 0;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return 0;   // Dropped frame.
    const int64_t split_fence = monotonic_ns();
    present_split.acquire_ns += uint64_t(split_fence - split_acquire);
    check(vkWaitForFences(device, 1, &present_fence, VK_TRUE, UINT64_MAX), "present fence");
    check(vkResetFences(device, 1, &present_fence), "present fence reset");
    const int64_t split_record = monotonic_ns();
    present_split.fence_ns += uint64_t(split_record - split_fence);
    present_split.record_start = split_record;

    check(vkResetCommandBuffer(present_command, 0), "present command reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(present_command, &begin), "present begin");

    // The engine leaves its image in TRANSFER_SRC; the swapchain image is
    // undefined for this frame. Both sides get an explicit transition so the
    // blit never relies on what a previous frame left behind.
    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = swapchain_images[index];
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier barriers[2] = {to_src, to_dst};
    vkCmdPipelineBarrier(present_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, barriers);

    // A turned picture has to trade its width for height when it is fitted, so
    // the aspect used for the fit is the source's after the turn.
    const bool turned = present_rotation == 90 || present_rotation == 270;
    const int32_t fit_w = turned ? sh : sw;
    const int32_t fit_h = turned ? sw : sh;
    const double scale = std::min(double(swapchain_extent.width) / fit_w, double(swapchain_extent.height) / fit_h);
    int32_t dw = std::max(1, int32_t(fit_w * scale));
    int32_t dh = std::max(1, int32_t(fit_h * scale));
    int32_t dx = (int32_t(swapchain_extent.width) - dw) / 2;
    int32_t dy = (int32_t(swapchain_extent.height) - dh) / 2;

    if (turned && build_rotate_pipeline(swapchain_format)) {
        // One triangle over the destination rectangle, sampling the source
        // through UVs rotated to match. The render pass clears, so the bars are
        // black without a separate clear.
        if (rotate_view_format != format || !rotate_view) {
            if (rotate_view) vkDestroyImageView(device, rotate_view, nullptr);
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image;
            view.format = format;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device, &view, nullptr, &rotate_view) != VK_SUCCESS) {
                rotate_view = VK_NULL_HANDLE;
                return -1;
            }
            rotate_view_format = format;
        }
        VkDescriptorImageInfo source_info{rotate_sampler, rotate_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = rotate_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &source_info;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        VkImageMemoryBarrier to_sample = to_src;
        to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkImageMemoryBarrier to_target = to_dst;
        to_target.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_target.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkImageMemoryBarrier sample_barriers[2] = {to_sample, to_target};
        vkCmdPipelineBarrier(present_command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 2, sample_barriers);

        VkClearValue clear{};
        clear.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo begin_pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin_pass.renderPass = rotate_pass;
        begin_pass.framebuffer = VK_NULL_HANDLE;   // filled in below
        begin_pass.renderArea = {{0, 0}, swapchain_extent};
        begin_pass.clearValueCount = 1;
        begin_pass.pClearValues = &clear;
        // One framebuffer per swapchain image, built on first use and kept: the
        // compositor may still be reading an image when the next frame starts, so
        // tearing its attachment down every frame is not safe.
        if (rotate_framebuffers.empty()) {
            rotate_framebuffers.assign(swapchain_images.size(), VK_NULL_HANDLE);
            rotate_view_cache.assign(swapchain_images.size(), VK_NULL_HANDLE);
        }
        if (index >= rotate_framebuffers.size()) return -1;
        if (!rotate_framebuffers[index]) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = swapchain_images[index];
            view.format = swapchain_format;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &rotate_view_cache[index]), "rotate target view");
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = rotate_pass;
            fb.attachmentCount = 1;
            fb.pAttachments = &rotate_view_cache[index];
            fb.width = swapchain_extent.width;
            fb.height = swapchain_extent.height;
            fb.layers = 1;
            check(vkCreateFramebuffer(device, &fb, nullptr, &rotate_framebuffers[index]), "rotate framebuffer");
        }
        begin_pass.framebuffer = rotate_framebuffers[index];

        vkCmdBeginRenderPass(present_command, &begin_pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(present_command, VK_PIPELINE_BIND_POINT_GRAPHICS, rotate_pipeline);
        // The quad is positioned in NDC, so the viewport covers the whole
        // surface and the scissor keeps the draw inside the destination.
        VkViewport viewport{0.f, 0.f, float(swapchain_extent.width), float(swapchain_extent.height), 0.f, 1.f};
        VkRect2D scissor{{0, 0}, swapchain_extent};
        vkCmdSetViewport(present_command, 0, 1, &viewport);
        vkCmdSetScissor(present_command, 0, 1, &scissor);
        const UvRect uv = source_uv(width, height, crop);
        // Destination quad in NDC: half-size and centre.
        struct {
            float scale_x, scale_y, offset_x, offset_y;
            float u0, v0, u1, v1;
            uint32_t rotation;
        } push{
            float(dw) / swapchain_extent.width, float(dh) / swapchain_extent.height,
            (float(dx) + float(dw) * 0.5f) / swapchain_extent.width * 2.f - 1.f,
            (float(dy) + float(dh) * 0.5f) / swapchain_extent.height * 2.f - 1.f,
            uv.u0, uv.v0, uv.u1, uv.v1, uint32_t(present_rotation)};
        vkCmdPushConstants(present_command, rotate_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
        vkCmdBindDescriptorSets(present_command, VK_PIPELINE_BIND_POINT_GRAPHICS, rotate_layout, 0, 1,
                                &rotate_set, 0, nullptr);
        vkCmdDraw(present_command, 6, 1, 0, 0);
        vkCmdEndRenderPass(present_command);

        // The render pass already left the image in TRANSFER_DST_OPTIMAL (its
        // finalLayout), so the only transition left is the hand-off to the
        // compositor. Declaring the colour-attachment layout there would
        // mismatch what the image is actually in.
        VkImageMemoryBarrier present_barrier = to_target;
        present_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        present_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        present_barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(present_command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &present_barrier);
        check(vkEndCommandBuffer(present_command), "present end");
        return submit_present(index, width, height, crop, sx, sy, sw, sh);
    }

    // Bars only where the blit actually leaves part of the image uncovered.
    if (dx || dy || dw != int32_t(swapchain_extent.width) || dh != int32_t(swapchain_extent.height)) {
        VkClearColorValue black{};
        vkCmdClearColorImage(present_command, swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &to_dst.subresourceRange);
        VkMemoryBarrier after_clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after_clear.srcAccessMask = after_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(present_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &after_clear, 0, nullptr, 0, nullptr);
    }

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (flip_present) {
        // Mirror along Y. The image then reaches the compositor the way this
        // display scans it out; the engine's negative-height viewport already
        // flipped rasterisation, so the two must not both be applied blindly.
        blit.srcOffsets[0] = {sx, sy + sh, 0};
        blit.srcOffsets[1] = {sx + sw, sy, 1};
    } else {
        blit.srcOffsets[0] = {sx, sy, 0};
        blit.srcOffsets[1] = {sx + sw, sy + sh, 1};
    }
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
    // [diag] gfx-no-present-blit：跳过每帧的上屏放大 blit，只用来把这块 GPU/CPU 成本从帧时间里
    // 摘出来量。画面会停在旧内容上，属纯测量开关，不进正式版本。
    // [diag] gfx-no-present-blit skips the per-frame upscale blit so its cost can be attributed. The screen
    // keeps showing stale content; measurement only.
    if (!no_present_blit)
    vkCmdBlitImage(present_command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapchain_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    VkImageMemoryBarrier to_present = to_dst;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(present_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_present);
    check(vkEndCommandBuffer(present_command), "present end");

    return submit_present(index, width, height, crop, sx, sy, sw, sh);
    if (presented == 1 || presented == 2 || presented % 300 == 0)
        ot2_host_log("D2R present n=%llu source=%dx%d crop=%d,%d %dx%d -> %dx%d at %d,%d flip=%d",
                     (unsigned long long)presented.load(), width, height, sx, sy, sw, sh, dw, dh, dx, dy,
                     flip_present ? 1 : 0);
    return 0;
}

void log_to_host(void *userdata, const char *message) {
    (void)userdata;
    ot2_host_log("D2R %s", message);
}

[[noreturn]] void fatal_from_engine(void *userdata, const char *message) {
    (void)userdata;
    ot2_host_die(message);
}


// The rotate path keeps a view and a framebuffer per swapchain image. They are
// tied to those images and must go when the swapchain does -- a framebuffer left
// pointing at a destroyed image renders into nothing without any Vulkan error.
void release_rotate_targets() {
    for (auto framebuffer : rotate_framebuffers)
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
    for (auto view : rotate_view_cache)
        if (view) vkDestroyImageView(device, view, nullptr);
    rotate_framebuffers.clear();
    rotate_view_cache.clear();
}

// Destroys the swapchain and everything pinned to its images.
void destroy_swapchain() {
    release_rotate_targets();
    for (auto view : swapchain_views) vkDestroyImageView(device, view, nullptr);
    swapchain_views.clear();
    swapchain_images.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

// A VkSurfaceKHR belongs to exactly one ANativeWindow. Once that window is gone
// the surface can never produce a swapchain again, so it has to go too --
// keeping it was what made every resume fail with VK_ERROR_SURFACE_LOST_KHR.
void destroy_surface() {
    if (surface && instance) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
}

bool create_surface_and_swapchain() {    if (!window) return false;
    if (!surface) {
        VkAndroidSurfaceCreateInfoKHR create{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        create.window = window;
        if (!soft_check(vkCreateAndroidSurfaceKHR(instance, &create, nullptr, &surface), "Android surface")) {
            surface = VK_NULL_HANDLE;
            return false;
        }
        VkBool32 supported = 0;
        if (!soft_check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, queue_family, surface, &supported), "surface support")) {
            destroy_surface();
            return false;
        }
        if (!supported) {
            ot2_host_log("D2R graphics queue cannot present on this window");
            destroy_surface();
            return false;
        }
    }
    if (swapchain) return true;

    VkSurfaceCapabilitiesKHR caps{};
    if (!soft_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities")) {
        destroy_surface();
        return false;
    }
    uint32_t count = 0;
    if (!soft_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "surface formats")) {
        destroy_surface();
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (!soft_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "surface formats")) {
        destroy_surface();
        return false;
    }
    if (!count) {
        ot2_host_log("D2R surface reports no format");
        destroy_surface();
        return false;
    }
    VkSurfaceFormatKHR chosen{};
    for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
        for (const auto &f : formats)
            if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    if (!chosen.format) {
        ot2_host_log("D2R surface lacks a display-encoded UNORM format");
        destroy_surface();
        return false;
    }

    swapchain_format = chosen.format;
    // preTransform IDENTITY and the window's own size: this host does the
    // rotation itself (see the rotate path in present_to_surface), so claiming a
    // pre-rotated buffer here would make the compositor apply the window's
    // transform a second time -- the picture then arrives stretched to the wrong
    // aspect because the buffer is mapped by a rotated extent that no longer
    // matches what was drawn into it.
    swapchain_extent = {uint32_t(ANativeWindow_getWidth(window)), uint32_t(ANativeWindow_getHeight(window))};
    if (!swapchain_extent.width || !swapchain_extent.height) {
        swapchain_extent = caps.currentExtent;
        if (caps.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
            caps.currentTransform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
            std::swap(swapchain_extent.width, swapchain_extent.height);
    }
    if (swapchain_extent.width == UINT32_MAX)
        swapchain_extent = {uint32_t(ANativeWindow_getWidth(window)), uint32_t(ANativeWindow_getHeight(window))};

    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface;
    // 队列深度：保持驱动给的 caps.minImageCount（Adreno 通常 3 张）。
    //
    // 2026-10-26 曾试过 minImageCount+2（撑到 5 张）以期让提交与执行跨帧重叠，
    // NP02J 主菜单同场景 A/B 结果是无差别，已回退：
    //     深度 3：present_cpu 1.77~1.86ms  queue 1.28ms  fps 50.2~50.4
    //     深度 5：present_cpu 1.85~1.91ms  queue 1.19~1.35ms  fps 50.1~50.4
    // 原因：本机 present 阻塞本来就只有 1.2~1.3ms，不在关键路径上；+2 张反而多约
    // 一帧输入延迟。结论——present 不是 NP02J 的瓶颈，不要再往这里投入。
    //
    // ⚠ 改这一行之前先看清后端：graphics/backend.cpp 里有一份几乎复制粘贴的交换链
    //   创建代码，那是 ot2 后端的实现。native/CMakeLists.txt 按 S2A_GRAPHICS_BACKEND
    //   二选一编译，d2r 后端下 backend.cpp 根本不参与编译。只改那边等于没改
    //   （本项曾因此空转一轮，误把无关的测量波动当成收益）。
    create.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount) create.minImageCount = std::min(create.minImageCount, caps.maxImageCount);
    create.imageFormat = chosen.format;
    create.imageColorSpace = chosen.colorSpace;
    create.imageExtent = swapchain_extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // We rotate the picture ourselves, so the buffer is already upright for the
    // display's current rotation.
    create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create.compositeAlpha = VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.clipped = VK_TRUE;
    if (!soft_check(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain), "swapchain")) {
        swapchain = VK_NULL_HANDLE;
        // A stale surface is the usual cause; drop it so the retry starts clean.
        destroy_surface();
        return false;
    }

    if (!soft_check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "swapchain images")) {
        destroy_swapchain(); destroy_surface(); return false;
    }
    swapchain_images.resize(count);
    if (!soft_check(vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data()), "swapchain images")) {
        destroy_swapchain(); destroy_surface(); return false;
    }
    swapchain_views.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapchain_images[i];
        view.format = swapchain_format;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!soft_check(vkCreateImageView(device, &view, nullptr, &swapchain_views[i]), "swapchain view")) {
            destroy_swapchain(); destroy_surface(); return false;
        }
    }
    static int created_count;
    ot2_host_log("D2R surface#%d %ux%u format=%d images=%u preTransform=%d currentTransform=%d",
                 ++created_count, swapchain_extent.width, swapchain_extent.height,
                 int(swapchain_format), count, int(create.preTransform), int(caps.currentTransform));
    return true;
}

void create_device() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "OT2 d2r host";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_extensions;
    check(vkCreateInstance(&instance_info, nullptr, &instance), "instance");

    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "device count");
    if (!count) ot2_host_die("No Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "devices");
    physical = devices[0];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    ot2_host_log("D2R device %s api=%u.%u.%u", properties.deviceName,
                 VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                 VK_VERSION_PATCH(properties.apiVersion));

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        const auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & required) == required) { queue_family = i; break; }
    }
    if (queue_family == UINT32_MAX) ot2_host_die("No graphics+compute queue: the engine requires both");

    // Every feature the engine may ask for is enabled up front and it validates
    // the request against what the driver actually has, so enabling broadly here
    // only widens what a title can use -- it never arms a feature implicitly.
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical, &available);
    VkPhysicalDeviceVulkan12Features available12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &available12;
    vkGetPhysicalDeviceFeatures2(physical, &query);
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    draw_parameters.shaderDrawParameters = VK_TRUE;
    if (properties.apiVersion >= VK_API_VERSION_1_2) draw_parameters.pNext = &available12;

    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &available;
    device_info.pNext = &draw_parameters;
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    check(vkCreateDevice(physical, &device_info, nullptr, &device), "device");
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    // Same derivation graphics/backend.cpp uses, so a title sees the same
    // limits whichever backend is compiled in.
    device_features = available;
    const auto &limits = properties.limits;
    host_limits.uniform_range = limits.maxUniformBufferRange;
    host_limits.uniform_bindings = limits.maxPerStageDescriptorUniformBuffers;
    host_limits.storage_bindings = limits.maxPerStageDescriptorStorageBuffers;
    host_limits.sampled_images = limits.maxPerStageDescriptorSampledImages;
    host_limits.samplers = limits.maxPerStageDescriptorSamplers;
    host_limits.storage_images = limits.maxPerStageDescriptorStorageImages;
    host_limits.color_targets = std::min(limits.maxColorAttachments, limits.maxFragmentOutputAttachments);
    host_limits.vertex_bindings = limits.maxVertexInputBindings;
    host_limits.vertex_attributes = limits.maxVertexInputAttributes;
    host_limits.texture_size = std::min(limits.maxImageDimension1D, limits.maxImageDimension2D);
    host_limits.texture_3d = limits.maxImageDimension3D;
    host_limits.texture_cube = limits.maxImageDimensionCube;
    host_limits.texture_layers = limits.maxImageArrayLayers;
    host_limits.buffer_texels = limits.maxTexelBufferElements;
    std::copy_n(limits.maxComputeWorkGroupSize, 3, host_limits.compute_size);
    std::copy_n(limits.maxComputeWorkGroupCount, 3, host_limits.compute_count);
    host_limits.compute_threads = limits.maxComputeWorkGroupInvocations;
    host_limits.anisotropy = available.samplerAnisotropy ? uint32_t(limits.maxSamplerAnisotropy) : 1;
    host_limits.astc = available.textureCompressionASTC_LDR ? 1u : 0u;
    host_limits.subpixel_bits = limits.subPixelPrecisionBits;

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family;
    check(vkCreateCommandPool(device, &pool, nullptr, &present_pool), "present pool");
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = present_pool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device, &command, &present_command), "present command");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device, &semaphore, nullptr, &present_done), "present semaphore");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fence, nullptr, &present_fence), "present fence");
}

void start_engine() {
    if (engine_ready || !device || bundle_directory.empty()) return;

    VkPhysicalDeviceFeatures enabled{};
    vkGetPhysicalDeviceFeatures(physical, &enabled);
    VkPhysicalDeviceVulkan12Features available12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &available12;
    vkGetPhysicalDeviceFeatures2(physical, &query);

    xbg_game_config config{};
    config.size = sizeof config;
    config.physical = physical;
    config.device = device;
    config.queue = queue;
    config.queue_family = queue_family;
    config.enabled_core = enabled;
    config.enabled_shader_draw_parameters = VK_TRUE;
    config.fence_timeout_ns = 1000000000ULL;   // 1 s: a real hang dies instead of spinning.
    config.shader_directory = bundle_directory.c_str();
    config.userdata = nullptr;
    config.present = present_to_surface;
    config.log = log_to_host;
    config.fatal = fatal_from_engine;

    char error[512] = {0};
    if (xbg_game_initialize(&config, error, sizeof error)) ot2_host_die(error[0] ? error : "d2r engine init failed");
    xbg_game_set_sampler_mirror_clamp_to_edge(available12.samplerMirrorClampToEdge);
    engine_ready = true;
    ot2_host_log("D2R engine ready shaders=%s", bundle_directory.c_str());
}

int submit_present(uint32_t index, uint32_t width, uint32_t height,
                   const int32_t crop[4], int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &present_command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &present_done;
    const int64_t split_submit = monotonic_ns();
    if (present_split.record_start) present_split.record_ns += uint64_t(split_submit - present_split.record_start);
    check(vkQueueSubmit(queue, 1, &submit, present_fence), "present submit");
    const int64_t split_queue = monotonic_ns();
    present_split.submit_ns += uint64_t(split_queue - split_submit);

    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &present_done;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain;
    info.pImageIndices = &index;
    VkResult result = vkQueuePresentKHR(queue, &info);
    present_split.queue_ns += uint64_t(monotonic_ns() - split_queue);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vkQueueWaitIdle(queue);
        // Rebuild lazily: the surface size is read again on the next present.
        destroy_swapchain();
        report_present_split();
        return 0;
    }
    // VK_SUBOPTIMAL_KHR is not a reason to rebuild here. This host presents with
    // preTransform IDENTITY while the panel reports ROTATE_90, so the driver
    // answers SUBOPTIMAL on every single frame -- and treating that as "rebuild"
    // recreated the swapchain, its image views and its framebuffers 500+ times in
    // 45 seconds. That starved the main thread into an ANR (an "Application Not
    // Responding" window sat on top of the game and swallowed every touch) and it
    // also meant the frame-statistics path below was never reached, which is why
    // the overlay only ever showed "--". The picture is correct precisely because
    // IDENTITY is deliberate; only a real out-of-date surface needs a new chain.
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        vkQueueWaitIdle(queue);
        // Same reasoning as the acquire path: a lost surface is recoverable, and
        // any other present failure costs one frame, not the process.
        if (result == VK_ERROR_SURFACE_LOST_KHR) destroy_surface();
        destroy_swapchain();
        ot2_host_log("D2R present failed: %d (frame dropped, chain rebuilt)", int(result));
        report_present_split();
        return 0;
    }
    record_present_timing();
    report_present_split();
    const uint64_t count = ++presented;
    if (count == 1 || count % 100 == 0)
        ot2_host_log("D2R present n=%llu source=%ux%u crop=%d,%d %dx%d rotation=%d surface=%ux%u",
                     (unsigned long long)count, width, height, sx, sy, sw, sh, present_rotation,
                     swapchain_extent.width, swapchain_extent.height);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// vkhost_* surface seen by frontend.c
// ---------------------------------------------------------------------------

extern "C" void vkhost_configure(const char *path);

extern "C" void vkhost_configure_package(const char *path, const char *offline) {
    std::lock_guard lock(state_mutex);
    shader_directory = path ? path : "";
    // The engine looks for its marker files in the parent of its shader
    // directory, which is how the rest of this host's diagnostics are found too.
    bundle_directory = shader_directory + "/vk";
    flip_present = std::filesystem::exists(shader_directory + "/present-flip-y");
    // gfx-1to1：把上屏缓冲改成 crop 尺寸，让放大交给合成器，省掉每帧一次全屏放大 blit。
    // 标记缺失时保持 false，与未打补丁时完全一致。
    present_1to1 = std::filesystem::exists(shader_directory + "/gfx-1to1") ||
                   std::filesystem::exists(shader_directory + "/vk/gfx-1to1");
    ot2_host_log("D2R present_1to1=%d (marker gfx-1to1)", present_1to1 ? 1 : 0);
    no_present_blit = std::filesystem::exists(shader_directory + "/gfx-no-present-blit") ||
                      std::filesystem::exists(shader_directory + "/vk/gfx-no-present-blit");
    if (no_present_blit) ot2_host_log("D2R [diag] present upscale blit skipped (marker gfx-no-present-blit)");
    // Orientation: the swapchain is created with preTransform = IDENTITY at the
    // window's own size, which is what makes the compositor apply the display's
    // rotation exactly once. The picture is therefore already upright by the time
    // it reaches the swapchain image and no rotation is applied here.
    //
    // The markers below exist for the case where that division of labour is
    // wrong (a surface that reports no usable transform, or a window whose size
    // disagrees with the display): `present-rotate-90` / `-180` / `-270` turn the
    // picture in this host instead. Using one together with preTransform
    // IDENTITY rotates twice, so they are diagnostics, not a normal setting.
    present_rotation = 0;
    for (const auto &option : {std::make_pair("/present-rotate-90", 90),
                               std::make_pair("/present-rotate-180", 180),
                               std::make_pair("/present-rotate-270", 270)})
        if (std::filesystem::exists(shader_directory + option.first)) present_rotation = option.second;
    ot2_host_log("D2R bundles %s flip_y=%d rotation=%d", bundle_directory.c_str(),
                 flip_present ? 1 : 0, present_rotation);
    start_engine();
}

extern "C" void vkhost_set_window(ANativeWindow *next) {
    std::lock_guard lock(state_mutex);
    if (next == window) return;
    if (next) ANativeWindow_acquire(next);
    if (window) ANativeWindow_release(window);
    const bool had_window = window != nullptr;
    window = next;
    if (device) {
        // The surface is bound to the window it was created from, and the
        // compositor retires that window on every trip to the background. Drop
        // both together: keeping the surface was what made the next
        // vkCreateSwapchainKHR answer VK_ERROR_SURFACE_LOST_KHR (-1000000000)
        // and, through check(), kill the process on resume.
        vkQueueWaitIdle(queue);
        destroy_swapchain();
        if (had_window || !window) destroy_surface();
    }
    if (!window) {
        suspend_present = true;
        return;
    }
    suspend_present = false;
    if (device) create_surface_and_swapchain();
}

extern "C" void vkhost_init() {
    std::lock_guard lock(state_mutex);
    if (device) return;
    create_device();
    create_surface_and_swapchain();
    start_engine();
}

extern "C" void vkhost_suspend(int on) {
    std::lock_guard lock(state_mutex);
    suspend_present = on != 0;
}

extern "C" void vkhost_pool_add(const void *memory, uint64_t size) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_legacy_pool_add(const_cast<void *>(memory), size_t(size));
}

extern "C" void vkhost_pool_del(const void *memory) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_legacy_pool_del(const_cast<void *>(memory));
}

extern "C" void vkhost_reg_tex(const dpool_t *pool, int id, const tex_t *texture, const view_t *view) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_register_texture(pool, id, texture, view);
}

extern "C" void vkhost_reg_samp(const dpool_t *pool, int id, const samp_par *sampler) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_register_sampler(pool, id, sampler);
}

extern "C" void vkhost_tex_created(const tex_t *texture) {
    std::lock_guard lock(state_mutex);
    if (engine_ready && texture) xbg_texture_invalidate(texture);
}

extern "C" void vkhost_tex_release(tex_t *texture) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_legacy_tex_release(texture);
}

extern "C" void vkhost_tex_retain(uint64_t serial) { (void)serial; }   // The engine keys its tables by serial itself.
extern "C" void vkhost_tex_unref(uint64_t serial) { (void)serial; }

extern "C" void vkhost_prog_release(prog_t *program) {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_legacy_prog_release(program);
}

extern "C" void vkhost_program(const prog_t *program) {
    // The engine resolves each stage's bundle itself from the program's
    // NVNshaderData, so there is nothing to hand over here.
    (void)program;
}

extern "C" void vkhost_set_tex_pool_n(int count) { (void)count; }

extern "C" void vkhost_descriptor_pool_del(const dpool_t *pool, int sampler) {
    (void)pool;
    (void)sampler;
}

extern "C" int vkhost_device_limit(NVNdeviceInfo property, int maximum) {
    std::lock_guard lock(state_mutex);
    if (!device) vkhost_init();
    return nvn_limit_for_host(property, maximum, &host_limits);
}

extern "C" void vkhost_submit(int count, const uint64_t *handles) {
    std::lock_guard lock(state_mutex);
    if (!engine_ready) return;
    create_surface_and_swapchain();
    xbg_submit(count, handles);
    last_fence.store(xbg_legacy_last_fence(), std::memory_order_relaxed);
}

extern "C" uint64_t vkhost_last_fence() { return last_fence.load(std::memory_order_relaxed); }

extern "C" int vkhost_wait(uint64_t fence, uint64_t timeout) {
    if (!engine_ready) return 0;
    return xbg_legacy_wait(fence, timeout);
}

extern "C" void vkhost_finish() {
    std::lock_guard lock(state_mutex);
    if (engine_ready) xbg_legacy_finish();
}

extern "C" uint64_t vkhost_present(const tex_t *texture, int interval, const int *crop) {
    std::lock_guard lock(state_mutex);
    if (!engine_ready || !texture) return last_fence.load(std::memory_order_relaxed);
    create_surface_and_swapchain();
    // The engine reads only these three fields of the window it is given; the
    // real NVNwindow lives in frontend.c and is not visible here.
    win_t target{};
    target.native = window;
    target.interval = interval;
    if (crop) memcpy(target.crop, crop, sizeof target.crop);
    xbg_present_texture(&target, texture);
    last_fence.store(xbg_legacy_last_fence(), std::memory_order_relaxed);
    return last_fence.load(std::memory_order_relaxed);
}

// CPU -> texture. NVN's pool memory is the authority for every texture the CPU
// owns, so the write lands there (block-linear, per level and layer) and the
// engine is told to drop the GPU copy it may already hold. This is how a game
// gets a font atlas, a video frame or a generated thumbnail on screen: leaving
// it as a no-op silently discarded every such upload, which is why CPU-built
// textures showed up as whatever the pool happened to contain.
//
// A texture the GPU has already rendered into cannot simply be re-uploaded --
// the engine refuses to merge a partial CPU write into it -- so that case is
// reported and the bytes stay in pool memory only.
extern "C" void vkhost_write_texels(const tex_t *texture, const view_t *view, const NVNcopyRegion *region,
                                    const void *source, int64_t row, int64_t slice) {
    if (!texture || !region || !source) return;
    if (!texture->mem) {
        ot2_host_log("D2R WriteTexels: texture serial=%llu has no pool storage; write dropped",
                     (unsigned long long)texture->serial);
        return;
    }
    const unsigned level = view && view->set_levels ? unsigned(view->base_level) : 0;
    const unsigned layer = view && view->set_layers ? unsigned(view->min_layer) : 0;
    int bw = 0, bh = 0;
    const int bytes = nvn_fmt_bpb(texture->d.format, &bw, &bh);
    ot2::CopyLayout layout;
    if (!bytes || !ot2::copy_layout(region->width, region->height, region->depth, bw, bh, bytes, row, slice, layout)) {
        ot2_host_log("D2R WriteTexels: bad region/stride %dx%dx%d format=%#x", region->width, region->height,
                     region->depth, texture->d.format);
        return;
    }
    const std::span<const uint8_t> pixels(static_cast<const uint8_t *>(source), layout.extent);
    if (!ot2::write_texture_memory(*texture, level, layer, *region, pixels, row, slice)) {
        ot2_host_log("D2R WriteTexels: unsupported region (%d,%d,%d)+%dx%dx%d level=%u layer=%u format=%#x target=%d",
                     region->xoffset, region->yoffset, region->zoffset, region->width, region->height, region->depth,
                     level, layer, texture->d.format, texture->d.target);
        return;
    }
    if (!engine_ready) return;
    if (xbg_legacy_cpu_wrote_texture(texture) == 1)
        ot2_host_log("D2R WriteTexels: texture serial=%llu is GPU-owned; the CPU bytes stayed in pool memory",
                     (unsigned long long)texture->serial);
}

extern "C" void vkhost_read_texels(const tex_t *texture, unsigned level, unsigned layer, const NVNcopyRegion *region,
                                   uint8_t *destination, size_t row_stride, size_t image_stride) {
    if (!engine_ready) return;
    xbg_legacy_read_texels(texture, level, layer, region, destination, row_stride, image_stride);
}

extern "C" uint64_t vkhost_frame_count() { return presented.load(std::memory_order_relaxed); }

// out[0] fps, out[1] mean ms, out[2] p95 ms, out[3] source (0 = 提交, <0 = no data).
// The d2r engine is driven by the guest's own presents, so every sample here is
// a submission interval -- the same meaning the ot2 backend reports as 提交.
extern "C" void vkhost_frame_stats(double out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    int64_t samples[kFrameWindow];
    int n = 0;
    {
        std::lock_guard lock(frame_mutex);
        n = frame_fill;
        for (int i = 0; i < n; ++i) samples[i] = frame_ns[i];
    }
    if (n <= 0) {
        out[3] = -1;   // nothing has been presented yet
        return;
    }
    int64_t sum = 0;
    std::sort(samples, samples + n);
    for (int i = 0; i < n; ++i) sum += samples[i];
    const double mean_ns = double(sum) / n;
    out[0] = mean_ns > 0 ? 1e9 / mean_ns : 0.0;
    out[1] = mean_ns / 1e6;
    out[2] = double(samples[std::min(n - 1, int(double(n) * 0.95))]) / 1e6;
    out[3] = 0;
}

extern "C" void vkhost_configure(const char *path) { vkhost_configure_package(path, nullptr); }

// ---------------------------------------------------------------------------
// Self-check probes.
//
// With the ot2 backend these drive that renderer's own fixtures and report
// whether each invariant held. The d2r engine owns its replay and has no
// equivalent hooks, so reporting them as "not run" is the honest answer -- the
// probe report distinguishes "failed" from "not applicable" and must not claim
// a pass it never executed. The NVN device contract tests gate on these, which
// is why they exist at all rather than being left undefined.
// ---------------------------------------------------------------------------
extern "C" int vkhost_probe_transfers() { return 1; }
extern "C" int vkhost_probe_deswizzle() { return 1; }
extern "C" int vkhost_probe_descriptor_reuse() { return 1; }
extern "C" int vkhost_probe_resource_lifecycle() { return 1; }
extern "C" int vkhost_probe_texture_coherence() { return 1; }
