// Dedicated host services. ABI layouts follow the D2R host and the original SDK.
#include "host_services.h"
#include "text_input.h"
#include "thread_services.h"
#include "graphics/backend.h"
#include "graphics/nvn_dirty.h"
#include "save_transaction.h"
#include "time_zone.h"
#include "resource_reader.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <memory>
#include <atomic>
#include <cstdio>

namespace ot2 {
namespace {
using Registers=uint64_t*;
template<class T> T* ptr(uint64_t p) { return reinterpret_cast<T*>(p); }
const char* str(uint64_t p) { return ptr<const char>(p); }
uint64_t ok(Registers) { return 0; }
// Dump an os::SystemEventType handed to a wait. Kept for one-off inspection:
// hooking the wait entry points themselves is not viable, because answering
// "not signalled" without blocking turns the caller's loop into a spin of its
// own. The syscall path reports the same handle without that cost.
static void dump_system_event(const char* call,const void* event) {
    static std::atomic<unsigned> seen{0};
    const unsigned n=seen.fetch_add(1,std::memory_order_relaxed);
    if(n>=24) return;
    unsigned char raw[32]{};
    memcpy(raw,event,sizeof raw);
    char words[8*9+1]={};
    size_t used=0;
    uint32_t handle=0; int handle_at=-1;
    for(unsigned i=0;i<8;i++) {
        const uint32_t value=uint32_t(raw[i*4])|uint32_t(raw[i*4+1])<<8|uint32_t(raw[i*4+2])<<16|uint32_t(raw[i*4+3])<<24;
        used+=size_t(snprintf(words+used,sizeof(words)-used,"%08x ",value));
        // 0x1000..0x1fff is the range this host hands out, so a word in it is
        // the handle whichever slot the SDK keeps it in.
        if(handle_at<0 && value>=0x1000u && value<0x2000u) { handle=value; handle_at=int(i); }
    }
    ot2_host_log("SYSEVENT %s #%u at=%p handle=%#x slot=%d words=%s",
        call,n,event,handle,handle_at,words);
}
uint64_t yes(Registers) { return 1; }
std::string romfs,user;
std::map<int,std::string> addon_content;
uint64_t program_id=legacy_ot2_program_id;
// Stand-in service manager sessions: which service each handle was opened for,
// and the distinct services asked for so far.
std::mutex sm_mutex;
std::map<uint64_t,std::string> sm_sessions;
std::set<std::string> sm_requested;
std::mutex activity_mutex;bool activity_active=true,focus_notification=false;unsigned activity_focus=1;
std::chrono::steady_clock::time_point active_since=std::chrono::steady_clock::now();
uint64_t active_ns=0;
uint64_t active_time(){std::lock_guard lock(activity_mutex);return active_ns+(activity_active?std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-active_since).count():0);}

std::atomic<unsigned> save_notice{0};
void notify_save(unsigned notice,int error){save_notice.fetch_or(notice);ot2_host_log("SAVE_NOTICE kind=%u errno=%d",notice,error);}
struct Mount {std::string directory;bool read_only;std::shared_ptr<SaveTransaction> save;bool save_read_only=false;};
std::map<std::string,Mount> mounts;
std::atomic<bool> io_prefetch{true};
std::atomic<bool> stream_io{true};
std::atomic<uint64_t> io_calls{0},io_bytes{0},io_wall_ns{0},io_reused_fds{0},io_hints{0};
ResourcePrefetch& resource_prefetch(){static ResourcePrefetch queue;return queue;}
struct File {int descriptor;std::shared_ptr<SaveTransaction> save;bool resource=false;bool stream=false;uint64_t last_end=0,prefetch_end=0;std::shared_ptr<ResourceReadFile> reader;
    // Reads issued through this handle. The stream log used to count reads
    // across the whole session and print only the first six, which is why the
    // one read that matters -- the result jingle -- could never be seen: it is
    // the seventh stream the title opens. Counting per handle makes the first
    // reads of *every* stream visible, which is what a comparison against the
    // reference build needs.
    uint32_t reads=0;};
// Stream playback position probe. A screen that waits for a streamed sound to
// finish only advances when the reads walk forward through the file; if the
// offset keeps restarting at 0 the reader is re-fetching the same block on every
// audio frame and the "finished" edge never arrives. From the outside that is
// indistinguishable from a hung state machine, so the offsets are worth logging.
std::atomic<uint64_t> stream_reads{0};
std::map<uint64_t,File> files;
struct DirectoryEntry {char name[0x301];uint8_t pad0[3];int8_t type;uint8_t pad1[3];int64_t size;};
static_assert(sizeof(DirectoryEntry)==0x310);
struct Directory {std::vector<DirectoryEntry> entries;size_t position=0;};
std::map<uint64_t,Directory> directories;
struct CacheList {std::vector<int32_t> indices;size_t position=0;};
std::map<uint64_t,CacheList> cache_lists;
std::mutex filesystem_mutex;
uint64_t next_handle=0x100000;
constexpr uint32_t PathNotFound=0x202,PathExists=0x402,PermissionDenied=0x3202,IoError=0x2a2;
uint32_t error() {
    if(errno==ENOENT || errno==ENOTDIR) return PathNotFound;
    if(errno==EEXIST) return PathExists;
    if(errno==EACCES || errno==EPERM || errno==EROFS) return PermissionDenied;
    return IoError;
}
// Called with filesystem_mutex. Reject traversal before any host filesystem operation.
std::string path(const char* guest,bool write=false) {
    std::string input(guest);auto colon=input.find(':');
    if(colon==std::string::npos) return {};
    const auto it=mounts.find(input.substr(0,colon));
    if(it==mounts.end() || (write && it->second.read_only) || (it->second.save&&!it->second.save->ready())) return {};
    std::filesystem::path relative=input.substr(colon+1);
    relative=relative.relative_path();
    for(const auto& component:relative) if(component=="..") return {};
    return (std::filesystem::path(it->second.directory)/relative).string();
}
std::shared_ptr<SaveTransaction> save_for_path(const char* path){
    std::string name(path);auto colon=name.find(':');if(colon==std::string::npos)return {};
    auto i=mounts.find(name.substr(0,colon));return i==mounts.end()?nullptr:i->second.save;
}
void poison_file(uint64_t handle,int e){auto it=files.find(handle);if(it!=files.end()&&it->second.save)it->second.save->poison(e);}
void invalidate_save_handles(const std::shared_ptr<SaveTransaction>& save){
    for(auto i=files.begin();i!=files.end();)if(i->second.save==save){close(i->second.descriptor);i=files.erase(i);}else ++i;
}
uint64_t mount(const char* name,const std::string& directory,bool read_only,bool transactional=false) {
    std::lock_guard lock(filesystem_mutex);
    if(!name || !*name || strchr(name,':') || strchr(name,'/')) return IoError;
    if(mounts.contains(name))return PathExists;
    std::shared_ptr<SaveTransaction> save;
    bool save_read_only=false;
    if(transactional){
        for(const auto& [_,m]:mounts)if((m.save&&m.save->committed()==directory)||(m.save_read_only&&m.directory==directory))return PermissionDenied;
        save=std::make_shared<SaveTransaction>(directory);
        if(int e=save->begin()){
            // The last committed tree is still usable even when disk pressure
            // prevents a working copy. Never write directly into that tree.
            std::error_code ec;
            if(!std::filesystem::is_directory(std::filesystem::symlink_status(directory,ec))){notify_save(SaveCommitFailed,e);return IoError;}
            save.reset();read_only=true;save_read_only=true;notify_save(SaveReadOnly,e);
        }
    }
    if(!read_only) {std::error_code ec;std::filesystem::create_directories(directory,ec);if(ec)return IoError;}
    auto target=save?save->working():directory;
    mounts[name]={target,read_only,save,save_read_only};ot2_host_log("MOUNT %s: -> %s readonly=%d",name,target.c_str(),read_only);return 0;
}
int fd(uint64_t handle) {auto it=files.find(handle);return it==files.end()?-1:it->second.descriptor;}
// Read once: this gates per-open logging, and stat()ing a marker on every open
// would cost more than the log it enables.
bool tracing_files(){static const bool on=ot2_game_flag("trace-services")!=0;return on;}
uint64_t open_file(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]),r[2]&2);
    if(p.empty()){if(tracing_files())ot2_host_log("OPEN_REJECT name=%s mode=%llu",str(r[1]),(unsigned long long)r[2]);return PermissionDenied;}
    int mode=(r[2]&3)==3?O_RDWR:(r[2]&2)?O_WRONLY:O_RDONLY;
    int f=open(p.c_str(),mode|O_CLOEXEC|O_NOFOLLOW);
    if(f<0){if(errno!=ENOENT && errno!=ENOTDIR)ot2_host_log("OPEN failed %s errno=%d",str(r[1]),errno);
        else if(tracing_files())ot2_host_log("OPEN_MISS name=%s",str(r[1]));return error();}
    const auto h=next_handle++;files[h]={f,save_for_path(str(r[1])),mode==O_RDONLY&&!romfs.empty()&&p.starts_with(romfs+"/")};
    files[h].stream=p.size()>6 && p.compare(p.size()-6,6,".bfstm")==0;
    if(files[h].stream){struct stat ss{};if(!fstat(f,&ss))ot2_host_log("STREAM_OPEN %s handle=%llu bytes=%lld",str(r[1]),(unsigned long long)h,(long long)ss.st_size);}*ptr<uint64_t>(r[0])=h;
    if(files[h].resource)files[h].reader=std::make_shared<ResourceReadFile>(f);
    ot2_host_log("OPEN %s handle=%llu",str(r[1]),(unsigned long long)h);return 0;
}
uint64_t read_file(Registers r) {
    // Keep this inode alive across close/unmount/rollback without serializing
    // all resource reads. The handle's fd may otherwise be reused mid-pread.
    const auto started=std::chrono::steady_clock::now();
    int f;bool prefetch=false,owned=false,stream=false;std::shared_ptr<ResourceReadFile> reader;{
        std::lock_guard lock(filesystem_mutex);auto it=files.find(r[1]);if(it==files.end()||int64_t(r[2])<0||r[4]>INT64_MAX-r[2])return IoError;
        auto& file=it->second;reader=file.reader;stream=file.stream;
        owned=stream_io&&bool(reader);f=owned?reader->fd:fcntl(file.descriptor,F_DUPFD_CLOEXEC,0);
        prefetch=io_prefetch&&file.resource&&r[4]>=65536&&file.last_end==r[2]&&r[2]+r[4]>file.prefetch_end;
        file.last_end=r[2]+r[4];if(prefetch)file.prefetch_end=file.last_end+2*1024*1024;
    }
#ifdef __ANDROID__
    if(f>=0&&prefetch){if(owned){if(resource_prefetch().add(reader,r[2]+r[4],2*1024*1024))++io_hints;}else posix_fadvise(f,r[2]+r[4],2*1024*1024,POSIX_FADV_WILLNEED);}
#endif
    if(f<0)return IoError;
    struct CloseRead {int fd;~CloseRead(){if(fd>=0)close(fd);}} keep{owned?-1:f};
    if(owned)++io_reused_fds;
    size_t total=0;auto* dest=ptr<uint8_t>(r[3]);
    NvnDirtyWrite writable_destination(dest,r[4]);
    while(total<r[4]) {auto n=pread(f,dest+total,r[4]-total,r[2]+total);if(n<0){if(errno==EINTR)continue;return error();}if(!n)break;total+=n;}
    ++io_calls;io_bytes+=total;io_wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    // Only the stream files speak: they are the ones whose position has to walk
    // forward. Sampled per handle, not per session -- see File::reads.
    if(stream) {
        uint32_t n=0;
        {std::lock_guard lock(filesystem_mutex);auto it=files.find(r[1]);if(it!=files.end())n=++it->second.reads;}
        const bool short_read=total<r[4];
        if(n<=4 || n%64==0 || short_read)
            ot2_host_log("STREAM_READ #%u handle=%llu off=%lld chunk=%llu got=%llu%s",
                n,(unsigned long long)r[1],(long long)r[2],
                (unsigned long long)r[4],(unsigned long long)total,short_read?" EOF":"");
    }
    if(r[0])*ptr<uint64_t>(r[0])=total;return 0;
}
uint64_t write_file(Registers r) {
    // No log used to exist here at all, so "the session never wrote to the save"
    // could not be told apart from "writes are simply not logged". The completion
    // of a minigame is exactly where the reference build writes its save, so this
    // has to be observable.
    {
        static std::atomic<unsigned> seen{0};
        const unsigned n=seen.fetch_add(1,std::memory_order_relaxed)+1;
        if(n<=8 || n%256==0)ot2_host_log("WRITE #%u handle=%llu off=%lld size=%llu (writes so far %u)",
            n,(unsigned long long)r[0],(long long)r[1],(unsigned long long)r[3],n);
    }
    std::lock_guard lock(filesystem_mutex);int f=fd(r[0]);if(f<0 || int64_t(r[1])<0)return IoError;
    if(int e=save_test_fault("write")){poison_file(r[0],e);return IoError;}
    size_t done=0;while(done<r[3]){auto n=pwrite(f,ptr<const uint8_t>(r[2])+done,r[3]-done,r[1]+done);if(n<0){if(errno==EINTR)continue;poison_file(r[0],errno);return error();}if(!n){poison_file(r[0],EIO);return IoError;}done+=n;}
    if(r[4] && (*ptr<uint32_t>(r[4])&1) && fsync(f)){poison_file(r[0],errno);return error();}return 0;
}
uint64_t commit(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto it=mounts.find(str(r[0]));
    // The mount lookup and the read-only refusals used to be silent, so a refused
    // commit looked exactly like a commit that was never requested -- and the title
    // reports the refusal to the player as "this ghost will not be saved".
    {
        static std::atomic<unsigned> seen{0};
        const unsigned n=seen.fetch_add(1,std::memory_order_relaxed)+1;
        if(n<=16 || n%256==0)ot2_host_log("COMMIT_REQ #%u name=%s found=%d read_only=%d",
            n,str(r[0]),it!=mounts.end()?1:0,it!=mounts.end()?int(it->second.read_only):-1);
    }
    if(it==mounts.end())return PermissionDenied;
    // D2R's degraded save mount has no transaction to commit. Writes already
    // fail and the UI reports that this session cannot save.
    if(it->second.save_read_only)return 0;
    if(it->second.read_only)return PermissionDenied;
    if(auto save=it->second.save){
        // Rollback replaces the work tree. No handle may keep writing the
        // discarded inode and later appear to have saved successfully.
        if(save->failed())invalidate_save_handles(save);
        int e=save->commit();
        ot2_host_log("SAVE_COMMIT result=%d published=%d",e,save->published());
        if(e)notify_save(save->published()?SaveDurabilityUncertain:SaveCommitFailed,e);
        return e?IoError:0;
    }
    for(const auto& [handle,file]:files){int f=file.descriptor;int flags=fcntl(f,F_GETFL);if(flags>=0 && (flags&O_ACCMODE)!=O_RDONLY && fsync(f))return error();}
    int d=open(it->second.directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(d<0)return error();int rc=fsync(d);close(d);return rc?IoError:0;
}
// Read-only mounts still report AlreadyExists for an existing create target,
// as in D2R. This lets SDK "ensure directory/file" paths proceed to read it.
bool create_target_exists(const char* guest){auto p=path(guest);struct stat st{};return !p.empty()&&!lstat(p.c_str(),&st);}
// SDK 5.4.0.0 appended a trailing `int option` to CreateFile, so the mangled
// name grew a third parameter and the two-argument entry stopped matching.
// Nothing in the body reads past r[1], so one function serves both spellings.
uint64_t create_file(Registers r) {
    std::lock_guard lock(filesystem_mutex);if(int64_t(r[1])<0)return PermissionDenied;if(create_target_exists(str(r[0])))return PathExists;auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;
    auto save=save_for_path(str(r[0]));int f=open(p.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(f<0){if(save&&(errno==ENOSPC||errno==EDQUOT||errno==EIO))save->poison(errno);return error();}
    int rc=ftruncate(f,r[1]),e=rc?errno:0;if(close(f)&&!e)e=errno;
    if(e){if(save)save->poison(e);unlink(p.c_str());return IoError;}return 0;
}
uint64_t open_directory(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]));
    if(p.empty())return PathNotFound;
    auto* dir=opendir(p.c_str());if(!dir)return error();
    Directory snapshot;
    while(auto* ent=readdir(dir)) {
        if(!strcmp(ent->d_name,".") || !strcmp(ent->d_name,".."))continue;
        struct stat st{};auto child=p+"/"+ent->d_name;
        if(lstat(child.c_str(),&st) || S_ISLNK(st.st_mode))continue;
        bool is_dir=S_ISDIR(st.st_mode);if(!(r[2]&(is_dir?1:2)))continue;
        DirectoryEntry item{};strncpy(item.name,ent->d_name,sizeof(item.name)-1);
        item.type=is_dir?0:1;item.size=is_dir?0:st.st_size;snapshot.entries.push_back(item);
    }
    closedir(dir);const auto h=next_handle++;directories.emplace(h,std::move(snapshot));*ptr<uint64_t>(r[0])=h;
    ot2_host_log("DIRECTORY %s entries=%zu",str(r[1]),directories.at(h).entries.size());return 0;
}
std::string cache_directory(int32_t index){return index<0 || index>65535?std::string{}:user+"/cache/"+std::to_string(index);}
uint64_t check_storage_size(int64_t size,int64_t journal) {
    if(size<0 || journal<0)return IoError;
    struct statvfs st{};if(statvfs(user.c_str(),&st))return error();
    uint64_t available=uint64_t(st.f_bavail)*st.f_frsize;
    return uint64_t(size)>available || uint64_t(journal)>available-uint64_t(size)?0x6402:0;
}
uint64_t create_cache(Registers r) {
    std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[0]));if(directory.empty())return IoError;
    if(auto result=check_storage_size(r[1],r[2]))return result;
    std::error_code ec;if(std::filesystem::exists(directory,ec))return PathExists;if(ec)return IoError;
    std::filesystem::create_directories(directory+"/data",ec);if(ec)return IoError;
    std::ofstream metadata(directory+"/sizes",std::ios::binary);int64_t sizes[]={int64_t(r[1]),int64_t(r[2])};
    metadata.write(reinterpret_cast<const char*>(sizes),sizeof(sizes));metadata.close();if(!metadata){std::filesystem::remove_all(directory,ec);return IoError;}return 0;
}
uint64_t mount_cache(Registers r) {
    auto directory=cache_directory(int32_t(r[1]));if(directory.empty())return IoError;
    std::error_code ec;if(!std::filesystem::is_directory(directory+"/data",ec))return PathNotFound;
    return mount(str(r[0]),directory+"/data",false);
}
uint64_t open_cache_list(Registers r) {
    std::lock_guard lock(filesystem_mutex);CacheList snapshot;std::error_code ec;
    auto directory=user+"/cache";if(std::filesystem::exists(directory,ec)) {
        for(std::filesystem::directory_iterator i(directory,ec),end;!ec&&i!=end;i.increment(ec)) {
            auto name=i->path().filename().string();if(name.empty() || name.find_first_not_of("0123456789")!=std::string::npos || name.size()>5)continue;
            int index=std::stoi(name);if(index<=65535 && std::filesystem::is_directory(i->path()/"data",ec))snapshot.indices.push_back(index);
        }
    }
    if(ec)return IoError;std::sort(snapshot.indices.begin(),snapshot.indices.end());auto h=next_handle++;cache_lists.emplace(h,std::move(snapshot));*ptr<uint64_t>(r[0])=h;return 0;
}
uint64_t calendar_time(Registers r) {
    if(!r[2])return 116+(901<<9);
    return time_zone_convert(ptr<CalendarTime>(r[0]),ptr<CalendarAdditionalInfo>(r[1]),*ptr<int64_t>(r[2]),ptr<void>(r[3]));
}
uint64_t calendar_time_current(Registers r) {
    if(!r[2])return 116+(901<<9);
    std::array<char,36> location{};std::array<uint8_t,0x4000> rule{};
    if(auto result=time_zone_location(location.data()))return result;
    if(auto result=time_zone_load(rule.data(),location.data()))return result;
    return time_zone_convert(ptr<CalendarTime>(r[0]),ptr<CalendarAdditionalInfo>(r[1]),*ptr<int64_t>(r[2]),rule.data());
}
// Original SDK libc uses UTC without its Horizon timezone service. D2R also
// replaces these three exports. Keep the original SDK formatting/UTC routines;
// its tm ABI agrees with both supported 64-bit hosts, including zone pointers.
static_assert(sizeof(tm)==56&&offsetof(tm,tm_isdst)==32&&offsetof(tm,tm_gmtoff)==40&&offsetof(tm,tm_zone)==48);
const std::array<uint64_t,2> uid{0x4f54324c4f43414cULL,1};
uint64_t display_object=0x444953504c4159,layer_object=0x4c41594552,native_window_object=0x414e44524f4944;
uint64_t display_event(Registers r) {
    const uint64_t args[]={r[0],1,1,0,0,0,0,0};
    auto result=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
    ot2_host_log("VI_VSYNC_EVENT CreateSystemEvent result=%#llx out=%p",(unsigned long long)result,ptr<void>(r[0]));
    if(result)return result;
    const auto handle=ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",args);
    ot2_host_log("VI_VSYNC_EVENT writable handle=%#llx",(unsigned long long)handle);
    std::thread([handle]{
        auto deadline=std::chrono::steady_clock::now();
        while(ot2_signal_event(handle)){deadline+=std::chrono::nanoseconds(16666667);std::this_thread::sleep_until(deadline);}
    }).detach();
    return 0;
}
uint64_t open_user(Registers r) {auto* out=ptr<uint64_t>(r[0]);out[0]=uid[0];out[1]=uid[1];out[2]=0x4f543255534552ULL;return 1;}
uint64_t unavailable(Registers){return 0x2a2;}
thread_local int socket_error=50; // Horizon/FreeBSD ENETDOWN, not Android errno.
uint64_t socket_failure(Registers){socket_error=50;return uint64_t(-1);}
uint64_t completed_account_event(Registers r){
    // Let the original SDK construct its own version-specific SystemEvent ABI.
    const uint64_t args[]={r[1],1,1,0,0,0,0,0};
    auto result=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);if(result)return result;
    auto handle=ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",args);
    return ot2_signal_event(handle)?0:0x2a2;
}
}
void set_io_prefetch(bool enabled){io_prefetch=enabled;}
extern "C" void ot2_host_stream_profile(int enabled){
    stream_io=enabled!=0;if(!enabled)resource_prefetch().clear();
    ot2_host_log("RESOURCE_IO enabled=%d calls=%llu mb=%.3f wall_ms=%.3f reused_fd=%llu hints=%llu",enabled,(unsigned long long)io_calls.exchange(0),io_bytes.exchange(0)/1048576.,io_wall_ns.exchange(0)/1e6,(unsigned long long)io_reused_fds.exchange(0),(unsigned long long)io_hints.exchange(0));
}
void configure_services(const std::string& romfs_directory,const std::string& user_directory,uint64_t identity) {if(!identity)throw std::runtime_error("Invalid game ProgramId");romfs=romfs_directory;user=user_directory;program_id=identity;
    addon_content.clear();
    auto root=std::filesystem::path(romfs_directory).parent_path()/"AddOnContent";
    std::error_code ec;
    for(std::filesystem::directory_iterator it(root,ec),end;!ec&&it!=end;it.increment(ec)){
        const auto name=it->path().filename().string();
        if(name.empty()||name.size()>4||name[0]=='0'||name.find_first_not_of("0123456789")!=std::string::npos)continue;
        int index=std::stoi(name);
        if(index>=1&&index<=2000&&std::filesystem::is_directory(it->symlink_status(ec)))addon_content.emplace(index,it->path().string());
    }
    std::lock_guard lock(activity_mutex);active_ns=0;active_since=std::chrono::steady_clock::now();}
