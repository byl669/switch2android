// nn::pl shared font.
//
// The title asks the pl service for the console's shared font, waits for it to
// report loaded, then hands the data to the SDK's own font engine. This host has
// no pl service, so the request never completes and a title that waits for it
// stalls in a 100 ms poll (observed at main+0x47ad20, which loops on
// nn::pl::GetSharedFontLoadState until it returns 1).
//
// The data the console ships is a TTF behind an 8-byte BFTTF header. Titles that
// import nn::fontll::ScalableFontEngineHelper::Decode -- this one does -- receive
// the header and decode it themselves, so the header has to be built here exactly
// as a real console presents it:
//
//   +0x00  u32  "already decoded" marker 0x18029a7f
//   +0x04  u32  obfuscated length: bswap32(ttf_bytes ^ 0x49621806)
//   +0x08  ...  the TTF itself
//
// The TTF comes from a real face, but "a font is a font" is only true up to
// coverage and it cost this port its Chinese dialogue to learn that. A missing
// glyph here is not a cosmetic substitution: this title's engine draws its own
// notdef, which is *empty*, so the character disappears while its advance is
// kept -- "嗨！嗨！" reads as "！ ！" and "最近，大脑有点……？" loses the 有 and
// the ellipsis. The console never shows that because the shared font is a full
// CJK face. tools/make-shared-font.sh builds the substitute: GB2312 + the most
// common GBK-only hanzi by frequency + the U+2000..U+206F punctuation block, at
// ~2.2 MB. Two things about it are load-bearing and look arbitrary:
//
//   - Basic Latin U+0020..U+007E has to be in it. Without ASCII the engine dies
//     with a null dereference at 0x38 while it is still bringing the face up,
//     right after nvnTextureBuilderSetStride; it is the space and the digits it
//     looks up first.
//   - It cannot simply be a full GBK face. Past ~5 MB the engine's allocation
//     comes back empty and the same 0x38 crash follows (11.3 MB and 5.1 MB both
//     reproduce it, 2.2 MB boots).
//
// What is still not explained: some dialogue lines drop characters that the
// face definitely has, stably, while UI labels and other lines render whole.
// That is a separate defect from coverage and is not addressed here.
//
// One type is the exception and cannot come from a substituted face at all: pl
// type 5, the "Nintendo extension", is where the controller-button icons live.
// Game text refers to them by private-use code point (U+E0E0 = A, U+E0B5 = +,
// U+E0EA = D-pad), so no device face has a glyph to substitute and the engine's
// notdef stands in -- that is the "?" in "? 开始" and in every button prompt
// along the bottom of the menu. That face is carried in the library
// (nintendo_ext_font.h).
#include "host_services.h"
#include "nintendo_ext_font.h"
#include "thread_services.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ot2 {
namespace {
using R = uint64_t*;

struct Font {
    uint8_t* base{};   // BFTTF header + TTF
    size_t total{};    // header + TTF
};
// Two faces, because the console ships six and they are not interchangeable:
// types 0..4 (standard, the three CJK ones, Korean) all answer with the device's
// own face, but type 5 is the Nintendo extension -- the only place the button
// icons live. Serving type 5 from the standard face is exactly the state the
// console never has: every prompt glyph in U+E090..U+E102 misses and the engine
// draws its notdef, which is the "?" in "? 开始".
Font font;
Font ext_font;
std::once_flag font_once;

// Ordinary TrueType faces an Android device is expected to have. A collection
// (.ttc) is deliberately not in this list: the engine's face lookup is built for
// a single-face file, and every CJK face Android ships is a .ttc -- which is why
// the CJK coverage comes from a single-face file the operator puts in the title's
// user directory (see font_candidates below) rather than from the system paths.
//
// Order matters, and the first system entry is the one that has been measured to
// reach the title's frame loop. The face that actually covers this title's
// Chinese is the one tools/make-shared-font.sh puts in the user directory
// (~2.2 MB); the system faces above it cover Latin only and are a last resort. A
// full CJK face does not work here at any size that matters: 8 MB, 5.1 MB and
// 11.3 MB all leave the engine with nothing to allocate, which surfaces as the
// null dereference at 0x38 described at the top of this file.
const char* const font_paths[] = {
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
    "/system/fonts/NotoSans-Regular.ttf",
    "/system/fonts/RobotoCondensed-Regular.ttf",
};

// A path from a marker file is a convenience for adb; the authoritative override
// is a real file in the game's work directory, which survives an app restart and
// needs no shell access to install.
std::string marker_path(const char* name) {
    std::ifstream in(name);
    std::string path;
    if (in) std::getline(in, path);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) path.pop_back();
    return path;
}

