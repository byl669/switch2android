#include "loader.h"
#include "third_party/lz4/lz4.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace ot2 {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void range(size_t offset, size_t count, size_t total) {
    require(offset <= total && count <= total - offset, "NSO range exceeds image");
}
template<class T> T read(const std::vector<uint8_t>& data, size_t offset) {
    range(offset, sizeof(T), data.size());
    T value;
    std::memcpy(&value, data.data()+offset, sizeof(T));
    return value;
}
struct Symbol {
    std::string name;
    uint8_t info, visibility;
    uint16_t section;
    uint64_t value;
};
struct Module {
    std::string name;
    std::vector<uint8_t> source;
    std::vector<Symbol> symbols;
    std::unordered_map<int64_t, uint64_t> dynamic;
    uint8_t* memory = nullptr;
    size_t allocation = 0, image_bytes = 0;
    size_t relocation_count = 0;
    size_t text_offset=0, text_size=0, ro_offset=0, data_offset=0;
    std::array<uint8_t,32> build_id{};
    explicit Module(std::string name_) : name(std::move(name_)) {}
    ~Module() { if (memory) munmap(memory, allocation); }
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(memory); }
    uintptr_t symbol_address(const Symbol& symbol) const {
        if (symbol.section == 0xFFF1) return symbol.value; // SHN_ABS
        require(symbol.value <= image_bytes, "Symbol address exceeds module");
        return base() + symbol.value;
    }
    void load(const std::string& directory) {
        std::ifstream file(directory + "/" + name, std::ios::binary | std::ios::ate);
        require(file.good(), "Required NSO file missing");
        const auto length = file.tellg();
        require(length >= 256 && length <= 512 * 1024 * 1024, "Invalid NSO file size");
        std::vector<uint8_t> packed(static_cast<size_t>(length));
        file.seekg(0); file.read(reinterpret_cast<char*>(packed.data()), length);
        require(file.good() && std::memcmp(packed.data(), "NSO0", 4)==0, "Invalid NSO file");
        std::memcpy(build_id.data(),packed.data()+0x40,build_id.size());
        uint32_t flags = read<uint32_t>(packed, 0xC);
        text_offset=read<uint32_t>(packed,0x14); text_size=read<uint32_t>(packed,0x18);
        ro_offset=read<uint32_t>(packed,0x24); data_offset=read<uint32_t>(packed,0x34);
        uint64_t image_size = uint64_t(read<uint32_t>(packed, 0x34)) +
                              read<uint32_t>(packed, 0x38) + read<uint32_t>(packed, 0x3C);
        require(image_size > 0 && image_size <= 512 * 1024 * 1024, "Invalid NSO image size");
        image_bytes=size_t(image_size);
        // Anonymous mappings already supply zero BSS pages. Keep only the
        // initialized image as staging; do not allocate/copy the zero tail twice.
        source.resize(uint64_t(read<uint32_t>(packed,0x34))+read<uint32_t>(packed,0x38));
        uint64_t previous_end = 0;
        for (size_t i=0; i<3; ++i) {
            uint32_t file_offset=read<uint32_t>(packed, 0x10+i*16);
            uint32_t offset=read<uint32_t>(packed, 0x14+i*16);
            uint32_t size=read<uint32_t>(packed, 0x18+i*16);
            uint32_t stored=(flags & (1U<<i)) ? read<uint32_t>(packed, 0x60+i*4) : size;
            require(offset >= previous_end, "NSO sections overlap");
            previous_end = uint64_t(offset) + size;
            range(file_offset, stored, packed.size()); range(offset, size, source.size());
            if (flags & (1U<<i)) {
                auto count=LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()+file_offset),
                                               reinterpret_cast<char*>(source.data()+offset), stored, size);
                require(count >= 0 && static_cast<uint32_t>(count)==size, "LZ4 decompression failed");
            } else std::memcpy(source.data()+offset, packed.data()+file_offset, size);
        }
        uint32_t ro=read<uint32_t>(packed, 0x24), ro_size=read<uint32_t>(packed, 0x28);
        uint32_t str_offset=read<uint32_t>(packed, 0x90), str_size=read<uint32_t>(packed, 0x94);
        uint32_t sym_offset=read<uint32_t>(packed, 0x98), sym_size=read<uint32_t>(packed, 0x9C);
        range(str_offset, str_size, ro_size); range(sym_offset, sym_size, ro_size);
        require(sym_size % 24 == 0, "Invalid dynamic symbol table");
        for (size_t i=0; i<sym_size/24; ++i) {
            size_t at = size_t(ro)+sym_offset+i*24;
            uint32_t name_at=read<uint32_t>(source, at);
            range(name_at, 1, str_size);
            const char* text=reinterpret_cast<const char*>(source.data()+ro+str_offset+name_at);
            const char* end=static_cast<const char*>(std::memchr(text, 0, str_size-name_at));
            require(end != nullptr, "Unterminated symbol name");
            symbols.push_back({std::string(text,end), source[at+4], uint8_t(source[at+5]&3),
                               read<uint16_t>(source,at+6), read<uint64_t>(source,at+8)});
        }
        uint32_t mod=read<uint32_t>(source,4);
        require(read<uint32_t>(source,mod)==0x30444F4D, "Missing MOD0");
        int64_t dynamic_offset=int64_t(mod)+read<int32_t>(source,mod+4);
        require(dynamic_offset>=0, "Invalid dynamic offset");
        bool terminated=false;
        for (size_t i=0; i<4096; ++i) {
            int64_t tag=read<int64_t>(source,dynamic_offset+i*16);
            if (!tag) { terminated=true; break; }
            dynamic[tag]=read<uint64_t>(source,dynamic_offset+i*16+8);
        }
        require(terminated, "Unterminated dynamic table");
        long page=sysconf(_SC_PAGESIZE);
        require(page>0, "Page size unavailable");
        allocation=((image_bytes+page-1)/page)*page;
        void* mapped=mmap(nullptr,allocation,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(mapped != MAP_FAILED, "Module mmap failed");
        memory=static_cast<uint8_t*>(mapped);
        std::memcpy(memory,source.data(),source.size());
    }
};
struct Export { Module* module; const Symbol* symbol; };