uint64_t configured_program_id(){return program_id;}
unsigned take_save_notice(){return save_notice.exchange(0);}
void set_activity_focus(unsigned state){
    if(state<1 || state>3)ot2_host_die("Invalid activity focus state %u",state);
    {std::lock_guard lock(activity_mutex);auto now=std::chrono::steady_clock::now();
        if(activity_active)active_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-active_since).count();
        active_since=now;if(activity_focus!=state){focus_notification=true;ot2_host_log("ACTIVITY_FOCUS state=%u",state);}activity_focus=state;activity_active=state==1;}
    set_audio_active(state==1);
}
void set_activity_active(bool active){set_activity_focus(active?1:2);}
namespace {
// Implemented in guest_entry.S: clears both result registers and returns.
extern "C" void ot2_native_zero();
// A title sometimes treats a weak import as if it must return an object: the
// next instruction reads a vtable through the result and calls into it. When
// nothing provides the symbol the import resolves to zero and that read faults.
// Hand back a stand-in instead — an object whose whole vtable answers with zero
// — so the call lands on code and the title keeps running. The caller sees a
// service that reports "nothing", which is the same answer a missing account
// would give without taking the process down with it.
void* stand_in_object() {
    static void* object=[]{
        constexpr size_t entries=512;   // room for any slot a caller dispatches to
        auto** table=static_cast<void**>(calloc(entries,sizeof(void*)));
        for(size_t i=0;i<entries;i++)table[i]=reinterpret_cast<void*>(ot2_native_zero);
        auto** holder=static_cast<void**>(calloc(16,sizeof(void*)));
        holder[0]=static_cast<void*>(table);   // an object starts with its vtable
        return static_cast<void*>(holder);
    }();
    return object;
}
}
const std::vector<HostService>& compatibility_services() {    // An SDK reaches Horizon's service manager whenever it wants something the
    // host has not bound by symbol. Every title this host supports answers its
    // service calls locally, so none of them needs a live sm: session — but an
    // SDK that has not been satisfied will block on an IPC reply that never
    // arrives, or dereference a session it never managed to open.
    static const std::vector<HostService> services={
        {"_ZN2nn2sm10InitializeEv",ok},
        {"_ZN2nn2sm8FinalizeEv",ok},
        // bind: the stand-in session keeps a caller's later requests inside
        // this process, and records which service each one was for.
        // unknown: nothing answers them outside diagnostic mode, but the
        // alternative is a null dereference inside the SDK.
        {"_ZN2nn2sm16GetServiceHandleEPNS_3svc6HandleEPKcm",[](Registers r)->uint64_t{
            const char* name=r[1]?str(r[1]):"";
            static std::atomic<uint64_t> next{0x52000000};
            const uint64_t handle=next.fetch_add(1,std::memory_order_relaxed);
            {
                std::lock_guard lock(sm_mutex);
                sm_sessions[handle]=name;
                if(sm_requested.insert(name).second)
                    ot2_host_log("SM_SERVICE '%s' -> session %#llx (distinct so far: %zu) from %p",
                        name,(unsigned long long)handle,sm_requested.size(),__builtin_return_address(0));
            }
            if(r[0])*ptr<uint64_t>(r[0])=handle;
            return 0;
        }},
        {"_ZN2nn8settings15GetLanguageCodeEPNS0_12LanguageCodeE",[](Registers r)->uint64_t{
            ot2_host_log("COMPAT GetLanguageCode out=%p -> 1",(void*)r[0]);
            if(r[0])*ptr<uint64_t>(r[0])=1;
            return 0;
        }},
        {"_ZN2nn8settings29GetAvailableLanguageCodeCountEv",[](Registers)->uint64_t{
            ot2_host_log("COMPAT GetAvailableLanguageCodeCount -> 1");
            return 1;
        }},
        {"_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi",[](Registers r)->uint64_t{
            ot2_host_log("COMPAT GetAvailableLanguageCodes out=%p max=%d -> 1",(void*)r[0],int(int32_t(r[1])));
            if(r[0]&&int32_t(r[1])>0)*ptr<uint64_t>(r[0])=1;
            return 1;
        }},
        {"_ZN2nn2oe18GetDesiredLanguageEv",[](Registers)->uint64_t{return 1;}},
        // nn::account::detail::ObjectHolder has no implementation in any module
        // this title ships (the methods are weak imports that resolve to zero),
        // and the SDK calls GetForApplication then immediately dispatches
        // through the result. Answer with the stand-in object so the account
        // path reports "no user" instead of faulting on a null vtable.
        {"_ZNK2nn7account6detail12ObjectHolder17GetForApplicationEv",[](Registers)->uint64_t{
            ot2_host_log("ACCOUNT ObjectHolder::GetForApplication -> stand-in");
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZNK2nn7account6detail12ObjectHolder19GetForSystemServiceEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZNK2nn7account6detail12ObjectHolder19GetForAdministratorEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZN2nn7account6detail12ObjectHolderC1Ev",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account6detail12ObjectHolderC2Ev",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account10InitializeEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account8FinalizeEv",[](Registers)->uint64_t{return 0;}},
        // The same holder is reached through several accessor spellings in the
        // SDK; every one of them is a weak import that resolves to zero, and the
        // caller dereferences whatever comes back. Answer them all with the
        // stand-in so the account path stays alive whichever name it takes.
        {"_ZN2nn7account6detail27GetAccountServiceObjectBaseEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZN2nn7account6detail39GetAccountServiceObjectForAdministratorEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZN2nn7account6detail39GetAccountServiceObjectForSystemServiceEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        {"_ZNK2nn7account6detail12ObjectHolder7GetBaseEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
        // The acquire helpers take an out-pointer: report success and store the
        // stand-in, so a caller that checks the Result still gets an object.
        {"_ZN2nn7account6detail12ObjectHolder22AcquireForApplicatoionEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account6detail12ObjectHolder23AcquireForAdministratorEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account6detail12ObjectHolder23AcquireForSystemServiceEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account6detail12ObjectHolder7ReleaseEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account6detail12ObjectHolder14InitializeWithEONS_2sf13SharedPointerINS0_31IAccountServiceForAdministratorEEE",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn3aoc6detail15GetObjectHolderEv",[](Registers)->uint64_t{
            return reinterpret_cast<uint64_t>(stand_in_object());
        }},
    };
    return services;
}
const char* compatibility_session_service(uint64_t handle) {
    std::lock_guard lock(sm_mutex);
    const auto found=sm_sessions.find(handle);
    return found==sm_sessions.end()?nullptr:found->second.c_str();
}
size_t compatibility_service_count() {
    std::lock_guard lock(sm_mutex);
    return sm_requested.size();
}
// Every event this host hands to the title, as the *write* end, so the host can
// wake it when the condition it stands for happens.
//
// Why a table at all: `nn::os::SystemEvent` is an inter-process event pair in
// the SDK, and only the far end can signal it. On hardware the far end is the
// hid/vi/audren server; here it is this process, so the host has to keep the
// write handle to have anything to signal with. A window of 32 is far more than
// this title binds (9) and keeps the lookup a linear scan of a fixed array.
struct AttachedEventSlot { std::atomic<uint32_t> writable; std::atomic<uint32_t> signals; };
AttachedEventSlot attached_events[32];
std::atomic<unsigned> attached_event_count{0};
uint32_t os_attach_system_event(void* raw,bool autoclear,unsigned clear_mode) {
    auto* event=static_cast<uint8_t*>(raw);
    if(!event) return 0xcc01;                       // InvalidArgument
    uint64_t create[8]={0,0,0,0,0,0,0,0};
    if(svc_thread(0x45,create)!=1) return 0xd401;   // hand the failure back
    const uint32_t writable=uint32_t(create[1]),readable=uint32_t(create[2]);
    // Same writes as detail::AttachInterProcessEvent, in the same order, so a
    // structure this host builds is indistinguishable from one the SDK built.
    memset(event,0,0x30);
    const uint64_t self=reinterpret_cast<uint64_t>(event);
    memcpy(event+0x00,&self,8);
    memcpy(event+0x08,&self,8);
    event[0x10]=clear_mode==1?1:0;                  // EventClearMode::Manual
    event[0x11]=1;                                  // inter-process event
    event[0x12]=autoclear?1:0;
    event[0x13]=1;                                  // writable end is set too
    memcpy(event+0x14,&readable,4);
    memcpy(event+0x18,&writable,4);
    event[0x28]=2;                                  // type tag: inter-process
    const unsigned slot=attached_event_count.fetch_add(1,std::memory_order_relaxed);
    if(slot<32) attached_events[slot].writable.store(writable,std::memory_order_release);
    return 0;
}
void signal_attached_events() {
    const unsigned count=std::min<unsigned>(attached_event_count.load(std::memory_order_acquire),32);
    for(unsigned i=0;i<count;i++) {
        const uint32_t writable=attached_events[i].writable.load(std::memory_order_acquire);
        if(!writable) continue;
        if(ot2_signal_event(writable)) attached_events[i].signals.fetch_add(1,std::memory_order_relaxed);
    }
}
unsigned attached_events_bound() {
    return std::min<unsigned>(attached_event_count.load(std::memory_order_acquire),32);
}
const std::vector<HostService>& host_services() {
    static const std::vector<HostService> services={
        {"_ZN2nv18InitializeGraphicsEPvm",[](Registers)->uint64_t{vkhost_init();return 0;}},
        {"nvnBootstrapLoader",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(ot2_nvn_lookup(str(r[0])));}},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE",[](Registers r)->uint64_t{
            // The dedicated output endpoint is fixed for the current session.
            // Manual-clear, matching the other event shims: an auto-clearing
            // notification would lose a signal that arrives between frames.
            const uint64_t args[]={r[0],1,1,0,0,0,0,0};
            const auto result=ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
            ot2_host_log("AUDIO_NOTIFY_EVENT CreateSystemEvent result=%#llx out=%p",(unsigned long long)result,ptr<void>(r[0]));
            return result;
        }},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv",[](Registers)->uint64_t{return 2;}},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE",[](Registers r)->uint64_t{memset(ptr<void>(r[0]),0,256);memcpy(ptr<void>(r[0]),"DeviceOut",10);return 0;}},
        {"_ZN2nn2vi10InitializeEv",ok},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&display_object);return 0;}},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&layer_object);return 0;}},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=reinterpret_cast<uint64_t>(&native_window_object);return 0;}},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",display_event},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=vkhost_frame_count();return 0;}},
        // Layer attributes never reach this host: the picture goes to our own
        // surface, not through Horizon's compositor. Answering the SDK's own
        // implementation is not possible -- it resolves the stand-in layer
        // through nn::vi's ObjectManager, finds nothing, and faults on the null
        // at sdk+0x275614 (measured, Koei's engine calls this right after
        // CreateLayer). Brain Academy never imports this entry, so gating it
        // changes nothing for that title.
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",ok},
        // LoadedGame owns the static NSO module registry; no Horizon RO session.
        {"_ZN2nn2ro10InitializeEv",ok},
        {"nnsslInitialize",ok},
        {"nnsslFinalize",ok},
        {"_ZN2nn7friends10InitializeEv",ok},
        // Native input has no Horizon shared-memory activation. Device state is
        // supplied by the Android frontend; initialization only selects types.
        {"_ZN2nn3hid18InitializeKeyboardEv",ok},
        {"_ZN2nn3hid15InitializeMouseEv",ok},
        {"_ZN2nn3hid21InitializeTouchScreenEv",ok},
        {"_ZN2nn3hid14InitializeNpadEv",ok},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",ok},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",ok},
        {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",ok},
        {"_ZN2nn3hid18GetNpadJoyHoldTypeEv",ok},
        // Controller state is answered here rather than through the hid service.
        // The SDK's hid client asks the service for its shared memory and for an
        // update event; a host that answers those with empty replies leaves the
        // title waiting on a handle that was never filled (that is what stalled
        // this one). Answering the public calls keeps the service out of the
        // path entirely, which is how the D2R host does it too.
        //
        // The npad entries themselves live in input.cpp. They used to be defined
        // in both tables, so the same patch site was claimed twice and whichever
        // list the installer reached last won -- and the copy here wrote nothing
        // but the sampling number, which is a controller that reports "connected"
        // and never a button. One definition, one owner: input.cpp.
        // Not hooked: answering either way is worse than leaving it alone.
        // Returning "timed out" spins (the caller recomputes its unchanged
        // deadline and calls again), and returning "signalled" is a lie the title
        // believes -- it advances past the wait and then dereferences the object
        // it expected that signal to have filled, which faults at address 0.
        // Leaving the SDK's own implementation in place keeps the run quiet.
        // What these waits are waiting on is reported from the syscall side by
        // WAIT_CALLER instead.
        // The one call that turns a service-provided handle into an
        // nn::os::SystemEventType. Everything the title waits on a SystemEvent
        // for is built here, which makes it the single place a half-built event
        // can be caught.
        //
        // What used to happen: the host answered success without writing the
        // structure, on the assumption that the caller had already filled it.
        // It has not -- this function is what fills it -- so the title ended up
        // polling an event whose readable handle is 0 and whose autoclear flag
        // is 0. `nn::os::detail::InterProcessEventImplByHorizon::TryWait`
        // (sdk+0x350ce8) reads both out of that structure; with handle 0 every
        // poll times out, and the title's own `while (!TryWait())` loop then
        // spins a core at thousands of syscalls a second without ever reaching
        // the next screen -- which is exactly the freeze this title shows after
        // "time up".
        //
        // The Mac profile of this title does the same thing differently and it
        // is the reference here: nxnative's dm/host/hle_thread.c creates a real
        // event in hle_bind_system_event and signals it for the controller that
        // is connected, with the note that without it "GetNpadState was never
        // called". This creates a real event too, and writes the structure with
        // the layout the SDK's own detail::AttachInterProcessEvent uses.
        //
        // The incoming handle is deliberately ignored: on this host it comes
        // either from a service stand-in that never created an event (0) or out
        // of the CMIF reply of a session that has none (0x6 on this title's
        // nine binds), and neither names anything waitable.
        {"_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE",
         [](Registers r)->uint64_t{
            const uint32_t supplied=uint32_t(r[1]);
            const auto result=ot2::os_attach_system_event(ptr<void>(r[0]),r[2]!=0,unsigned(r[3]));
            static std::atomic<unsigned> seen{0};
            const unsigned n=seen.fetch_add(1,std::memory_order_relaxed);
            if(n<32) {
                uintptr_t offset=0;
                const char* where=ot2_host_module_of(r[30],&offset);
                ot2_host_log("ATTACH_EVENT #%u event=%p supplied=%#x autoclear=%u clearmode=%u result=%#x bound=%u from=%s+0x%zx",
                    n,ptr<void>(r[0]),supplied,r[2],r[3],result,ot2::attached_events_bound(),
                    where?where:"?",offset);
            }
            // These events report "the controller or notification state
            // changed". Nothing on this host changes it behind the title's back,
            // so wake it once here (the hardware signals it once, when the
            // controller is attached at start-up) and again whenever the user's
            // input state changes -- signal_attached_events() is also called
            // from input.cpp on a press, a release and a touch.
            ot2::signal_attached_events();
            return result;
         }},
        {"_ZN2nn3hid20IsVibrationPermittedEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",ok},
        {"_ZN2nn3hid20GetNpadJoyAssignmentERKj",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",ok},
        {"_ZN2nn4nifm10InitializeEv",ok},
        {"_ZN2nn4nifm18IsNetworkAvailableEv",ok},
        {"_ZN2nn4nifm20CancelNetworkRequestEv",ok},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE",[](Registers)->uint64_t{return IoError;}},
        {"_ZN2nn6socket10InitializeEPvmmi",ok},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj",socket_failure},
        {"_ZN2nn6socket10SetSockOptEiiiPKvj",socket_failure},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"_ZN2nn6socket11GetNameInfoEPK8sockaddrjPcjS4_ji",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"_ZN2nn6socket11GetPeerNameEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo",ok},
        {"_ZN2nn6socket12GetLastErrnoEv",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"_ZN2nn6socket12GetLastErrorEv",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"_ZN2nn6socket4BindEiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket4PollEP6pollfdmi",socket_failure},
        {"_ZN2nn6socket4RecvEiPvmi",socket_failure},
        {"_ZN2nn6socket4SendEiPKvmi",socket_failure},
        {"_ZN2nn6socket5CloseEi",socket_failure},
        {"_ZN2nn6socket5FcntlEiiz",socket_failure},
        {"_ZN2nn6socket5IoctlEijPvm",socket_failure},
        {"_ZN2nn6socket6AcceptEiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket6ListenEii",socket_failure},
        {"_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval",socket_failure},
        {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket6SocketEiii",socket_failure},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj",socket_failure},
        {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj",socket_failure},
        {"_ZN2nn6socket8ShutdownEii",socket_failure},
        {"nnsocketBind",socket_failure},
        {"nnsocketClose",socket_failure},
        {"nnsocketConnect",socket_failure},
        {"nnsocketFcntl",socket_failure},
        {"nnsocketFreeAddrInfo",ok},
        {"nnsocketGetAddrInfo",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"nnsocketGetAddrInfoWithOptions",[](Registers r)->uint64_t{socket_error=50;if(r[3])*ptr<uint64_t>(r[3])=0;return 4;}},
        {"nnsocketGetHostByAddr",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByAddrWithOptions",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByName",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetHostByNameWithOptions",[](Registers)->uint64_t{socket_error=50;return 0;}},
        {"nnsocketGetLastErrno",[](Registers)->uint64_t{return uint32_t(socket_error);}},
        {"nnsocketGetNameInfo",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"nnsocketGetNameInfoWithOptions",[](Registers)->uint64_t{socket_error=50;return 4;}},
        {"nnsocketGetPeerName",socket_failure},
        {"nnsocketGetSockName",socket_failure},
        {"nnsocketGetSockOpt",socket_failure},
        {"nnsocketPoll",socket_failure},
        {"nnsocketRecv",socket_failure},
        {"nnsocketResolverGetOption",socket_failure},
        {"nnsocketResolverSetOption",socket_failure},
        {"nnsocketSend",socket_failure},
        {"nnsocketSetLastErrno",[](Registers r)->uint64_t{socket_error=int32_t(r[0]);return 0;}},
        {"nnsocketSetSockOpt",socket_failure},
        {"nnsocketSocket",socket_failure},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb",ok},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE",ok},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj",[](Registers r)->uint64_t{if(r[1])*ptr<int32_t>(r[1])=0;return 0;}},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev",ok},
        {"_ZN2nn3ngc15ProfanityFilterD1Ev",ok},
        {"_ZN2nn4time10InitializeEv",ok},
        {"_ZN2nn4time13IsInitializedEv",yes},
        {"_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE",[](Registers r)->uint64_t{*ptr<int64_t>(r[0])=time(nullptr);return 0;}},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE",[](Registers r)->uint64_t{return time_zone_location(ptr<void>(r[0]));}},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE",[](Registers r)->uint64_t{return time_zone_load(ptr<void>(r[0]),str(r[1]));}},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE",calendar_time},
        {"_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",calendar_time_current},
        {"_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE",[](Registers r)->uint64_t{CalendarTime c{};time_zone_utc(&c,nullptr,*ptr<int64_t>(r[0]));uint64_t out;memcpy(&out,&c,8);return out;}},
        {"localtime",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(localtime(ptr<const time_t>(r[0])));}},
        {"localtime_r",[](Registers r)->uint64_t{return reinterpret_cast<uint64_t>(localtime_r(ptr<const time_t>(r[0]),ptr<tm>(r[1])));}},
        {"mktime",[](Registers r)->uint64_t{return uint64_t(mktime(ptr<tm>(r[0])));}},
        // This SDK's helper only recognizes its own UTC string pointer, so
        // strftime("%Z") otherwise drops valid host-produced timezone names.
        {"__tm_to_tzname",[](Registers r)->uint64_t{const auto name=ptr<const tm>(r[0])->tm_zone;return reinterpret_cast<uint64_t>(name?name:"");}},
        {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE",show_keyboard},
        {"_ZN2nn7account10InitializeEv",ok},
        {"_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE",open_user},
        {"_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE",[](Registers r)->uint64_t{open_user(r);return 0;}},
        // Enumerating users must not fall through to the SDK's own service
        // client: that path needs nn::account::detail::ObjectHolder, which no
        // module in this title implements, and the caller dereferences the null
        // it gets back. Answer the list queries here as the iOS host does.
        {"_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi",[](Registers r)->uint64_t{
            *ptr<int32_t>(r[0])=0;
            if(int32_t(r[2])>0) { memcpy(ptr<void>(r[1]),uid.data(),16); *ptr<int32_t>(r[0])=1; }
            return 0;
        }},
        {"_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi",[](Registers r)->uint64_t{
            *ptr<int32_t>(r[0])=0;
            if(int32_t(r[2])>0) { memcpy(ptr<void>(r[1]),uid.data(),16); *ptr<int32_t>(r[0])=1; }
            return 0;
        }},
        // Entering an activity asks for the qualified users, not the open ones:
        // main+0x1d93fc calls this, the SDK implementation reaches
        // nn::account::detail::GetAccountServiceObjectForApplication, and that
        // import resolves to zero in this title -- the SDK then loads the first
        // field of the null object and the title dies at sdk+0x26f4. Answer the
        // query here, the same way as the two lists above.
        {"_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi",[](Registers r)->uint64_t{
            *ptr<int32_t>(r[0])=0;
            if(r[1]&&int32_t(r[2])>0) { memcpy(ptr<void>(r[1]),uid.data(),16); *ptr<int32_t>(r[0])=1; }
            ot2_host_log("ACCOUNT ListQualifiedUsers max=%d -> %d",int(int32_t(r[2])),*ptr<int32_t>(r[0]));
            return 0;
        }},
        {"_ZN2nn7account12GetUserCountEPi",[](Registers r)->uint64_t{*ptr<int32_t>(r[0])=1;return 0;}},
        // The roster screen asks whether the local user exists before it hands
        // out a save slot. The SDK's own answer opens the account service
        // object, which this host cannot provide, and the title then calls a
        // method on the null it gets back -- "主要" -> "入学登记" dies at
        // sdk+0x1d18 for exactly this reason. Answer the query here.
        {"_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE",[](Registers r)->uint64_t{
            auto* out=ptr<bool>(r[0]);
            auto* id=ptr<uint8_t>(r[1]);
            const bool same=out&&id&&memcmp(id,uid.data(),16)==0;
            if(out)*out=same;
            ot2_host_log("ACCOUNT GetUserExistence -> %d",int(same));
            return 0;
        }},
        // Creating a user is a system applet on the real device. The host
        // already has its one local user, so the request succeeds without
        // showing anything -- the caller re-enumerates and finds that user.
        {"_ZN2nn7account15ShowUserCreatorEv",[](Registers)->uint64_t{
            ot2_host_log("ACCOUNT ShowUserCreator -> ok (one local user already exists)");
            return 0;
        }},
        // Nothing opened the user on the title's behalf, so the list is empty
        // and the title opens the preselected user itself.
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi",[](Registers r)->uint64_t{
            *ptr<int32_t>(r[0])=0; return 0;
        }},
        {"_ZN2nn7account13PushOpenUsersEv",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE",[](Registers r)->uint64_t{
            memcpy(ptr<void>(r[0]),uid.data(),16); return 0;
        }},
        {"_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE",[](Registers r)->uint64_t{open_user(r);return 0;}},
        {"_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        // The SDK's own LoadProfileImage opens a profile through the account
        // service object and then reads the image through it. This host has no
        // account service, so the object it gets back is null while the open call
        // still reports success -- the title then faults on the first field of
        // that null object. Answer the call here instead of letting it half-work:
        // hand back a small valid JPEG so whatever decodes it has real data.
        // The profile picture is what the roster screen draws inside the user
        // card. Serving a stand-in JPEG is tempting, but the title decodes
        // whatever it gets into a texture of its own and aborts when the result
        // is not what it expects -- svcBreak with reason=0 right after
        // "PROFILE_IMAGE ... need=883", which reads as an allocation failure.
        // This host has no picture, and that is a state the title already
        // handles: report the failure and let it draw its default placeholder.
        {"_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE",[](Registers r)->uint64_t{
            if(r[0])*ptr<uint64_t>(r[0])=0;
            ot2_host_log("ACCOUNT LoadProfileImage -> unavailable (this host has no picture)");
            return 0x2a2;
        }},
        {"_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE",[](Registers r)->uint64_t{memset(ptr<void>(r[0]),0,33);memcpy(ptr<void>(r[0]),"Player",6);return 0;}},
        {"_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        // No Nintendo Account is linked here, so loading the cached profile
        // fails and the title skips its getters rather than reading a struct
        // that was never filled.
        {"_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext20GetAuthorizationCodeEPmPcm",[](Registers r)->uint64_t{if(r[1])*ptr<uint64_t>(r[1])=0;return 0x2a2;}},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext8GetStateEPmPcm",[](Registers r)->uint64_t{if(r[1])*ptr<uint64_t>(r[1])=0;return 0x2a2;}},
        // ---- nn::friends: every interface that talks to the service is cut ----
        // The roster screen pulls the friend list immediately after the account
        // lookups, to show friends' scores. This host never opens the friends
        // service, so the SDK's shim session comes back null and it reads the
        // vtable through that null -- the title dies at sdk+0x1a4320 with
        // x0=0 right after GetUserExistence. Answer every entry point here:
        // lists are empty, results report the cut, completion events fire at
        // once and notification events never fire.
        {"_ZN2nn7friends10InitializeEv",ok},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE",[](Registers r)->uint64_t{
            if(r[0])*ptr<bool>(r[0])=true; return 0;}},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPb",[](Registers r)->uint64_t{
            if(r[0])*ptr<bool>(r[0])=true; return 0;}},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextE",unavailable},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0;
            static std::atomic<unsigned> seen{0};
            if(seen.fetch_add(1)==0) ot2_host_log("FRIENDS GetFriendList -> empty list, success");
            return 0;
        }},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0; return 0;}},
        {"_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0; return 0;}},
        {"_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0; return 0;}},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi",unavailable},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendEPKNS_7account23NetworkServiceAccountIdEi",unavailable},
        // The overload without a Uid succeeds with an empty list: some titles
        // turn its failure into a hard error screen after the opening movie.
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0; return 0;}},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii",[](Registers r)->uint64_t{
            if(r[0])*ptr<int32_t>(r[0])=0; return 0x2a2;}},
        {"_ZN2nn7friends32CheckBlockedUserListAvailabilityEPb",[](Registers r)->uint64_t{
            if(r[0])*ptr<bool>(r[0])=false; return 0;}},
        {"_ZN2nn7friends32CheckBlockedUserListAvailabilityEPbRKNS_7account3UidE",[](Registers r)->uint64_t{
            if(r[0])*ptr<bool>(r[0])=false; return 0;}},
        {"_ZN2nn7friends30EnsureBlockedUserListAvailableEPNS0_12AsyncContextE",unavailable},
        {"_ZN2nn7friends30EnsureBlockedUserListAvailableEPNS0_12AsyncContextERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi",unavailable},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileEPKNS_7account23NetworkServiceAccountIdEi",unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_",unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyEb",unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyENS_7account23NetworkServiceAccountIdEb",unavailable},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionEv",unavailable},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionEv",unavailable},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE",unavailable},
        // AsyncContext / NotificationQueue / UserPresence: no IPC object is
        // created, so construction writes nothing and no request is ever sent.
        {"_ZN2nn7friends12AsyncContextC1Ev",ok},
        {"_ZN2nn7friends12AsyncContextC2Ev",ok},
        {"_ZN2nn7friends12AsyncContextD1Ev",ok},
        {"_ZN2nn7friends12AsyncContextD2Ev",ok},
        {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",completed_account_event},
        {"_ZN2nn7friends12AsyncContext9GetResultEv",unavailable},
        {"_ZNK2nn7friends12AsyncContext9GetResultEv",unavailable},
        {"_ZN2nn7friends12AsyncContext6CancelEv",ok},
        {"_ZN2nn7friends12AsyncContext7HasDoneEPb",[](Registers r)->uint64_t{if(r[0])*ptr<bool>(r[0])=true;return 0;}},
        {"_ZN2nn7friends17NotificationQueueC1Ev",ok},
        {"_ZN2nn7friends17NotificationQueueD1Ev",ok},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends17NotificationQueue14GetSystemEventEv",[](Registers r)->uint64_t{return r[0];}},
        {"_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE",unavailable},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc",unavailable},
        {"_ZN2nn7friends12UserPresence6CommitEv",unavailable},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE",[](Registers r)->uint64_t{*ptr<bool>(r[0])=false;return 0;}},
        {"_ZN2nn7account9CloseUserERKNS0_10UserHandleE",ok},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE",[](Registers r)->uint64_t{if(r[0])*ptr<uint64_t>(r[0])=0;return 0x2a2;}},
        {"_ZN2nn7account12AsyncContextC1Ev",ok},
        {"_ZN2nn7account12AsyncContextD1Ev",ok},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",unavailable},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE",completed_account_event},
        {"_ZN2nn7account12AsyncContext9GetResultEv",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev",ok},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev",ok},
        {"_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE",completed_account_event},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv",unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm",[](Registers r)->uint64_t{if(r[1])*ptr<uint64_t>(r[1])=0;return 0x2a2;}},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE",unavailable},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc",unavailable},
        {"_ZN2nn7friends12UserPresence6CommitEv",unavailable},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",unavailable},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_",unavailable},
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb",[](Registers)->uint64_t{return 0;}},
        {"_ZN2nn3err9ShowErrorENS_6ResultE",[](Registers r)->uint64_t{ot2_host_log("SDK_ERROR result=%x module=%u description=%u",unsigned(r[0]),unsigned(r[0])&511,unsigned(r[0])>>9);return 0;}},
        // Language. nn::oe::GetDesiredLanguage returns nn::settings::LanguageCode,
        // a char[8] carried in x0 -- not the nn::oe::Language enumeration. This
        // handler is reachable and does deliver "zh-Hans" (verified by logging the
        // call and its return value), but this title's UI language does NOT come
        // from here: answering with the enumeration 6, or with "fr", leaves the UI
        // in Japanese. The real source is the am applet's GetDesiredLanguage IPC
        // command, which this port answers with a stand-in session (all zeros), so
        // the title falls back to its own default. Kept as-is; see the handover
        // document §8.1 before changing it again.
        {"_ZN2nn2oe18GetDesiredLanguageEv",[](Registers)->uint64_t{uint64_t code=0;memcpy(&code,"zh-Hans",7);return code;}},
        // 0 is Handheld: only that mode makes a title read the touch panel.
        {"_ZN2nn2oe16GetOperationModeEv",ok},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv",[](Registers)->uint64_t{return active_time();}},
        {"_ZN2nn2oe18GetPerformanceModeEv",ok},
        {"_ZN2nn2oe20GetCurrentFocusStateEv",[](Registers)->uint64_t{std::lock_guard lock(activity_mutex);return activity_focus;}},
        {"_ZN2nn2oe25TryPopNotificationMessageEPj",[](Registers r)->uint64_t{std::lock_guard lock(activity_mutex);if(!focus_notification)return 0;*ptr<uint32_t>(r[0])=15;focus_notification=false;return 1;}},
        {"_ZN2nn2oe17FinishStartupLogoEv",ok},
        {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",ok},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",ok},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",ok},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb",ok},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",ok},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",ok},
        {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb",ok},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv",ok},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv",ok},
        {"_ZN2nn2oe16EnableScreenShotEv",ok},
        {"_ZN2nn2oe17DisableScreenShotEv",ok},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE",[](Registers r)->uint64_t{memcpy(ptr<void>(r[0]),uid.data(),16);return 0;}},
        {"_ZN2nn3aoc17CountAddOnContentEv",[](Registers)->uint64_t{return addon_content.size();}},
        {"_ZN2nn3aoc16ListAddOnContentEPiii",[](Registers r)->uint64_t{
            int offset=int32_t(r[1]),count=int32_t(r[2]);if(offset<0||count<=0||!r[0])return 0;
            int position=0,written=0;auto* out=ptr<int32_t>(r[0]);
            for(const auto& [index,_]:addon_content)if(position++>=offset&&written<count)out[written++]=index;
            return written;
        }},
        {"_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE",[](Registers r)->uint64_t{
            // The installed DLC set is immutable during a run. A real unsignaled
            // host event lets the original SDK wait without a service-session pointer.
            const uint64_t args[]={r[0],0,1,0,0,0,0,0};
            return ot2_call_guest("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",args);
        }},
        {"_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE",[](Registers r)->uint64_t{return r[0]==program_id && std::filesystem::is_directory(user+"/save");}},
        {"_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE",[](Registers r)->uint64_t{if(r[1]!=program_id || !std::filesystem::is_directory(user+"/save"))return PathNotFound;return mount(str(r[0]),user+"/save",true);}},
        {"_ZN2nn2fs7UnmountEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=mounts.find(str(r[0]));if(it!=mounts.end()){if(it->second.save)invalidate_save_handles(it->second.save);mounts.erase(it);}return 0;}},
        {"_ZN2nn2fs10CreateFileEPKcl",create_file},
        {"_ZN2nn2fs10CreateFileEPKcli",create_file},
        {"_ZN2nn2fs10DeleteFileEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;return unlink(p.c_str())?error():0;}},
        {"_ZN2nn2fs10RenameFileEPKcS2_",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto a=path(str(r[0]),true),b=path(str(r[1]),true);if(a.empty() || b.empty())return PermissionDenied;
            struct stat existing{};if(!lstat(b.c_str(),&existing))return PathExists;if(errno!=ENOENT)return error();return rename(a.c_str(),b.c_str())?error():0;}},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(fd(r[0])<0 || int64_t(r[1])<0)return IoError;
            if(int e=save_test_fault("resize")){poison_file(r[0],e);return IoError;}if(ftruncate(fd(r[0]),r[1])){poison_file(r[0],errno);return error();}return 0;}},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",write_file},
        {"_ZN2nn2fs9FlushFileENS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(fsync(fd(r[0]))){poison_file(r[0],errno);return error();}return 0;}},
        {"_ZN2nn2fs14CommitSaveDataEPKc",commit},
        {"_ZN2nn2fs6CommitEPKc",commit},
        {"_ZN2nn2fs15CreateDirectoryEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);if(create_target_exists(str(r[0])))return PathExists;auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;return mkdir(p.c_str(),0700)?error():0;}},
        {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto p=path(str(r[0]),true);if(p.empty())return PermissionDenied;
            for(const auto& [name,m]:mounts){std::error_code same_error;if(std::filesystem::equivalent(p,m.directory,same_error))return PermissionDenied;}
            std::error_code ec;if(!std::filesystem::is_directory(p,ec))return PathNotFound;std::filesystem::remove_all(p,ec);return ec?IoError:0;
        }},
        {"_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll",[](Registers r)->uint64_t{return check_storage_size(r[1],r[2]);}},
        {"_ZN2nn2fs18CreateCacheStorageEill",create_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKci",mount_cache},
        {"_ZN2nn2fs17MountCacheStorageEPKc",[](Registers r)->uint64_t{uint64_t args[]={r[0],0};return mount_cache(args);}},
        {"_ZN2nn2fs19GetCacheStorageSizeEPlS1_i",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[2]));if(directory.empty())return IoError;
            std::ifstream metadata(directory+"/sizes",std::ios::binary);if(!metadata)return PathNotFound;int64_t sizes[2]{};
            if(!metadata.read(reinterpret_cast<char*>(sizes),sizeof(sizes)))return IoError;*ptr<int64_t>(r[0])=sizes[0];*ptr<int64_t>(r[1])=sizes[1];return 0;
        }},
        {"_ZN2nn2fs18DeleteCacheStorageEi",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto directory=cache_directory(int32_t(r[0]));if(directory.empty())return IoError;
            for(const auto& [name,m]:mounts)if(m.directory==directory+"/data")return PermissionDenied;
            std::error_code ec;if(!std::filesystem::exists(directory,ec))return PathNotFound;std::filesystem::remove_all(directory,ec);return ec?IoError:0;
        }},
        {"_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE",open_cache_list},
        {"_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi",[](Registers r)->uint64_t{
            std::lock_guard lock(filesystem_mutex);auto it=cache_lists.find(r[2]);if(it==cache_lists.end() || int32_t(r[3])<0)return IoError;
            auto& list=it->second;size_t n=std::min<size_t>(int32_t(r[3]),list.indices.size()-list.position);auto* out=ptr<uint8_t>(r[1]);
            for(size_t i=0;i<n;i++){memset(out+i*32,0,32);memcpy(out+i*32,&list.indices[list.position++],4);}*ptr<int32_t>(r[0])=n;return 0;
        }},
        {"_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);return cache_lists.erase(r[0])?0:IoError;}},
        {"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=0x1000;return addon_content.contains(int32_t(r[1]))?0:0x7d402;}},
        {"_ZN2nn2fs17MountAddOnContentEPKciPvm",[](Registers r)->uint64_t{auto it=addon_content.find(int32_t(r[1]));return it==addon_content.end()?0x7d402:mount(str(r[0]),it->second,true);}},
        {"_ZN2nn2fs22QueryMountRomCacheSizeEPm",[](Registers r)->uint64_t{*ptr<uint64_t>(r[0])=4096;return 0;}},
        {"_ZN2nn2fs8MountRomEPKcPvm",[](Registers r)->uint64_t{return mount(str(r[0]),romfs,true);}},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",[](Registers r)->uint64_t{return mount(str(r[0]),user+"/save",false,true);}},
        {"_ZN2nn2fs21MountTemporaryStorageEPKc",[](Registers r)->uint64_t{return mount(str(r[0]),user+"/temporary",false);}},
        {"_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE",[](Registers)->uint64_t{std::error_code ec;std::filesystem::create_directories(user+"/save",ec);return ec?IoError:0;}},
        {"_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE",[](Registers)->uint64_t{return std::filesystem::is_directory(user+"/save");}},
        {"_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE",ok},
        {"_ZN2nn2fs25EnableGlobalFileDataCacheEPvm",ok},
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",open_file},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",read_file},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",[](Registers r)->uint64_t{uint64_t count=0;uint64_t args[]={reinterpret_cast<uint64_t>(&count),r[0],r[1],r[2],r[3]};auto result=read_file(args);return result?result:count==r[3]?0:IoError;}},
        // SDK 5.4.0.0 appended `const ReadOption&` to both ReadFile overloads.
        // The five leading arguments did not move, so the same body answers it.
        // Leaving these unmatched is not a quiet fallback: the guest then runs
        // nn::fs's own implementation, which resolves the host's stand-in
        // FileHandle through fsa and dereferences the null it finds (measured on
        // Dragon Quest Builders, sdk+0x56ab4 reading LINKDATA.BIN).
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE",read_file},
        {"_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE",[](Registers r)->uint64_t{uint64_t count=0;uint64_t args[]={reinterpret_cast<uint64_t>(&count),r[0],r[1],r[2],r[3]};auto result=read_file(args);return result?result:count==r[3]?0:IoError;}},
        {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);struct stat st{};if(fstat(fd(r[1]),&st))return error();*ptr<int64_t>(r[0])=st.st_size;return 0;}},
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=files.find(r[0]);if(it==files.end())return IoError;
            // Commit may follow CloseFile; sync writable content before losing
            // its descriptor, then Commit persists the mount directory entry.
            int f=it->second.descriptor,flags=fcntl(f,F_GETFL);uint32_t result=0;
            if(flags>=0 && (flags&O_ACCMODE)!=O_RDONLY && fsync(f)){poison_file(r[0],errno);result=error();}
            if(!it->second.reader&&close(f) && !result){poison_file(r[0],errno);result=error();}files.erase(it);return result;}},
        {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto p=path(str(r[1]));if(p.empty())return PathNotFound;struct stat st{};if(lstat(p.c_str(),&st))return error();*ptr<int32_t>(r[0])=S_ISDIR(st.st_mode)?0:1;return 0;}},
        {"_ZN2nn2fs16GetFreeSpaceSizeEPlPKc",[](Registers r)->uint64_t{struct statvfs st{};if(statvfs(user.c_str(),&st))return error();*ptr<int64_t>(r[0])=uint64_t(st.f_bavail)*st.f_frsize;return 0;}},
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",open_directory},
        {"_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=directories.find(r[1]);if(it==directories.end())return IoError;*ptr<int64_t>(r[0])=it->second.entries.size();return 0;}},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);auto it=directories.find(r[2]);if(it==directories.end() || int64_t(r[3])<0)return IoError;auto& d=it->second;size_t n=std::min<size_t>(r[3],d.entries.size()-d.position);if(n)memcpy(ptr<void>(r[1]),d.entries.data()+d.position,n*sizeof(DirectoryEntry));d.position+=n;*ptr<int64_t>(r[0])=n;return 0;}},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",[](Registers r)->uint64_t{std::lock_guard lock(filesystem_mutex);return directories.erase(r[0])?0:IoError;}},
    };
    return services;
}
}
