#include "nvn_program.h"
#include "traced_mutex.h"
#include "../thread_services.h"
#include <cstdlib>
#include <map>
#include <mutex>
namespace {
ot2::TracedMutex mutex{"nvn_program"};
struct Entry {std::shared_ptr<const NvnProgramVersion> owner;std::weak_ptr<const NvnProgramVersion> version;};
std::map<uint64_t,Entry> versions;
}
// Set by the build when shader keys should be printed (they are the
// lookup keys into the offline cache, so this is the one line that tells
// whether the host and the cache agree).
static const bool diagnostic_shader_keys=getenv("OT2_SHADER_KEYS")!=nullptr;
NvnProgramVersion::NvnProgramVersion(const prog_t& p):value(p){
    if(p.nstage>6)ot2_host_die("Program has too many stages");
    for(unsigned i=0;i<p.nstage;i++){
        // Only the control section is guaranteed valid here. A title is allowed
        // to name a code address that is not mapped yet and fill it in before
        // the first draw, and it may also set a placeholder control section
        // first (0x12345678 with a zero length). Neither is an error: the host
        // hashes what it was given and looks the key up in the offline cache,
        // so an unready stage simply finds nothing. The iOS host in nxnative
        // works the same way and never validates these fields.
        if(!p.sd[i].control) {
            // A stage with no control section yet. The title fills it in on a
            // later SetShaders call; SetShaders always reports success on the
            // real API, so a missing section must not stop the program either.
            value.sd[i].control=0;value.sd[i].data=0;
            ot2_host_log("SHADER stage[%u] no control yet",i);
            continue;
        }
        // The control section declares its own length at +0x20. Sections of at
        // least 0x880 bytes use the first 0x880; shorter ones (SDK 10's 3D
        // World has 0x850) must be hashed at their own size or the key cannot
        // match the offline cache.
        uint32_t nctl=0x880,own=0;memcpy(&own,static_cast<const uint8_t*>(p.sd[i].control)+0x20,4);
        if(own>=0x760&&own<0x880)nctl=own;
        memcpy(controls[i].data(),p.sd[i].control,nctl);
        // The key is defined over the control bytes only: the code area is a
        // pool address that may not be mapped yet, so it cannot take part.
        uint64_t hash=14695981039346656037ULL;for(uint32_t j=0;j<nctl;j++)hash=(hash^controls[i][j])*1099511628211ULL;
        char key[17];snprintf(key,sizeof(key),"%016llx",(unsigned long long)hash);keys[i]=key;

        uint32_t magic,size,offset,stage;
        memcpy(&magic,controls[i].data(),4);memcpy(&offset,controls[i].data()+0x700,4);memcpy(&size,controls[i].data()+0x704,4);memcpy(&stage,controls[i].data()+0x714,4);
        const bool ready=magic==0x98761234&&size>=8&&size<=8*1024*1024&&offset<=size&&stage<6&&stages[stage]==-1;
        if(!ready) {
            // Keep the stage out of the program but leave a trace: it is
            // resolved by a later SetShaders call once the title has data.
            value.sd[i].control=controls[i].data();value.sd[i].data=0;
            ot2_host_log("SHADER %s stage[%u] not ready magic=%#x size=%#x offset=%#x code=%s",
                key,i,magic,size,offset,p.sd[i].data?"mapped":"unmapped");
            continue;
        }
        const auto* code_bytes=reinterpret_cast<const uint8_t*>(p.sd[i].data);
        if(code_bytes)code[i].assign(code_bytes,code_bytes+size);
        else code[i].clear();
        // Now that the code area is in hand, the key is formed over it as well as
        // the control section. The provisional key above is control-only only
        // because an unmapped code address cannot take part before this point;
        // it stays in the "not ready" branch, where no draw can use the stage.
        //
        // This matters for this title in particular: 613 of its 2125 shaders
        // share a control section and differ only in code (the Bezel Engine keeps
        // one control per program family and one code area per variant), so a
        // control-only key answers every variant with whichever sibling was
        // translated first. nxnative's host and shader_split.py key the same pair.
        {
            uint64_t full=14695981039346656037ULL;
            for(uint32_t j=0;j<nctl;j++)full=(full^controls[i][j])*1099511628211ULL;
            for(unsigned char b: code[i])full=(full^b)*1099511628211ULL;
            snprintf(key,sizeof(key),"%016llx",(unsigned long long)full);keys[i]=key;
        }
        value.sd[i].control=controls[i].data();
        value.sd[i].data=code[i].empty()?0:reinterpret_cast<uint64_t>(code[i].data());
        stages[stage]=i;
        if(diagnostic_shader_keys)ot2_host_log("SHADER %s stage=%u READY size=%#x nctl=%#x code=%s",key,stage,size,nctl,code[i].empty()?"unmapped":"copied");
    }
}
extern "C" void nvn_program_publish(prog_t* p){
    auto version=std::make_shared<NvnProgramVersion>(*p);*p=version->value;
    std::lock_guard lock(mutex);versions[p->serial]={version,version};
    std::erase_if(versions,[](const auto& e){return !e.second.owner&&e.second.version.expired();});
}
extern "C" void nvn_program_release(uint64_t serial){std::lock_guard lock(mutex);auto i=versions.find(serial);if(i!=versions.end()){i->second.owner.reset();if(i->second.version.expired())versions.erase(i);}}
std::shared_ptr<const NvnProgramVersion> nvn_program_get(uint64_t serial){std::lock_guard lock(mutex);auto i=versions.find(serial);if(i==versions.end())ot2_host_die("Program version is no longer owned");auto p=i->second.version.lock();if(!p)ot2_host_die("Program version expired");return p;}
