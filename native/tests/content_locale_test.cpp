#include "../content_locale.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs=std::filesystem;

std::string read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}

void write(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(text.data(),static_cast<std::streamsize>(text.size()));
}

// The real script starts with a UTF-8 BOM and is wrapped in engine setup calls;
// the rewrite must touch the culture literal and not one byte more.
const std::string kScript="\xEF\xBB\xBF-- boot\nlocal engine = require 'BezelEngineInitializer'\nengine:SetDefaultCulture(\"jaJP\")\nengine:SetBaseResolution(1280, 720);\nengine:Initialize()\n";

fs::path fresh(const char* name) {
    const fs::path root=fs::temp_directory_path()/("ot2-content-locale-"+std::string(name));
    std::error_code ignored;fs::remove_all(root,ignored);
    fs::create_directories(root/"RomFS");
    return root;
}
}

int main() {
    using namespace ot2;

    // A title without a boot script is the common case: nothing to do, no error.
    {
        const fs::path root=fresh("noscript");
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(rewrite.status==CultureRewrite::kNoBootScript);
        fs::remove_all(root);
    }
    // A script that never sets a culture keeps whatever the engine defaults to.
    {
        const fs::path root=fresh("nocall");
        write(root/"RomFS"/"boot.lua","print 'hello'\n");
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(rewrite.status==CultureRewrite::kNoCultureCall);
        assert(read(root/"RomFS"/"boot.lua")=="print 'hello'\n");
        fs::remove_all(root);
    }
    // The rewrite replaces exactly the literal, keeping the BOM and every other byte.
    {
        const fs::path root=fresh("apply");
        write(root/"RomFS"/"boot.lua",kScript);
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(rewrite.status==CultureRewrite::kApplied&&rewrite.from=="jaJP"&&rewrite.to=="zhCN");
        std::string expected=kScript;expected.replace(expected.find("jaJP"),4,"zhCN");
        assert(read(root/"RomFS"/"boot.lua")==expected);
        assert(expected.size()==kScript.size()); // same length: nothing shifts
        assert(!fs::exists(root/"RomFS"/"boot.lua.culture")); // no staging left behind
        // Every boot runs this again, so the second pass must report and touch nothing.
        const auto again=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(again.status==CultureRewrite::kUnchanged&&again.from=="zhCN");
        assert(read(root/"RomFS"/"boot.lua")==expected);
        fs::remove_all(root);
    }
    // An empty culture is how the opt-out marker reaches this layer.
    {
        const fs::path root=fresh("empty");
        write(root/"RomFS"/"boot.lua",kScript);
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"");
        assert(rewrite.status==CultureRewrite::kNoBootScript);
        assert(read(root/"RomFS"/"boot.lua")==kScript);
        fs::remove_all(root);
    }
    // A literal longer than any culture code is not a culture; guessing would risk
    // corrupting an unrelated call that happens to share the name.
    {
        const fs::path root=fresh("long");
        write(root/"RomFS"/"boot.lua","engine:SetDefaultCulture(\"not-a-culture-code\")\n");
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(rewrite.status==CultureRewrite::kNoCultureCall);
        assert(read(root/"RomFS"/"boot.lua")=="engine:SetDefaultCulture(\"not-a-culture-code\")\n");
        fs::remove_all(root);
    }
    // An unwritable RomFS has to degrade to a log line, never to a failed boot.
    {
        const fs::path root=fresh("readonly");
        write(root/"RomFS"/"boot.lua",kScript);
        std::error_code error;
        fs::permissions(root/"RomFS",fs::perms::owner_read|fs::perms::owner_exec,fs::perm_options::replace,error);
        const auto rewrite=rewrite_boot_culture((root/"RomFS").string(),"zhCN");
        assert(rewrite.status==CultureRewrite::kWriteFailed&&rewrite.from=="jaJP");
        fs::permissions(root/"RomFS",fs::perms::owner_all,fs::perm_options::replace,error);
        assert(read(root/"RomFS"/"boot.lua")==kScript);
        fs::remove_all(root);
    }
    // The marker names the culture; without one the port asks for Simplified Chinese.
    {
        const fs::path root=fresh("marker");
        assert(content_language(root.string())=="zhCN");
        write(root/"content-language","  zh-Hant\n");
        assert(content_language(root.string())=="zh-Hant");
        for(const char* opt_out:{"keep","off","none","system","KEEP\n"}) {
            write(root/"content-language",opt_out);
            assert(content_language(root.string()).empty());
        }
        write(root/"content-language","\n");
        assert(content_language(root.string())=="zhCN");
        fs::remove_all(root);
    }
    puts("PASS boot.lua culture rewrite: no-script/no-call/apply/idempotent/opt-out/unknown-literal/read-only, marker default");
}
