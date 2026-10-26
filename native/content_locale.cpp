#include "content_locale.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace {
constexpr const char* kCall="SetDefaultCulture";
constexpr const char* kBootScript="boot.lua";
constexpr const char* kMarker="content-language";
constexpr const char* kDefaultCulture="zhCN";
// Longest culture code the engine knows is "zh-Hant" (7); anything longer is not
// a culture literal and is left alone rather than guessed at.
constexpr std::size_t kLongestCulture=8;

std::string trimmed(std::string text) {
    const auto visible=[](unsigned char c){return !std::isspace(c);};
    text.erase(text.begin(),std::find_if(text.begin(),text.end(),visible));
    text.erase(std::find_if(text.rbegin(),text.rend(),visible).base(),text.end());
    return text;
}

std::string lowered(const std::string& text) {
    std::string out=text;
    for(char& c:out)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// SetDefaultCulture("jaJP") -> the byte range of the culture literal. Only the
// first call is located: it is the one the engine initialises with, and a later
// call is a runtime language switch the title itself owns.
bool locate_culture(const std::string& text,std::size_t& begin,std::size_t& end) {
    const std::size_t call=text.find(kCall);
    if(call==std::string::npos)return false;
    const std::size_t open=text.find('"',call+std::strlen(kCall));
    if(open==std::string::npos)return false;
    const std::size_t close=text.find('"',open+1);
    if(close==std::string::npos||close-open-1>kLongestCulture)return false;
    begin=open+1;end=close;
    return true;
}
}

namespace ot2 {
std::string content_language(const std::string& game_directory) {
    std::ifstream in(std::filesystem::path(game_directory)/kMarker);
    if(!in)return kDefaultCulture;
    std::string value;std::getline(in,value);value=trimmed(value);
    if(value.empty())return kDefaultCulture;
    const std::string keyword=lowered(value);
    if(keyword=="keep"||keyword=="off"||keyword=="none"||keyword=="system")return {};
    return value;
}

CultureRewrite rewrite_boot_culture(const std::string& romfs_directory,const std::string& culture) {
    if(culture.empty())return CultureRewrite{};
    const std::filesystem::path script=std::filesystem::path(romfs_directory)/kBootScript;
    std::error_code error;
    if(!std::filesystem::is_regular_file(script,error))return CultureRewrite{};
    std::ifstream input(script,std::ios::binary);
    if(!input)return {CultureRewrite::kWriteFailed,{},{}};
    std::string text{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    input.close();
    std::size_t begin=0,end=0;
    if(!locate_culture(text,begin,end))return {CultureRewrite::kNoCultureCall,{},{}};
    const std::string from=text.substr(begin,end-begin);
    if(from==culture)return {CultureRewrite::kUnchanged,from,culture};
    text.replace(begin,end-begin,culture);
    // Written beside the script and renamed over it, so an interrupted write
    // cannot leave a truncated boot script that the engine would then run.
    const std::filesystem::path staging=std::filesystem::path(script).concat(".culture");
    {
        std::ofstream output(staging,std::ios::binary|std::ios::trunc);
        if(!output)return {CultureRewrite::kWriteFailed,from,culture};
        output.write(text.data(),static_cast<std::streamsize>(text.size()));
        if(!output)return {CultureRewrite::kWriteFailed,from,culture};
    }
    std::filesystem::rename(staging,script,error);
    if(error){std::error_code ignored;std::filesystem::remove(staging,ignored);return {CultureRewrite::kWriteFailed,from,culture};}
    return {CultureRewrite::kApplied,from,culture};
}

const char* culture_status_name(const CultureRewrite& rewrite) {
    switch(rewrite.status) {
    case CultureRewrite::kNoBootScript:return "boot.lua=none";
    case CultureRewrite::kNoCultureCall:return "boot.lua=no-culture-call";
    case CultureRewrite::kUnchanged:return "boot.lua=unchanged";
    case CultureRewrite::kApplied:return "boot.lua=applied";
    case CultureRewrite::kWriteFailed:return "boot.lua=write-failed";
    }
    return "boot.lua=?";
}
}
