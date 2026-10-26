#pragma once
#include <string>
namespace ot2 {
// A title may bake its interface language into a boot script instead of asking
// the system for one. When it does, no service-side switch can reach it: the
// language query an applet would normally answer is never called, so every
// host-side language answer is dead code for that title. The RomFS is a plain
// directory here, so the single line that decides the culture can be rewritten
// where the engine reads it.
struct CultureRewrite {
    enum Status{kNoBootScript,kNoCultureCall,kUnchanged,kApplied,kWriteFailed};
    Status status=kNoBootScript;
    std::string from,to;
};
// The culture a title should boot in: the value of "<game>/content-language"
// when that file names one, "zhCN" otherwise. The markers keep/off/none/system
// opt out of the default and leave the title exactly as it shipped.
std::string content_language(const std::string& game_directory);
// Rewrites the first SetDefaultCulture("..") call in <romfs>/boot.lua. An empty
// culture is a no-op. Never throws: a missing or read-only RomFS must not stop
// a title from booting, so every failure is reported through the status.
CultureRewrite rewrite_boot_culture(const std::string& romfs_directory,const std::string& culture);
// "boot.lua=applied" and friends, for the one log line the caller emits.
const char* culture_status_name(const CultureRewrite& rewrite);
}
