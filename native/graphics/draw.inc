// Dedicated NVN command replay on Vulkan. Included in backend.cpp's private namespace.
// Resources are retained until the enclosing NVN batch has completed on the GPU.
using Json=nlohmann::json;
IndexSummaries index_summaries;
struct GpuBuffer {VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};size_t size{};void* writeback{};size_t writeback_size{};const void* source{};VkBufferUsageFlags usage{};VkDeviceSize offset{};bool arena{},descriptor_stable{};std::shared_ptr<void> persistent;};
struct SoftwareSampleSlot {uint32_t index{};GpuBuffer buffer;};
std::vector<SoftwareSampleSlot> software_sample_slots;
uint64_t software_sample_draws=0;
#include "texel_views.inc"
std::vector<GpuBuffer> free_buffers;size_t free_buffer_bytes=0;
CacheBudget cache_budget;
void trim_free_buffers(){
    while(!free_buffers.empty()&&(free_buffers.size()>cache_budget.free_buffers||free_buffer_bytes>cache_budget.free_buffer_bytes)){
        const auto b=free_buffers.back();free_buffers.pop_back();free_buffer_bytes-=b.size;
        invalidate_buffer_views(b.buffer);vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);
    }
}
struct UploadArena {GpuBuffer storage;size_t used{};};
std::vector<UploadArena> upload_arenas;
bool flat_buffer_snapshots=true;
uint64_t flat_snapshot_lookups=0;
bool snapshot_front_cache=true;
uint64_t snapshot_front_hits=0,snapshot_front_misses=0;
struct ReadonlySnapshots {
    using Key=EpochCache<4,size_t>::Key;
    EpochCache<4,size_t> flat;
    DirectCache<4,size_t> front;
    std::vector<GpuBuffer> values;
    std::map<Key,GpuBuffer> tree;
    const GpuBuffer* find(const Key& key){
        if(flat_buffer_snapshots){
            if(snapshot_front_cache)if(auto* index=front.find(key)){++snapshot_front_hits;return &values[*index];}
            ++snapshot_front_misses;++flat_snapshot_lookups;auto* index=flat.find(key);
            if(index&&snapshot_front_cache)front.insert(key,*index);return index?&values[*index]:nullptr;
        }
        auto it=tree.find(key);return it==tree.end()?nullptr:&it->second;
    }
    void insert(const Key& key,const GpuBuffer& value){if(flat_buffer_snapshots){flat.insert(key,values.size());if(snapshot_front_cache)front.insert(key,values.size());values.push_back(value);}else tree.emplace(key,value);}
    void clear(){front.clear();flat.clear();values.clear();tree.clear();}
} readonly_snapshots;
struct BufferMirror {uintptr_t address;size_t bytes;GpuBuffer buffer;};
std::map<uintptr_t,BufferMirror> buffer_mirrors;
bool retiring_gpu_batch=false;
bool buffer_mirrors_enabled=true,building_mirrors=false;
uint64_t mirror_reuses=0,mirror_writebacks=0;
bool overlap(uintptr_t a,size_t size,uintptr_t b,size_t bytes){return size&&bytes&&(a>=b?a-b<bytes:b-a<size);}
BufferMirror* find_buffer_mirror(uintptr_t address,size_t size){auto it=buffer_mirrors.upper_bound(address);if(it==buffer_mirrors.begin())return nullptr;--it;auto& m=it->second;return address>=m.address&&size<=m.bytes&&address-m.address<=m.bytes-size?&m:nullptr;}
void ensure_cpu_buffer(uintptr_t address,size_t size){
    if(previous_cpu_overlap(address,size)){flush_commands();return;}
    if(dependency_index_enabled){if(pending_write_ranges.overlaps(address,size,dependency_index_checks))flush_commands();return;}
    for(const auto& c:pending_counters)if(overlap(address,size,c.address,sizeof(NVNcounterData))){flush_commands();return;}
    for(const auto& [base,m]:buffer_mirrors)if(overlap(address,size,base,m.bytes)){flush_commands();return;}
}
void reset_binding_cache();
GpuBuffer arena_buffer(const void* data,size_t size,VkBufferUsageFlags usage) {
    ensure_batch_slot();
    const size_t aligned=(size+upload_alignment-1)&~(upload_alignment-1);
    UploadArena* arena=nullptr;
    for(auto& a:upload_arenas)if(a.storage.size-a.used>=aligned){arena=&a;break;}
    if(!arena){GpuBuffer b{};b.size=std::max<size_t>(16*1024*1024,aligned);b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=b.size;ci.usage=b.usage;check(vkCreateBuffer(device,&ci,nullptr,&b.buffer),"upload arena buffer");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"upload arena memory");check(vkBindBufferMemory(device,b.buffer,b.memory,0),"upload arena bind");check(vkMapMemory(device,b.memory,0,b.size,0,&b.mapped),"upload arena map");++perf.buffer_allocs;
        upload_arenas.push_back({b,0});arena=&upload_arenas.back();
    }
    auto b=arena->storage;b.offset=arena->used;b.mapped=static_cast<uint8_t*>(b.mapped)+b.offset;b.size=size;b.source=data;b.usage=usage;b.arena=true;arena->used+=aligned;
    if(data)memcpy(b.mapped,data,size);else memset(b.mapped,0,size);return b;
}
bool compact_uniforms=true;
uint64_t uniform_allocation_bytes=0;
GpuBuffer make_buffer(const void* data,size_t size,VkBufferUsageFlags usage,size_t minimum=0,bool standalone=false) {
    PerfScope timing(perf.buffer_ns);perf.buffer_bytes+=size;if(usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)perf.vertex_bytes+=size;if(usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)perf.uniform_bytes+=size;if(usage&(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT))perf.storage_bytes+=size;
    // Robustness2 guarantees zero beyond the descriptor range. Its alignment
    // may round the check up, so physically zero every byte in that padding.
    // Other GPUs retain the original full constant-bank allocation.
    if(compact_uniforms&&robust_uniform_ranges&&usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        minimum=(std::max<size_t>(size,16)+robust_uniform_alignment-1)&~(robust_uniform_alignment-1);
    GpuBuffer b{};b.source=data;b.usage=usage;b.size=std::max<size_t>({size,minimum,16});
    if(usage==VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)uniform_allocation_bytes+=b.size;
    if(batching&&!standalone){auto a=arena_buffer(data&&size==b.size?data:nullptr,b.size,usage);a.source=data;if(data&&size!=b.size)memcpy(a.mapped,data,size);return a;}
    for(auto it=free_buffers.begin();it!=free_buffers.end();++it)if(it->usage==usage && it->size==b.size){
        auto reuse=*it;free_buffer_bytes-=reuse.size;free_buffers.erase(it);reuse.source=data;reuse.writeback=nullptr;reuse.writeback_size=0;memset(reuse.mapped,0,reuse.size);if(data&&size)memcpy(reuse.mapped,data,size);return reuse;
    }
    ++perf.buffer_allocs;VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=b.size;ci.usage=usage;
    check(vkCreateBuffer(device,&ci,nullptr,&b.buffer),"draw buffer");VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"draw buffer memory");check(vkBindBufferMemory(device,b.buffer,b.memory,0),"draw buffer bind");
    check(vkMapMemory(device,b.memory,0,b.size,0,&b.mapped),"draw buffer map");memset(b.mapped,0,b.size);if(data && size)memcpy(b.mapped,data,size);return b;
}
// Immutable, tracked geometry survives recordings. Each draw holds a shared
// lease until its actual fence; replacement/eviction never overwrites in-flight
// bytes. Only registered pool pages qualify, never arbitrary guest heap pages.
bool persistent_uploads=true;
uint64_t persistent_upload_hits=0,persistent_upload_saved=0,persistent_upload_creates=0;
struct PersistentUpload {GpuBuffer buffer;uint64_t stamp{},seen{},used{};};
std::map<ReadonlySnapshots::Key,PersistentUpload> persistent_upload_cache;
size_t persistent_upload_bytes=0;
uint64_t persistent_upload_clock=0;
bool trim_persistent_uploads(size_t incoming_bytes=0,size_t incoming_entries=0){
    while(persistent_upload_bytes+incoming_bytes>cache_budget.geometry_bytes||persistent_upload_cache.size()+incoming_entries>cache_budget.geometry_entries){
        auto oldest=persistent_upload_cache.end();for(auto i=persistent_upload_cache.begin();i!=persistent_upload_cache.end();++i)
            if(i->second.buffer.persistent.use_count()==1&&(oldest==persistent_upload_cache.end()||i->second.used<oldest->second.used))oldest=i;
        if(oldest==persistent_upload_cache.end())return false;
        persistent_upload_bytes-=oldest->second.buffer.size;persistent_upload_cache.erase(oldest);
    }
    return true;
}
GpuBuffer persistent_readonly(const void* data,size_t size,VkBufferUsageFlags usage,size_t minimum){
    if(!persistent_uploads||!batching||!data||size<4096||size>8*1024*1024||minimum>size||
       !(usage&(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT))||
       !nvn_dirty_tracked(data,size))return {};
    ReadonlySnapshots::Key key{uintptr_t(data),size,usage,minimum};const auto now=nvn_dirty_now();
    auto found=persistent_upload_cache.find(key);
    if(found!=persistent_upload_cache.end()){
        auto& e=found->second;
        if(e.seen==now||!nvn_dirty_since(data,size,e.stamp)){
            e.seen=now;e.used=++persistent_upload_clock;++persistent_upload_hits;persistent_upload_saved+=size;return e.buffer;
        }
        persistent_upload_bytes-=e.buffer.size;persistent_upload_cache.erase(found);
    }
    // Do not grow the resident cache when every candidate is leased. The
    // ordinary bounded recording arena remains the fallback for this upload.
    if(!trim_persistent_uploads(size,1))return {};
    const auto stamp=nvn_dirty_now();if(!nvn_dirty_protect(data,size))return {};
    auto b=make_buffer(data,size,usage,minimum,true);
    // One allocation owner. Descriptors and draw copies only extend its lease.
    auto buffer=b.buffer;auto memory=b.memory;
    b.persistent=std::shared_ptr<void>(reinterpret_cast<void*>(uintptr_t(buffer)),[buffer,memory](void*){
        invalidate_buffer_views(buffer);vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);
    });
    persistent_upload_cache.emplace(key,PersistentUpload{b,stamp,stamp,++persistent_upload_clock});
    persistent_upload_bytes+=b.size;++persistent_upload_creates;
    trim_persistent_uploads();
    return b;
}
#include "resource_uploads.inc"
// Guest-owned input ranges cannot be changed until the enclosing NVN submit
// completes. Explicit CPU buffer mutations and storage writebacks flush and
// invalidate this table, so later draws see the new data.
GpuBuffer readonly_buffer(const void* data,size_t size,VkBufferUsageFlags usage,size_t minimum=0){
    if(batching)if(auto* mirror=find_buffer_mirror(uintptr_t(data),std::max(size,minimum))){auto b=mirror->buffer;auto delta=uintptr_t(data)-mirror->address;b.offset+=delta;b.mapped=static_cast<uint8_t*>(b.mapped)+delta;b.size=std::max(size,minimum);b.source=data;++mirror_reuses;return b;}
    ReadonlySnapshots::Key key{uintptr_t(data),size,usage,minimum};
    if(batching)if(auto* b=readonly_snapshots.find(key))return *b;
    auto b=persistent_resource(data,size,usage,minimum);if(!b.buffer)b=persistent_readonly(data,size,usage,minimum);if(!b.buffer)b=make_buffer(data,size,usage,minimum);if(batching)readonly_snapshots.insert(key,b);return b;
}
GpuBuffer writable_buffer(const void* data,size_t size,VkBufferUsageFlags usage){
    if(building_mirrors&&batching){
        auto* mirror=find_buffer_mirror(uintptr_t(data),size);if(mirror)++mirror_reuses;
        if(!mirror){auto b=make_buffer(data,size,usage|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);mirror=&buffer_mirrors.emplace(uintptr_t(data),BufferMirror{uintptr_t(data),size,b}).first->second;pending_write_ranges.add(uintptr_t(data),size);}
        auto b=mirror->buffer;auto delta=uintptr_t(data)-mirror->address;b.offset+=delta;b.mapped=static_cast<uint8_t*>(b.mapped)+delta;b.size=size;b.source=data;return b;
    }
    auto b=make_buffer(data,size,usage);b.writeback=const_cast<void*>(data);b.writeback_size=size;return b;
}
void destroy_buffer(GpuBuffer& b) {
    if(b.persistent){b.persistent.reset();trim_persistent_uploads();return;}
    if(b.arena&&!b.writeback)return;
    if(batch_command&&!retiring_gpu_batch){auto copy=b;batch_retire.push_back([copy]()mutable{destroy_buffer(copy);});return;}
    if(b.writeback)memcpy(b.writeback,b.mapped,b.writeback_size);if(b.arena)return;
    if(free_buffers.size()<cache_budget.free_buffers && free_buffer_bytes+b.size<=cache_budget.free_buffer_bytes){b.writeback=nullptr;b.writeback_size=0;b.source=nullptr;free_buffer_bytes+=b.size;free_buffers.push_back(b);return;}
    invalidate_buffer_views(b.buffer);vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.buffer,nullptr);vkFreeMemory(device,b.memory,nullptr);}