std::string game_file(const char* name) {
    const std::string directory = ot2_game_directory();
    return directory.empty() ? std::string() : directory + "/" + name;
}

void add_if_present(std::vector<std::string>& out, const std::string& path) {
    if (!path.empty()) out.push_back(path);
}

// Standard face candidates, most specific first. The game directory wins so that a
// CJK-capable face can be installed on the device without touching the library.
std::vector<std::string> font_candidates() {
    std::vector<std::string> out;
    if (const char* env = getenv("OT2_SHARED_FONT"); env && *env) out.emplace_back(env);
    add_if_present(out, game_file("shared-font.otf"));
    add_if_present(out, game_file("shared-font.ttf"));
    add_if_present(out, marker_path("shared-font"));
    for (const char* path : font_paths) out.emplace_back(path);
    return out;
}

// Writes the 8-byte BFTTF header in front of a mapping that already holds the
// TTF at +8 and hands the pair back as the font. The mapping stays readable for
// the whole run: the address handed to the title is the header, not a copy.
Font wrap_mapping(void* mapping, size_t ttf_bytes, const char* label) {
    auto* p = static_cast<uint8_t*>(mapping);
    const uint32_t marker = 0x18029a7fu;
    const uint32_t length = __builtin_bswap32(uint32_t(ttf_bytes) ^ 0x49621806u);
    memcpy(p, &marker, 4);
    memcpy(p + 4, &length, 4);
    mprotect(mapping, ttf_bytes + 8, PROT_READ);
    ot2_host_log("SHARED_FONT %s (%zu byte TTF behind the BFTTF header)", label, ttf_bytes);
    return Font{p, ttf_bytes + 8};
}

Font map_file(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return {}; }
    const size_t bytes = size_t(st.st_size);
    // Anonymous, read-only afterwards: the pointer handed to the title has to
    // stay valid for the whole run, and the header is written once here.
    void* mapping = mmap(nullptr, bytes + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) { close(fd); return {}; }
    auto* p = static_cast<uint8_t*>(mapping);
    size_t got = 0;
    while (got < bytes) {
        const ssize_t n = pread(fd, p + 8 + got, bytes - got, off_t(got));
        if (n <= 0) break;
        got += size_t(n);
    }
    close(fd);
    if (got != bytes) {
        ot2_host_log("SHARED_FONT read short: %s (%zu / %zu)", path, got, bytes);
        munmap(mapping, bytes + 8);
        return {};
    }
    return wrap_mapping(mapping, bytes, path);
}