std::string sdk_leaf_probe(const std::unordered_map<std::string,Export>& exports) {
#if defined(__ANDROID__) && defined(__aarch64__)
    auto found=exports.find("_ZN2nn2os22GetSystemTickFrequencyEv");
    require(found!=exports.end(), "SDK frequency symbol missing");
    const auto& m=*found->second.module;
    const auto at=found->second.symbol->value;
    // Decode the complete leaf before executing. Permit only MOVZ W0, MOVK W0,
    // RET; no memory accesses, branches, TLS, SVC, x18 or external calls.
    auto low=read<uint32_t>(m.source,at), high=read<uint32_t>(m.source,at+4);
    require((low&0xFFE0001F)==0x52800000 && (high&0xFFE0001F)==0x72A00000 &&
            read<uint32_t>(m.source,at+8)==0xD65F03C0, "SDK leaf is not the verified instruction shape");
    uint32_t expected=((low>>5)&0xFFFF) | (((high>>5)&0xFFFF)<<16);
    require(expected==19200000, "SDK timer frequency differs from expected ABI");
    size_t page=sysconf(_SC_PAGESIZE);
    void* code=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    require(code!=MAP_FAILED,"SDK leaf allocation failed");
    std::memcpy(code,m.source.data()+at,12);
    __builtin___clear_cache(static_cast<char*>(code),static_cast<char*>(code)+12);
    if(mprotect(code,page,PROT_READ|PROT_EXEC)!=0) {
        munmap(code,page); throw std::runtime_error("SDK leaf executable protection failed");
    }
    auto value=reinterpret_cast<uint32_t(*)()>(code)();
    munmap(code,page);
    require(value==expected,"SDK leaf returned unexpected value");
    return "{\"symbol\":\"nn::os::GetSystemTickFrequency\",\"direct_execution\":true,\"returned_hz\":"+
           std::to_string(value)+"}";
#else
    (void)exports;
    return "null";
#endif
}
}

std::vector<std::string> discover_modules(const std::string& directory) {
    std::vector<std::pair<unsigned,std::string>> found;
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {
        if(!entry.is_regular_file())continue;
        std::ifstream input(entry.path(),std::ios::binary);char magic[4]{};input.read(magic,4);
        if(input.gcount()!=4||std::memcmp(magic,"NSO0",4))continue;
        auto name=entry.path().filename().string();unsigned order;
        if(name=="rtld")order=0;
        else if(name=="main")order=1;
        else if(name=="sdk")order=12;
        else if(name.size()==7&&name.starts_with("subsdk")&&name.back()>='0'&&name.back()<='9')order=2+unsigned(name.back()-'0');
        else throw std::runtime_error("Unsupported NSO module name: "+name);
        require(std::filesystem::canonical(entry.path()).parent_path()==std::filesystem::canonical(directory),"NSO module escapes ExeFS");
        found.emplace_back(order,std::move(name));
    }
    std::sort(found.begin(),found.end());
    require(std::any_of(found.begin(),found.end(),[](const auto& e){return e.second=="main";}),"Required main NSO missing");
    std::vector<std::string> names;for(auto& e:found)names.push_back(std::move(e.second));return names;
}