VkShaderStageFlagBits shader_stage(int s) {static const VkShaderStageFlagBits stages[]={VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT,VK_SHADER_STAGE_GEOMETRY_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_COMPUTE_BIT};return stages[s];}
std::string texture_type(int target) {
    static const char* names[]={"1d","2d","3d","1darray","2darray","2dms","2dmsarray","2d","cube","cubearray","buffer"};
    if(target<0 || target>10)ot2_host_die("Unknown texture target %d",target);return names[target];
}
void record_shader_facts(int stage,const NVNshaderData& sd,bool force=false) {
    auto key=shader_key(sd.control);static std::set<std::string> recorded;if(!force && !recorded.insert(key).second)return;Json facts;facts["key"]=key;facts["nvn_stage"]=stage;facts["tex"]=Json::object();facts["descriptors"]=Json::array();
    for(int slot=0;slot<128;slot++)for(int image=0;image<2;image++){
        uint32_t id=(image?render_state.image_handles:render_state.texture_handles)[stage][slot]&0xfffff;if(!id)continue;
        auto it=active_textures().find(id);if(it==active_textures().end()){facts["descriptors"].push_back({{"slot",slot},{"image",image},{"id",id},{"missing",true}});continue;}
        const auto& d=it->second;int target=d.has_view&&d.view.set_target?d.view.target:d.texture.d.target,fmt=d.has_view&&d.view.set_format?d.view.format:d.texture.d.format;
        if(!image)facts["tex"][std::to_string(slot)]=texture_type(target);
        facts["descriptors"].push_back({{"slot",slot},{"image",image},{"id",id},{"serial",d.texture.serial},{"target",target},{"format",fmt},{"width",d.texture.d.width},{"height",d.texture.d.height},{"base_format",d.texture.d.format},{"base_target",d.texture.d.target}});
    }
    std::ofstream(shader_directory+"/"+key+".facts.json")<<facts.dump(2);
}
struct Shader {VkShaderModule module{};Json reflection;std::unique_ptr<ShaderReflection> resources;std::vector<uint8_t> c1;std::set<uint32_t> inputs;bool facts_recorded=false;
    // True when the offline cache has no translation for this key. Such a stage
    // has no module, so the draw that needs it is skipped instead of built.
    bool missing=false;
    // The translated shader declares a uniform block at binding 0 that the
    // reflection does not name. That block is the renderer's own support buffer:
    // it carries the platform fields the shader reads but the title never binds
    // (render scale, BGRA swap, viewport transform). Left unbound it reads as
    // zero, and a zero render scale collapses every vertex of the draw onto one
    // point -- a frame that looks empty while every command was accepted.
    bool needs_support_buffer=false;
    // Storage class each binding was declared with, keyed by binding number.
    // The reflection groups a resource by what NVN called it; the descriptor type
    // has to follow what the translated shader actually declares. A buffer NVN
    // called a storage table can come out as a uniform block, and binding it as a
    // storage buffer then leaves the shader reading a descriptor of the wrong kind
    // -- which no amount of correct data makes work.
    std::map<uint32_t,uint32_t> binding_storage;
    std::vector<ShaderIntegerImage> last_integer_formats;VkShaderModule last_integer_module{};
};
std::map<std::string,Shader> shaders;
Shader* bound_shaders[6]{};
Shader& shader_for(const NVNshaderData& sd,const std::string& key) {
    if(auto it=shaders.find(key);it!=shaders.end())return it->second;
    auto base=shader_binary_directory+"/"+key;
    if(!std::filesystem::exists(base+".spv") || !std::filesystem::exists(base+".res.json")){
        // The cache shipped before the code area took part in the key is named by
        // the control section alone. A hit there is a translation of a *sibling*
        // shader: this title has 613 shaders that share a control section and
        // differ only in code, so the control-only name cannot tell them apart.
        // Using the sibling keeps the draw instead of skipping it, and the
        // substitution is counted so the gap is visible in the log rather than
        // looking like the shader simply worked.
        if(sd.control){
            const auto legacy=shader_key(reinterpret_cast<const void*>(sd.control));
            const auto legacy_base=shader_binary_directory+"/"+legacy;
            if(legacy!=key && std::filesystem::exists(legacy_base+".spv") && std::filesystem::exists(legacy_base+".res.json")){
                static std::atomic<unsigned> substituted{0};
                if(substituted.fetch_add(1,std::memory_order_relaxed)<32)
                    ot2_host_log("SHADER_FALLBACK %s -> %s (sibling that shares the control section)",key.c_str(),legacy.c_str());
                return shader_for(sd,legacy);
            }
        }
        // A complete cache that lacks this key means the packaged translation set
        // does not cover a shader this title builds. Stopping the whole process at
        // that point costs every remaining frame for one effect, so the stage gets
        // a placeholder that carries no module: the draw which needs it is skipped
        // and the key is named in the log for a later offline pass to translate.
        if(complete_shader_cache){
            Shader placeholder;placeholder.missing=true;
            placeholder.reflection=Json{{"key",key},{"cbufs",Json::array()},{"sbufs",Json::array()},{"textures",Json::array()},{"images",Json::array()}};
            placeholder.resources=std::make_unique<ShaderReflection>(placeholder.reflection);
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<32)
                ot2_host_log("SHADER_MISSING %s (not in the offline cache; draws using it are skipped)",key.c_str());
            return shaders.emplace(key,std::move(placeholder)).first->second;
        }
        ot2_host_log("WAIT_SHADER key=%s",key.c_str());
        for(int i=0;i<3000 && (!std::filesystem::exists(base+".spv") || !std::filesystem::exists(base+".res.json"));i++)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::ifstream binary(base+".spv",std::ios::binary|std::ios::ate);
    if(!binary)ot2_host_die("Translated shader missing: %s",key.c_str());size_t size=binary.tellg();
    if(size<20 || size%4)ot2_host_die("Invalid shader size: %s",key.c_str());std::vector<uint32_t> words(size/4);binary.seekg(0);binary.read(reinterpret_cast<char*>(words.data()),size);
    if(words[0]!=0x07230203)ot2_host_die("Invalid shader header: %s",key.c_str());
    Shader s;std::ifstream meta(base+".res.json");if(!meta)ot2_host_die("Shader metadata missing: %s",key.c_str());meta>>s.reflection;s.resources=std::make_unique<ShaderReflection>(s.reflection);if(s.resources->key!=key)ot2_host_die("Shader reflection key mismatch");
    std::set<uint32_t> input_ids;std::map<uint32_t,uint32_t> locations;std::set<uint32_t> member_locations;
    std::set<uint32_t> binding_zero;
    for(size_t at=5;at<words.size();) {auto count=words[at]>>16,op=words[at]&65535;if(!count || at+count>words.size())ot2_host_die("Corrupt SPIR-V instruction");
        if(op==59 && count>=4 && words[at+3]==1)input_ids.insert(words[at+2]);
        if(op==71 && count>=4 && words[at+2]==30)locations[words[at+1]]=words[at+3];
        // A vertex input declared as a struct carries its Location on the struct
        // member, not on the variable. glslang emits that form for cross-compiled
        // HLSL. Only scanning the variable decoration therefore reports "the
        // shader reads nothing but position" for a shader that reads seven inputs,
        // and the pipeline is then built without those attributes.
        if(op==72 && count>=5 && words[at+3]==30)member_locations.insert(words[at+4]);
        // Decoration 33 is Binding. Collected for every id first, then matched
        // against the variables below: an id can carry a binding and not be the
        // variable that uses it.
        if(op==71 && count>=4 && words[at+2]==33 && words[at+3]==0)binding_zero.insert(words[at+1]);
        at+=count;}
    for(auto id:input_ids)if(locations.contains(id))s.inputs.insert(locations[id]);
    s.inputs.insert(member_locations.begin(),member_locations.end());
    // Binding 0 as a Uniform variable is the support buffer. Any other storage
    // class at 0 would be a resource the reflection can name, and a binding the
    // reflection already names is not this one.
    for(size_t at=5;at<words.size();){auto count=words[at]>>16,op=words[at]&65535;if(!count||at+count>words.size())break;
        if(op==59 && count>=4 && words[at+3]==2 && binding_zero.contains(words[at+2]))s.needs_support_buffer=true;at+=count;}
    for(const auto& list:{s.resources->cbufs,s.resources->sbufs,s.resources->textures,s.resources->images})
        for(const auto& r:list)if(r.binding==0)s.needs_support_buffer=false;
    // Binding number -> storage class, straight from the variable declarations.
    // A variable carries both Binding (33) and its storage class; matching them up
    // needs the decoration and the OpVariable for the same id.
    {std::map<uint32_t,uint32_t> binding_of;
     for(size_t at=5;at<words.size();){auto count=words[at]>>16,op=words[at]&65535;if(!count||at+count>words.size())break;
        if(op==71&&count>=4&&words[at+2]==33)binding_of[words[at+1]]=words[at+3];at+=count;}
     for(size_t at=5;at<words.size();){auto count=words[at]>>16,op=words[at]&65535;if(!count||at+count>words.size())break;
        if(op==59&&count>=4){auto it=binding_of.find(words[at+2]);if(it!=binding_of.end())s.binding_storage[it->second]=words[at+3];}
        at+=count;}}
    {char probe[128]={0};size_t at=0;
     for(auto loc:s.inputs)at+=size_t(snprintf(probe+at,sizeof probe-at,"%s%u",at?",":"",loc));
     static std::atomic<unsigned> reported_inputs{0};
     if(reported_inputs.fetch_add(1,std::memory_order_relaxed)<40)
         ot2_host_log("SHADER_INPUTS key=%s variables=%zu members=%zu used=[%s]",key.c_str(),locations.size(),member_locations.size(),probe);}
    auto* control=static_cast<const uint8_t*>(sd.control);uint32_t offset=0,total=0;memcpy(&offset,control+0x700,4);memcpy(&total,control+0x704,4);
    if(offset>total || total>8*1024*1024)ot2_host_die("Invalid shader constants");
    auto* code=reinterpret_cast<const uint8_t*>(sd.data);s.c1.assign(code+offset,code+total);
    // The constant segment a shader carries has to be identified from the data,
    // not assumed: the same word at control+0x700 is a length in one toolchain and
    // something else in another, and taking the wrong pair hands the vertex stage
    // a matrix of garbage that scales the whole draw to a point. Printed next to
    // the control header so the two can be compared once and settled.
    {static std::atomic<unsigned> reported{0};
     if(reported.fetch_add(1,std::memory_order_relaxed)<12){
        char head[3*32+1]={0};
        for(int i=0;i<32;i++)snprintf(head+i*3,4,"%02x ",control[i]);
        char body[3*32+1]={0};
        for(int i=0;i<32&&size_t(i)<s.c1.size();i++)snprintf(body+i*3,4,"%02x ",s.c1[i]);
        ot2_host_log("SHADER_CONST key=%s off=%u total=%u c1=%zu ctl[0..32]=%s c1[0..32]=%s",
            key.c_str(),offset,total,s.c1.size(),head,body);}}
    s.module=shared_shader_module(words.data(),words.size());
    return shaders.emplace(key,std::move(s)).first->second;
}
const NVNshaderData& stage_data(int stage) {
    auto& p=bound_programs[stage];if(!p||p->stages[stage]<0)ot2_host_die("Program stage missing: %d",stage);
    return p->value.sd[p->stages[stage]];
}
Shader& stage_shader(int stage){
    if(!bound_shaders[stage]){
        auto& p=bound_programs[stage];
        // A program bound to a stage slot that carries no shader for it. The real
        // device would not be asked this either, but a title can leave a slot
        // bound from an earlier program; dying here stops every later frame, so
        // report which program it was and let the draw be skipped like any other
        // stage this host cannot build.
        if(!p || p->stages[stage]<0){
            static Shader absent;absent.missing=true;
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<16){
                char keys[160]={};size_t used=0;
                if(p) for(int i=0;i<6&&i<int(p->value.nstage);i++)
                    used+=size_t(snprintf(keys+used,sizeof(keys)-used,"%s%s",i?" ":"",p->keys[i].c_str()));
                ot2_host_log("STAGE_ABSENT stage=%d nstage=%u resolved=[%d %d %d %d %d %d] keys=%s",
                    stage,p?p->value.nstage:0,
                    p?(p->stages[0]>=0?1:0):0,p?(p->stages[1]>=0?1:0):0,p?(p->stages[2]>=0?1:0):0,
                    p?(p->stages[3]>=0?1:0):0,p?(p->stages[4]>=0?1:0):0,p?(p->stages[5]>=0?1:0):0,keys);
            }
            return absent;
        }
        const auto& data=p->value.sd[p->stages[stage]];
        bound_shaders[stage]=&shader_for(data,p->keys[p->stages[stage]]);
    }
    return *bound_shaders[stage];
}
uint64_t constant_bank_expanded_bytes=0;
bool direct_storage_decode=true;
std::vector<uint8_t> constant_bank(int stage,int slot,const Shader& shader) {
    constant_bank_expanded_bytes+=65536;
    std::vector<uint8_t> out(65536);
    if(slot>=3 && slot<19){auto& u=render_state.uniforms[stage][slot-3];if(!u.addr && u.size)ot2_host_die("Missing uniform address");if(u.size>65536)ot2_host_die("Uniform exceeds 64 KiB");if(u.addr){ensure_cpu_buffer(u.addr,u.size);memcpy(out.data(),reinterpret_cast<const void*>(u.addr),u.size);}}
    else if(slot==1){if(shader.c1.size()>out.size())ot2_host_die("Shader c1 too large");memcpy(out.data(),shader.c1.data(),shader.c1.size());}
    else if(slot==2){for(int i=0;i<8;i++){uint32_t image=render_state.image_handles[stage][i];memcpy(out.data()+0x120+i*8,&image,4);}for(int i=0;i<128;i++){uint32_t tex=render_state.texture_handles[stage][i],samp=render_state.sampler_handles[stage][i];memcpy(out.data()+0x168+i*8,&tex,4);memcpy(out.data()+0x568+i*8,&samp,4);}}
    else if(slot==0){int base=stage==0?0x110:stage==1?0x510:stage==5?0x310:-1;if(base<0)ot2_host_die("Storage table for stage %d pending",stage);
        for(int i=0;i<16;i++){const auto& b=render_state.storage[stage][i];memcpy(out.data()+base+16*i,&b.addr,8);uint32_t size=b.size;memcpy(out.data()+base+16*i+8,&size,4);}}
    else ot2_host_die("Unknown constant bank: %d",slot);return out;
}
uint32_t constant_word(int stage,int slot,unsigned word,const Shader& shader){
    const size_t offset=size_t(word)*4;uint32_t result=0;
    if(offset>65536-4)ot2_host_die("Constant handle offset overflow");
    if(slot>=3&&slot<19){auto& u=render_state.uniforms[stage][slot-3];if(u.size>65536||(!u.addr&&u.size))ot2_host_die("Invalid uniform range");
        if(offset<u.size){auto bytes=std::min<size_t>(4,u.size-offset);ensure_cpu_buffer(u.addr+offset,bytes);memcpy(&result,reinterpret_cast<const void*>(u.addr+offset),bytes);}return result;}
    if(slot==1){if(shader.c1.size()>65536)ot2_host_die("Shader c1 too large");if(offset<shader.c1.size())memcpy(&result,shader.c1.data()+offset,std::min<size_t>(4,shader.c1.size()-offset));return result;}
    if(slot==2){
        if(offset>=0x120&&offset<0x160)return (offset-0x120)%8?0:uint32_t(render_state.image_handles[stage][(offset-0x120)/8]);
        if(offset>=0x168&&offset<0x568)return (offset-0x168)%8?0:uint32_t(render_state.texture_handles[stage][(offset-0x168)/8]);
        if(offset>=0x568&&offset<0x968)return (offset-0x568)%8?0:uint32_t(render_state.sampler_handles[stage][(offset-0x568)/8]);
        return 0;
    }
    if(slot==0){int base=stage==0?0x110:stage==1?0x510:stage==5?0x310:-1;if(base<0)ot2_host_die("Storage table for stage %d pending",stage);
        if(offset>=size_t(base)&&offset<size_t(base)+16*16){auto relative=offset-base;const auto& b=render_state.storage[stage][relative/16];
            switch(relative%16){case 0:return uint32_t(b.addr);case 4:return uint32_t(b.addr>>32);case 8:return uint32_t(b.size);default:return 0;}}
        return 0;
    }
    ot2_host_die("Unknown constant bank: %d",slot);
}
uint32_t packed_handle(const ShaderResource& r,int stage,const Shader& shader) {
    uint32_t h=r.handle;int slots=r.cbuf_slot;uint32_t type=h>>28,a=h&0x3fff,b=(h>>14)&0x3fff;
    int cb=slots<0?2:slots&65535,sb=slots<0?cb:((uint32_t(slots)>>16)?int(uint32_t(slots)>>16)-1:cb);
    if(type==4)return a;
    uint32_t result=constant_word(stage,cb,a,shader);
    if(type){uint32_t samp=b;if(type!=3){samp=constant_word(stage,sb,b,shader);}if(type==2 || type==3)samp<<=20;result|=samp;}
    return result;
}
struct StorageBinding {uint64_t address;uint32_t size;};
// The renderer's support buffer, laid out as the translated shaders expect it:
//   +0    alpha_test        u32
//   +16   is_bgra           u32[8]
//   +144  viewport_inverse  f4
//   +160  viewport_size     f4
//   +176  frag_scale_count  i32
//   +192  render_scale      f4[73]   (only .x is read)
// Everything else is zero, which is what the platform fills in when alpha test is
// off and the destination is not BGRA. render_scale is the entry that must not be
// zero: the vertex stage multiplies the projected position by it, so an unbound
// buffer scales the whole draw to nothing.
std::vector<uint8_t> support_buffer_bytes(){
    // Mirror of the renderer support buffer every translated shader declares at
    // binding 0. The platform fills these fields in before the shader runs; a zero
    // left here is not "no effect", it is a value the vertex stage multiplies by.
    //   +0    alpha_test        u32
    //   +16   is_bgra           u32[8]
    //   +144  viewport_inverse  f4
    //   +160  viewport_size     f4
    //   +176  frag_scale_count  i32
    //   +192  render_scale      f4[73]
    std::vector<uint8_t> bytes(1392,0);
    const float width=1280.f, height=720.f;
    auto put=[&](size_t offset,float x,float y,float z,float w){
        auto* p=reinterpret_cast<float*>(bytes.data()+offset);
        p[0]=x;p[1]=y;p[2]=z;p[3]=w;};
    put(144,2.f/width,2.f/height,1.f,1.f);   // viewport_inverse
    put(160,width,height,0.f,0.f);           // viewport_size
    *reinterpret_cast<uint32_t*>(bytes.data()+176)=1;   // frag_scale_count
    for(int i=0;i<73;i++)put(192+size_t(i)*16,1.f,1.f,1.f,1.f);   // render_scale
    return bytes;
}
StorageBinding storage_binding(int stage,const ShaderResource& resource,const Shader& shader){    const size_t offset=size_t(resource.cb_offset)*4;
    if(offset>65536-12)ot2_host_die("Storage-buffer table offset invalid");
    if(direct_storage_decode){
        const auto word=resource.cb_offset;const auto slot=resource.cb_slot;
        auto low=constant_word(stage,slot,word,shader),high=constant_word(stage,slot,word+1,shader);
        return {uint64_t(low)|(uint64_t(high)<<32),constant_word(stage,slot,word+2,shader)};
    }
    StorageBinding result{};auto bank=constant_bank(stage,resource.cb_slot,shader);
    memcpy(&result.address,bank.data()+offset,8);memcpy(&result.size,bank.data()+offset+8,4);return result;
}
bool gpu_deswizzle(const tex_t&,unsigned,unsigned,GpuBuffer&,const GpuBuffer* =nullptr);
bool gpu_deswizzle_enabled=true;
void upload_packaged(const tex_t& t) {
    auto* existing=find_existing_image(t);
    materialize_clears(t.serial);
    if(t.mem)ensure_cpu_buffer(uintptr_t(t.mem),t.L.size);
    if(auto* cached=existing){
        // A nonzero packaged block height preserves the builder's has-layout
        // bit within OT2's 128-byte object ABI. GPU-owned targets are separate.
        if(!t.d.pkg_block_h || !t.mem)return;
        auto& image=*cached;const auto now=nvn_dirty_now();
        if(image.source_stamp && image.source_tracked && nvn_dirty_tracked(t.mem,t.L.size) &&
           (now==image.source_seen || !nvn_dirty_since(t.mem,t.L.size,image.source_stamp))){image.source_seen=now;return;}
    }
    if(t.d.samples>1){image_for(t);return;}
    auto& image=image_for(t);if(!t.mem)ot2_host_die("Texture has no backing storage");
    if(++image.source_uploads>1){static unsigned reuploads=0;if(++reuploads<=3)ot2_host_log("GPU_TEXTURE_REUPLOAD serial=%llu uploads=%llu",(unsigned long long)t.serial,(unsigned long long)image.source_uploads);}
    if(t.d.pkg_block_h){image.source_tracked=nvn_dirty_protect(t.mem,t.L.size);image.source_stamp=nvn_dirty_now();image.source_seen=image.source_stamp;}
    int bw,bh,bytes=nvn_fmt_bpb(t.d.format,&bw,&bh);if(!bytes)ot2_host_die("Unknown packaged format");
    // A new upload means the CPU may have changed the same address even within
    // this batch. Share one immutable source copy across its mips, never the
    // address-only draw snapshot cache across distinct texture versions.
    GpuBuffer source{};
    if(!image.bc&&gpu_deswizzle_enabled&&t.L.size<=max_storage_range&&!(t.d.flags&0x10)&&!packed_depth_stencil(image))source=make_buffer(t.mem,t.L.size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for(unsigned level=0;level<image.levels;level++)for(unsigned layer=0;layer<image.layers;layer++) {
        unsigned w=std::max(1U,t.d.width>>level),h=std::max(1U,t.d.height>>level),z=image.is_3d?std::max(1U,t.d.depth>>level):1;
        size_t row=(w+bw-1)/bw*bytes,rows=(h+bh-1)/bh,pixels=size_t(w)*h*z;bool packed=packed_depth_stencil(image);
        auto bc_info=bc_format(t.d.format);
        auto count=image.bc?pixels*bc_info.pixel_bytes:packed?pixels*5:row*rows*z;auto b=make_buffer(nullptr,count,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,(count+3)&~size_t(3));
        std::vector<uint8_t> interleaved;if(packed||image.bc)interleaved.resize(row*rows*z);
        auto* destination=packed||image.bc?interleaved.data():static_cast<uint8_t*>(b.mapped);
        if(t.d.flags&0x10){if(level || layer || z>1)ot2_host_die("Layered linear texture pending");size_t pitch=t.d.stride?t.d.stride:(row+t.L.align-1)&~size_t(t.L.align-1);if(pitch<row)ot2_host_die("Linear texture pitch smaller than row");for(size_t y=0;y<rows;y++)memcpy(destination+y*row,t.mem+y*pitch,row);}
        else if(!(source.buffer&&gpu_deswizzle(t,level,layer,b,&source))&&nvn_deswizzle(&t.d,level,layer,t.mem,destination,row))ot2_host_die("Packaged texture deswizzle failed");
        if(packed)split_depth_stencil(image.format,static_cast<uint8_t*>(b.mapped),pixels,0,interleaved.data(),pixels);
        if(image.bc){
            image.bc->write({level,layer,0,0,0,w,h,z},interleaved.data());
            const size_t decoded_slice=size_t(w)*h*bc_info.pixel_bytes;
            for(unsigned slice=0;slice<z;slice++)bc_decode(t.d.format,{interleaved.data()+slice*row*rows,row*rows},row,w,h,{static_cast<uint8_t*>(b.mapped)+slice*decoded_slice,decoded_slice},size_t(w)*bc_info.pixel_bytes);
        }
        VkBufferImageCopy copy{};copy.bufferOffset=b.offset;copy.imageSubresource={image.aspects,level,layer,1};copy.imageExtent={w,h,z};auto cmd=begin_commands();barrier(cmd,image,image.layout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        auto regions=transfer_regions(image,copy,pixels);
        vkCmdCopyBufferToImage(cmd,b.buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,regions.size(),regions.data());finish_commands(cmd);destroy_buffer(b);
    }
    if(source.buffer)destroy_buffer(source);
}
std::unordered_map<FixedKey<64>,VkImageView,FixedKey<64>::Hash> image_views;
std::unordered_map<FixedKey<64>,VkSampler,FixedKey<64>::Hash> sampler_objects;
bool reuse_resource_objects=true;
VkImageView make_view(const tex_t& texture,const view_t* view,bool attachment=false) {
    auto& img=image_for(texture);auto range=subresources(img,view);if(attachment){range.levelCount=1;if(img.is_3d){range.baseArrayLayer=view&&view->set_layers?view->min_layer:0;range.layerCount=view&&view->set_layers&&view->nlayers?view->nlayers:std::max(1U,texture.d.depth>>range.baseMipLevel)-range.baseArrayLayer;}}
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=img.image;ci.format=view && view->set_format?format(view->format):img.format;
    if(img.bc){
        if(attachment)ot2_host_die("Compressed texture cannot be a render target");
        if(view&&view->set_format){auto base=bc_format(texture.d.format),other=bc_format(view->format);
            // sRGB and linear pairs share decoded bytes. Other BC view
            // reinterpretations need a distinct decode, never a raw RGBA view.
            auto canonical=[](int f){switch(f){case NVN_FORMAT_RGB_DXT1_SRGB:return int(NVN_FORMAT_RGB_DXT1);case NVN_FORMAT_RGBA_DXT1_SRGB:return int(NVN_FORMAT_RGBA_DXT1);case NVN_FORMAT_RGBA_DXT3_SRGB:return int(NVN_FORMAT_RGBA_DXT3);case NVN_FORMAT_RGBA_DXT5_SRGB:return int(NVN_FORMAT_RGBA_DXT5);case NVN_FORMAT_BPTC_UNORM_SRGB:return int(NVN_FORMAT_BPTC_UNORM);default:return f;}};
            if(!other.block_bytes||canonical(view->format)!=canonical(texture.d.format))ot2_host_die("BC view reinterpretation requires another decode");
            ci.format=format(other.unpacked);
        }
    }
    int target=view && view->set_target?view->target:texture.d.target;
    if(attachment)ci.viewType=range.layerCount>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    else switch(target){case NVN_TEXTURE_TARGET_1D:ci.viewType=VK_IMAGE_VIEW_TYPE_1D;break;case NVN_TEXTURE_TARGET_3D:ci.viewType=VK_IMAGE_VIEW_TYPE_3D;break;case NVN_TEXTURE_TARGET_1D_ARRAY:ci.viewType=VK_IMAGE_VIEW_TYPE_1D_ARRAY;break;case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:case NVN_TEXTURE_TARGET_2D_ARRAY:ci.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;break;case NVN_TEXTURE_TARGET_CUBEMAP:ci.viewType=VK_IMAGE_VIEW_TYPE_CUBE;break;case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:ci.viewType=VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;break;default:ci.viewType=VK_IMAGE_VIEW_TYPE_2D;}
    if(!attachment && (range.aspectMask&VK_IMAGE_ASPECT_DEPTH_BIT))range.aspectMask=(view&&view->set_depth_mode?view->depth_mode:texture.d.depth_stencil_mode)?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT;
    ci.subresourceRange=range;
    if(!attachment){VkComponentSwizzle sw[6]={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A};
        uint8_t components[4];for(int i=0;i<4;i++){components[i]=view&&view->set_swizzle?(view->swizzle>>(3*i))&7:texture.d.swizzle[i];if(components[i]>=6)ot2_host_die("Invalid texture component swizzle");
            // Maxwell D24S8/D32S8 exposes stencil in G. A Vulkan stencil-only
            // view exposes that plane in R, so preserve the guest channel map.
            if(range.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT&&components[i]==3)components[i]=2;}
        ci.components={sw[components[0]],sw[components[1]],sw[components[2]],sw[components[3]]};}
    FixedKey<64> key;key.add(ci.image);key.add(ci.format);key.add(ci.viewType);key.add(ci.subresourceRange);key.add(ci.components);
    if(auto it=image_views.find(key);it!=image_views.end())return it->second;
    VkImageView result;check(vkCreateImageView(device,&ci,nullptr,&result),"texture view");image_views.emplace(std::move(key),result);return result;
}
VkSamplerAddressMode wrap_mode(unsigned m) {switch(m){case 0:case 5:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;case 1:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case 2:case 3:return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;case 6:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;case 7:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;default:ot2_host_die("Sampler wrap mode %u pending",m);}}
bool empty_descriptor(unsigned id,unsigned capacity){return id<256||id==0xfffff||(capacity&&id>=capacity);}
VkSampler default_sampler(bool shadow=false){
    static VkSampler samplers[2]{};auto& sampler=samplers[shadow];if(sampler)return sampler;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};ci.magFilter=ci.minFilter=VK_FILTER_NEAREST;ci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU=ci.addressModeV=ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;ci.maxLod=VK_LOD_CLAMP_NONE;ci.compareEnable=shadow;ci.compareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    check(vkCreateSampler(device,&ci,nullptr,&sampler),"default empty-slot sampler");return sampler;
}
VkSampler make_sampler(unsigned id,bool shadow=false) {
    auto it=active_samplers().find(id);if(it==active_samplers().end()){
        if(empty_descriptor(id,active_sampler_capacity))return default_sampler(shadow);
        ot2_host_die("Sampler %u is unregistered",id);
    }auto& entry=it->second;if(reuse_resource_objects&&entry.cached)return entry.cached;const auto& p=entry.parameters;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};ci.magFilter=p.magf?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.minFilter=p.minf&1?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    ci.mipmapMode=p.minf>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;ci.addressModeU=wrap_mode(p.ws);ci.addressModeV=wrap_mode(p.wt);ci.addressModeW=wrap_mode(p.wr);
    ci.mipLodBias=p.lod_bias;ci.minLod=p.minf<2?0:p.lod_min;ci.maxLod=p.minf<2?0.25f:p.lod_max;ci.anisotropyEnable=p.aniso>1;ci.maxAnisotropy=std::max(1.0f,p.aniso);
    ci.compareEnable=p.cmp_mode;ci.compareOp=VkCompareOp(std::max(1,int(p.cmp_func))-1);ci.borderColor=p.border[0]?VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:p.border[3]?VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    FixedKey<64> key;key.add(ci.magFilter);key.add(ci.minFilter);key.add(ci.mipmapMode);key.add(ci.addressModeU);key.add(ci.addressModeV);key.add(ci.addressModeW);key.add(ci.mipLodBias);key.add(ci.minLod);key.add(ci.maxLod);key.add(ci.anisotropyEnable);key.add(ci.maxAnisotropy);key.add(ci.compareEnable);key.add(ci.compareOp);key.add(ci.borderColor);
    if(auto it=sampler_objects.find(key);it!=sampler_objects.end())return entry.cached=it->second;
    VkSampler result;check(vkCreateSampler(device,&ci,nullptr,&result),"sampler");sampler_objects.emplace(std::move(key),result);return entry.cached=result;
}
// Pipeline objects are immutable; descriptor sets from structurally identical
// layouts remain compatible. Keep compiled pipelines across repeated frames.
std::map<std::string,VkPipeline> pipeline_objects;
bool dynamic_depth_bounds=true;
bool pipeline_dynamic(const VkGraphicsPipelineCreateInfo& ci,VkDynamicState state){
    if(!ci.pDynamicState)return false;
    for(unsigned i=0;i<ci.pDynamicState->dynamicStateCount;++i)if(ci.pDynamicState->pDynamicStates[i]==state)return true;
    return false;
}
bool packed_descriptor_keys=true;
bool retain_texel_descriptors=true;
struct PipelineKey {
    std::string data;
    template<class T> void add(const T& x){data.append(reinterpret_cast<const char*>(&x),sizeof(x));}
    template<class T> void array(const T* x,size_t n){add(n);if(n)data.append(reinterpret_cast<const char*>(x),sizeof(T)*n);}
    void bindings(const std::vector<VkDescriptorSetLayoutBinding>& bs){add(bs.size());for(auto& b:bs){
        if(packed_descriptor_keys)append_key_fields(data,b.binding,b.descriptorType,b.descriptorCount,b.stageFlags);
        else {add(b.binding);add(b.descriptorType);add(b.descriptorCount);add(b.stageFlags);}
    }}
};
std::string pipeline_key(const VkComputePipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    PipelineKey key;key.data="C";key.add(ci.stage.module);key.bindings(bindings);return key.data;
}
std::string pipeline_key(const VkGraphicsPipelineCreateInfo& ci,const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    PipelineKey key;key.data="G";key.add(ci.stageCount);for(unsigned i=0;i<ci.stageCount;i++){key.add(ci.pStages[i].stage);key.add(ci.pStages[i].module);}key.bindings(bindings);
    const auto& r=*static_cast<const VkPipelineRenderingCreateInfo*>(ci.pNext);key.array(r.pColorAttachmentFormats,r.colorAttachmentCount);key.add(r.depthAttachmentFormat);key.add(r.stencilAttachmentFormat);key.add(r.viewMask);
    const auto& vi=*ci.pVertexInputState;key.array(vi.pVertexAttributeDescriptions,vi.vertexAttributeDescriptionCount);key.array(vi.pVertexBindingDescriptions,vi.vertexBindingDescriptionCount);
    key.add(ci.pInputAssemblyState->topology);key.add(ci.pInputAssemblyState->primitiveRestartEnable);
    // Viewport and scissor are dynamic; moving UI does not create pipelines.
    const auto& rs=*ci.pRasterizationState;key.add(rs.depthClampEnable);key.add(rs.rasterizerDiscardEnable);key.add(rs.polygonMode);key.add(rs.cullMode);key.add(rs.frontFace);key.add(rs.depthBiasEnable);key.add(rs.depthBiasConstantFactor);key.add(rs.depthBiasClamp);key.add(rs.depthBiasSlopeFactor);key.add(rs.lineWidth);
    const auto& ms=*ci.pMultisampleState;key.add(ms.rasterizationSamples);key.add(ms.sampleShadingEnable);key.add(ms.minSampleShading);key.add(ms.alphaToCoverageEnable);key.add(ms.alphaToOneEnable);key.add(ms.pSampleMask?*ms.pSampleMask:~0u);
    const auto& ds=*ci.pDepthStencilState;key.add(ds.depthTestEnable);key.add(ds.depthWriteEnable);key.add(ds.depthCompareOp);key.add(ds.stencilTestEnable);key.add(ds.depthBoundsTestEnable);
    if(!pipeline_dynamic(ci,VK_DYNAMIC_STATE_DEPTH_BOUNDS)){key.add(ds.minDepthBounds);key.add(ds.maxDepthBounds);}key.add(ds.front);key.add(ds.back);
    // Static and dynamic pipelines are not interchangeable, even when their
    // current numeric values happen to match.
    key.array(ci.pDynamicState?ci.pDynamicState->pDynamicStates:nullptr,ci.pDynamicState?ci.pDynamicState->dynamicStateCount:0);
    const auto& cb=*ci.pColorBlendState;key.add(cb.logicOpEnable);key.add(cb.logicOp);key.array(cb.pAttachments,cb.attachmentCount);key.array(cb.blendConstants,4);
    return key.data;
}
#include "pipeline_warmup.inc"
#include "descriptors.inc"
std::map<std::string,VkShaderModule> null_shader_modules;
VkShaderModule software_depth_module(VkShaderModule module){
    return depth_compatible_module(module);
}
VkShaderModule software_null_module(VkShaderModule module,const std::vector<uint32_t>& bindings,const ShaderReflection& reflection){
    if(bindings.empty())return module;
    // Unbound bindings in another stage must not create identical variants of
    // this module or consume its bounded cache. Reflection is immutable.
    std::vector<uint32_t> sorted;sorted.reserve(bindings.size());
    for(const auto* group:{&reflection.sbufs,&reflection.textures,&reflection.images})for(const auto& r:*group)
        if(std::find(bindings.begin(),bindings.end(),r.binding)!=bindings.end())sorted.push_back(r.binding);
    if(sorted.empty())return module;
    std::sort(sorted.begin(),sorted.end());sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    std::string key;legacy_key_add(key,module);for(auto binding:sorted)legacy_key_add(key,binding);
    if(auto found=null_shader_modules.find(key);found!=null_shader_modules.end())return found->second;
    if(null_shader_modules.size()>=4096)ot2_host_die("Software null shader variant limit exceeded");
    std::vector<uint32_t> words;{std::lock_guard lock(module_mutex);words=module_records.at(module_keys.at(module)).words;}
    auto patched=shader_null_descriptors(words,sorted);auto result=shared_shader_module(patched.data(),patched.size());
    null_shader_modules.emplace(std::move(key),result);return result;
}
std::map<VkShaderModule,VkShaderModule> sample_count_modules;
VkShaderModule software_samples_module(VkShaderModule module){
    if(auto it=sample_count_modules.find(module);it!=sample_count_modules.end())return it->second;
    if(sample_count_modules.size()>=4096)ot2_host_die("Software sample shader limit exceeded");
    std::vector<uint32_t> words;{std::lock_guard lock(module_mutex);words=module_records.at(module_keys.at(module)).words;}
    auto patched=shader_count_samples(words);auto result=shared_shader_module(patched.data(),patched.size());
    sample_count_modules.emplace(module,result);return result;
}
bool gpu_deswizzle(const tex_t& texture,unsigned level,unsigned layer,GpuBuffer& output,const GpuBuffer* prepared){
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
    if(nvn_deswizzle_geom(&texture.d,level,layer,&row,&rows,&slices,&base,&th,&td,&bpb)||texture.L.size>max_storage_range||output.size>max_storage_range||base>UINT32_MAX||uint64_t(row)*rows*slices>UINT32_MAX)return false;
    auto source=prepared?*prepared:make_buffer(texture.mem,texture.L.size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint32_t geometry[]={row,rows,slices,uint32_t(base),th,td,0,0};auto params=make_buffer(geometry,sizeof(geometry),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::vector<VkDescriptorSetLayoutBinding> bindings{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    auto* descriptor=acquire_descriptors(bindings);
    VkDescriptorBufferInfo infos[]={{source.buffer,source.offset,source.size},{output.buffer,output.offset,output.size},{params.buffer,params.offset,params.size}};
    VkWriteDescriptorSet writes[3]{};for(unsigned i=0;i<3;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=descriptor->set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(device,3,writes,0,nullptr);
    static VkShaderModule module{};if(!module)module=shared_shader_module(deswizzle_comp,std::size(deswizzle_comp));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=descriptor->owner->pipeline_layout;
    auto pipeline=cached_pipeline(ci,bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,ci.layout,descriptor->set);
    auto groups=uint32_t((uint64_t(row)*rows*slices+255)/256);if(gpu_profile_enabled)gpu_profile_region_begin(cmd,"deswizzle");vkCmdDispatch(cmd,std::min(groups,65535u),(groups+65534u)/65535u,1);gpu_profile_region_end(cmd);finish_commands(cmd);
    auto retire=[descriptor,source,params,prepared]()mutable{release_descriptors(descriptor);if(!prepared)destroy_buffer(source);destroy_buffer(params);};if(batch_command)batch_retire.push_back(retire);else retire();return true;
}
uint64_t bank_generation[6][2]{};
struct CachedBank {uint64_t generation{};GpuBuffer buffer;};
CachedBank cached_banks[6][2];
std::map<std::pair<float,float>,GpuBuffer> support_cache;
std::map<std::string,std::shared_ptr<DescriptorSlot>> bound_descriptor_cache;
bool reuse_bindings=true;
uint64_t binding_cache_hits=0;
// Adreno device A/B reduced update count but increased replay time. Keep this
// capability opt-in until a device/workload demonstrates an actual benefit.
bool dynamic_buffer_descriptors=false;
bool reuse_descriptor_signature=true;
uint64_t descriptor_set_updates=0;
uint64_t resource_generation=1;
void reset_prepared_bindings();
void reset_binding_cache(){for(auto& stage:cached_banks)for(auto& bank:stage)bank={};support_cache.clear();bound_descriptor_cache.clear();reset_prepared_bindings();}
GpuBuffer cached_bank(int stage,int slot,const Shader& shader){
    int which=slot==0?0:1;auto& entry=cached_banks[stage][which];
    if(batching&&reuse_bindings&&entry.buffer.buffer&&entry.generation==bank_generation[stage][which])return entry.buffer;
    auto bytes=constant_bank(stage,slot,shader);auto b=make_buffer(bytes.data(),bytes.size(),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,65536);
    if(batching&&reuse_bindings)entry={bank_generation[stage][which],b};return b;
}
GpuBuffer cached_support(){
    auto key=std::pair(render_state.viewport[2],render_state.viewport[3]);if(batching&&reuse_bindings)if(auto i=support_cache.find(key);i!=support_cache.end())return i->second;
    std::array<uint8_t,2048> bytes{};for(int i=0;i<73;i++){float one=1;memcpy(bytes.data()+192+16*i,&one,4);}float viewport[]={key.first,key.second,0,0};memcpy(bytes.data()+160,viewport,16);
    auto b=make_buffer(bytes.data(),bytes.size(),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,65536);if(batching&&reuse_bindings)support_cache.emplace(key,b);return b;
}
struct ImageSerialSet {
    std::vector<uint64_t> values;
    bool contains(uint64_t serial)const{return std::find(values.begin(),values.end(),serial)!=values.end();}
    void insert(uint64_t serial){if(!contains(serial))values.push_back(serial);}
    void clear(){values.clear();}
};
bool precise_resource_dependencies=false;
uint64_t precise_dependency_hits=0,precise_dependency_checks=0,precise_dependency_fallbacks=0;
struct ResourceDependencies {
    struct Range {const void* address;size_t size;};
    std::vector<Range> ranges;bool complete=true;
    uint64_t stamp=precise_resource_dependencies?nvn_dirty_now():0;
    void reset(){ranges.clear();complete=true;stamp=precise_resource_dependencies?nvn_dirty_now():0;}
    void watch(const void* p,size_t size,bool protected_source=false){
        if(!precise_resource_dependencies||!complete||!size)return;
        for(auto& r:ranges)if(r.address==p&&r.size==size)return;
        if(ranges.size()==64||!p||(!protected_source&&!nvn_dirty_protect(p,size))){complete=false;return;}
        ranges.push_back({p,size});
    }
    void append(const ResourceDependencies& from){
        if(!precise_resource_dependencies||!complete)return;
        if(!from.complete){complete=false;return;}
        stamp=std::min(stamp,from.stamp);
        for(auto& r:from.ranges){
            bool found=false;for(auto& old:ranges)if(old.address==r.address&&old.size==r.size){found=true;break;}
            if(!found){if(ranges.size()==64){complete=false;return;}ranges.push_back(r);}
        }
    }
    bool unchanged(uint64_t stamp)const{
        if(!precise_resource_dependencies||!complete){++precise_dependency_fallbacks;return false;}
        for(auto& r:ranges){++precise_dependency_checks;if(nvn_dirty_since(r.address,r.size,stamp))return false;}
        ++precise_dependency_hits;return true;
    }
};
std::optional<ShaderIntegerImage> integer_texel_format(uint32_t binding,int format){
#define INTEGER_TEXEL(n,v,s) case NVN_FORMAT_##n:return ShaderIntegerImage{binding,s,spv::ImageFormat##v}
    switch(format){
    INTEGER_TEXEL(R8UI,R8ui,0);INTEGER_TEXEL(R16UI,R16ui,0);INTEGER_TEXEL(R32UI,R32ui,0);
    INTEGER_TEXEL(RG8UI,Rg8ui,0);INTEGER_TEXEL(RG16UI,Rg16ui,0);INTEGER_TEXEL(RG32UI,Rg32ui,0);
    INTEGER_TEXEL(RGBA8UI,Rgba8ui,0);INTEGER_TEXEL(RGBA16UI,Rgba16ui,0);INTEGER_TEXEL(RGBA32UI,Rgba32ui,0);
    INTEGER_TEXEL(R8I,R8i,1);INTEGER_TEXEL(R16I,R16i,1);INTEGER_TEXEL(R32I,R32i,1);
    INTEGER_TEXEL(RG8I,Rg8i,1);INTEGER_TEXEL(RG16I,Rg16i,1);INTEGER_TEXEL(RG32I,Rg32i,1);
    INTEGER_TEXEL(RGBA8I,Rgba8i,1);INTEGER_TEXEL(RGBA16I,Rgba16i,1);INTEGER_TEXEL(RGBA32I,Rgba32i,1);
    INTEGER_TEXEL(RGB10A2UI,Rgb10a2ui,0);default:return {};
    }
#undef INTEGER_TEXEL
}
std::map<std::string,VkShaderModule> integer_texel_modules;
VkShaderModule integer_texel_module(VkShaderModule original,const std::vector<ShaderIntegerImage>& formats,const ShaderReflection& reflection){
    std::vector<ShaderIntegerImage> selected;
    for(const auto& f:formats){bool used=false;for(const auto& r:reflection.textures)used|=r.binding==f.binding;for(const auto& r:reflection.images)used|=r.binding==f.binding;if(used)selected.push_back(f);}
    if(selected.empty())return original;
    std::sort(selected.begin(),selected.end(),[](const auto& a,const auto& b){return a.binding<b.binding;});
    PipelineKey key;key.add(original);for(const auto& f:selected){key.add(f.binding);key.add(f.signedness);key.add(f.image_format);}
    if(auto found=integer_texel_modules.find(key.data);found!=integer_texel_modules.end())return found->second;
    if(integer_texel_modules.size()>=4096)ot2_host_die("Integer texel shader variant limit exceeded");
    std::vector<uint32_t> words;{std::lock_guard lock(module_mutex);words=module_records.at(module_keys.at(original)).words;}
    auto patched=shader_integer_images(words,selected);auto result=shared_shader_module(patched.data(),patched.size());
    integer_texel_modules.emplace(std::move(key.data),result);return result;
}
std::map<std::string,VkShaderModule> integer_output_modules;
bool reuse_integer_selections=true;
uint64_t integer_selection_hits=0,integer_selection_misses=0;
VkShaderModule integer_texel_module(Shader& shader,const std::vector<ShaderIntegerImage>& formats){
    if(formats.empty())return shader.module;
    const auto same=[](const auto& a,const auto& b){return a.binding==b.binding&&a.signedness==b.signedness&&a.image_format==b.image_format;};
    if(reuse_integer_selections&&shader.last_integer_module&&formats.size()==shader.last_integer_formats.size()&&std::equal(formats.begin(),formats.end(),shader.last_integer_formats.begin(),same)){
        ++integer_selection_hits;return shader.last_integer_module;
    }
    ++integer_selection_misses;
    auto module=integer_texel_module(shader.module,formats,*shader.resources);
    // One selection per immutable shader; formats include all bound stages so
    // a binding/type change cannot reuse a stale specialization. No GPU objects
    // are owned here, and the existing module cache still enforces its limit.
    if(reuse_integer_selections){shader.last_integer_formats=formats;shader.last_integer_module=module;}
    return module;
}
VkShaderModule integer_output_module(VkShaderModule original){
    std::vector<ShaderIntegerOutput> outputs;
    for(int i=0;i<render_targets.n;i++)if(auto* t=render_targets.color[i]){
        auto* v=render_targets.has_cview[i]?&render_targets.cview[i]:nullptr;
        if(auto f=integer_texel_format(i,v&&v->set_format?v->format:t->d.format))outputs.push_back({uint32_t(i),f->signedness});
    }
    if(outputs.empty())return original;PipelineKey key;key.add(original);for(auto f:outputs){key.add(f.location);key.add(f.signedness);}
    if(auto found=integer_output_modules.find(key.data);found!=integer_output_modules.end())return found->second;
    if(integer_output_modules.size()>=4096)ot2_host_die("Integer output shader variant limit exceeded");
    std::vector<uint32_t> words;{std::lock_guard lock(module_mutex);words=module_records.at(module_keys.at(original)).words;}
    auto patched=shader_integer_outputs(words,outputs);auto result=shared_shader_module(patched.data(),patched.size());integer_output_modules.emplace(std::move(key.data),result);return result;
}
struct DrawResources {
    ResourceDependencies dependencies;
    std::vector<uint32_t> null_bindings;
    std::vector<ShaderIntegerImage> integer_images;
    std::vector<GpuBuffer> buffers;std::vector<VkImageView> views;std::vector<VkSampler> samps;std::vector<VkBufferView> texel_views;
    std::vector<TexelViewEntry*> texel_view_leases;
    std::vector<VkDescriptorSetLayoutBinding> bindings;std::vector<VkDescriptorBufferInfo> buffer_infos;std::vector<VkDescriptorImageInfo> image_infos;
    struct Write {uint32_t binding;VkDescriptorType type;size_t index;int kind;};std::vector<Write> writes;
    ImageSerialSet sampled_images;
    std::vector<VkWriteDescriptorSet> descriptor_writes;
    std::vector<DescriptorUpdateData> descriptor_update_data;
    std::vector<DescriptorValue> descriptor_values;
    std::vector<uint8_t> descriptor_changed;
    std::vector<std::pair<uint32_t,uint32_t>> dynamic_bindings;
    std::vector<uint32_t> dynamic_offsets;
    PipelineKey descriptor_signature;
    VkShaderStageFlags current_stage=VK_SHADER_STAGE_ALL;
    VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkDescriptorPool pool{};VkDescriptorSet set{};VkPipeline pipeline{};
    std::shared_ptr<DescriptorSlot> cached_descriptors;
    bool preparation_cacheable=true;
    // Only called after the enclosing GPU fence, or on the synchronous path.
    // Retain vector capacity, but release every owned resource and descriptor.
    void reset(){if(!cached_descriptors){if(pool)vkDestroyDescriptorPool(device,pool,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,nullptr);}
        for(size_t i=0;i<texel_views.size();++i)release_texel_view({texel_views[i],texel_view_leases[i]});for(auto& b:buffers)destroy_buffer(b);
        cached_descriptors.reset();buffers.clear();views.clear();samps.clear();texel_views.clear();bindings.clear();buffer_infos.clear();image_infos.clear();writes.clear();sampled_images.clear();descriptor_writes.clear();
        dynamic_bindings.clear();dynamic_offsets.clear();texel_view_leases.clear();descriptor_update_data.clear();null_bindings.clear();integer_images.clear();dependencies.reset();
        set_layout={};layout={};pool={};set={};pipeline={};current_stage=VK_SHADER_STAGE_ALL;preparation_cacheable=true;
    }
    ~DrawResources(){reset();}
    void adopt(uint32_t binding,GpuBuffer b,VkDescriptorType type){buffers.push_back(b);bindings.push_back({binding,type,1,current_stage,nullptr});writes.push_back({binding,type,buffer_infos.size(),false});buffer_infos.push_back({b.buffer,b.offset,b.size});}
    void buffer(uint32_t binding,const void* data,size_t size,VkDescriptorType type,bool writeback=false,bool readonly=false,bool immutable=false) {
        if(!immutable)dependencies.watch(data,size);
        auto usage=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto b=writeback?writable_buffer(data,size,usage):(readonly?readonly_buffer(data,size,usage,type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?65536:16):make_buffer(data,size,usage,type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?65536:16));adopt(binding,b,type);
    }
    void image(const ShaderResource& ref,int stage,const Shader& shader,bool storage) {
        if(int(ref.array)!=1 || bool(ref.separate))ot2_host_die("Descriptor arrays/separate sampler resources pending");
        auto handle=packed_handle(ref,stage,shader),id=handle&0xfffff;auto* descriptor=texture_descriptor(id);
        if(!descriptor){
            if(!empty_descriptor(id,active_texture_capacity))ot2_host_die("Texture descriptor %u absent, stage %d handle=%x",id,stage,uint32_t(ref.handle));
            if(!null_descriptors){null_bindings.push_back(ref.binding);return;}
            if((ref.type&255)==2){
                auto type=storage?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                bindings.push_back({ref.binding,type,1,current_stage,nullptr});writes.push_back({ref.binding,type,texel_views.size(),2});texel_views.push_back(VK_NULL_HANDLE);texel_view_leases.push_back(nullptr);
            }else{
                auto type=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings.push_back({ref.binding,type,1,current_stage,nullptr});writes.push_back({ref.binding,type,image_infos.size(),true});
                image_infos.push_back({storage?VK_NULL_HANDLE:make_sampler(handle>>20,(ref.type&1024)!=0),VK_NULL_HANDLE,storage?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            }
            return;
        }
        auto& d=*descriptor;int target=d.has_view&&d.view.set_target?d.view.target:d.texture.d.target;
        static constexpr int types[]={1,3,4,257,259,515,771,3,5,261,2};
        // Maxwell's array-cube sampling instruction can address a single cube.
        // Vulkan requires the descriptor view to match the shader's array type:
        // expose the same six faces as one cube, retaining all other view state.
        const bool cube_as_array=!storage&&target==NVN_TEXTURE_TARGET_CUBEMAP&&(int(ref.type)&1023)==261;
        if(target<0 || target>10 || ((int(ref.type)&1023)!=types[target]&&!cube_as_array)){record_shader_facts(stage,stage_data(stage),true);ot2_host_die("Shader resource type mismatch: %s binding=%u expected=%u target=%d; facts saved",std::string(shader.reflection["key"]).c_str(),uint32_t(ref.binding),uint32_t(ref.type),target);}
        if(target==NVN_TEXTURE_TARGET_BUFFER) {
            const auto& t=d.texture;int fmt=d.has_view&&d.view.set_format?d.view.format:t.d.format;int bw,bh;int bytes=nvn_fmt_bpb(fmt,&bw,&bh);
            if(auto numeric=integer_texel_format(ref.binding,fmt))integer_images.push_back(*numeric);
            if(!bytes || bw!=1 || bh!=1 || !t.mem)ot2_host_die("Invalid texel buffer descriptor");size_t size=size_t(bytes)*t.d.width;
            size_t index=buffers.size();for(size_t i=0;i<buffers.size();i++)if(buffers[i].source==t.mem && buffers[i].size==size && (buffers[i].usage&VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)){index=i;break;}
            bool writable=storage&&(uint32_t(ref.flags)&4);auto usage=VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
            if(index==buffers.size())buffers.push_back(writable?writable_buffer(t.mem,size,usage):readonly_buffer(t.mem,size,usage));
            auto& b=buffers[index];if(writable&&!building_mirrors){b.writeback=t.mem;b.writeback_size=size;}
            VkBufferViewCreateInfo vi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};vi.buffer=b.buffer;vi.offset=b.offset;vi.format=format(fmt);vi.range=size;
            auto lease=acquire_texel_view(vi,b.arena||bool(b.persistent));
            uint32_t binding=ref.binding;auto type=storage?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            bindings.push_back({binding,type,1,current_stage,nullptr});writes.push_back({binding,type,texel_views.size(),2});texel_views.push_back(lease.view);texel_view_leases.push_back(lease.cached);return;
        }
        upload_packaged(d.texture);auto& img=image_for(d.texture);
        const int sampled_format=d.has_view&&d.view.set_format?d.view.format:d.texture.d.format;
        const bool stencil_plane=(img.aspects&VK_IMAGE_ASPECT_STENCIL_BIT)&&(d.has_view&&d.view.set_depth_mode?d.view.depth_mode:d.texture.d.depth_stencil_mode);
        if(stencil_plane)integer_images.push_back({uint32_t(ref.binding),0,spv::ImageFormatR8ui});
        else if(auto numeric=integer_texel_format(ref.binding,sampled_format))integer_images.push_back(*numeric);
        if(d.texture.d.pkg_block_h&&d.texture.mem)dependencies.watch(d.texture.mem,d.texture.L.size,img.source_tracked);
        if(storage&&img.bc)ot2_host_die("Compressed texture cannot be a storage image");
        if(d.texture.d.pkg_block_h&&d.texture.mem&&!img.source_tracked)preparation_cacheable=false;
        bool feedback=false;for(int i=0;i<render_targets.n;i++)feedback|=render_targets.color[i]&&attachment_identity(*render_targets.color[i])==img.identity;
        VkImageLayout required=storage||feedback||!dynamic_rendering||img.aspects!=VK_IMAGE_ASPECT_COLOR_BIT?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if(img.layout!=required){auto cmd=begin_commands();barrier(cmd,img,img.layout,required);finish_commands(cmd);}
        sampled_images.insert(img.identity);
        VkImageView view;
        if(cube_as_array){
            auto sampled=d.has_view?d.view:view_t{};sampled.set_target=1;sampled.target=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
            if(subresources(img,&sampled).layerCount!=6)ot2_host_die("Single cube sampled as array must expose six faces");
            // make_view caches by full type/range/format/swizzle. Do not replace
            // cached_view: another shader may still sample this descriptor as a cube.
            view=make_view(d.texture,&sampled);
        }else{
            if(!reuse_resource_objects||!d.cached_view)d.cached_view=make_view(d.texture,d.has_view?&d.view:nullptr);
            view=d.cached_view;
        }
        views.push_back(view);
        VkSampler sampler{};if(!storage){sampler=make_sampler(handle>>20,(ref.type&1024)!=0);samps.push_back(sampler);}
        auto type=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;uint32_t binding=ref.binding;
        bindings.push_back({binding,type,1,current_stage,nullptr});writes.push_back({binding,type,image_infos.size(),true});image_infos.push_back({sampler,view,required});
    }
    void stage(int stage,const Shader& shader,int group=-1) {
        current_stage=shader_stage(stage);
        if(precise_resource_dependencies){
            uint32_t mask=0;for(int g=0;g<4;g++)if(group<0||group==g)mask|=shader.resources->bank_masks[g];
            for(int bank=3;bank<19;bank++)if(mask&(1u<<bank)){auto& u=render_state.uniforms[stage][bank-3];dependencies.watch(reinterpret_cast<const void*>(u.addr),u.size);}
        }
        if(group<0||group==0)for(auto& r:shader.resources->cbufs){int slot=r.slot;
            if(slot>=3&&slot<19){const auto& u=render_state.uniforms[stage][slot-3];if(u.size>65536||(!u.addr&&u.size))ot2_host_die("Invalid uniform range");
                // Diagnostic: the first constant bank carries a projection and then
                // a second 4x4 that differs on every draw, which is the shape of a
                // per-element transform. Replacing that second block with "map 0..1
                // to clip space" answers whether the vertex stage reads it at all:
                // the matrix is symmetric under row- and column-major reading, so a
                // frame that fills is unambiguous. Written as an affine matrix so
                // w ends at 1 either way, which the title's own block does not.
                if(ui_matrix_probe&&stage==0&&u.addr&&u.size>=128){
                    static std::vector<uint8_t> probe_bank(65536);
                    const size_t n=std::min<size_t>(u.size,probe_bank.size());
                    memcpy(probe_bank.data(),reinterpret_cast<const void*>(u.addr),n);
                    auto* f=reinterpret_cast<float*>(probe_bank.data());
                    const size_t count=n/4;
                    if(count>=32){
                        const float m[16]={2,0,0,-1, 0,2,0,-1, 0,0,1,0, 0,0,0,1};
                        for(int k=0;k<16;k++)f[16+k]=m[k];
                        static std::atomic<unsigned> reported{0};
                        if(reported.fetch_add(1,std::memory_order_relaxed)<64)
                            ot2_host_log("UI_MATRIX_PROBE slot=%d size=%zu",slot,n);
                    }
                }
                // Diagnostic: a 2D sprite program places a unit quad through a
                // model matrix whose scale is the sprite's size in pixels. Scaling
                // that matrix here tells the two possible causes apart -- the quad
                // is too small for the screen, or the draw never reaches the
                // rasteriser at all -- without touching the title's memory.
                if(sprite_pixels_forced&&stage==0&&u.addr&&u.size>=112&&sprite_scale_probe){
                    static std::vector<uint8_t> probe_bank(65536);
                    const size_t n=std::min<size_t>(u.size,probe_bank.size());
                    memcpy(probe_bank.data(),reinterpret_cast<const void*>(u.addr),n);
                    auto* f=reinterpret_cast<float*>(probe_bank.data());
                    for(int k:{16,21,26})f[k]*=sprite_scale_probe;
                    buffer(r.binding,probe_bank.data(),n,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,false,true,true);
                    continue;
                }
                // A title binds only the bytes it considers live, and the shader
                // declares a fixed struct. On the console the slot is one physical
                // driver segment, so a read past the bound range finds whatever the
                // title left there -- and a title does leave data there: the bank at
                // 912 bytes carries a rotation and translation starting exactly at
                // 912. Here the range is a descriptor limit, so those reads return
                // nothing. Extending the range to the end of the containing page
                // gives the shader what the console would, without ever crossing
                // into memory the title has not mapped.
                constexpr size_t uniform_page=4096;
                const size_t page_end=(size_t(u.addr)+uniform_page)&~(uniform_page-1);
                const size_t reach=std::min<size_t>(uniform_page,page_end-size_t(u.addr));
                if(u.addr&&reach>u.size){
                    static thread_local std::vector<uint8_t> padded;
                    padded.assign(reach,0);
                    const auto* source=reinterpret_cast<const uint8_t*>(u.addr);
                    memcpy(padded.data(),source,u.size);
                    if(uniform_tail_from_memory)memcpy(padded.data()+u.size,source+u.size,reach-u.size);
                    buffer(r.binding,padded.data(),padded.size(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,false,true);
                    static std::atomic<unsigned> reported{0};
                    if(reported.fetch_add(1,std::memory_order_relaxed)<8)
                        ot2_host_log("UNIFORM_EXTENDED slot=%d declared=%zu bound=%zu tail=%s",slot,u.size,reach,
                            uniform_tail_from_memory?"memory":"zero");
                }
                else buffer(r.binding,reinterpret_cast<const void*>(u.addr),u.size,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,false,true);}
            else if(slot==1){buffer(r.binding,shader.c1.data(),shader.c1.size(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,false,true,true);}
            else {adopt(r.binding,cached_bank(stage,slot,shader),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);}}
        if((group<0||group==0)&&shader.needs_support_buffer&&
           std::none_of(bindings.begin(),bindings.end(),[](const auto& b){return b.binding==0;})){
            // Filled once: the platform fields do not depend on the draw.
            static const std::vector<uint8_t> bytes=support_buffer_bytes();
            buffer(0,bytes.data(),bytes.size(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,false,true,true);
        }
        if(group<0||group==1)for(auto& r:shader.resources->sbufs){auto b=storage_binding(stage,r,shader);
            // What NVN calls a storage table can come out of the translator as a
            // uniform block; the descriptor has to match the declaration or the
            // shader reads a descriptor of the wrong kind and every value it loads
            // is undefined, however correct the bytes behind it are.
            auto declared=shader.binding_storage.find(r.binding);
            const bool as_uniform=declared!=shader.binding_storage.end()&&declared->second==2u;
            const VkDescriptorType type=as_uniform?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if(as_uniform){static std::atomic<unsigned> reported{0};
                if(reported.fetch_add(1,std::memory_order_relaxed)<48)
                    ot2_host_log("SBUF_AS_UNIFORM binding=%u size=%u (translator emitted a uniform block)",r.binding,b.size);}
            if(!b.address&&!b.size){
                if(!null_descriptors){null_bindings.push_back(r.binding);continue;}
                bindings.push_back({r.binding,type,1,current_stage,nullptr});writes.push_back({r.binding,type,buffer_infos.size(),false});buffer_infos.push_back({VK_NULL_HANDLE,0,VK_WHOLE_SIZE});continue;
            }
            if(!b.address || !b.size)ot2_host_die("Storage buffer is not bound");buffer(r.binding,reinterpret_cast<const void*>(b.address),b.size,type,(uint32_t(r.flags)&1)!=0,(uint32_t(r.flags)&1)==0);}
        if(group<0||group==2)for(auto& r:shader.resources->textures)image(r,stage,shader,false);if(group<0||group==3)for(auto& r:shader.resources->images)image(r,stage,shader,true);
    }
    void finish() {
        current_stage=VK_SHADER_STAGE_ALL;
        adopt(0,cached_support(),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        if(dynamic_buffer_descriptors){
            uint32_t uniforms=0,storage=0;
            for(size_t i=0;i<writes.size();++i){auto& w=writes[i];
                if(w.kind!=0)continue;auto& info=buffer_infos[w.index];
                // Null/whole-size resources and non-representable offsets keep
                // static descriptors. Arena offsets satisfy both alignments.
                if(!info.buffer||info.range==VK_WHOLE_SIZE||info.offset>UINT32_MAX)continue;
                if(w.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&uniforms<dynamic_uniform_limit){w.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;++uniforms;}
                else if(w.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER&&storage<dynamic_storage_limit){w.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;++storage;}
                else continue;
                bindings[i].descriptorType=w.type;
                dynamic_bindings.emplace_back(w.binding,uint32_t(info.offset));info.offset=0;
            }
            std::sort(dynamic_bindings.begin(),dynamic_bindings.end());
            for(auto [binding,offset]:dynamic_bindings)dynamic_offsets.push_back(offset);
        }
        PipelineKey temporary_signature;
        auto& signature=reuse_descriptor_signature?descriptor_signature:temporary_signature;
        signature.data.clear();signature.bindings(bindings);
        const auto layout_key_bytes=signature.data.size();
        for(const auto& w:writes){
            if(packed_descriptor_keys){
                if(w.kind==2)append_key_fields(signature.data,w.binding,w.type,w.kind,texel_views[w.index]);
                else if(w.kind==1){const auto& v=image_infos[w.index];append_key_fields(signature.data,w.binding,w.type,w.kind,v.imageView,v.sampler,v.imageLayout);}
                else {const auto& v=buffer_infos[w.index];append_key_fields(signature.data,w.binding,w.type,w.kind,v.buffer,v.offset,v.range);}
            }else {signature.add(w.binding);signature.add(w.type);signature.add(w.kind);if(w.kind==2)signature.add(texel_views[w.index]);else if(w.kind==1){auto& v=image_infos[w.index];signature.add(v.imageView);signature.add(v.sampler);signature.add(v.imageLayout);}else {auto& v=buffer_infos[w.index];signature.add(v.buffer);signature.add(v.offset);signature.add(v.range);}}
        }
        if(batching&&reuse_descriptors&&reuse_bindings)if(auto found=bound_descriptor_cache.find(signature.data);found!=bound_descriptor_cache.end()){
            cached_descriptors=found->second;set_layout=cached_descriptors->owner->set_layout;layout=cached_descriptors->owner->pipeline_layout;set=cached_descriptors->set;++binding_cache_hits;return;
        }
        bool unchanged_contents=false;
        VkDescriptorUpdateTemplate update_template{};
        // Slot arenas live across recordings. Resource slabs also qualify:
        // their last owner invalidates idle contents/deltas before destroying
        // the VkBuffer, while this draw retains its slice through the fence.
        // Slice bytes/owners may change without changing a valid descriptor.
        // Cached texel views have independent fence leases; eviction invalidates
        // descriptor contents/deltas before destroying the handle. Uncached
        // per-draw views and other standalone buffers remain excluded.
        bool stable_contents=batching&&(texel_views.empty()||retain_texel_descriptors);
        for(size_t i=0;i<texel_views.size()&&stable_contents;++i)stable_contents&=!texel_views[i]||texel_view_leases[i];
        for(const auto& b:buffers)stable_contents&=b.arena||b.descriptor_stable;
        if(reuse_descriptors){
            cached_descriptors={acquire_descriptors(bindings,stable_contents?&signature.data:nullptr,&unchanged_contents,std::string_view(signature.data.data(),layout_key_bytes),&update_template),release_descriptors};
            set_layout=cached_descriptors->owner->set_layout;
            layout=cached_descriptors->owner->pipeline_layout;
            set=cached_descriptors->set;
        }else{
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();check(vkCreateDescriptorSetLayout(device,&li,nullptr,&set_layout),"descriptor layout");
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&set_layout;check(vkCreatePipelineLayout(device,&pi,nullptr,&layout),"pipeline layout");
        std::map<VkDescriptorType,uint32_t> counts;for(auto& b:bindings)counts[b.descriptorType]++;std::vector<VkDescriptorPoolSize> sizes;for(auto& [type,n]:counts)sizes.push_back({type,n});
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pci.maxSets=1;pci.poolSizeCount=sizes.size();pci.pPoolSizes=sizes.data();check(vkCreateDescriptorPool(device,&pci,nullptr,&pool),"descriptor pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&set_layout;check(vkAllocateDescriptorSets(device,&ai,&set),"descriptor allocation");
        }
        if(!unchanged_contents){
            descriptor_values.clear();descriptor_changed.clear();size_t changed=0;
            const bool track_values=incremental_descriptors&&retain_descriptor_contents&&stable_contents&&cached_descriptors;
            const bool delta=track_values&&cached_descriptors->values_epoch==descriptor_contents_epoch&&cached_descriptors->values_linear==linear_descriptor_delta;
            if(track_values)for(const auto& w:writes){
                DescriptorValue value;value.binding=w.binding;value.type=w.type;value.kind=w.kind;
                if(w.kind==2)value.texel=texel_views[w.index];else if(w.kind==1)value.image=image_infos[w.index];else value.buffer=buffer_infos[w.index];
                bool same=false;if(delta){const auto& old=cached_descriptors->values;
                    // A slot usually returns to the same ordered layout. A
                    // permutation safely rewrites mismatched positions: full
                    // equality includes binding/type/kind, not only payload.
                    // Thus no sort or binary search is needed on this path.
                    if(linear_descriptor_delta){const auto i=descriptor_values.size();same=i<old.size()&&old[i]==value;}
                    else {auto it=std::lower_bound(old.begin(),old.end(),value.binding,[](const auto& a,uint32_t binding){return a.binding<binding;});same=it!=old.end()&&*it==value;}}
                descriptor_values.push_back(value);descriptor_changed.push_back(!same);changed+=!same;
            }
            if(!track_values)changed=writes.size();
            descriptor_delta_written+=changed;descriptor_delta_skipped+=writes.size()-changed;
            if(update_template&&changed==writes.size()){
                descriptor_update_data.resize(writes.size());
                for(size_t i=0;i<writes.size();++i){const auto& w=writes[i];auto& data=descriptor_update_data[i];
                    if(w.kind==2)data.texel=texel_views[w.index];else if(w.kind==1)data.image=image_infos[w.index];else data.buffer=buffer_infos[w.index];
                }
                vkUpdateDescriptorSetWithTemplate(device,set,update_template,descriptor_update_data.data());++descriptor_template_updates;
            }else{
            auto& ws=descriptor_writes;ws.clear();for(size_t i=0;i<writes.size();++i){if(track_values&&!descriptor_changed[i])continue;auto& w=writes[i];VkWriteDescriptorSet v{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};v.dstSet=set;v.dstBinding=w.binding;v.descriptorCount=1;v.descriptorType=w.type;if(w.kind==2)v.pTexelBufferView=&texel_views[w.index];else if(w.kind==1)v.pImageInfo=&image_infos[w.index];else v.pBufferInfo=&buffer_infos[w.index];ws.push_back(v);}if(!ws.empty())vkUpdateDescriptorSets(device,ws.size(),ws.data(),0,nullptr);
            }
            if(changed)++descriptor_set_updates;
            if(cached_descriptors){
                if(track_values){if(!linear_descriptor_delta)std::sort(descriptor_values.begin(),descriptor_values.end(),[](const auto& a,const auto& b){return a.binding<b.binding;});cached_descriptors->values=descriptor_values;cached_descriptors->values_epoch=descriptor_contents_epoch;cached_descriptors->values_linear=linear_descriptor_delta;}
                else {cached_descriptors->values.clear();cached_descriptors->values_epoch=0;}
            }
            if(stable_contents&&cached_descriptors&&retain_descriptor_contents){cached_descriptors->contents=signature.data;cached_descriptors->contents_epoch=descriptor_contents_epoch;}
        }
        if(batching&&reuse_descriptors&&reuse_bindings&&texel_views.empty())bound_descriptor_cache.emplace(signature.data,cached_descriptors);
    }
};
// Four independent groups per shader stage. A new UBO can invalidate handle
// indirection too; caller versions deliberately account for that dependency.
// The cache owns only arena-backed, read-only resources of this recording.
bool grouped_resources=true;
extern bool reuse_resource_preparation;
uint64_t group_versions[6][4]{},group_epoch=1,group_hits=0,group_misses=0;
struct PreparedGroup {uint64_t epoch{},version{},dirty{};const Shader* shader{};std::unique_ptr<DrawResources> value;};
std::array<std::array<PreparedGroup,4>,6> prepared_groups;
void invalidate_resource_groups(){++group_epoch;for(auto& s:prepared_groups)for(auto& g:s)if(g.value)g.value->reset();}
void change_resource_groups(int stage,unsigned mask){for(int g=0;g<4;++g)if(mask&(1u<<g))++group_versions[stage][g];}
void change_resource_bank(int stage,int bank){
    auto* shader=bound_shaders[stage];if(!shader){change_resource_groups(stage,15);return;}
    for(int group=0;group<4;++group)if(shader->resources->bank_masks[group]&(1u<<bank))++group_versions[stage][group];
}
void append_group(DrawResources& to,const DrawResources& from){
    to.dependencies.append(from.dependencies);
    to.integer_images.insert(to.integer_images.end(),from.integer_images.begin(),from.integer_images.end());
    const size_t bi=to.buffer_infos.size(),ii=to.image_infos.size();
    to.buffers.insert(to.buffers.end(),from.buffers.begin(),from.buffers.end());
    to.null_bindings.insert(to.null_bindings.end(),from.null_bindings.begin(),from.null_bindings.end());
    to.bindings.insert(to.bindings.end(),from.bindings.begin(),from.bindings.end());
    to.buffer_infos.insert(to.buffer_infos.end(),from.buffer_infos.begin(),from.buffer_infos.end());
    to.image_infos.insert(to.image_infos.end(),from.image_infos.begin(),from.image_infos.end());
    for(auto w:from.writes){w.index+=w.kind?ii:bi;to.writes.push_back(w);}
    for(auto serial:from.sampled_images.values)to.sampled_images.insert(serial);
    to.preparation_cacheable&=from.preparation_cacheable;
}
void prepare_stage_groups(DrawResources& out,int stage,const Shader& shader){
    if(!grouped_resources||!batching||!reuse_resource_preparation||!reuse_resource_objects||shader.resources->side_effects){out.stage(stage,shader);return;}
    for(int g=0;g<4;++g){
        const auto& refs=g==0?shader.resources->cbufs:g==1?shader.resources->sbufs:g==2?shader.resources->textures:shader.resources->images;
        if(refs.empty())continue;
        auto& cached=prepared_groups[stage][g];
        // Texel buffers carry per-draw view leases. They cannot use this cache;
        // avoid preparing and copying a temporary group just to reject it.
        if(g>=2&&std::any_of(refs.begin(),refs.end(),[](const auto& r){return (r.type&255)==2;})){out.stage(stage,shader,g);continue;}
        // Capture before checking ranges. A concurrent later write must remain
        // newer than this stamp instead of being swallowed by a second read.
        const auto observed_dirty=nvn_dirty_now();
        if(cached.value&&cached.epoch==group_epoch&&cached.version==group_versions[stage][g]&&cached.shader==&shader&&(cached.dirty==observed_dirty||cached.value->dependencies.unchanged(cached.dirty))){
            cached.dirty=observed_dirty;cached.value->dependencies.stamp=observed_dirty;append_group(out,*cached.value);++group_hits;continue;
        }
        ++group_misses;if(!cached.value)cached.value=std::make_unique<DrawResources>();
        auto* value=cached.value.get();value->reset();value->stage(stage,shader,g);
        bool eligible=value->preparation_cacheable&&value->texel_views.empty();for(const auto& b:value->buffers)eligible&=(b.arena||bool(b.persistent))&&!b.writeback;
        if(!eligible){
            // Texel leases and standalone resources have their own retirement
            // rules. Build directly into the owning draw on this fallback.
            // Move the vectors so no resources are destroyed before execution.
            size_t bi=out.buffer_infos.size(),ii=out.image_infos.size(),ti=out.texel_views.size();
            for(auto w:value->writes){w.index+=w.kind==2?ti:w.kind==1?ii:bi;out.writes.push_back(w);}
            auto move_append=[](auto& a,auto& b){a.insert(a.end(),std::make_move_iterator(b.begin()),std::make_move_iterator(b.end()));b.clear();};
            move_append(out.buffers,value->buffers);move_append(out.bindings,value->bindings);move_append(out.buffer_infos,value->buffer_infos);move_append(out.image_infos,value->image_infos);
            move_append(out.null_bindings,value->null_bindings);move_append(out.texel_views,value->texel_views);move_append(out.texel_view_leases,value->texel_view_leases);
            move_append(out.integer_images,value->integer_images);
            for(auto serial:value->sampled_images.values)out.sampled_images.insert(serial);
            out.dependencies.append(value->dependencies);out.preparation_cacheable&=value->preparation_cacheable;value->reset();cached.epoch=0;continue;
        }
        cached.epoch=group_epoch;cached.version=group_versions[stage][g];cached.dirty=precise_resource_dependencies?value->dependencies.stamp:nvn_dirty_now();cached.shader=&shader;append_group(out,*value);
    }
}
bool reuse_draw_objects=true;
uint64_t draw_object_creates=0,draw_object_reuses=0;
std::vector<std::unique_ptr<DrawResources>> idle_draw_objects;
std::unique_ptr<DrawResources> acquire_draw_resources(){
    if(reuse_draw_objects&&!idle_draw_objects.empty()){auto r=std::move(idle_draw_objects.back());idle_draw_objects.pop_back();++draw_object_reuses;return r;}
    ++draw_object_creates;return std::make_unique<DrawResources>();
}
void recycle_draw_resources(DrawResources* r){
    r->reset();if(reuse_draw_objects&&idle_draw_objects.size()<8192)idle_draw_objects.emplace_back(r);else delete r;
}
// Only immutable descriptors backed by this recording slot's arenas may be
// reused. Slot flush clears the snapshot before an arena can be recycled.
// Every draw still owns a descriptor lease until its real completion fence.
struct PreparedBindings {
    ResourceDependencies dependencies;
    uint64_t generation{},dirty{};bool compute{};
    std::shared_ptr<DescriptorSlot> descriptors;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<uint32_t> dynamic_offsets;
    std::vector<uint32_t> null_bindings;
    std::vector<ShaderIntegerImage> integer_images;
    ImageSerialSet sampled;
} prepared_bindings;
bool reuse_resource_preparation=true;
uint64_t resource_preparation_hits=0,resource_preparation_misses=0;
void reset_prepared_bindings(){invalidate_resource_groups();++resource_generation;prepared_bindings.descriptors.reset();}
bool restore_prepared_bindings(DrawResources& r,bool compute){
    auto& c=prepared_bindings;
    const auto observed_dirty=nvn_dirty_now();
    if(!batching||!reuse_resource_preparation||!reuse_bindings||!reuse_descriptors||!c.descriptors||c.generation!=resource_generation||c.compute!=compute||(c.dirty!=observed_dirty&&!c.dependencies.unchanged(c.dirty)))return false;
    c.dirty=observed_dirty;c.dependencies.stamp=observed_dirty;r.dependencies.append(c.dependencies);
    r.cached_descriptors=c.descriptors;r.set_layout=c.descriptors->owner->set_layout;r.layout=c.descriptors->owner->pipeline_layout;r.set=c.descriptors->set;
    r.bindings=c.bindings;r.dynamic_offsets=c.dynamic_offsets;r.null_bindings=c.null_bindings;r.integer_images=c.integer_images;r.sampled_images=c.sampled;++resource_preparation_hits;++binding_cache_hits;return true;
}
void remember_prepared_bindings(const DrawResources& r,bool compute,bool side_effects){
    ++resource_preparation_misses;prepared_bindings.descriptors.reset();
    if(!batching||!reuse_resource_preparation||!r.cached_descriptors||!r.preparation_cacheable||side_effects||!r.texel_views.empty())return;
    for(const auto& b:r.buffers)if(!b.arena||b.writeback)return;
    prepared_bindings.dependencies=r.dependencies;prepared_bindings.generation=resource_generation;prepared_bindings.dirty=precise_resource_dependencies?r.dependencies.stamp:nvn_dirty_now();prepared_bindings.compute=compute;
    prepared_bindings.descriptors=r.cached_descriptors;prepared_bindings.bindings=r.bindings;prepared_bindings.dynamic_offsets=r.dynamic_offsets;prepared_bindings.null_bindings=r.null_bindings;prepared_bindings.sampled=r.sampled_images;
    prepared_bindings.integer_images=r.integer_images;
}
bool update_binding_bytes(void* dst,const void* src,size_t bytes){
    if(!memcmp(dst,src,bytes))return false;memcpy(dst,src,bytes);++resource_generation;return true;
}
VkBlendFactor blend_factor(unsigned f) {switch(f){case 0:case 1:return VK_BLEND_FACTOR_ZERO;case 2:return VK_BLEND_FACTOR_ONE;case 3:return VK_BLEND_FACTOR_SRC_COLOR;case 4:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;case 5:return VK_BLEND_FACTOR_SRC_ALPHA;case 6:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_DST_ALPHA;case 8:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;case 9:return VK_BLEND_FACTOR_DST_COLOR;case 10:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;case 11:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;case 16:return VK_BLEND_FACTOR_SRC1_COLOR;case 17:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;case 18:return VK_BLEND_FACTOR_SRC1_ALPHA;case 19:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;case 0x61:return VK_BLEND_FACTOR_CONSTANT_COLOR;case 0x62:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;case 0x63:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case 0x64:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;default:ot2_host_die("Unknown blend factor %u",f);}}
VkPrimitiveTopology topology(int p) {switch(p){case 0:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 1:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case 6:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;default:ot2_host_die("Primitive %d pending",p);}}
uint64_t encoded_draws=0;
bool synchronous_capacity_flush=false;
bool precise_draw_dependencies=true;
bool reflected_preflight_banks=true;
uint64_t preflight_uniform_checks=0;
bool reuse_pipeline_preparation=true;
uint64_t pipeline_preparation_hits=0,pipeline_preparation_misses=0;
bool versioned_pipeline_state=true;
uint64_t graphics_state_revision=1,pipeline_version_hits=0;
bool update_pipeline_bytes(void* dst,const void* src,size_t size){
    if(!memcmp(dst,src,size))return false;memcpy(dst,src,size);++graphics_state_revision;return true;
}
template<class T>void update_pipeline_value(T& dst,const T& value){update_pipeline_bytes(&dst,&value,sizeof(T));}
struct PreparedGraphicsPipeline {
    VkPipeline pipeline{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::array<VkPipelineColorBlendAttachmentState,8> blend{};
};
std::unordered_map<FixedKey<768>,PreparedGraphicsPipeline,FixedKey<768>::Hash> prepared_graphics_pipelines;
FixedKey<128> last_pipeline_version_key;
PreparedGraphicsPipeline last_version_pipeline;
FixedKey<768> graphics_preparation_key(int primitive,VkPipelineLayout layout,const VkFormat* colors,VkFormat depth,uint32_t samples,std::span<VkPipelineShaderStageCreateInfo> stages){
    FixedKey<768> key;key.add(primitive);key.add(layout);key.add(render_targets.n);
    for(int i=0;i<render_targets.n;i++)key.add(colors[i]);key.add(depth);key.add(samples);
    for(int i=0;i<5;i++){VkShaderModule module{};for(auto& stage:stages)if(stage.stage==shader_stage(i))module=stage.module;key.add(module);}
    key.add(render_state.attributes);key.add(render_state.streams);key.add(render_state.blends);
    key.add(render_state.color);key.add(render_state.channel_mask);key.add(render_state.multisample);
    key.add(render_state.polygon);key.add(render_state.depth_stencil);key.add(dynamic_depth_bounds);
    key.add(render_state.depth_bounds.enable);
    if(!dynamic_depth_bounds){key.add(render_state.depth_bounds.minimum);key.add(render_state.depth_bounds.maximum);}
    key.add(render_state.sample_mask);key.add(render_state.raster_discard);key.add(render_state.depth_clamp);
    key.add(render_state.line_width);key.add(render_state.depth_bias);key.add(render_state.blend_color);
    key.add(render_state.stencil_reference);key.add(render_state.stencil_compare_mask);key.add(render_state.stencil_write_mask);
    return key;
}

// Preflight all CPU reads and overlapping GPU buffer uses before allocating any
// resources for this draw. A fallback flush must never reset an arena already
// borrowed by a half-built draw. Unsupported partial aliases take the existing
// synchronous route; exact/contained aliases remain shared on the GPU.
void prepare_buffer_mirrors(std::span<const std::pair<int,Shader*>> stages,uint32_t operation,const uint8_t* parameters){
    struct Use{uintptr_t address;size_t size,minimum;bool write,cpu;};static std::vector<Use> uses;uses.clear();
    auto add=[&](uintptr_t address,size_t size,size_t minimum,bool write,bool cpu=false){if(address&&size)uses.push_back({address,size,minimum,write,cpu});};
    for(auto [stage,shader]:stages){
        // Reflection includes both shader constants and banks read by the host
        // to decode indirect texture/storage handles. Bound but unused banks
        // cannot create a CPU-read hazard for this draw.
        const auto* masks=shader->resources->bank_masks;
        uint32_t banks=reflected_preflight_banks?((masks[0]|masks[1]|masks[2]|masks[3])>>3):0xffffu;
        while(banks){const auto slot=std::countr_zero(banks);banks&=banks-1;
            const auto& u=render_state.uniforms[stage][slot];if(u.addr){++preflight_uniform_checks;ensure_cpu_buffer(u.addr,u.size);add(u.addr,u.size,0,false,true);}}
        for(auto& r:shader->resources->sbufs){auto b=storage_binding(stage,r,*shader);add(b.address,b.size,16,r.flags&1);}
        for(auto& r:shader->resources->cbufs)if(r.slot>=3){auto& u=render_state.uniforms[stage][r.slot-3];add(u.addr,u.size,65536,false,true);}
        for(int image=0;image<2;image++)for(auto& r:image?shader->resources->images:shader->resources->textures){auto handle=packed_handle(r,stage,*shader);auto* descriptor=texture_descriptor(handle&0xfffff);if(!descriptor)continue;auto& t=descriptor->texture;
            if(t.d.target==NVN_TEXTURE_TARGET_BUFFER){int bw,bh;add(uintptr_t(t.mem),size_t(nvn_fmt_bpb(t.d.format,&bw,&bh))*t.d.width,0,image&&(r.flags&4));}else if(t.mem){ensure_cpu_buffer(uintptr_t(t.mem),t.L.size);add(uintptr_t(t.mem),t.L.size,0,false,true);}}
        if(stage==0)for(auto location:shader->inputs){if(location>=16)ot2_host_die("Vertex attribute exceeds host ABI");auto& attr=render_state.attributes[location];if(!attr.fmt)continue;if(attr.stream>=16)ot2_host_die("Vertex stream exceeds host ABI");auto& b=render_state.vertex_buffers[attr.stream];add(b.address,b.size,0,false);}
    }
    if(operation==OP_DRAW_IDX){auto& p=*reinterpret_cast<const p_draw_idx*>(parameters);auto size=size_t(p.count)*(p.itype==0?1:p.itype==1?2:4);ensure_cpu_buffer(p.ibuf,size);add(p.ibuf,size,0,false,true);}
    for(const auto& u:uses)if(previous_cpu_overlap(u.address,u.size)){flush_commands();break;}
    bool compatible=buffer_mirrors_enabled&&batching;
    for(size_t i=0;i<uses.size();i++)if(uses[i].write)for(size_t j=0;j<uses.size();j++)if(i!=j&&overlap(uses[i].address,uses[i].size,uses[j].address,uses[j].size)){
        if(uses[j].cpu||uses[i].address!=uses[j].address||uses[i].size<std::max(uses[j].size,uses[j].minimum))compatible=false;
    }
    bool flush=!compatible&&!buffer_mirrors.empty();
    for(const auto& u:uses)for(const auto& [base,m]:buffer_mirrors)if(overlap(u.address,u.size,base,m.bytes)){
        auto need=std::max(u.size,u.minimum);if(!find_buffer_mirror(u.address,need)||(u.address-base)%upload_alignment)flush=true;
    }
    if(flush)flush_commands();
    building_mirrors=compatible;
    if(compatible)for(const auto& u:uses)if(u.write)writable_buffer(reinterpret_cast<void*>(u.address),u.size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}
#include "pass_plan.inc"
void execute_draw(uint32_t operation,const uint8_t* parameters) {
    if(operation!=OP_DISPATCH && !render_state.render_enable)return;
    // Conditional render: compare the two sampled counters the title bracketed its
    // occlusion test with (NVNcounterData::counter at addr and addr+16).
    //
    // The comparison is deliberately conservative. The second sample is written by
    // the GPU some frames later, so at draw time the pair can still read as equal
    // because neither has been filled yet -- and treating that as "nothing passed"
    // would hide content that is actually visible. Only a decided answer skips the
    // draw; an undecided one draws, which costs overdraw instead of geometry that
    // disappears. A title whose counters never arrive therefore renders more than
    // it should, never less.
    if(operation!=OP_DISPATCH && render_state.render_cond_mode>=0) {
        if(!render_state.render_cond_addr) return;      // no storage named: nothing can pass
        const auto* cond=reinterpret_cast<const uint64_t*>(render_state.render_cond_addr);
        const uint64_t before=cond[0],after=cond[2];    // counter fields, stride sizeof(NVNcounterData)
        static std::atomic<unsigned> reported{0};
        if(!before&&!after) {
            ++undecided_conditional_draws;
            if(reported.fetch_add(1,std::memory_order_relaxed)<8)
                ot2_host_log("RENDER_COND undecided at %#llx (both counters zero); drawing",
                             (unsigned long long)render_state.render_cond_addr);
        } else {
            const bool equal=before==after;
            const bool render=render_state.render_cond_mode==0?equal:!equal;
            ++decided_conditional_draws;
            if(!render){++skipped_conditional_draws;return;}
        }
    }
    if(operation==OP_DRAW_IDX){auto& p=*reinterpret_cast<const p_draw_idx*>(parameters);if(p.count<0||p.itype<0||p.itype>2||(p.count&&!p.ibuf))ot2_host_die("Invalid indexed draw range");}
    // No shader invocation or query sample can result from an empty launch.
    // Do not skip non-empty draws based solely on their color/depth output.
    if(operation==OP_DRAW){const auto& d=*reinterpret_cast<const p_draw*>(parameters);if(!d.count||!d.instances){++empty_draws;return;}}
    if(operation==OP_DRAW_IDX){const auto& d=*reinterpret_cast<const p_draw_idx*>(parameters);if(!d.count||!d.instances){++empty_draws;return;}}
    if(operation==OP_DISPATCH){const auto& d=*reinterpret_cast<const p_dispatch*>(parameters);if(!d.x||!d.y||!d.z){++empty_draws;return;}}
    const bool diagnose=trace_draw_completion&&presented_frames.load()>=900;
    if(diagnose){
        flush_commands();
        ot2_host_log("DRAW_COMPLETION begin=%llu frame=%llu operation=%u",(unsigned long long)(encoded_draws+1),(unsigned long long)presented_frames.load(),operation);
        for(int stage=0;stage<6;stage++)if(bound_programs[stage]&&bound_programs[stage]->stages[stage]>=0&&((operation==OP_DISPATCH)==(stage==5)))
            ot2_host_log("DRAW_SHADER stage=%d key=%s",stage,bound_programs[stage]->keys[bound_programs[stage]->stages[stage]].c_str());
    }
    PerfScope timing(perf.draw_ns);
    // The preflight below resolves each CPU-read and GPU-buffer range before
    // any snapshot is built, including the uniform banks holding descriptors.
    // Unrelated storage writes in older batches are not a dependency.
    if(!precise_draw_dependencies)ensure_previous_cpu_writes();
    bool side_effects=false;std::array<std::pair<int,Shader*>,6> shader_stages;size_t stage_count=0;
    for(int stage=0;stage<6;stage++)if(render_state.programs[stage]&&((operation==OP_DISPATCH)==(stage==5))){
        Shader& sh=stage_shader(stage);
        if(sh.missing){
            static std::atomic<unsigned> reported{0};++skipped_missing_draws;
            if(reported.fetch_add(1,std::memory_order_relaxed)<24)
                ot2_host_log("DRAW_SKIPPED stage=%d operation=%u (shader not translated)",stage,operation);
            return;
        }
        shader_stages[stage_count++]={stage,&sh};
    }
    const bool emulate_depth=operation!=OP_DISPATCH&&!depth_bounds_supported&&render_state.depth_bounds.enable;
    bool emulate_samples=operation!=OP_DISPATCH&&samples_active&&geometry_query_workaround&&render_state.programs[2];
    bool check_sample_shape=emulate_samples;
#ifdef OT2_NVN_TESTING
    check_sample_shape|=operation!=OP_DISPATCH&&samples_active&&force_software_samples;
#endif
    if(check_sample_shape){
        bool sample_shape=!render_state.depth_stencil.dt&&!render_state.depth_stencil.st&&!render_state.depth_bounds.enable&&!render_state.multisample.a2c&&render_state.sample_mask==~0u;
        for(int i=0;i<render_targets.n;i++)if(render_targets.color[i])sample_shape&=render_targets.color[i]->d.samples<=1;
        if(render_targets.depth)sample_shape&=render_targets.depth->d.samples<=1;
        if(emulate_samples&&!sample_shape)ot2_host_die("Geometry occlusion fallback requires single-sample drawing without depth/stencil tests");
#ifdef OT2_NVN_TESTING
        emulate_samples|=force_software_samples&&sample_shape;
#endif
    }
    if(emulate_depth){
        if(!render_targets.depth||render_state.depth_stencil.dw||render_state.depth_stencil.st)ot2_host_die("Software depth bounds requires read-only depth and no stencil side effects");
        materialize_clears(render_targets.depth->serial);
    }
    prepare_buffer_mirrors({shader_stages.data(),stage_count},operation,parameters);
    if(!encoded_draws||(diagnose&&operation!=OP_DISPATCH&&render_targets.n&&render_targets.color[0]&&render_targets.color[0]->d.target==NVN_TEXTURE_TARGET_3D))capture_draw(operation,parameters);
    auto owned=acquire_draw_resources();auto& resources=*owned;std::array<VkPipelineShaderStageCreateInfo,6> stage_infos;size_t stage_index=0;Shader* vertex=nullptr;
    const bool reused_resources=!emulate_depth&&!emulate_samples&&restore_prepared_bindings(resources,operation==OP_DISPATCH);
    for(int stage=0;stage<6;stage++)if(render_state.programs[stage] && ((operation==OP_DISPATCH)==(stage==5))) {
        auto& shader=stage_shader(stage);if(!shader.facts_recorded){record_shader_facts(stage,stage_data(stage));shader.facts_recorded=true;}if(!reused_resources)prepare_stage_groups(resources,stage,shader);side_effects|=shader.resources->side_effects;if(stage==0)vertex=&shader;
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};s.stage=shader_stage(stage);s.module=shader.module;s.pName="main";stage_infos[stage_index++]=s;
    }
    std::span<VkPipelineShaderStageCreateInfo> stages{stage_infos.data(),stage_index};
    for(size_t i=0;i<stages.size();i++)stages[i].module=integer_texel_module(*shader_stages[i].second,resources.integer_images);
    for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT)stage.module=integer_output_module(stage.module);
    if(!null_descriptors)for(size_t i=0;i<stages.size();i++)stages[i].module=software_null_module(stages[i].module,resources.null_bindings,*shader_stages[i].second->resources);
    if(emulate_depth){
        bool fragment=false;for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT){stage.module=software_depth_module(stage.module);fragment=true;}
        if(!fragment||side_effects)ot2_host_die("Software depth bounds requires a fragment shader without storage side effects");
        auto& texture=*render_targets.depth;auto& image=image_for(texture);
        auto view=render_targets.has_dview?render_targets.dview:view_t{};
        auto range=subresources(image,&view);
        if(image.samples!=VK_SAMPLE_COUNT_1_BIT||image.is_3d||range.layerCount!=1)ot2_host_die("Software depth bounds requires one single-sample 2D depth layer");
        view.set_levels=view.set_layers=view.set_depth_mode=view.set_target=view.set_swizzle=1;
        view.nlevels=view.nlayers=1;view.depth_mode=0;view.target=NVN_TEXTURE_TARGET_2D;view.swizzle=2|(3<<3)|(4<<6)|(5<<9);
        auto input_view=make_view(texture,&view);
        resources.current_stage=VK_SHADER_STAGE_FRAGMENT_BIT;
        const float bounds[]={render_state.depth_bounds.minimum,render_state.depth_bounds.maximum};
        resources.adopt(depth_bounds_uniform_binding,make_buffer(bounds,sizeof(bounds),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,16),VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        resources.bindings.push_back({depth_bounds_input_binding,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
        resources.writes.push_back({depth_bounds_input_binding,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,resources.image_infos.size(),1});
        resources.image_infos.push_back({VK_NULL_HANDLE,input_view,VK_IMAGE_LAYOUT_GENERAL});resources.views.push_back(input_view);
        resources.sampled_images.insert(image.identity);resources.preparation_cacheable=false;
    }
    GpuBuffer sample_buffer{};
    if(emulate_samples){
        ++software_sample_draws;
        bool fragment=false;for(auto& stage:stages)if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT){stage.module=software_samples_module(stage.module);fragment=true;}
        if(!fragment)ot2_host_die("Geometry occlusion fallback requires a fragment shader");
        const uint64_t zero=0;sample_buffer=make_buffer(&zero,8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        resources.current_stage=VK_SHADER_STAGE_FRAGMENT_BIT;resources.adopt(software_samples_binding,sample_buffer,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        resources.preparation_cacheable=false;side_effects=true;
    }
    if(!reused_resources){resources.finish();if(!emulate_depth&&!emulate_samples)remember_prepared_bindings(resources,operation==OP_DISPATCH,side_effects);}
    building_mirrors=false;
    if(operation==OP_DISPATCH){if(stages.size()!=1)ot2_host_die("Compute program is missing");VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage=stages[0];ci.layout=resources.layout;resources.pipeline=cached_pipeline(ci,resources.bindings);auto cmd=begin_commands();bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources.pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,resources.layout,resources.set,resources.dynamic_offsets);auto& p=*reinterpret_cast<const p_dispatch*>(parameters);if(gpu_profile_enabled)gpu_profile_region_begin(cmd,"compute:"+bound_programs[5]->keys[bound_programs[5]->stages[5]]);vkCmdDispatch(cmd,p.x,p.y,p.z);gpu_profile_region_end(cmd);finish_commands(cmd);}
    else {
        if(!vertex)ot2_host_die("Vertex program is missing");bool indexed=operation==OP_DRAW_IDX;auto* draw=reinterpret_cast<const p_draw*>(parameters);auto* indices=reinterpret_cast<const p_draw_idx*>(parameters);
        int primitive=indexed?indices->prim:draw->prim;
        // NVN often binds the unused tail of an entire streaming allocation.
        // Preserve vertex/instance IDs, but upload only the prefix that this
        // draw can fetch (including attribute offsets and one conservative slot).
        uint64_t vertex_end=uint64_t(draw->first)+draw->count;
        if(indexed){auto highest=index_summaries.maximum(reinterpret_cast<const void*>(indices->ibuf),indices->count,indices->itype);int64_t end=int64_t(highest)+indices->base_vertex+1;if(end<0)ot2_host_die("Negative vertex range");vertex_end=end;}
        uint64_t instance_end=indexed?uint64_t(indices->base_instance)+indices->instances:uint64_t(draw->base_instance)+draw->instances;
        VkFormat colors[8]{};VkRenderingAttachmentInfo attachments[8]{};VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};VkExtent2D extent{UINT32_MAX,UINT32_MAX};uint32_t render_layers=UINT32_MAX;
        auto attachment=[&](const tex_t& t,const view_t* view,VkRenderingAttachmentInfo& out){auto& img=image_for(t);out.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;out.imageView=make_view(t,view,true);resources.views.push_back(out.imageView);out.imageLayout=img.aspects==VK_IMAGE_ASPECT_COLOR_BIT&&!resources.sampled_images.contains(fast_image_identity?img.identity:image_for(t).identity)?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
            if(img.layout!=out.imageLayout){auto cmd=begin_commands();barrier(cmd,img,img.layout,out.imageLayout);finish_commands(cmd);}
            out.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;out.storeOp=VK_ATTACHMENT_STORE_OP_STORE;auto level=view&&view->set_levels?view->base_level:0;auto layers=img.is_3d?std::max(1U,t.d.depth>>level):img.layers;if(view&&view->set_layers)layers=view->nlayers?view->nlayers:layers-view->min_layer;render_layers=std::min(render_layers,layers);extent.width=std::min(extent.width,std::max(1U,t.d.width>>level));extent.height=std::min(extent.height,std::max(1U,t.d.height>>level));return view&&view->set_format?format(view->format):img.format;};
        for(int i=0;i<render_targets.n;i++)if(auto* t=render_targets.color[i])colors[i]=attachment(*t,render_targets.has_cview[i]?&render_targets.cview[i]:nullptr,attachments[i]);
        VkFormat depth_format=VK_FORMAT_UNDEFINED;if(render_targets.depth)depth_format=attachment(*render_targets.depth,render_targets.has_dview?&render_targets.dview:nullptr,depth);
        if(extent.width==UINT32_MAX){extent={uint32_t(std::max(1.f,render_state.viewport[2])),uint32_t(std::max(1.f,render_state.viewport[3]))};render_layers=1;}
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=render_targets.n;rendering.pColorAttachmentFormats=colors;rendering.depthAttachmentFormat=depth_format;
        // Packed depth/stencil is detached only when neither aspect participates.
        // Keep dimensions/layers above, so clipping and render extent do not change.
        //
        // A depth image smaller than the rendering area is detached as well. This
        // title switches to a colour target of its own size for the 2D and
        // composite passes while the depth image still belongs to the 1280x720
        // scene pass, and Vulkan leaves a rendering area outside an attachment
        // undefined: every draw then writes nothing and reports no error. Those
        // passes are two-dimensional and test no depth, so dropping the attachment
        // costs them nothing and is what puts the frame on screen.
        bool depth_too_small=false;
        if(render_targets.depth) {
            const auto dw=uint32_t(render_targets.depth->d.width), dh=uint32_t(render_targets.depth->d.height);
            static std::atomic<unsigned> reported{0};
            if(reported.fetch_add(1,std::memory_order_relaxed)<12)
                ot2_host_log("DEPTH_ATTACH depth %ux%u fmt=%u serial=%llu vs rendering %ux%u (dt=%d dw=%d df=%d)",
                    dw,dh,render_targets.depth->d.format,(unsigned long long)render_targets.depth->serial,
                    extent.width,extent.height,render_state.depth_stencil.dt,render_state.depth_stencil.dw,render_state.depth_stencil.df);
            if(dw && dh && (dw<extent.width || dh<extent.height)) depth_too_small=true;
        } else {
            static std::atomic<unsigned> none{0};
            if(none.fetch_add(1,std::memory_order_relaxed)<4)
                ot2_host_log("DEPTH_ATTACH none (rendering %ux%u dt=%d)",extent.width,extent.height,render_state.depth_stencil.dt);
        }
        // The guard on render_targets.depth is load-bearing: everything behind
        // detach_depth dereferences the attachment, so a pass without a depth
        // image must never be able to set it.
        const bool detach_depth=render_targets.depth&&(depth_too_small||force_no_depth||(prune_unused_depth&&!render_state.depth_stencil.dt&&!render_state.depth_stencil.st&&!render_state.depth_bounds.enable));
        if(detach_depth){materialize_clears(render_targets.depth->serial);depth_format=VK_FORMAT_UNDEFINED;depth.imageView={};rendering.depthAttachmentFormat=depth_format;++pruned_depth;}
        bool stencil=depth_format==VK_FORMAT_D24_UNORM_S8_UINT || depth_format==VK_FORMAT_D32_SFLOAT_S8_UINT;if(stencil)rendering.stencilAttachmentFormat=depth_format;
        // Vertex addresses/ranges are draw inputs, not immutable PSO state.
        std::array<std::pair<unsigned,size_t>,16> vertex_buffers{};size_t vertex_buffer_count=0;uint32_t bound_streams=0;
        for(auto loc:vertex->inputs){if(loc>=16)ot2_host_die("Vertex location >=16 pending");const auto& a=render_state.attributes[loc];
            // A shader input the title never gave a format. The console feeds a
            // disabled attribute in that case, and a title that omits one either
            // does not read it or does not care what it holds. Vulkan leaves the
            // value undefined when no attribute description is provided, which is
            // the closest match; dying here instead would end the run at the first
            // draw that leaves one out, which is what this used to do.
            if(!a.fmt){static std::atomic<unsigned> reported{0};++unbound_attribute_draws;
                if(reported.fetch_add(1,std::memory_order_relaxed)<16)
                    ot2_host_log("ATTRIBUTE_UNBOUND location=%d (shader reads it, title never bound it)",loc);
                continue;}
            if(a.stream>=16)ot2_host_die("Vertex stream exceeds host ABI");
            if(!(bound_streams&(1u<<a.stream))){bound_streams|=1u<<a.stream;
                const auto& stream=render_state.streams[a.stream];if(stream.divisor>1)ot2_host_die("Instance divisor >1 pending");
                auto& b=render_state.vertex_buffers[a.stream];if(!b.address || !b.size)ot2_host_die("Vertex buffer %u unbound",uint32_t(a.stream));uint64_t attribute_end=0;
                for(auto location:vertex->inputs){const auto& attr=render_state.attributes[location];if(attr.stream==a.stream)attribute_end=std::max<uint64_t>(attribute_end,attr.offset+16);}
                uint64_t prefix=(stream.divisor?instance_end:vertex_end)*stream.stride+attribute_end;
                vertex_buffers[vertex_buffer_count++]={a.stream,resources.buffers.size()};
                // Diagnostic: the projection this draw carries maps pixels to clip
                // space (1/640, -1/360), so a vertex of 0..1 lands inside one pixel.
                // Rewriting the positions as pixel coordinates with the same winding
                // answers whether the geometry is expected in pixels: content in the
                // lower-right quadrant means yes and the values the title supplies
                // are not reaching us intact; no content means this draw is not the
                // one that draws the panel.
                if(sprite_pixels_forced&&vertex->inputs.size()<=1&&a.stream==0&&a.offset==0&&
                   stream.stride==28&&stream.divisor==0){
                    const size_t vertices=size_t(std::max<uint64_t>(vertex_end,4));
                    const size_t bytes=vertices*stream.stride;
                    if(bytes<=1u<<20){
                        std::vector<uint8_t> copy(bytes);
                        memcpy(copy.data(),reinterpret_cast<const void*>(b.address),std::min<size_t>(bytes,b.size));
                        const float corners[4][2]={{0.f,0.f},{640.f,0.f},{0.f,360.f},{640.f,360.f}};
                        for(size_t v=0;v<vertices;v++){
                            auto* f=reinterpret_cast<float*>(copy.data()+v*stream.stride);
                            f[0]=corners[v%4][0];f[1]=corners[v%4][1];f[2]=0.f;}
                        resources.buffers.push_back(make_buffer(copy.data(),copy.size(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
                        static std::atomic<unsigned> reported{0};
                        if(reported.fetch_add(1,std::memory_order_relaxed)<6)
                            ot2_host_log("SPRITE_PIXEL_POS stride=%u vertices=%zu",uint32_t(stream.stride),vertices);
                        continue;
                    }
                }
                // Diagnostic: the per-instance record is what gives a normalised
                // unit quad its size on screen. Overwriting the rect with a real
                // rectangle tells whether that attribute is the size source at all:
                // a frame that fills proves it is and that the title's own value is
                // being read from the wrong place; an unchanged frame proves the
                // size lives somewhere the vertex stage never sees.
                if(sprite_pixels_forced&&a.stream==1&&stream.stride>0&&stream.stride<=256&&
                   render_state.attributes[6].stream==1){
                    const size_t instances=size_t(std::max<uint64_t>(instance_end,1));
                    const size_t bytes=(instances+1)*stream.stride;
                    if(bytes<=8u<<20&&b.size>=bytes){
                        std::vector<uint8_t> copy(bytes);
                        memcpy(copy.data(),reinterpret_cast<const void*>(b.address),bytes);
                        const uint32_t off=render_state.attributes[6].offset;
                        for(size_t k=0;k<instances;k++){
                            auto* f=reinterpret_cast<float*>(copy.data()+k*stream.stride+off);
                            f[0]=0.f;f[1]=0.f;f[2]=sprite_rect_w;f[3]=sprite_rect_h;}
                        resources.buffers.push_back(make_buffer(copy.data(),copy.size(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
                        static std::atomic<unsigned> reported{0};
                        if(reported.fetch_add(1,std::memory_order_relaxed)<4)
                            ot2_host_log("SPRITE_RECT_FORCED off=%u stride=%u instances=%zu rect=%gx%g",
                                off,uint32_t(stream.stride),instances,double(sprite_rect_w),double(sprite_rect_h));
                        continue;
                    }
                }
                // Diagnostic: a 2D sprite program can place each sprite from a
                // per-instance rectangle instead of from the vertex buffer. The
                // title hands over a unit rectangle, so if the shader reads it the
                // sprite is one pixel wide and the frame looks empty while every
                // draw is in fact correct. Scaling a copy of that rectangle tells
                // the two apart: a frame that grows proves the attribute is read
                // and that the size comes from somewhere else.
                if(sprite_instance_scale!=1.f&&a.stream==1&&stream.stride>0&&stream.stride<=256&&
                   render_state.attributes[6].stream==1){
                    const size_t instances=size_t(instance_end?instance_end:1);
                    const size_t bytes=(instances+1)*stream.stride;
                    if(bytes<=8u<<20&&b.size>=bytes){
                        std::vector<uint8_t> copy(bytes);
                        memcpy(copy.data(),reinterpret_cast<const void*>(b.address),bytes);
                        for(size_t k=0;k<instances;k++){
                            auto* f=reinterpret_cast<float*>(copy.data()+k*stream.stride+render_state.attributes[6].offset);
                            f[0]*=sprite_instance_scale;f[1]*=sprite_instance_scale;}
                        resources.buffers.push_back(make_buffer(copy.data(),copy.size(),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
                        continue;
                    }
                }
                resources.buffers.push_back(readonly_buffer(reinterpret_cast<void*>(b.address),std::min<uint64_t>(b.size,prefix),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));}}
        const auto& v=render_state.viewport;VkViewport vp{v[0],v[1]+v[3],v[2],-v[3],v[4],v[5]};const auto& sc=render_state.scissor;int sx=std::max(0,sc.x),sy=std::max(0,sc.y);VkRect2D scissors{{sx,sy},{uint32_t(std::max(0,std::min(int(extent.width),sc.x+sc.w)-sx)),uint32_t(std::max(0,std::min(int(extent.height),sc.y+sc.h)-sy))}};
        uint32_t attachment_samples=0;auto check_samples=[&](const tex_t* t){if(!t)return;auto samples=uint32_t(image_for(*t).samples);if(attachment_samples&&attachment_samples!=samples)ot2_host_die("Mixed attachment sample counts");attachment_samples=samples;};
        for(int i=0;i<render_targets.n;i++)check_samples(render_targets.color[i]);check_samples(render_targets.depth);
        FixedKey<128> version_key;version_key.add(graphics_state_revision);version_key.add(primitive);version_key.add(resources.layout);version_key.add(render_targets.n);
        version_key.add(colors);version_key.add(depth_format);version_key.add(attachment_samples);version_key.add(dynamic_depth_bounds);
        for(int i=0;i<5;i++){VkShaderModule module{};for(auto& stage:stages)if(stage.stage==shader_stage(i))module=stage.module;version_key.add(module);}
        PreparedGraphicsPipeline prepared;
        if(null_descriptors&&versioned_pipeline_state&&reuse_pipeline_preparation&&last_version_pipeline.pipeline&&last_pipeline_version_key==version_key){prepared=last_version_pipeline;++pipeline_version_hits;++pipeline_preparation_hits;}
        else{
        auto preparation_key=graphics_preparation_key(primitive,resources.layout,colors,depth_format,attachment_samples,stages);
        auto found=prepared_graphics_pipelines.find(preparation_key);
        if(reuse_pipeline_preparation&&found!=prepared_graphics_pipelines.end()){prepared=found->second;++pipeline_preparation_hits;}
        else{
        ++pipeline_preparation_misses;
        std::vector<VkVertexInputAttributeDescription> attributes;std::vector<VkVertexInputBindingDescription> bindings;
        for(auto loc:vertex->inputs){const auto& a=render_state.attributes[loc];if(!a.fmt)continue;attributes.push_back({loc,uint32_t(a.stream),format(a.fmt),uint32_t(a.offset)});}
        for(size_t i=0;i<vertex_buffer_count;i++){auto binding=vertex_buffers[i].first;const auto& stream=render_state.streams[binding];bindings.push_back({binding,stream.stride,stream.divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX});}
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=bindings.size();vi.pVertexBindingDescriptions=bindings.data();vi.vertexAttributeDescriptionCount=attributes.size();vi.pVertexAttributeDescriptions=attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=topology(primitive);
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vs.viewportCount=1;vs.pViewports=&vp;vs.scissorCount=1;vs.pScissors=&scissors;
        auto& pol=render_state.polygon;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=pol.mode==2?VK_POLYGON_MODE_FILL:pol.mode==1?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_POINT;
        // The viewport has a negative height (line 1409): it turns Vulkan's
        // y-down framebuffer back into the console's y-up window, which is the
        // orientation the title's geometry and its "front face" enum are both
        // written for. That flip also reverses the rasterised winding, and the
        // front-face setting is defined over framebuffer-space winding, so a
        // front-face value carried over unchanged selects the *other* side of
        // every triangle. Culling then removes exactly what the title meant to
        // keep: with cull disabled the draw is visible, with it enabled the draw
        // writes nothing at all -- which is what separated the working 3D passes
        // from every composite pass in the frame trace.
        rs.cullMode=cull_none_forced?VK_CULL_MODE_NONE:pol.cull;
        rs.frontFace=(pol.front?!winding_not_flipped:winding_not_flipped)?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth=1;rs.depthBiasEnable=pol.offset_en!=0;rs.depthBiasConstantFactor=render_state.depth_bias.units;rs.depthBiasSlopeFactor=render_state.depth_bias.factor;rs.depthBiasClamp=render_state.depth_bias.clamp;
        rs.rasterizerDiscardEnable=render_state.raster_discard;rs.depthClampEnable=render_state.depth_clamp;rs.lineWidth=render_state.line_width;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;ms.alphaToCoverageEnable=render_state.multisample.a2c;ms.pSampleMask=&render_state.sample_mask;
        if(attachment_samples)ms.rasterizationSamples=VkSampleCountFlagBits(attachment_samples);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};const auto& d=render_state.depth_stencil;ds.depthTestEnable=d.dt;ds.depthWriteEnable=d.dw;
        // NVN_DEPTH_FUNC_EQUAL is how a title that runs a depth prepass shades only
        // the fragments that pass already wrote: the prepass fills depth, this pass
        // tests for exactly that value and writes no depth of its own. A depth-only
        // pass does reach this host and writes the same image this test reads.
        //
        // What does not hold is the bit-exactness. The prepass and this pass run
        // different shader stages, so their depth values differ in the last bits and
        // EQUAL rejects every fragment: the geometry is correct and the colour target
        // still stays empty. Widening to LEQUAL keeps what the test is for -- a
        // fragment hidden behind the prepass surface has a larger depth and still
        // fails -- while dropping the requirement that the two be identical.
        // Measured by capture: with EQUAL the scene target receives nothing; with
        // LEQUAL the same six draws accumulate content draw after draw.
        const bool approximate_equal=render_state.depth_stencil.dt&&int(d.df)==3;
        const int depth_function=approximate_equal?4:std::max(1,int(d.df));
        ds.depthTestEnable=d.dt;ds.depthWriteEnable=d.dw;
        ds.depthCompareOp=VkCompareOp(depth_function-1);ds.stencilTestEnable=d.st;ds.depthBoundsTestEnable=render_state.depth_bounds.enable;ds.minDepthBounds=render_state.depth_bounds.minimum;ds.maxDepthBounds=render_state.depth_bounds.maximum;
        ds.front={VkStencilOp(std::max(1,int(d.fail_f))-1),VkStencilOp(std::max(1,int(d.zpass_f))-1),VkStencilOp(std::max(1,int(d.zfail_f))-1),VkCompareOp(std::max(1,int(d.sf_f))-1),render_state.stencil_compare_mask[0],render_state.stencil_write_mask[0],render_state.stencil_reference[0]};
        ds.back={VkStencilOp(std::max(1,int(d.fail_b))-1),VkStencilOp(std::max(1,int(d.zpass_b))-1),VkStencilOp(std::max(1,int(d.zfail_b))-1),VkCompareOp(std::max(1,int(d.sf_b))-1),render_state.stencil_compare_mask[1],render_state.stencil_write_mask[1],render_state.stencil_reference[1]};
        VkPipelineColorBlendAttachmentState blend[8]{};for(int i=0;i<render_targets.n;i++){auto& b=render_state.blends[i];blend[i]={VkBool32((render_state.color.blend_en>>i)&1),blend_factor(b.src_rgb),blend_factor(b.dst_rgb),VkBlendOp(std::max(1,int(b.eq_rgb))-1),blend_factor(b.src_a),blend_factor(b.dst_a),VkBlendOp(std::max(1,int(b.eq_a))-1),VkColorComponentFlags((render_state.channel_mask.mask>>(4*i))&15)};}
        // Diagnostic: draw every pass opaque. If the frame starts showing content
        // with blending off, the geometry and the shaders are fine and the blend
        // translation is what hides the result; if it stays identical, the draws
        // never write these pixels at all and blending is not the cause.
        if(opaque_blend_forced)for(int i=0;i<8;i++){blend[i].blendEnable=VK_FALSE;blend[i].srcColorBlendFactor=VK_BLEND_FACTOR_ONE;blend[i].dstColorBlendFactor=VK_BLEND_FACTOR_ZERO;blend[i].srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend[i].dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend[i].colorWriteMask=0xf;}
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.logicOpEnable=render_state.color.logic_op!=NVN_LOGIC_OP_COPY;cb.logicOp=VkLogicOp(render_state.color.logic_op);cb.attachmentCount=render_targets.n;cb.pAttachments=blend;memcpy(cb.blendConstants,render_state.blend_color,16);
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.pNext=&rendering;ci.stageCount=stages.size();ci.pStages=stages.data();ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vs;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&cb;ci.layout=resources.layout;
        VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BOUNDS};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=dynamic_depth_bounds?3:2;dynamic.pDynamicStates=dynamic_states;ci.pDynamicState=&dynamic;
        prepared.pipeline=cached_pipeline(ci,resources.bindings);prepared.depth=ds;std::copy_n(blend,8,prepared.blend.begin());
        if(reuse_pipeline_preparation)prepared_graphics_pipelines.insert_or_assign(preparation_key,prepared);
        }
        last_pipeline_version_key=version_key;last_version_pipeline=prepared;
        }
        resources.pipeline=prepared.pipeline;const auto& ds=prepared.depth;const auto& blend=prepared.blend;
        VkBuffer ib{};VkDeviceSize ib_offset=0;VkIndexType it=VK_INDEX_TYPE_UINT16;
        if(indexed){if(indices->itype==0){std::vector<uint16_t> converted(indices->count);auto* src=reinterpret_cast<const uint8_t*>(indices->ibuf);for(int i=0;i<indices->count;i++)converted[i]=src[i];auto b=make_buffer(converted.data(),converted.size()*2,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);ib=b.buffer;ib_offset=b.offset;resources.buffers.push_back(b);}else{it=indices->itype==1?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;auto b=readonly_buffer(reinterpret_cast<const void*>(indices->ibuf),size_t(indices->count)*(indices->itype==1?2:4),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);ib=b.buffer;ib_offset=b.offset;resources.buffers.push_back(b);}}
        // STORE_OP_NONE preserves prior memory only when the entire rendering
        // instance has no writes to that aspect. DONT_CARE does not provide
        // that guarantee. Unlike Metal, Vulkan fixes store operations at begin:
        // a later writer must end this instance before it can write. A writable
        // instance stays writable even when its final draws are read-only.
        constexpr uint32_t depth_bit=1u<<8,stencil_bit=1u<<9;
        uint32_t bound=0,writes=0;bool attachment_alias=side_effects;
        for(int i=0;i<render_targets.n;++i)if(render_targets.color[i]){
            bound|=1u<<i;if(blend[i].colorWriteMask)writes|=1u<<i;
            attachment_alias|=resources.sampled_images.contains(attachment_identity(*render_targets.color[i]));
        }
        if(render_targets.depth&&!detach_depth){bound|=depth_bit;if(ds.depthTestEnable&&ds.depthWriteEnable)writes|=depth_bit;attachment_alias|=resources.sampled_images.contains(attachment_identity(*render_targets.depth));}
        auto stencil_writes=[](const VkStencilOpState& s){return (s.writeMask&255)&&(s.failOp!=VK_STENCIL_OP_KEEP||s.depthFailOp!=VK_STENCIL_OP_KEEP||s.passOp!=VK_STENCIL_OP_KEEP);};
        if(stencil){bound|=stencil_bit;if(ds.stencilTestEnable&&(stencil_writes(ds.front)||stencil_writes(ds.back)))writes|=stencil_bit;}
        // Shader image/storage side effects and attachment sampling need a
        // stronger alias proof. Keep STORE and the existing pass boundary.
        if(attachment_alias){writes=bound;if(readonly_stores_enabled)++attachment_stores.fallbacks;}
        uint32_t no_store=readonly_stores_enabled&&!attachment_alias?bound&~writes:0;
        // The two aspects of a packed image share physical storage. Device
        // regression: omitting either aspect alone preserves the forest, but
        // omitting both corrupts visibility. Until that combined path has an
        // independent reproducer, keep one store for the shared image. This is
        // a format-level conservative fallback, not a scene/shader exception.
        if(stencil&&(no_store&(depth_bit|stencil_bit))==(depth_bit|stencil_bit))no_store&=~stencil_bit;
        for(int i=0;i<render_targets.n;++i)if(no_store&(1u<<i))attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingAttachmentInfo stencil_attachment=depth;
        // Only exact full-subresource clears are load operations. Sampling,
        // copies, masks and narrower render extents materialize the old path.
        bool folded=false;
        for(int i=0;i<render_targets.n;++i)if(auto* t=render_targets.color[i])folded|=consume_clear(*t,render_targets.has_cview[i]?&render_targets.cview[i]:nullptr,extent,render_layers,VK_IMAGE_ASPECT_COLOR_BIT,attachments[i]);
        if(render_targets.depth&&!detach_depth){auto* view=render_targets.has_dview?&render_targets.dview:nullptr;
            folded|=consume_clear(*render_targets.depth,view,extent,render_layers,VK_IMAGE_ASPECT_DEPTH_BIT,depth);
            if(stencil)folded|=consume_clear(*render_targets.depth,view,extent,render_layers,VK_IMAGE_ASPECT_STENCIL_BIT,stencil_attachment);
        }
        // An unmatched clear may target a different layer/extent of a bound
        // attachment. Complete it before beginning rendering, not at submit.
        for(int i=0;i<render_targets.n;++i)if(render_targets.color[i])materialize_clears(render_targets.color[i]->serial);
        if(render_targets.depth)materialize_clears(render_targets.depth->serial);
        if(folded)end_rendering();
        for(int i=0;i<render_targets.n;++i)if(auto* t=render_targets.color[i]){auto& img=image_for(*t);if(img.layout!=attachments[i].imageLayout){auto c=begin_commands();barrier(c,img,img.layout,attachments[i].imageLayout);finish_commands(c);}}
        if(render_targets.depth&&!detach_depth){auto& img=image_for(*render_targets.depth);if(img.layout!=depth.imageLayout){auto c=begin_commands();barrier(c,img,img.layout,depth.imageLayout);finish_commands(c);}}
        for(int i=0;i<render_targets.n;++i)if(attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR){writes|=1u<<i;no_store&=~(1u<<i);attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;}
        if(depth.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR){writes|=depth_bit;no_store&=~depth_bit;}
        if(stencil_attachment.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR){writes|=stencil_bit;no_store&=~stencil_bit;}
        if(no_store&depth_bit)depth.storeOp=VK_ATTACHMENT_STORE_OP_NONE;
        if(no_store&stencil_bit)stencil_attachment.storeOp=VK_ATTACHMENT_STORE_OP_NONE;
        uint32_t discard_store=0;
        if(!side_effects)for(int i=0;i<render_targets.n;++i)if(auto* t=render_targets.color[i];t&&!resources.sampled_images.contains(attachment_identity(*t))&&dead_store_before_clear(*t,render_targets.has_cview[i]?&render_targets.cview[i]:nullptr,replay_next_command)){
            attachments[i].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;discard_store|=1u<<i;
        }
        PipelineKey pass;pass.add(discard_store);pass.add(extent);pass.add(render_layers);pass.add(render_targets.n);for(int i=0;i<render_targets.n;i++){pass.add(attachments[i].imageView);pass.add(attachments[i].imageLayout);}pass.add(depth.imageView);pass.add(depth.imageLayout);if(!depth_bounds_supported)pass.add(emulate_depth);
        if(!merge_enabled || side_effects || rendering_key!=pass.data)end_rendering();
        if(rendering_command&&(rendering_no_store&writes)){++attachment_stores.promotions;end_rendering();}
        auto cmd=begin_commands(true);
        if(!rendering_command){
            // A complete memory dependency between passes also covers sampled
            // images written by the preceding pass; attachment order within a
            // pass is supplied by Vulkan's rasterization ordering.
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            if(!precise_barriers)vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,0,nullptr);
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea={{0,0},extent};ri.layerCount=render_layers;ri.colorAttachmentCount=render_targets.n;ri.pColorAttachments=attachments;if(render_targets.depth&&!detach_depth)ri.pDepthAttachment=&depth;if(stencil)ri.pStencilAttachment=&stencil_attachment;
            for(int i=0;i<render_targets.n;++i)note_attachment_store(attachments[i],0);
            if(ri.pDepthAttachment)note_attachment_store(*ri.pDepthAttachment,1);
            if(ri.pStencilAttachment)note_attachment_store(*ri.pStencilAttachment,2);
            if(gpu_profile_enabled){auto& p=bound_programs[1];std::string label="render:"+std::to_string(extent.width)+"x"+std::to_string(extent.height)+":"+(p&&p->stages[1]>=0?p->keys[p->stages[1]]:"depth");gpu_profile_region_begin(cmd,std::move(label));}
            dead_attachment_stores+=std::popcount(discard_store);
            begin_rendering_compatible(cmd,ri,rendering,VkSampleCountFlagBits(attachment_samples?attachment_samples:1),emulate_depth);rendering_command=cmd;rendering_key=std::move(pass.data);rendering_no_store=no_store;++perf.passes;
        }
        uint32_t query_slot=pending_queries;if(samples_active){if(query_slot>=query_capacity)ot2_host_die("Occlusion batch overflow");++pending_queries;if(emulate_samples){if(batching)software_sample_slots.push_back({query_slot,sample_buffer});}else vkCmdBeginQuery(cmd,occlusion_query,query_slot,precise_occlusion?VK_QUERY_CONTROL_PRECISE_BIT:0);}
        bind_viewport(cmd,vp);bind_scissor(cmd,scissors);
        // Diagnostic: paint the attachment from inside the very rendering
        // instance the suspect draw is recorded in. If the screen changes, the
        // pass and its command buffer do reach the GPU and the draw itself writes
        // nothing; if it does not, the draw never had a live pass to begin with.
        if(paint_draws_probe&&render_targets.n&&operation==OP_DRAW_IDX&&indices->count<=10){
            VkClearAttachment paint{};paint.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;paint.colorAttachment=0;
            paint.clearValue.color.float32[0]=1.f;paint.clearValue.color.float32[1]=0.f;paint.clearValue.color.float32[2]=1.f;paint.clearValue.color.float32[3]=1.f;
            VkClearRect area{};area.rect={{0,0},extent};area.layerCount=1;
            vkCmdClearAttachments(cmd,1,&paint,1,&area);
        }
        bind_pipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,resources.pipeline);bind_descriptors(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,resources.layout,resources.set,resources.dynamic_offsets);
        // Emit after binding, including after a static draw-clear and in every
        // new recording. Never reuse the bounds stored in PreparedGraphicsPipeline.
        if(dynamic_depth_bounds&&depth_bounds_supported){const auto& bounds=render_state.depth_bounds;vkCmdSetDepthBounds(cmd,bounds.enable?bounds.minimum:0.f,bounds.enable?bounds.maximum:1.f);}
        for(auto& [binding,index]:std::span(vertex_buffers.data(),vertex_buffer_count)){VkDeviceSize offset=resources.buffers[index].offset;bind_vertex(cmd,binding,resources.buffers[index].buffer,offset);}
        // The count of draws that actually reached Vulkan, as opposed to the ones
        // that were prepared for. A frame loop can look healthy while every draw is
        // filtered out earlier, so this is the number to compare a blank screen
        // against.
        draws_submitted.fetch_add(1,std::memory_order_relaxed);
        if(indexed){bind_index(cmd,ib,ib_offset,it);indices_submitted.fetch_add((unsigned long long)indices->count,std::memory_order_relaxed);vkCmdDrawIndexed(cmd,indices->count,indices->instances,0,indices->base_vertex,indices->base_instance);}
        else{vertices_submitted.fetch_add((unsigned long long)draw->count,std::memory_order_relaxed);vkCmdDraw(cmd,draw->count,draw->instances,draw->first,draw->base_instance);}
        if(samples_active&&!emulate_samples)vkCmdEndQuery(cmd,occlusion_query,query_slot);
        if(!batching || side_effects)finish_commands(cmd);else ++batch_operations;
        if(samples_active&&!batching){drain_gpu_locked();uint64_t value=0;if(emulate_samples)memcpy(&value,sample_buffer.mapped,8);else check(vkGetQueryPoolResults(device,occlusion_query,query_slot,1,8,&value,8,VK_QUERY_RESULT_64_BIT),"occlusion results");counter_epoch->samples+=value;pending_queries=0;}

    }
    // Storage writes must reach guest memory before a later draw snapshots it.
    bool writeback=false;for(auto& b:resources.buffers)writeback|=b.writeback!=nullptr;
    if(writeback)flush_commands();
    if(side_effects)reset_prepared_bindings();
    if(batch_command){auto* retained=owned.release();batch_retire.push_back([retained]{recycle_draw_resources(retained);});}
    else recycle_draw_resources(owned.release());
    // Capacity is not a guest CPU dependency. The next slot can be recorded
    // while this one executes; ensure_batch_slot still enforces the ring bound.
    if(batch_operations>=128)flush_commands(synchronous_capacity_flush);
    if(diagnose){flush_commands();ot2_host_log("DRAW_COMPLETION end=%llu",(unsigned long long)(encoded_draws+1));}
    encoded_draws++;if(operation!=OP_DISPATCH)for(int i=0;i<render_targets.n;i++)if(render_targets.color[i])image_for(*render_targets.color[i]).drawn=encoded_draws;
    if(operation!=OP_DISPATCH)diagnostic_draw(encoded_draws,operation,parameters);
    if(encoded_draws<=10 || (verbose_gpu_logs&&encoded_draws%100==0))ot2_host_log("GPU_DRAW_ENCODED count=%llu operation=%u",(unsigned long long)encoded_draws,operation);
}
