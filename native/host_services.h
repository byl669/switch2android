#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ot2 {
struct HostService { const char* name; uint64_t (*call)(uint64_t*); };
const std::vector<HostService>& host_services();
// Symbol bindings that stand in for Horizon's service manager and the getters
// that would otherwise open a session through it.
const std::vector<HostService>& compatibility_services();
// Which service a stand-in session was opened for, or null when the handle did
// not come from the service manager. Valid until the next open.
const char* compatibility_session_service(uint64_t handle);
size_t compatibility_service_count();
const std::vector<HostService>& audio_services();
// AudioRenderer (audren) memory-pool handshake, which has no server in this process.
const std::vector<HostService>& audio_renderer_services();
// nn::pl shared font, served from the device's own TrueType faces.
const std::vector<HostService>& shared_font_services();
// Whether the title's own module imports the SDK's font decode helper. A title
// that decodes the shared font itself has to be handed the header, because the
// helper deobfuscates in place; one that passes the address straight to the font
// engine has to be handed the TTF past that header. The answer comes from main's
// import table and is set once during boot.
void set_shared_font_decodes(bool decodes);
bool shared_font_decodes();
const std::vector<HostService>& input_services();
const std::vector<HostService>& movie_services();
// Builds an nn::os::SystemEventType from a real host event pair, exactly the
// way the SDK's own detail::AttachInterProcessEvent does. The field offsets are
// taken from that function (sdk+0x349830) and cross-checked against every read
// site: +0x00/+0x08 self pointers, +0x10 manual clear, +0x11 inter-process,
// +0x12 autoclear, +0x13 writable present, +0x14 readable handle, +0x18
// writable handle, +0x28 type tag (2 = inter-process). Returns 0 or a Horizon
// error result.
uint32_t os_attach_system_event(void* event,bool autoclear,unsigned clear_mode);
// Wakes every event handed out by os_attach_system_event. Those events stand in
// for "the controller/notification state changed", which on this host is what
// the user's input does, so the input path calls this when it changes.
void signal_attached_events();
unsigned attached_events_bound();
void set_activity_active(bool active);
void set_io_prefetch(bool enabled);
// nn::oe::FocusState: 1 = focused, 2 = visible without focus, 3 = background.
void set_activity_focus(unsigned state);
void set_audio_active(bool active);
bool probe_audio_recovery();
// ---- AudioRenderer (audren) output sink ---------------------------------
// A title that renders its audio graph with nn::audio::AudioRenderer never calls
// AudioOut, so the adapter above never opens a stream and the mixed result used
// to be dropped on the floor. These three entry points give the renderer the
// same Output the AudioOut path uses -- one AAudio stream, the same focus
// handling, the same error recovery and the same paced-silent fallback -- so the
// graph's DeviceSink reaches the speaker.
//
// open returns a sink id, or 0 when no output is available; write takes the
// interleaved signed 16-bit samples the renderer produced for one frame.
uint64_t audren_sink_open(int rate,int channels);
void audren_sink_write(uint64_t sink,const int16_t* interleaved,int frames);
void audren_sink_close(uint64_t sink);
// Legacy profile only. New launches supply the validated game's ProgramId.
constexpr uint64_t legacy_ot2_program_id=0x0100a3501946e000ULL;
void configure_services(const std::string& romfs,const std::string& user_directory,uint64_t program_id=legacy_ot2_program_id);
uint64_t configured_program_id();
// Coalesced process-wide notifications; the Android UI drains on its main
// thread and retains them across Activity recreation until acknowledged.
enum SaveNotice : unsigned { SaveReadOnly=1, SaveCommitFailed=2, SaveDurabilityUncertain=4 };
unsigned take_save_notice();
}

// Names the loaded module containing an address, for logs that report guest
// return addresses. Returns nullptr when the address is outside every module.
extern "C" const char* ot2_host_module_of(uint64_t address,uintptr_t* offset);