struct LoadedGame::Impl {
    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::string,Export> exports;
    explicit Impl(const std::string& directory) {
    for (const auto& name : discover_modules(directory)) {
        auto module=std::make_unique<Module>(name);
        module->load(directory);
        modules.push_back(std::move(module));
    }
    for (auto& module:modules) for (auto& symbol:module->symbols) {
        auto binding=symbol.info>>4;
        if (symbol.section==0 || symbol.name.empty() || (binding!=1 && binding!=2) || symbol.visibility==2) continue;
        auto old=exports.find(symbol.name);
        if (old==exports.end() || ((old->second.symbol->info>>4)==2 && binding==1))
            exports[symbol.name]={module.get(), &symbol};
    }
    for (auto& module:modules) {
        for (const auto& tags: {std::pair<int64_t,int64_t>{7,8}, {23,2}}) {
            if (!module->dynamic.contains(tags.first)) continue;
            auto offset=module->dynamic.at(tags.first);
            auto size=module->dynamic.at(tags.second);
            require(size%24==0, "Invalid RELA size");
            range(offset,size,module->source.size());
            for (size_t at=offset; at<offset+size; at+=24) {
                auto destination=read<uint64_t>(module->source,at);
                auto info=read<uint64_t>(module->source,at+8);
                auto addend=read<int64_t>(module->source,at+16);
                uint32_t type=info&0xFFFFFFFF, index=info>>32;
                range(destination,8,module->image_bytes);
                uintptr_t value=0;
                if (type==1027) {
                    require(index==0 && addend>=0 && uint64_t(addend)<=module->image_bytes, "Invalid RELATIVE relocation");
                    value=module->base()+addend;
                } else if (type==257 || type==1025 || type==1026) {
                    require(index<module->symbols.size(), "Relocation symbol index invalid");
                    const auto& symbol=module->symbols[index];
                    uintptr_t address=0;
                    if (symbol.section!=0 && ((symbol.info>>4)==0 || symbol.visibility==2 || symbol.visibility==3)) {
                        address=module->symbol_address(symbol);
                    } else if (auto found=exports.find(symbol.name); found!=exports.end()) {
                        address=found->second.module->symbol_address(*found->second.symbol);
                    } else if (symbol.section!=0) {
                        address=module->symbol_address(symbol);
                    } else require((symbol.info>>4)==2 || index==0, "Unresolved strong import");
                    value=address+addend;
                } else throw std::runtime_error("Unsupported AArch64 relocation");
                std::memcpy(module->memory+destination,&value,8);
                ++module->relocation_count;
            }
        }

    }
    for(auto& module:modules)std::vector<uint8_t>().swap(module->source);
    }
};
LoadedGame::LoadedGame(const std::string& directory) : impl(std::make_unique<Impl>(directory)) {}
LoadedGame::~LoadedGame() = default;
uintptr_t LoadedGame::symbol(const std::string& name) const {
    auto it=impl->exports.find(name);
    return it==impl->exports.end() ? 0 : it->second.module->symbol_address(*it->second.symbol);
}
std::vector<std::string> LoadedGame::imported_symbols(const std::string& module) const {
    std::vector<std::string> result;
    for (const auto& m:impl->modules) {
        if (m->name!=module) continue;
        for (const auto& symbol:m->symbols) if (symbol.section==0 && !symbol.name.empty()) result.push_back(symbol.name);
        break;
    }
    return result;
}
bool LoadedGame::imports(const std::string& module, const std::string& prefix) const {
    for (const auto& m:impl->modules) {
        if (m->name!=module) continue;
        for (const auto& symbol:m->symbols)
            if (symbol.section==0 && symbol.name.size()>=prefix.size() && symbol.name.compare(0,prefix.size(),prefix)==0) return true;
        return false;
    }
    return false;
}
std::vector<ModuleView> LoadedGame::modules() const {
    std::vector<ModuleView> result;
    for (const auto& m:impl->modules) result.push_back({m->name,m->memory,m->allocation,m->text_offset,m->text_size,m->ro_offset,m->data_offset,m->build_id});
    return result;
}
std::vector<uintptr_t> LoadedGame::initializers() const {
    std::vector<uintptr_t> result;
    // rtld's initialization callback uses module dependency order: SDK, middleware, application.
    for (auto it=impl->modules.rbegin();it!=impl->modules.rend();++it) {
        const auto& m=**it;
        if (m.name=="rtld") continue;
        if (auto f=m.dynamic.find(12); f!=m.dynamic.end() && f->second) result.push_back(m.base()+f->second);
    }
    return result;
}
void LoadedGame::make_executable() {
    for (const auto& m:impl->modules) {
        require(mprotect(m->memory,m->allocation,PROT_READ|PROT_WRITE)==0,"Module data protection failed");
        const size_t page=sysconf(_SC_PAGESIZE);
        const size_t end=(m->text_offset+m->text_size+page-1)&~(page-1);
        require(m->text_offset%page==0 && end<=m->ro_offset,"Text page alignment overlaps rodata");
        __builtin___clear_cache(reinterpret_cast<char*>(m->memory),reinterpret_cast<char*>(m->memory+end));
        require(mprotect(m->memory+m->text_offset,end-m->text_offset,PROT_READ|PROT_EXEC)==0,"Module executable protection failed");
        require(mprotect(m->memory+m->ro_offset,m->data_offset-m->ro_offset,PROT_READ)==0,"Module rodata protection failed");
    }
}

