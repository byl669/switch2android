#pragma once
#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <array>

namespace ot2 {
// Platform module names in rtld/main/subsdkN/sdk dependency order. Metadata
// files are ignored; unexpected NSO names fail instead of silently disappearing.
std::vector<std::string> discover_modules(const std::string& directory);
// Maps and links actual modules, then freezes all memory READ-ONLY and releases it.
// Android additionally executes one validated, isolated SDK constant-return leaf.
// No constructors or game entry points are executed by this probe.
std::string inspect_modules(const std::string& directory);
struct ModuleView {
    std::string name;
    uint8_t* base;
    size_t size;
    size_t text_offset;
    size_t text_size;
    size_t ro_offset;
    size_t data_offset;
    std::array<uint8_t,32> build_id{};
};
class LoadedGame {
public:
    explicit LoadedGame(const std::string& directory);
    ~LoadedGame();
    uintptr_t symbol(const std::string& name) const;
    // Whether the named module imports a symbol whose name starts with prefix.
    // A title's own code decides some host policy by which SDK entry it links
    // against, and that decision is made before the first call, so the answer
    // has to come from the module's symbol table rather than from a guess.
    bool imports(const std::string& module, const std::string& prefix) const;
    std::vector<std::string> imported_symbols(const std::string& module) const;
    std::vector<ModuleView> modules() const;
    std::vector<uintptr_t> initializers() const;
    void make_executable();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
