#include "loader.h"
#include "host_services.h"
#include "text_input.h"
#include "guest.h"
#include "save_transaction.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include "graphics/backend.h"
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>

namespace {
std::string quote(const std::string& value) {
    std::string out="\"";
    for (unsigned char c:value) {
        if (c=='"' || c=='\\') out+='\\';
        if (c>=32) out+=char(c);
    }
    return out+'"';
}
void check(VkResult result) {
    if (result!=VK_SUCCESS) throw std::runtime_error("Vulkan operation failed: "+std::to_string(result));
}
struct Gpu {
    VkInstance instance{}; VkDevice device{}; VkPhysicalDevice physical{};
    VkImage image{}; VkDeviceMemory image_memory{};
    VkBuffer buffer{}; VkDeviceMemory buffer_memory{};
    VkCommandPool pool{}; VkFence fence{};
    ~Gpu() {
        if(device) {
            vkDeviceWaitIdle(device);
            if(fence) vkDestroyFence(device,fence,nullptr);
            if(pool) vkDestroyCommandPool(device,pool,nullptr);
            if(buffer) vkDestroyBuffer(device,buffer,nullptr);
            if(buffer_memory) vkFreeMemory(device,buffer_memory,nullptr);
            if(image) vkDestroyImage(device,image,nullptr);
            if(image_memory) vkFreeMemory(device,image_memory,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if(instance) vkDestroyInstance(instance,nullptr);
    }
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        for(uint32_t i=0;i<properties.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("Required Vulkan memory type missing");
    }
};
std::string gpu_probe(std::string& compatibility_error) {
    Gpu g;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName="OT2 compatibility probe"; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo=&app;
    check(vkCreateInstance(&instance,nullptr,&g.instance));
    uint32_t count=0; check(vkEnumeratePhysicalDevices(g.instance,&count,nullptr));
    if(!count) throw std::runtime_error("No Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(g.instance,&count,devices.data())); g.physical=devices.front();
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(g.physical,&properties);
    VkPhysicalDeviceFeatures features; vkGetPhysicalDeviceFeatures(g.physical,&features);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if(properties.apiVersion>=VK_API_VERSION_1_3){query.pNext=&f13;f13.pNext=&robust;}
    else query.pNext=&robust;
    vkGetPhysicalDeviceFeatures2(g.physical,&query);
    if(properties.apiVersion<VK_API_VERSION_1_1)compatibility_error="当前版本需要 Vulkan 1.1 或更高版本。";
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&count,families.data());
    uint32_t family=0;
    while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    if(family==count) throw std::runtime_error("No Vulkan graphics queue");
    float priority=1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex=family; queue_info.queueCount=1; queue_info.pQueuePriorities=&priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount=1; device_info.pQueueCreateInfos=&queue_info;
    check(vkCreateDevice(g.physical,&device_info,nullptr,&g.device));
    VkQueue queue; vkGetDeviceQueue(g.device,family,0,&queue);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType=VK_IMAGE_TYPE_2D; image_info.format=VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent={16,16,1}; image_info.mipLevels=1; image_info.arrayLayers=1;
    image_info.samples=VK_SAMPLE_COUNT_1_BIT; image_info.tiling=VK_IMAGE_TILING_OPTIMAL;
    image_info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateImage(g.device,&image_info,nullptr,&g.image));
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(g.device,g.image,&requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=g.memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g.device,&allocate,nullptr,&g.image_memory));
    check(vkBindImageMemory(g.device,g.image,g.image_memory,0));
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size=16*16*4; buffer_info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(g.device,&buffer_info,nullptr,&g.buffer));
    vkGetBufferMemoryRequirements(g.device,g.buffer,&requirements);
    allocate.allocationSize=requirements.size;
    allocate.memoryTypeIndex=g.memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(g.device,&allocate,nullptr,&g.buffer_memory));
    check(vkBindBufferMemory(g.device,g.buffer,g.buffer_memory,0));
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.queueFamilyIndex=family;
    check(vkCreateCommandPool(g.device,&pool_info,nullptr,&g.pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool=g.pool; command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_info.commandBufferCount=1;
    VkCommandBuffer command; check(vkAllocateCommandBuffers(g.device,&command_info,&command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; check(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image=g.image; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkClearColorValue color{{0.25f,0.5f,0.75f,1.f}};
    vkCmdClearColorImage(command,g.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={16,16,1};
    vkCmdCopyImageToBuffer(command,g.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,g.buffer,1,&copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=g.buffer;
    host.size=VK_WHOLE_SIZE; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    check(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(g.device,&fence_info,nullptr,&g.fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
    check(vkQueueSubmit(queue,1,&submit,g.fence)); check(vkWaitForFences(g.device,1,&g.fence,VK_TRUE,5'000'000'000));
    void* mapped=nullptr; check(vkMapMemory(g.device,g.buffer_memory,0,VK_WHOLE_SIZE,0,&mapped));
    bool correct=true; const std::array<int,4> expected{64,128,191,255};
    const auto* bytes=static_cast<uint8_t*>(mapped);
    for(size_t i=0;i<1024;++i) if(std::abs(int(bytes[i])-expected[i%4])>1) correct=false;
    vkUnmapMemory(g.device,g.buffer_memory);
    if(!correct) throw std::runtime_error("GPU readback differs from clear color");
    std::ostringstream out;
    out << "{\"device\":" << quote(properties.deviceName) << ",\"api\":\""
        << VK_VERSION_MAJOR(properties.apiVersion) << '.' << VK_VERSION_MINOR(properties.apiVersion)
        << '.' << VK_VERSION_PATCH(properties.apiVersion) << "\",\"offscreen_clear_readback\":true,\"texture_bc\":"
        << (features.textureCompressionBC?"true":"false") << ",\"texture_astc\":"
        << (features.textureCompressionASTC_LDR?"true":"false") << ",\"shader_int64\":"
        << (features.shaderInt64?"true":"false") << ",\"dynamic_rendering\":" << (f13.dynamicRendering?"true":"false")
        << ",\"null_descriptor\":" << (robust.nullDescriptor?"true":"false")
        << ",\"depth_bounds\":" << (features.depthBounds?"true":"false")
        << ",\"render_path\":" << quote(f13.dynamicRendering&&features.depthBounds?"dynamic":"legacy")
        << ",\"bc_path\":" << quote(features.textureCompressionBC?"native":"decoded")
        << ",\"null_path\":" << quote(robust.nullDescriptor?"native":"shader") << '}';
    return out.str();
}
bool native_fixture() {
    const size_t page=sysconf(_SC_PAGESIZE);
    void* code=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(code==MAP_FAILED) return false;
    // Independently authored ARM64 fixture: mov w0,#42; ret. Never a game function.
    const uint32_t instructions[]={0x52800540,0xD65F03C0};
    std::memcpy(code,instructions,sizeof(instructions));
    __builtin___clear_cache(static_cast<char*>(code),static_cast<char*>(code)+sizeof(instructions));
    bool passed=false;
    if(mprotect(code,page,PROT_READ|PROT_EXEC)==0) passed=reinterpret_cast<int(*)()>(code)()==42;
    munmap(code,page);
    return passed;
}
bool save_transaction_probe(const std::string& parent){
    std::string temporary=parent+"/save-probe-XXXXXX";
    if(!mkdtemp(temporary.data()))return false;
    struct Cleanup {std::string path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{temporary};
    auto committed=temporary+"/save";std::filesystem::create_directory(committed);
    std::ofstream(committed+"/a")<<"old-a";std::ofstream(committed+"/b")<<"old-b";
    auto read=[](const std::string& path){std::ifstream f(path);return std::string(std::istreambuf_iterator<char>(f),{});};
    ot2::SaveTransaction save(committed);if(save.begin())return false;
    std::ofstream(save.working()+"/a")<<"new-a";std::ofstream(save.working()+"/b")<<"new-b";
    if(read(committed+"/a")!="old-a"||read(committed+"/b")!="old-b"||save.commit())return false;
    if(read(committed+"/a")!="new-a"||read(committed+"/b")!="new-b")return false;
    std::ofstream(save.working()+"/a")<<"broken";save.poison(ENOSPC);
    if(!save.commit()||read(save.working()+"/a")!="new-a")return false;
    ot2::SaveTransaction remount(committed);
    return !remount.begin()&&read(remount.working()+"/a")=="new-a"&&read(remount.working()+"/b")=="new-b";
}
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_ot2_port_MainActivity_runNativeProbe(JNIEnv* env,jclass,jstring path,jstring private_path,jboolean inspect_modules) {
    const char* text=env->GetStringUTFChars(path,nullptr);
    if(!text) return nullptr;
    std::string directory(text); env->ReleaseStringUTFChars(path,text);
    const char* private_text=env->GetStringUTFChars(private_path,nullptr);if(!private_text)return nullptr;
    std::string private_directory(private_text);env->ReleaseStringUTFChars(private_path,private_text);
    std::ostringstream out;
    out << "{\"page_size\":" << sysconf(_SC_PAGESIZE) << ",\"synthetic_arm64_fixture\":"
        << (native_fixture()?"true":"false");
    try {bool passed=save_transaction_probe(private_directory);out << ",\"save_atomic_publish\":" << (passed?"true":"false");}
    catch(const std::exception& e){out << ",\"save_probe_error\":" << quote(e.what());}
    std::string compatibility_error;
    // A bring-up run is about the system service layer, not the display path.
    // A software renderer (an emulator's llvmpipe, for instance) cannot satisfy
    // the texture probes below and would abort before the guest ever runs, so
    // the marker skips the graphics section entirely and lets the boot proceed.
    std::error_code diagnostic_ec;
    const bool diagnostic=!private_directory.empty()&&
        std::filesystem::exists(std::filesystem::path(private_directory)/"diagnostic-svc",diagnostic_ec);
    if(diagnostic) {
        out << ",\"vulkan\":{\"skipped\":\"diagnostic\"},\"graphics_probes_skipped\":true";
        __android_log_print(ANDROID_LOG_INFO,"OT2Probe","diagnostic mode: graphics probes skipped");
        if(!directory.empty()&&inspect_modules) {
            try { auto loaded=ot2::inspect_modules(directory); out << ",\"loader\":" << loaded; }
            catch(const std::exception& e) { out << ",\"loader_error\":" << quote(e.what()); }
        }
        out << ",\"game_booted\":false}";
        const auto skipped=out.str();
        __android_log_print(ANDROID_LOG_INFO,"OT2Probe","%s",skipped.c_str());
        return env->NewStringUTF(skipped.c_str());
    }
    try { auto gpu=gpu_probe(compatibility_error); out << ",\"vulkan\":" << gpu; }
    catch(const std::exception& e) { compatibility_error="GPU 基础验证未通过。";out << ",\"vulkan_error\":" << quote(e.what()); }
    if(!compatibility_error.empty()){
        out << ",\"compatibility_error\":" << quote(compatibility_error) << ",\"game_booted\":false}";
        const auto result=out.str();__android_log_print(ANDROID_LOG_INFO,"OT2Probe","%s",result.c_str());return env->NewStringUTF(result.c_str());
    }
    out << ",\"nvn_texture_transfers\":" << (vkhost_probe_transfers()?"true":"false");
    out << ",\"nvn_descriptor_reuse\":" << (vkhost_probe_descriptor_reuse()?"true":"false");
    out << ",\"nvn_resource_lifecycle\":" << (vkhost_probe_resource_lifecycle()?"true":"false");
    out << ",\"aaudio_disconnect_reopen_injected\":" << (ot2::probe_audio_recovery()?"true":"false");
    try { auto execution=ot2::execution_probe(); out << ",\"execution_bridge\":" << execution; }
    catch(const std::exception& e) { out << ",\"execution_bridge_error\":" << quote(e.what()); }
    out << ",\"nvn_texture_cpu_coherence\":" << (vkhost_probe_texture_coherence()?"true":"false");
    out << ",\"nvn_gpu_deswizzle\":" << (vkhost_probe_deswizzle()?"true":"false");
    if(!directory.empty()&&inspect_modules) {
        try { auto loaded=ot2::inspect_modules(directory); out << ",\"loader\":" << loaded; }
        catch(const std::exception& e) { out << ",\"loader_error\":" << quote(e.what()); }
    }
    out << ",\"game_booted\":false}";
    const auto result=out.str();
    __android_log_print(ANDROID_LOG_INFO,"OT2Probe","%s",result.c_str());
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_bootNative(JNIEnv* env,jclass,jstring path,jstring log_path,jlong program_id) {
    const char* dir=env->GetStringUTFChars(path,nullptr);
    const char* log=env->GetStringUTFChars(log_path,nullptr);
    std::string directory(dir), output(log);
    env->ReleaseStringUTFChars(path,dir);env->ReleaseStringUTFChars(log_path,log);
    try { ot2::boot_game(directory,output,uint64_t(program_id)); }
    catch(const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR,"OT2Guest","Boot exception: %s",e.what()); }
}
extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeSurface(JNIEnv* env,jclass,jobject surface) {
    vkhost_set_window(surface?ANativeWindow_fromSurface(env,surface):nullptr);
}

extern "C" JNIEXPORT jlong JNICALL Java_dev_ot2_port_MainActivity_getNativeFrameCount(JNIEnv*,jclass) {return vkhost_frame_count();}
extern "C" JNIEXPORT jdoubleArray JNICALL Java_dev_ot2_port_MainActivity_getNativeFrameStats(JNIEnv* env,jclass){double values[4];vkhost_frame_stats(values);auto result=env->NewDoubleArray(4);if(result)env->SetDoubleArrayRegion(result,0,4,values);return result;}
extern "C" JNIEXPORT jint JNICALL Java_dev_ot2_port_MainActivity_setNativeTargetFps(JNIEnv*,jclass,jint fps){ot2::set_frame_rate_target(fps);return ot2::frame_rate_applied();}

extern "C" JNIEXPORT void JNICALL Java_dev_ot2_port_MainActivity_setNativeFocus(JNIEnv*,jclass,jint state) {
    if(state==3)ot2::freeze_guest_clock(1,true);
    ot2::set_activity_focus(state);ot2::set_text_input_available(state!=3);vkhost_suspend(state==3);
    if(state!=3)ot2::freeze_guest_clock(1,false);
}
extern "C" JNIEXPORT jobjectArray JNICALL Java_dev_ot2_port_MainActivity_getNativeTextInput(JNIEnv* env,jclass){
    auto pending=ot2::pending_text_input();if(!pending)return nullptr;auto& r=*pending;
    auto ascii=[](uint64_t n){auto s=std::to_string(n);return std::u16string(s.begin(),s.end());};
    std::u16string values[]={ascii(r.id),r.title,r.subtitle,r.guide,r.initial,ascii(r.minimum),ascii(r.maximum)};
    auto strings=env->FindClass("java/lang/String");auto result=env->NewObjectArray(7,strings,nullptr);env->DeleteLocalRef(strings);
    for(unsigned i=0;i<7;i++){auto value=env->NewString(reinterpret_cast<const jchar*>(values[i].data()),values[i].size());env->SetObjectArrayElement(result,i,value);env->DeleteLocalRef(value);}return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_ot2_port_MainActivity_completeNativeTextInput(JNIEnv* env,jclass,jlong id,jboolean accepted,jstring text){
    std::u16string value;if(text){auto* chars=env->GetStringChars(text,nullptr);value.assign(reinterpret_cast<const char16_t*>(chars),env->GetStringLength(text));env->ReleaseStringChars(text,chars);}
    return ot2::complete_text_input(id,accepted,value);
}
extern "C" JNIEXPORT jint JNICALL Java_dev_ot2_port_MainActivity_takeNativeSaveNotice(JNIEnv*,jclass) {return ot2::take_save_notice();}