std::string inspect_modules(const std::string& directory) {
    std::vector<std::unique_ptr<Module>> modules;
    for (const auto& name : discover_modules(directory)) {
        auto module=std::make_unique<Module>(name);
        module->load(directory);
        modules.push_back(std::move(module));
    }
    std::unordered_map<std::string,Export> exports;
    for (auto& module:modules) for (auto& symbol:module->symbols) {
        auto binding=symbol.info>>4;
        if (symbol.section==0 || symbol.name.empty() || (binding!=1 && binding!=2) || symbol.visibility==2) continue;
        auto old=exports.find(symbol.name);
        if (old==exports.end() || ((old->second.symbol->info>>4)==2 && binding==1))
            exports[symbol.name]={module.get(), &symbol};
    }
    size_t total=0;
    for (auto& module:modules) {
        for (const auto& tags: {std::pair<int64_t,int64_t>{7,8}, {23,2}}) {
            if (!module->dynamic.contains(tags.first)) continue;
            auto offset=module->dynamic.at(tags.first);
            auto size=module->dynamic.at(tags.second);
            require(size%24==0, "Invalid RELA size");
            range(offset,size,module->source.size());
            for (size_t at=offset; at<offset+size; at+=24) {
                auto destination=read<uint64_t>(module->source,at);
                auto info=read<uint64_t>(module->source,at+8);
                auto addend=read<int64_t>(module->source,at+16);
                uint32_t type=info&0xFFFFFFFF, index=info>>32;
                range(destination,8,module->image_bytes);
                uintptr_t value=0;
                if (type==1027) {
                    require(index==0 && addend>=0 && uint64_t(addend)<=module->image_bytes, "Invalid RELATIVE relocation");
                    value=module->base()+addend;
                } else if (type==257 || type==1025 || type==1026) {
                    require(index<module->symbols.size(), "Relocation symbol index invalid");
                    const auto& symbol=module->symbols[index];
                    uintptr_t address=0;
                    if (symbol.section!=0 && ((symbol.info>>4)==0 || symbol.visibility==2 || symbol.visibility==3)) {
                        address=module->symbol_address(symbol);
                    } else if (auto found=exports.find(symbol.name); found!=exports.end()) {
                        address=found->second.module->symbol_address(*found->second.symbol);
                    } else if (symbol.section!=0) {
                        address=module->symbol_address(symbol);
                    } else require((symbol.info>>4)==2 || index==0, "Unresolved strong import");
                    value=address+addend;
                } else throw std::runtime_error("Unsupported AArch64 relocation");
                std::memcpy(module->memory+destination,&value,8);
                ++module->relocation_count;
            }
        }
        total+=module->relocation_count;
    }
    // No PROT_EXEC on the modules: guest TLS, x18 and SVC adaptation is incomplete.
    for (auto& module:modules)
        require(mprotect(module->memory,module->allocation,PROT_READ)==0, "Module read-only protection failed");
    std::ostringstream out;
    out << "{\"status\":\"mapped_and_relocated\",\"entrypoint_executed\":false,\"executable\":false,\"relocations\":" << total << ",\"modules\":[";
    for (size_t i=0;i<modules.size();++i) {
        if (i) out << ',';
        auto& m=modules[i];
        out << "{\"name\":\"" << m->name << "\",\"bytes\":" << m->allocation
            << ",\"relocations\":" << m->relocation_count << '}';
    }
    out << "],\"sdk_leaf\":" << sdk_leaf_probe(exports) << '}';
    return out.str();
}
}
