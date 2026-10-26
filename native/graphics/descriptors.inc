// The submit lock serializes all access. A descriptor set is leased to one
// draw until its real GPU fence retires DrawResources. Never update a set that
// another recorded or pending draw can still reference.
struct DescriptorLayout;
struct DescriptorSlot;
struct DescriptorPoolBlock;
using DescriptorContentsIndex=std::unordered_multimap<std::string_view,DescriptorSlot*>;
struct DescriptorValue {
    uint32_t binding{};VkDescriptorType type{};int kind{};
    VkDescriptorBufferInfo buffer{};VkDescriptorImageInfo image{};VkBufferView texel{};
    bool operator==(const DescriptorValue& b)const{
        if(binding!=b.binding||type!=b.type||kind!=b.kind)return false;
        if(kind==2)return texel==b.texel;
        if(kind==1)return image.sampler==b.image.sampler&&image.imageView==b.image.imageView&&image.imageLayout==b.image.imageLayout;
        return buffer.buffer==b.buffer.buffer&&buffer.offset==b.buffer.offset&&buffer.range==b.buffer.range;
    }
};
bool incremental_descriptors=true;
bool linear_descriptor_delta=true;
uint64_t descriptor_delta_written=0,descriptor_delta_skipped=0;
struct DescriptorSlot {
    DescriptorLayout* owner;
    DescriptorPoolBlock* block{};
    VkDescriptorSet set{};
    struct Link {DescriptorSlot* previous{};DescriptorSlot* next{};};
    Link idle_links[2];
    DescriptorContentsIndex::node_type contents_node;
    std::string contents;
    uint64_t contents_epoch{},values_epoch{};
    bool values_linear=false;
    std::vector<DescriptorValue> values;
    bool contents_indexed=false;
};
// Both queues borrow the slot's links. Acquiring/releasing an idle descriptor
// must not allocate/free two list nodes on every draw.
struct DescriptorIdleList {
    unsigned link_index=0;
    DescriptorSlot* first{};DescriptorSlot* last{};size_t count=0;
    bool empty()const{return count==0;}
    size_t size()const{return count;}
    DescriptorSlot* front()const{return first;}
    DescriptorSlot* back()const{return last;}
    void push_back(DescriptorSlot* slot){
        auto& link=slot->idle_links[link_index];link={last,nullptr};
        if(last)last->idle_links[link_index].next=slot;else first=slot;
        last=slot;++count;
    }
    void erase(DescriptorSlot* slot){
        auto& link=slot->idle_links[link_index];
        if(link.previous)link.previous->idle_links[link_index].next=link.next;else first=link.next;
        if(link.next)link.next->idle_links[link_index].previous=link.previous;else last=link.previous;
        link={};--count;
    }
    void pop_front(){erase(first);}
};
struct DescriptorLayout {
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout pipeline_layout{};
    std::vector<VkDescriptorPoolSize> sizes;
    DescriptorIdleList idle{1};
    std::vector<DescriptorPoolBlock*> available_blocks;
};
// One driver allocation serves up to 64 sets of the same layout. The former
// one-pool-per-set scheme exhausted Adreno mappings despite free system RAM.
struct DescriptorPoolBlock {
    DescriptorLayout* owner{};
    VkDescriptorPool pool{};
    std::vector<VkDescriptorSet> free_sets;
    size_t capacity{},live{};
};
std::unordered_map<VkDescriptorPool,std::unique_ptr<DescriptorPoolBlock>> descriptor_blocks;
std::map<std::string,DescriptorLayout> descriptor_layouts;
DescriptorIdleList idle_descriptors;
struct LayoutInputHash {
    using is_transparent=void;
    size_t operator()(std::string_view key)const{return std::hash<std::string_view>{}(key);}
};
// The update payload follows the *input* order, not the canonical layout's
// sorted order. Two permutations share a layout but require different templates.
union DescriptorUpdateData {
    VkDescriptorBufferInfo buffer;
    VkDescriptorImageInfo image;
    VkBufferView texel;
    DescriptorUpdateData():buffer{}{}
};
struct DescriptorLayoutInput {
    DescriptorLayout* owner{};
    VkDescriptorUpdateTemplate update{};
};
std::unordered_map<std::string,DescriptorLayoutInput,LayoutInputHash,std::equal_to<>> descriptor_layout_inputs;
bool use_descriptor_templates=false;
uint64_t descriptor_template_updates=0,descriptor_template_creates=0;
bool reuse_layout_inputs=true;
uint64_t descriptor_layout_input_hits=0,descriptor_contents_node_creates=0;
// Views point into their slot's string, which stays immutable while indexed.
// Only fence-retired slots enter this index; pending descriptors are untouched.
DescriptorContentsIndex idle_descriptor_contents;
uint64_t descriptor_contents_epoch=1,descriptor_contents_hits=0;
bool retain_descriptor_contents=true;
bool retain_descriptor_working_set=true;
size_t descriptor_live_slots=0;
void remove_descriptor_contents(DescriptorSlot* slot){
    if(!slot->contents_indexed)return;
    auto [first,last]=idle_descriptor_contents.equal_range(slot->contents);
    for(auto it=first;it!=last;++it)if(it->second==slot){slot->contents_node=idle_descriptor_contents.extract(it);break;}
    slot->contents_indexed=false;
}
void invalidate_descriptor_contents(){
    ++descriptor_contents_epoch;
    while(!idle_descriptor_contents.empty()){
        auto it=idle_descriptor_contents.begin();auto* slot=it->second;
        slot->contents_node=idle_descriptor_contents.extract(it);slot->contents_indexed=false;
    }
}
// Complex OT2 views exceed 1,024 simultaneously leased sets. Keeping fewer
// idle sets than a recurring batch needs causes pool creation every frame.
// This is a lazy ceiling, not an up-front allocation; pending leases never
// count against it and are never evicted. Retain the small limit for A/B.
size_t max_idle_descriptors=8192;
uint64_t descriptor_layout_creates{},descriptor_pool_creates{},descriptor_reuses{},descriptor_pool_destroys{};
bool reuse_descriptors=true;
void cache_allocation_pressure();
void trim_descriptors();
uint64_t descriptor_allocation_retries=0;
uint64_t descriptor_slot_creates=0;
#ifdef OT2_NVN_TESTING
unsigned test_descriptor_pool_failures=0;
#endif
VkResult create_descriptor_block(DescriptorLayout& entry,size_t count){
    auto block=std::make_unique<DescriptorPoolBlock>();block->owner=&entry;block->capacity=count;
    auto sizes=entry.sizes;for(auto& size:sizes)size.descriptorCount*=count;
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets=count;ci.poolSizeCount=sizes.size();ci.pPoolSizes=sizes.data();
#ifdef OT2_NVN_TESTING
    if(test_descriptor_pool_failures){--test_descriptor_pool_failures;return VK_ERROR_OUT_OF_HOST_MEMORY;}
#endif
    auto result=vkCreateDescriptorPool(device,&ci,nullptr,&block->pool);if(result!=VK_SUCCESS)return result;
    std::vector<VkDescriptorSetLayout> layouts(count,entry.set_layout);block->free_sets.resize(count);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=block->pool;ai.descriptorSetCount=count;ai.pSetLayouts=layouts.data();
    result=vkAllocateDescriptorSets(device,&ai,block->free_sets.data());
    if(result!=VK_SUCCESS){vkDestroyDescriptorPool(device,block->pool,nullptr);return result;}
    auto* raw=block.get();descriptor_blocks.emplace(block->pool,std::move(block));entry.available_blocks.push_back(raw);++descriptor_pool_creates;
    return VK_SUCCESS;
}
void destroy_descriptor_slot(DescriptorSlot* slot){
    auto* block=slot->block;auto& available=block->owner->available_blocks;
    if(block->free_sets.empty())available.push_back(block);
    block->free_sets.push_back(slot->set);--block->live;delete slot;--descriptor_live_slots;
    if(!block->live){
        auto found=std::find(available.begin(),available.end(),block);
        if(found==available.end())ot2_host_die("Descriptor block missing from availability list");
        *found=available.back();available.pop_back();
        const auto pool=block->pool;vkDestroyDescriptorPool(device,pool,nullptr);descriptor_blocks.erase(pool);++descriptor_pool_destroys;
    }
}
DescriptorSlot* acquire_descriptors(const std::vector<VkDescriptorSetLayoutBinding>& input,const std::string* contents=nullptr,bool* unchanged=nullptr,std::string_view input_key={},VkDescriptorUpdateTemplate* update=nullptr) {
    if(unchanged)*unchanged=false;
    if(update)*update=VK_NULL_HANDLE;
    if(retain_descriptor_contents&&contents){
        auto [first,last]=idle_descriptor_contents.equal_range(*contents);
        for(auto it=first;it!=last;++it){auto* slot=it->second;
            if(slot->contents_epoch!=descriptor_contents_epoch)continue;
            slot->contents_node=idle_descriptor_contents.extract(it);slot->contents_indexed=false;
            slot->owner->idle.erase(slot);
            idle_descriptors.erase(slot);
            ++descriptor_reuses;++descriptor_contents_hits;if(unchanged)*unchanged=true;return slot;
        }
    }
    DescriptorLayout* selected=nullptr;
    DescriptorLayoutInput* ordered=nullptr;
    // DrawResources already serialized these exact fields into its signature.
    // The string-view lookup skips a second vector copy, sort and string build.
    if(reuse_layout_inputs&&!input_key.empty())if(auto found=descriptor_layout_inputs.find(input_key);found!=descriptor_layout_inputs.end()){
        ordered=&found->second;selected=ordered->owner;++descriptor_layout_input_hits;
    }
    if(!selected){
    auto bindings=input;
    std::sort(bindings.begin(),bindings.end(),[](const auto& a,const auto& b){return a.binding<b.binding;});
    // The same binding can be reached twice: the support buffer is injected per
    // group, and a group that is appended as well as prepared contributes its
    // entries twice. One layout entry per binding is what Vulkan wants, and the
    // duplicates always agree on type, so the first one stands. A duplicate that
    // disagrees on type is a real conflict and still stops the run.
    for(size_t i=1;i<bindings.size();){
        if(bindings[i].binding!=bindings[i-1].binding){++i;continue;}
        if(bindings[i].descriptorType!=bindings[i-1].descriptorType||bindings[i].descriptorCount!=bindings[i-1].descriptorCount)
            ot2_host_die("Descriptor binding %u used as two types",bindings[i].binding);
        bindings.erase(bindings.begin()+long(i));
    }
    PipelineKey key;key.bindings(bindings);
    auto [it,inserted]=descriptor_layouts.try_emplace(key.data);
    auto& entry=it->second;
    if(inserted){
        for(size_t i=0;i<bindings.size();i++){
            if(bindings[i].pImmutableSamplers || (i&&bindings[i-1].binding==bindings[i].binding)){
                // Naming the offender is the difference between a five-minute fix
                // and another run: the same binding twice and an immutable sampler
                // are two different mistakes.
                char list[256]={0};size_t used=0;
                for(size_t k=0;k<bindings.size()&&used<sizeof list-24;k++)
                    used+=size_t(snprintf(list+used,sizeof list-used," %u:t%u%s",bindings[k].binding,
                        unsigned(bindings[k].descriptorType),bindings[k].pImmutableSamplers?"(imm)":""));
                ot2_host_die("Invalid cached descriptor layout:%s",list);
            }
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount=bindings.size();li.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device,&li,nullptr,&entry.set_layout),"cached descriptor layout");
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount=1;pi.pSetLayouts=&entry.set_layout;
        check(vkCreatePipelineLayout(device,&pi,nullptr,&entry.pipeline_layout),"cached pipeline layout");
        std::map<VkDescriptorType,uint32_t> counts;
        for(const auto& binding:bindings)counts[binding.descriptorType]+=binding.descriptorCount;
        for(auto [type,count]:counts)entry.sizes.push_back({type,count});
        ++descriptor_layout_creates;
    }
    selected=&entry;
    if(reuse_layout_inputs&&!input_key.empty())ordered=&descriptor_layout_inputs.emplace(input_key,DescriptorLayoutInput{selected}).first->second;
    }
    if(use_descriptor_templates&&ordered&&update){
        if(!ordered->update){
            std::vector<VkDescriptorUpdateTemplateEntry> entries;entries.reserve(input.size());
            for(size_t i=0;i<input.size();++i){const auto& binding=input[i];
                if(binding.descriptorCount!=1)ot2_host_die("Descriptor update payload requires one resource per binding");
                entries.push_back({binding.binding,0,1,binding.descriptorType,i*sizeof(DescriptorUpdateData),0});
            }
            VkDescriptorUpdateTemplateCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
            ci.descriptorUpdateEntryCount=entries.size();ci.pDescriptorUpdateEntries=entries.data();
            ci.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;ci.descriptorSetLayout=selected->set_layout;
            check(vkCreateDescriptorUpdateTemplate(device,&ci,nullptr,&ordered->update),"descriptor update template");
            ++descriptor_template_creates;
        }
        *update=ordered->update;
    }
    auto& entry=*selected;
    const bool preserve=retain_descriptor_working_set&&retain_descriptor_contents&&contents;
    // Keep other useful contents until the lazy global budget is full. The
    // previous LIFO policy overwrote tomorrow's hits on every contents miss.
    // Count pending leases too: retention cannot grow unchecked behind fences.
    if(!entry.idle.empty()&&(!preserve||!entry.idle.back()->contents_indexed||descriptor_live_slots>=max_idle_descriptors)){
        auto* slot=preserve&&entry.idle.back()->contents_indexed?entry.idle.front():entry.idle.back();
        entry.idle.erase(slot);
        idle_descriptors.erase(slot);remove_descriptor_contents(slot);slot->contents.clear();slot->contents_epoch=0;if(!contents)slot->values_epoch=0;++descriptor_reuses;return slot;
    }
    if(entry.available_blocks.empty()){
        auto result=create_descriptor_block(entry,64);
        if(result==VK_ERROR_OUT_OF_HOST_MEMORY||result==VK_ERROR_OUT_OF_DEVICE_MEMORY||result==VK_ERROR_OUT_OF_POOL_MEMORY||result==VK_ERROR_FRAGMENTED_POOL){
            ++descriptor_allocation_retries;
            ot2_host_log("DESCRIPTOR_ALLOCATION_PRESSURE result=%d live=%zu blocks=%zu",result,descriptor_live_slots,descriptor_blocks.size());
            cache_allocation_pressure();
            // Reclaim retired slots, never pending sets. Retry once with the
            // smallest block if no existing block became available.
            max_idle_descriptors=0;trim_descriptors();max_idle_descriptors=cache_budget.descriptors;
            result=entry.available_blocks.empty()?create_descriptor_block(entry,1):VK_SUCCESS;
        }
        check(result,"cached descriptor block");
    }
    auto* block=entry.available_blocks.back();auto* slot=new DescriptorSlot{&entry};slot->block=block;
    slot->set=block->free_sets.back();block->free_sets.pop_back();++block->live;
    if(block->free_sets.empty())entry.available_blocks.pop_back();
    ++descriptor_slot_creates;++descriptor_live_slots;return slot;
}
void trim_descriptors() {
    // Only already-retired leases are candidates, including when a diagnostic
    // lowers the budget. Never destroy resources referenced by pending work.
    while(idle_descriptors.size()>max_idle_descriptors){
        auto* oldest=idle_descriptors.front();idle_descriptors.pop_front();
        auto& slots=oldest->owner->idle;
        slots.erase(oldest);
        remove_descriptor_contents(oldest);destroy_descriptor_slot(oldest);
    }
}
void release_descriptors(DescriptorSlot* slot) {
    slot->owner->idle.push_back(slot);
    idle_descriptors.push_back(slot);
    if(retain_descriptor_contents&&!slot->contents.empty()&&slot->contents_epoch==descriptor_contents_epoch){
        if(slot->contents_node.empty()){idle_descriptor_contents.emplace(std::string_view(slot->contents),slot);++descriptor_contents_node_creates;}
        else {slot->contents_node.key()=slot->contents;slot->contents_node.mapped()=slot;idle_descriptor_contents.insert(std::move(slot->contents_node));}
        slot->contents_indexed=true;
    }
    trim_descriptors();
}
void report_descriptors() {
    stats_log("DESCRIPTOR_CACHE enabled=%u layouts=%zu idle=%zu layout_creates=%llu pool_creates=%llu reuses=%llu limit=%zu pool_destroys=%llu blocks=%zu allocation_retries=%llu",reuse_descriptors,descriptor_layouts.size(),idle_descriptors.size(),(unsigned long long)descriptor_layout_creates,(unsigned long long)descriptor_pool_creates,(unsigned long long)descriptor_reuses,max_idle_descriptors,(unsigned long long)descriptor_pool_destroys,descriptor_blocks.size(),(unsigned long long)descriptor_allocation_retries);
    trim_descriptors();
}