// The extension face is carried in the library: the device has no Nintendo
// fonts, and the icons it provides are the one thing no device face can stand in
// for. A file still wins when one is named, so the artwork can be swapped
// without rebuilding.
Font map_bytes(const unsigned char* data, size_t bytes, const char* label) {
    if (!data || bytes == 0) return {};
    void* mapping = mmap(nullptr, bytes + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return {};
    memcpy(static_cast<uint8_t*>(mapping) + 8, data, bytes);
    return wrap_mapping(mapping, bytes, label);
}

std::string ext_override_path() {
    // OT2_SHARED_FONT_EXT is for adb-driven experiments; a file in the game
    // directory is the durable switch, the marker file the legacy one.
    if (const char* env = getenv("OT2_SHARED_FONT_EXT"); env && *env) return env;
    const std::string path = game_file("nintendo_ext.ttf");
    if (!path.empty() && access(path.c_str(), R_OK) == 0) return path;
    return marker_path("shared-font-ext");
}

void build_font() {
    for (const std::string& path : font_candidates()) {
        font = map_file(path.c_str());
        if (font.base) break;
    }
    if (!font.base)
        ot2_host_log("SHARED_FONT no usable TTF found; the title will wait for a font that never arrives");

    const std::string ext_path = ext_override_path();
    if (!ext_path.empty()) ext_font = map_file(ext_path.c_str());
    if (!ext_font.base) ext_font = map_bytes(kNintendoExtFont, kNintendoExtFontSize, "built-in Nintendo extension");
    if (!ext_font.base)
        ot2_host_log("SHARED_FONT no Nintendo extension face; button icons will draw as \"?\"");
}

// type 5 is the Nintendo extension; everything else shares the standard face.
inline Font& get(int type) {
    std::call_once(font_once, build_font);
    return type == 5 && ext_font.base ? ext_font : font;
}
} // namespace

// The console ships the shared font as BFTTF: an 8-byte header (a "decoded"
// marker plus the obfuscated length) followed by the obfuscated TTF. A title that
// imports nn::fontll::ScalableFontEngineHelper::Decode gets the header and calls
// Decode itself, which deobfuscates in place and answers address + 8. A title that
// does not import it -- Unity's player is one -- hands whatever it is given
// straight to the font engine, which expects a TTF: the header then makes every
// face fail to load, every glyph lookup miss and every text measurement come back
// zero. A zero measurement is not cosmetic: the UI is laid out from it, so panels
// and glyph quads end up with no width at all and nothing reaches the screen.
static std::atomic<int> decodes_helper{-1};
void set_shared_font_decodes(bool decodes) {
    decodes_helper.store(decodes ? 1 : 0, std::memory_order_release);
    ot2_host_log("SHARED_FONT title %s ScalableFontEngineHelper::Decode, so it gets %s",
                 decodes ? "imports" : "does not import",
                 decodes ? "the BFTTF header" : "the TTF itself");
}
bool shared_font_decodes() {
    return decodes_helper.load(std::memory_order_acquire) > 0;
}
namespace {
// Bytes at the front of the buffer that the title must not be shown.
size_t header_size() {
    const int state = decodes_helper.load(std::memory_order_acquire);
    if (state < 0) return 8;  // never decided: the TTF itself is the safe answer
    return state ? 0 : 8;
}
} // namespace

const std::vector<HostService>& shared_font_services() {
    static const std::vector<HostService> table = {
        // RequestSharedFontLoad(type): on a console this starts an asynchronous
        // load. Here the data is available on demand, so the follow-up state query
        // answers immediately and nothing is left pending.
        {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", [](R r) -> uint64_t {
            const Font& f = get(int(r[0]));
            ot2_host_log("SHARED_FONT request type=%llu total=%zu",(unsigned long long)r[0],f.total);
            return 0;
        }},
        // GetSharedFontLoadState(type) -> 1 loaded / 0 not
        {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", [](R r) -> uint64_t {
            return get(int(r[0])).base ? 1 : 0;
        }},
        // GetSharedFontAddress(type) -> the BFTTF header when the title decodes it
        // itself, otherwise the TTF past that header
        {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", [](R r) -> uint64_t {
            const Font& f = get(int(r[0]));
            return f.base ? reinterpret_cast<uint64_t>(f.base + header_size()) : 0;
        }},
        // GetSharedFontSize(type) -> whatever the address above spans
        {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", [](R r) -> uint64_t {
            const Font& f = get(int(r[0]));
            return f.total ? uint64_t(f.total - header_size()) : 0;
        }},
    };
    return table;
}

} // namespace ot2
