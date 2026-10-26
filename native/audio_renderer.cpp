// nn::audio AudioRenderer (audren).
//
// This title builds an audio graph with the AudioRenderer API rather than the
// older AudioOut API. Most of that API is pure client-side bookkeeping inside the
// SDK: building the config, acquiring voices and mixes, filling in a memory pool.
// Those calls already work here because nothing replaces them.
//
// ---- Part one: the memory-pool handshake --------------------------------
//
// The attach handshake cannot work on its own. nn::audio memory pools have a
// state field that the *renderer server* advances:
//
//   Acquired(1) -> RequestAttach(4) -> Attached(5)
//
// RequestAttachMemoryPool only records the request and moves the field to 4. The
// server is supposed to process it on the next render cycle and write 5 back. No
// audren server exists in this process, so the field stays at 4 forever and a
// title that waits for Attached spins on a 5 ms poll (observed at main+0x2ed2b4,
// which sleeps via nn::os::SleepThread and re-reads the state through
// nn::audio::MemoryPoolInfo::GetState).
//
// The field itself is reachable: GetState is "return *(u32*)(info + 0x10)" and the
// caller passes a holder whose first word points at that info object. So the
// handshake is completed here by writing the state the server would have written.
//
// ---- Part two: rendering ------------------------------------------------
//
// The renderer server is not needed either: newer SDKs link the whole renderer
// into the application for the "CPU rendering + manual execution" mode
// (AudioRendererParameter renderingDevice = 1, execution mode = 1). In that mode
// OpenAudioRenderer goes through the in-process DFC manager -- install_service_
// initializers already redirects CreateAudioRendererManagerByHipc to it -- and
// rendering happens where nn::audio::ExecuteAudioRendererRendering is called.
//
// So the host rewrites the parameter to CPU + manual and calls the SDK original.
// Whether the rewritten parameter is accepted is the one thing that cannot be
// known statically, so a rejected open falls back to the original parameter and
// latches that choice instead of failing the title's audio start-up.
//
// The step-around matters: a takeover replaces the function's first instruction,
// so "call the SDK original" goes through ot2_original_entry rather than the
// export address, which would re-enter the takeover.
#include "host_services.h"
#include "thread_services.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

namespace ot2 {
namespace {
using R = uint64_t*;

// nn::audio::MemoryPoolState. Only the two transitions a title performs are
// needed, plus the initial state so a detach has somewhere to return to.
enum : uint32_t {
    PoolAcquired = 1,
    PoolDetached = 3,
    PoolRequestAttach = 4,
    PoolAttached = 5,
};

// The pool object handed to the client is a holder whose first word points at the
// info object that carries the state. Reading through a pointer the title built is
// only safe when it actually points into guest memory, so the caller validates
// first: a wrong guess here would take the process down instead of logging.
inline uint64_t impl_of(uint64_t holder) {
    uint64_t impl = 0;
    if (!holder) return 0;
    memcpy(&impl, reinterpret_cast<const void*>(holder), sizeof impl);
    return impl;
}
inline uint32_t read_state(uint64_t impl) {
    uint32_t state = 0;
    if (!impl) return 0;
    memcpy(&state, reinterpret_cast<const void*>(impl + 0x10), sizeof state);
    return state;
}
inline void write_state(uint64_t impl, uint32_t state) {
    if (!impl) return;
    memcpy(reinterpret_cast<void*>(impl + 0x10), &state, sizeof state);
}

// A pointer is only followed when its top bits look like a user-space address on
// this platform (the title's heaps sit around 0x4..., its stacks around 0x7...).
// This is a guard against following a null or a small scalar, not a proof.
inline bool plausible(uint64_t address) {
    return address > 0x10000 && (address >> 40) == 0;
}

// ---- AudioRendererParameter ---------------------------------------------
// The counts come first, then the two fields that select CPU rendering and
// manual execution. 0x40 bytes are copied so the revision word some SDKs append
// stays valid; the SDK reads only its own size. The offsets are the ones this
// SDK uses -- the log line below prints every field, so a wrong guess is visible
// as a nonsensical count next to a plausible one.
enum : uint32_t {
    P_SAMPLE_RATE = 0x00,
    P_SAMPLES_PER_FRAME = 0x04,
    P_MIX_BUFFERS = 0x08,
    P_SUBMIXES = 0x0C,
    P_VOICES = 0x10,
    P_SINKS = 0x14,
    P_EFFECTS = 0x18,
    P_SPLITTERS = 0x24,
    P_DEVICE = 0x2C,       // nn::audio::RenderingDevice: 0 = DSP, 1 = CPU
    P_MODE = 0x30,         // nn::audio::ExecutionMode: 0 = automatic, 1 = manual
    P_CAP = 0x40,
    DeviceCpu = 1,
    ModeManual = 1,
};

struct Param {
    uint8_t bytes[P_CAP];
    void fetch(uint64_t source) {
        if (source) memcpy(bytes, reinterpret_cast<const void*>(source), P_CAP);
        else memset(bytes, 0, P_CAP);
    }
    int32_t at(uint32_t offset) const {
        int32_t value = 0;
        memcpy(&value, bytes + offset, sizeof value);
        return value;
    }
    void set(uint32_t offset, int32_t value) { memcpy(bytes + offset, &value, sizeof value); }
};

void log_param(const char* tag, const Param& p) {
    char line[P_CAP * 5];
    size_t used = 0;
    for (uint32_t i = 0; i < P_CAP; i += 4)
        used += size_t(snprintf(line + used, sizeof line - used, "%s%02x=%d", i ? " " : "", i, p.at(i)));
    ot2_host_log("AUDREN %s %s", tag, line);
}

// The SDK export, stepped around when a gate now sits on it.
uint64_t original(const char* name) {
    const uint64_t address = ot2_guest_symbol_address(name);
    if (!address) return 0;
    return ot2_original_entry(address);
}
uint64_t call_original(const char* name, std::initializer_list<uint64_t> values) {
    const uint64_t entry = original(name);
    if (!entry) {
        // A hook whose original cannot be reached must say so once. It used to
        // alternate between symbol names and print on every call, which both
        // buried the signal and cost ~700 log lines a second in the failing case.
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16)
            ot2_host_log("[RED] audren: no reachable original for %s", name);
        return 0;
    }
    uint64_t arguments[8] {};
    std::copy(values.begin(), values.end(), arguments);
    return ot2_call_guest_address(entry, arguments);
}

constexpr const char* kWorkSize = "_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE";
constexpr const char* kOpenEvent = "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm";
constexpr const char* kOpenPlain = "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm";
constexpr const char* kExecute = "_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE";
constexpr const char* kCreateThread = "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi";
constexpr const char* kStartThread = "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE";
constexpr const char* kClose = "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE";
constexpr const char* kStart = "_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE";
constexpr const char* kRequestUpdate = "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE";
constexpr const char* kElapsedFrames = "_ZN2nn5audio33GetAudioRendererElapsedFrameCountEPKNS0_19AudioRendererConfigE";
constexpr const char* kState = "_ZN2nn5audio21GetAudioRendererStateENS0_19AudioRendererHandleE";
constexpr const char* kInitConfig = "_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm";
// Voice feeding. The minigame result screen stalls right after the result jingle
// has been read and closed, so the one thing worth counting is whether that
// jingle ever reaches a voice. AppendWaveBuffer is what hands a decoded block to
// the mixer; a steady append rate during the stall means the jingle is playing
// and the wait is somewhere else.
constexpr const char* kAppendWave = "_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE";
// Voice observation. Feeding a voice and *observing* it are different questions,
// and only the second one can block a state machine: a title that waits for a
// one-shot sound to end polls GetReleasedWaveBuffer / GetVoicePlayState /
// GetVoicePlayedSampleCount, and those are pure client-side reads that were
// never hooked, so their call rate was invisible. A count that keeps climbing
// while the minigame result screen is stuck proves the wait is on audio; a count
// of zero proves it cannot be, which is worth as much.
constexpr const char* kGetPlayState = "_ZN2nn5audio17GetVoicePlayStateEPKNS0_9VoiceTypeE";
constexpr const char* kGetReleasedWave = "_ZN2nn5audio21GetReleasedWaveBufferEPNS0_9VoiceTypeE";
constexpr const char* kGetPlayedSamples = "_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE";
constexpr const char* kGetWaveCount = "_ZN2nn5audio18GetWaveBufferCountEPKNS0_9VoiceTypeE";
constexpr const char* kSetPlayState = "_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE";
// Where the pipeline actually stalls. The renderer object can be consumed in
// three steps and the voices only advance if all three happen: the game's
// RequestUpdate marshals the client graph, the server side generates a command
// list, and the DSP (CPU version here) processes it. `GetVoicePlayedSampleCount`
// staying at zero means one of the last two never runs, and these two symbols
// say which.
constexpr const char* kSendToDsp = "_ZN2nn5audio6server17AudioRenderSystem16SendCommandToDspEv";
constexpr const char* kDspProcess = "_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv";
// Command generation is the step between the two: it turns the server-side graph
// into the command list the DSP processes. A zero-length list every frame means
// the graph is empty (the voices never made it in), which is a completely
// different bug from "the DSP does not run" -- and both look identical from the
// voice counters.
constexpr const char* kGenCommand = "_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm";
// The info path, i.e. the *other* half of a frame. Disassembling this SDK settles
// the split: AudioRenderSystem::Update (0x59a4c) takes the mutex at +0x1d8,
// memset's the out buffer and runs the whole InfoUpdater chain (UpdateBehaviorInfo,
// UpdateMemoryPools, UpdateVoiceChannelResources, UpdateVoices, UpdateEffects,
// UpdateSplitterInfo, UpdateMixes, UpdateSinks); ExecuteAudioRendererRendering
// (0x5a55c) only does GenerateCommand -> SetProcessTimeMax -> Setup -> Process and
// never touches InfoUpdater. So Update moves the graph in and out, Execute runs the
// DSP, and a played-sample count that never moves means either the DSP produced no
// samples or the results never came back. Counting UpdateVoices separates those.
constexpr const char* kUpdateVoices = "_ZN2nn5audio6server11InfoUpdater12UpdateVoicesERNS1_12VoiceContextEPNS1_14MemoryPoolInfoEi";

// 1 = ask the SDK for CPU rendering + manual execution, 0 = leave the parameter
// alone. Latched to 0 when the SDK rejects the rewritten parameter, so one
// failed open does not put the title into a retry loop.
//
// The initial choice is undecided until the first open, because the marker that
// selects automatic mode lives in the title's data directory and is not
// readable during static initialisation. The marker exists to answer one
// question by measurement: whether this SDK's in-process renderer manager drives
// the graph on its own, or whether the audio pipeline has to be driven from here.
std::atomic<int> g_cpu_manual{-1};
int cpu_manual_mode() {
    int value = g_cpu_manual.load(std::memory_order_relaxed);
    if (value >= 0) return value;
    value = ot2_game_flag("audren-auto") ? 0 : 1;
    g_cpu_manual.store(value, std::memory_order_relaxed);
    return value;
}

// OpenAudioRenderer result plus what the renderer was opened with.
struct Renderer {
    std::atomic<uint64_t> handle[4] {};
    // Writable side of the SystemEvent the title passed to OpenAudioRenderer.
    //
    // In automatic mode the renderer server signals this event once per rendered
    // frame, and the title's audio thread waits on it before feeding the next
    // wave block to its voices. Rewriting the parameter to CPU + manual removes
    // that server, so nothing would ever signal it: the feeder never wakes, every
    // streamed sound stops after the blocks it pre-filled, and any flow that
    // waits for a sound to finish (the minigame result jingle) waits forever.
    // This host therefore plays the server's part and signals it per frame.
    std::atomic<uint32_t> frame_event{0};
    std::atomic<int> open{0};
    std::atomic<uint64_t> executes{0};
    std::atomic<int> render_run{0};
    int frames_per_second{48000};   // filled from the parameter at open
    int samples_per_frame{240};
    // The AudioRendererConfig the title builds, plus counters for the calls the
    // title itself makes. Measured on a live run: the title calls
    // RequestUpdateAudioRenderer, StartAudioRenderer and
    // GetAudioRendererElapsedFrameCount on its own, about once per 5 ms frame,
    // and the elapsed-frame count tracks the update count one for one (60001
    // calls -> 59998 frames). So the graph is driven entirely from the title and
    // the host thread below only has to wait on Execute. This was worth measuring
    // because the minigame result screen stalls forever right after the result
    // jingle is read from disk, which looked like "the jingle never finishes
    // playing"; the answer is that audio keeps advancing during the stall and is
    // not the blocker. Keep the counters: they are the cheap way to re-check that
    // conclusion instead of re-deriving it.
    std::atomic<uint64_t> config{0};
    std::atomic<uint64_t> updates{0};
    std::atomic<uint64_t> starts{0};
    // AppendWaveBuffer calls. This is the last link of the same investigation:
    // update/start/elapsed proved the mixer advances, but not that the result
    // jingle actually reaches a voice. A one-shot wave is appended once and then
    // reported released by the renderer, so a count that stops dead while the
    // result screen is up would mean the wave never made it into the graph.
    std::atomic<uint64_t> appends{0};
    // Voice-observation probes (see kGetPlayState & co). Separate counters so a
    // stall can be attributed to one specific question the title keeps asking.
    std::atomic<uint64_t> play_state{0};
    std::atomic<uint64_t> released_wave{0};
    std::atomic<uint64_t> played_samples{0};
    std::atomic<uint64_t> wave_count{0};
    std::atomic<uint64_t> set_play_state{0};
    std::atomic<uint64_t> send_to_dsp{0};
    std::atomic<uint64_t> dsp_process{0};
    std::atomic<uint64_t> gen_bytes{0};
    std::atomic<uint64_t> gen_max{0};
    std::atomic<uint64_t> gen_calls{0};
    std::atomic<uint64_t> update_voices{0};
    // The command list the last GenerateCommand produced. Kept so the Process probe
    // can look at the mix buffer *after* the DSP ran: a command list that is built
    // and consumed but leaves the mix buffer silent means no voice is contributing,
    // which is a completely different failure from "the results never came back".
    std::atomic<uint64_t> cmd_list{0};
    std::atomic<uint64_t> cmd_len{0};
    std::atomic<uint64_t> mix_peak{0};      // highest |sample| seen in the mix buffer
    std::atomic<uint64_t> mix_silent{0};    // frames whose mix buffer was entirely zero
};
Renderer g_renderer;

// ---- per-voice trace table ----------------------------------------------
// Every probe above counts calls across the whole session and prints the first
// few, which is the wrong shape for this question. The title owns about a dozen
// voices and creates them at different times; "the fifth AppendWaveBuffer of the
// session" says nothing about whether the *result jingle* ever reached a voice.
//
// The reference build answers the question from the file side: it reads the
// jingle in two reads (0+131072, then 131072+6592) and opens the result BGM
// about two seconds later, i.e. the splash waits for the jingle to finish. So
// what has to be observable here is a per-voice timeline: first append, first
// Play, first sample actually played, and the first state change. Only the
// transitions are printed, so the cost is a handful of lines per voice.
constexpr unsigned VoiceSlots = 32;
struct VoiceTrace {
    std::atomic<uint64_t> handle{0};
    std::atomic<uint32_t> appends{0};
    std::atomic<uint32_t> append_failures{0};
    std::atomic<uint64_t> states{0};        // bit set of PlayState values ever set
    std::atomic<uint64_t> played_max{0};    // highest GetVoicePlayedSampleCount seen
    std::atomic<uint64_t> state_max{0};     // highest GetVoicePlayState value seen
};
VoiceTrace g_voices[VoiceSlots];

VoiceTrace* voice_slot(uint64_t handle) {
    if (!handle) return nullptr;
    unsigned spare = VoiceSlots;
    for (unsigned i = 0; i < VoiceSlots; i++) {
        const uint64_t current = g_voices[i].handle.load(std::memory_order_relaxed);
        if (current == handle) return &g_voices[i];
        if (!current && spare == VoiceSlots) spare = i;
    }
    if (spare == VoiceSlots) return nullptr;
    uint64_t expected = 0;
    if (!g_voices[spare].handle.compare_exchange_strong(expected, handle,
                                                        std::memory_order_relaxed))
        return nullptr;
    return &g_voices[spare];
}

// ---- DeviceSink to the speaker ------------------------------------------
// The last step of a real frame, and the one this host used to skip: the
// generated command list ends in a DeviceSink command that names the mix buffers
// holding the finished stereo mix, and the audren server hands those to the audio
// device. With CPU rendering nothing writes them out, so the graph produced a
// perfectly good mix that nobody could hear -- the mix probe below showed
// `loud=480/1440 peak=17064` while the tablet stayed silent.
//
// Layout (SDK 16, and the reference host's dm/host/hle_audren.c capture()):
// command +0x04 enabled, +0x05 id, +0x06 size as s16; DeviceSink is id 0x13 and
// 0x148 bytes, with the input count at +0x130 and the mix buffer indices as s16
// at +0x134. Mix buffers are int32 holding 16-bit range, so the sink clamps --
// the SDK keeps headroom until here on purpose.
constexpr uint8_t kSinkCommandId = 0x13;
constexpr uint16_t kSinkCommandSize = 0x148;
// The system default 6-to-2 downmix in Q15: front 1.0, center 0.707, LFE 0.251,
// back 0.707. Only used when a title drives six channels into the sink.
enum : int32_t { DownFront = 32768, DownCentre = 23167, DownLfe = 8224, DownBack = 23167 };

std::atomic<uint64_t> g_sink{0};
std::atomic<uint64_t> g_sink_frames{0};

uint64_t sink_handle(int rate, int channels) {
    uint64_t sink = g_sink.load(std::memory_order_acquire);
    if (sink) return sink;
    if (ot2_game_flag("audren-silent-sink")) return 0;
    const uint64_t opened = audren_sink_open(rate, channels);
    uint64_t expected = 0;
    if (!g_sink.compare_exchange_strong(expected, opened)) {
        if (opened) audren_sink_close(opened);
        return expected;
    }
    return opened;
}

// Runs right after CommandListProcessor::Process, i.e. with a finished mix.
void capture_sink() {
    const uint64_t list = g_renderer.cmd_list.load(std::memory_order_relaxed);
    const uint64_t len = g_renderer.cmd_len.load(std::memory_order_relaxed);
    if (!list || len < 0x20) return;
    const auto* base = reinterpret_cast<const uint8_t*>(list);
    uint64_t mix = 0;
    uint32_t ncmd = 0, spf = 0, rate = 0;
    memcpy(&mix, base + 8, 8);
    memcpy(&ncmd, base + 4, 4);
    memcpy(&spf, base + 0x14, 4);
    memcpy(&rate, base + 0x18, 4);
    if (!plausible(mix) || !spf || spf > 1024) return;
    if (rate != 48000) {
        static std::atomic<unsigned> once{0};
        if (once.fetch_add(1, std::memory_order_relaxed) < 1)
            ot2_host_log("[RED] audren: renderer sample rate %u; the sink only takes 48000", rate);
        return;
    }
    const auto* mix_samples = reinterpret_cast<const int32_t*>(mix);
    const uint8_t* q = base + 0x20;
    const uint8_t* end = base + len;
    for (uint32_t i = 0; i < ncmd && q + 8 <= end; i++) {
        int16_t csize = 0;
        memcpy(&csize, q + 6, 2);
        if (csize <= 0 || q + csize > end) return;
        if (q[5] == kSinkCommandId && uint16_t(csize) == kSinkCommandSize && q[4]) {
            uint32_t inputs = 0;
            memcpy(&inputs, q + 0x130, 4);
            if (!inputs || inputs > 6) return;
            const auto* index = reinterpret_cast<const int16_t*>(q + 0x134);
            const uint64_t sink = sink_handle(int(rate), 2);
            if (!sink) return;
            const int32_t* channel[6];
            for (uint32_t j = 0; j < inputs; j++)
                channel[j] = mix_samples + size_t(uint16_t(index[j])) * spf;
            int16_t pcm[2 * 1024];
            for (uint32_t s = 0; s < spf; s++) {
                int32_t left, right;
                if (inputs >= 6) {
                    const int64_t centre = int64_t(channel[2][s]) * DownCentre + int64_t(channel[3][s]) * DownLfe;
                    left = int32_t((int64_t(channel[0][s]) * DownFront + centre + int64_t(channel[4][s]) * DownBack) >> 15);
                    right = int32_t((int64_t(channel[1][s]) * DownFront + centre + int64_t(channel[5][s]) * DownBack) >> 15);
                } else {
                    left = channel[0][s];
                    right = inputs > 1 ? channel[1][s] : left;
                }
                left = left > 32767 ? 32767 : left < -32768 ? -32768 : left;
                right = right > 32767 ? 32767 : right < -32768 ? -32768 : right;
                pcm[2 * s] = int16_t(left);
                pcm[2 * s + 1] = int16_t(right);
            }
            int peak = 0;
            for (uint32_t k = 0; k < 2 * spf; k++) {
                const int sample = pcm[k];
                const int magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak) peak = magnitude;
            }
            if (!peak) g_renderer.mix_silent.fetch_add(1, std::memory_order_relaxed);
            else g_renderer.mix_peak.store(std::max<uint64_t>(g_renderer.mix_peak.load(std::memory_order_relaxed), uint64_t(peak)),
                                           std::memory_order_relaxed);
            audren_sink_write(sink, pcm, int(spf));
            const uint64_t written = g_sink_frames.fetch_add(spf, std::memory_order_relaxed) + spf;
            if (written == spf || written % (48000ul * 10) < spf)
                ot2_host_log("AUDREN sink wrote %llu frames (%.1f s) inputs=%u spf=%u peak=%d",
                             (unsigned long long)written, double(written) / 48000.0, inputs, spf, peak);
            return;
        }
        q += csize;
    }
}

// ---- render thread -------------------------------------------------------
// In CPU + manual mode nothing renders on its own: the title's own renderer only
// runs where nn::audio::ExecuteAudioRendererRendering is called, and on hardware
// that call came from the audio server's thread. There is no server here, so the
// host provides the thread and paces it at the frame period
// (samplesPerFrame / sampleRate, 5 ms here).
//
// The thread is created through the SDK's nn::os::CreateThread rather than as a
// plain pthread: then ThreadType, the thread handle and the TLS slots are all in
// place, and the renderer's own nn::os locks see a proper owner. A bare pthread
// with a hand-registered context would deadlock the first time the SDK took a
// mutex under it.
static void render_main(void* argument);

bool spawn_render_thread() {
    // The stack must be guest memory. nn::os::CreateThread hands it to
    // svcMapStack, which copies it into the guest stack area and only accepts a
    // source that already lives in the guest heap -- a host allocation is
    // rejected with MAP_STACK_REJECT and the thread never starts. The title's own
    // malloc is the guest heap, so the stack comes from there.
    // svcMapStack also needs the source 4 KiB aligned, which malloc does not
    // promise for a 64 KiB block, so the allocation is rounded up and aligned.
    constexpr uint64_t stack_size = 0x10000;
    const uint64_t raw = call_original("malloc", {stack_size + 0x2000});
    const uint64_t stack = raw ? ((raw + 0xFFF) & ~uint64_t(0xFFF)) : 0;
    void* thread_type = calloc(1, 0x200);      // nn::os::ThreadType is 0x1c0 bytes here
    if (!stack || !thread_type) {
        ot2_host_log("[RED] audren: could not allocate a guest stack (malloc %#llx)", (unsigned long long)raw);
        free(thread_type);
        return false;
    }
    // Priority 9 sits below the game's own threads (44/45) so rendering can never
    // starve the frame loop; the renderer only needs 5 ms of work per 5 ms frame.
    const uint64_t created = call_original(kCreateThread,
        {uintptr_t(thread_type), uintptr_t(render_main), 0, stack, stack_size, 9});
    if (created) {
        ot2_host_log("[RED] audren: nn::os::CreateThread failed %#llx (stack %#llx)", (unsigned long long)created, (unsigned long long)stack);
        free(thread_type);
        return false;
    }
    call_original(kStartThread, {uintptr_t(thread_type)});
    return true;
}

static void render_main(void* argument) {
    (void)argument;
    g_renderer.render_run.store(1, std::memory_order_release);
    // AudioRendererHandle is two words passed by value, so both go in registers.
    const uint64_t first = g_renderer.handle[0].load(std::memory_order_relaxed);
    const uint64_t second = g_renderer.handle[1].load(std::memory_order_relaxed);
    ot2_host_log("AUDREN render thread started handle=%#llx %#llx", (unsigned long long)first, (unsigned long long)second);
    const long period_ns = g_renderer.samples_per_frame > 0 && g_renderer.frames_per_second > 0
        ? 1000000000L / g_renderer.frames_per_second * g_renderer.samples_per_frame
        : 5000000L;
    const uint32_t frame_event = g_renderer.frame_event.load(std::memory_order_relaxed);
    uint64_t frames = 0;
    uint64_t last_exec_result = 0;
    // Pace to an absolute deadline, not "sleep the period after the work".
    // Sleeping after the work makes the period work-plus-period, so the renderer
    // produced 240 samples every ~5.3 ms instead of every 5 ms: measured from the
    // sink log, 10 s of audio took 10.6 s of wall clock, i.e. the renderer ran 6%
    // slow and the output dropped the difference. Each iteration carries exactly
    // samples_per_frame worth of material, so the period is the audio clock.
    timespec deadline {};
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    while (g_renderer.open.load(std::memory_order_acquire) == 1) {
        last_exec_result = call_original(kExecute, {first, second});
        // Stand in for the automatic-mode server: one signal per rendered frame.
        if (frame_event) ot2_signal_event(frame_event);
        ++frames;
        if (frames <= 3 || frames % 12000 == 0)
            ot2_host_log("AUDREN rendered %llu frames (period %ld ns) event=%#x",
                         (unsigned long long)frames, period_ns, frame_event);
        // Per-voice health line, every 20 s of render time. The three questions
        // the title's stream feeder asks are play-state, played-samples and
        // released-wave; if none of them ever moves the feeder cannot advance to
        // the next 128 KiB of the file, which is exactly what a frozen stream
        // looks like from the file side. "a" = appends, "x" = refused appends,
        // "p" = highest played-sample count, "s" = highest play state.
        if (frames % 4000 == 0) {
            char line[400];
            size_t used = 0;
            unsigned shown = 0;
            for (unsigned i = 0; i < VoiceSlots && shown < 5; i++) {
                const uint64_t handle = g_voices[i].handle.load(std::memory_order_relaxed);
                if (!handle) continue;
                const int wrote = snprintf(line + used, sizeof line - used,
                    " %#llx(a%u/x%u/p%llu/s%llu)", (unsigned long long)handle,
                    g_voices[i].appends.load(std::memory_order_relaxed),
                    g_voices[i].append_failures.load(std::memory_order_relaxed),
                    (unsigned long long)g_voices[i].played_max.load(std::memory_order_relaxed),
                    (unsigned long long)g_voices[i].state_max.load(std::memory_order_relaxed));
                if (wrote <= 0 || size_t(wrote) >= sizeof line - used) break;
                used += size_t(wrote);
                ++shown;
            }
            ot2_host_log("AUDREN voices frame=%llu%s | send=%llu dsp=%llu gen_bytes=%llu gen_max=%llu upd_voices=%llu silent=%llu peak=%llu exec_result=%#llx",
                         (unsigned long long)frames, line,
                         (unsigned long long)g_renderer.send_to_dsp.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.dsp_process.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.gen_bytes.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.gen_max.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.update_voices.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.mix_silent.load(std::memory_order_relaxed),
                         (unsigned long long)g_renderer.mix_peak.load(std::memory_order_relaxed),
                         (unsigned long long)last_exec_result);
        }
        // Advance the deadline by one period and sleep to it. If the renderer has
        // fallen more than one period behind (a stall, a long lock wait), restart
        // the cadence instead of rendering a burst to catch up: burst frames would
        // be queued straight into the output and play too fast.
        deadline.tv_nsec += period_ns;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }
        timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long late = (now.tv_sec - deadline.tv_sec) * 1000000000L + (now.tv_nsec - deadline.tv_nsec);
        if (late > period_ns) {
            deadline = now;
        } else {
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
        }
    }
    ot2_host_log("AUDREN render thread stopped after %llu frames", (unsigned long long)frames);
    g_renderer.render_run.store(0, std::memory_order_release);
}

uint64_t open_common(const char* name, uint64_t out, uint64_t event, uint64_t param, uint64_t work, uint64_t size, bool has_event) {
    Param original_param;
    original_param.fetch(param);
    static std::atomic<unsigned> logged{0};
    const unsigned n = logged.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) {
        log_param("open.param", original_param);
        ot2_host_log("AUDREN open request=%s work=%#llx size=%llu outputs=%d samples=%d mix=%d voices=%d sinks=%d effects=%d splitters=%d device=%d mode=%d",
                     name, (unsigned long long)work, (unsigned long long)size,
                     original_param.at(P_SAMPLE_RATE), original_param.at(P_SAMPLES_PER_FRAME),
                     original_param.at(P_MIX_BUFFERS), original_param.at(P_VOICES),
                     original_param.at(P_SINKS), original_param.at(P_EFFECTS),
                     original_param.at(P_SPLITTERS), original_param.at(P_DEVICE), original_param.at(P_MODE));
    }
    Param rewritten = original_param;
    const bool wanted = cpu_manual_mode() != 0;
    if (wanted) {
        rewritten.set(P_DEVICE, DeviceCpu);
        rewritten.set(P_MODE, ModeManual);
    }
    uint64_t result;
    if (has_event) {
        result = call_original(name, {out, event, uintptr_t(rewritten.bytes), work, size});
    } else {
        result = call_original(name, {out, uintptr_t(rewritten.bytes), work, size});
    }
    if (result && wanted) {
        ot2_host_log("[RED] audren: open with CPU/manual rejected (%#llx), falling back to the original parameter",
                     (unsigned long long)result);
        g_cpu_manual.store(0, std::memory_order_relaxed);
        result = has_event
            ? call_original(name, {out, event, param, work, size})
            : call_original(name, {out, param, work, size});
    }
    if (result == 0 && out) {
        ot2_host_log("AUDREN opened handle=%#llx %#llx mode=%s device=%d",
                     (unsigned long long)((uint64_t*)out)[0], (unsigned long long)((uint64_t*)out)[1],
                     wanted ? "CPU/manual" : "original", wanted ? DeviceCpu : original_param.at(P_DEVICE));
        // Only the event form is the renderer the title drives: the plain form is
        // reached from inside the SDK (it arrives with the rewritten parameter
        // already), so adopting its handle would make two opens compete for one
        // render thread. This mirrors the upstream line.
        if (has_event) {
            uint64_t words[4] {};
            memcpy(words, reinterpret_cast<const void*>(out), sizeof words);
            for (int i = 0; i < 4; i++) g_renderer.handle[i].store(words[i], std::memory_order_relaxed);
            // The title created this SystemEvent and waits on its readable side;
            // the writable handle is what the missing server would have used.
            uint32_t writable = 0;
            if (event) {
                const uint64_t args[] = {event, 0, 0, 0, 0, 0, 0, 0};
                writable = uint32_t(ot2_call_guest("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE", args));
            }
            g_renderer.frame_event.store(writable, std::memory_order_relaxed);
            ot2_host_log("AUDREN frame event=%#llx writable=%#x", (unsigned long long)event, writable);
            g_renderer.frames_per_second = original_param.at(P_SAMPLE_RATE);
            g_renderer.samples_per_frame = original_param.at(P_SAMPLES_PER_FRAME);
            if (wanted) {
                g_renderer.open.store(1, std::memory_order_release);
                if (!spawn_render_thread()) g_renderer.open.store(2, std::memory_order_release);
            }
        }
    } else if (result) {
        ot2_host_log("[RED] audren: open failed %#llx", (unsigned long long)result);
    }
    return result;
}
} // namespace

const std::vector<HostService>& audio_renderer_services() {
    static const std::vector<HostService> table = {
        // RequestAttachMemoryPool(MemoryPoolType* pool)
        //
        // Left to the SDK by default. The earlier version answered this by writing
        // Attached(5) into the state word, on the theory that "there is no server, so
        // nobody will". That was true while the renderer ran in automatic mode with a
        // real audren server that did not exist here; it became wrong once the open was
        // switched to CPU + manual, because then the server side of audren runs inside
        // this process too: AudioRenderSystem::Update calls InfoUpdater::UpdateMemoryPools,
        // which is `PoolMapper::Update(MemoryPoolInfo*, InParameter*, OutStatus*)` -- the
        // step that maps the pool and records its address in MemoryPoolInfo.
        //
        // Faking the state skips that step. Everything downstream still looks healthy --
        // voices are acquired, wave buffers are appended, 34 commands are generated and
        // CommandListProcessor::Process returns success every frame -- and the mix buffer
        // stays pure silence forever, because no data source can resolve its samples.
        // Measured with the stub in place: `AUDREN mix loud=0/1440 peak=0` on 6000
        // consecutive frames, and GetVoicePlayedSampleCount stuck at 0 for the whole run.
        // The reference host (nxnative dm/host/hle_audren.c) does not touch the memory
        // pool at all, which is the tell: the handshake is supposed to complete itself.
        //
        // The marker `audren-pool-stub` restores the old behaviour for an A/B run.
        {"_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", [](R r) -> uint64_t {
            const uint64_t pool = r[0];
            const uint64_t impl = plausible(pool) ? impl_of(pool) : 0;
            const uint32_t before = read_state(impl);
            static std::atomic<unsigned> seen{0};
            const unsigned n = seen.fetch_add(1, std::memory_order_relaxed);
            const bool stub = ot2_game_flag("audren-pool-stub");
            if (n < 16)
                ot2_host_log("AUDREN attach pool=%#llx impl=%#llx before=%u -> %s",
                             (unsigned long long)pool, (unsigned long long)impl, before,
                             stub ? "5 (host stub)" : "the SDK handshake");
            if (!stub)
                return call_original("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE", {r[0]});
            write_state(impl, PoolAttached);
            return 0;
        }},
        // RequestDetachMemoryPool(MemoryPoolType* pool): same reasoning, same marker.
        {"_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", [](R r) -> uint64_t {
            const uint64_t pool = r[0];
            const uint64_t impl = plausible(pool) ? impl_of(pool) : 0;
            static std::atomic<unsigned> seen{0};
            const unsigned n = seen.fetch_add(1, std::memory_order_relaxed);
            const bool stub = ot2_game_flag("audren-pool-stub");
            if (n < 16)
                ot2_host_log("AUDREN detach pool=%#llx impl=%#llx before=%u -> %s",
                             (unsigned long long)pool, (unsigned long long)impl, read_state(impl),
                             stub ? "3 (host stub)" : "the SDK handshake");
            if (!stub)
                return call_original("_ZN2nn5audio23RequestDetachMemoryPoolEPNS0_14MemoryPoolTypeE", {r[0]});
            write_state(impl, PoolDetached);
            return 0;
        }},
        // GetAudioRendererWorkBufferSize(const AudioRendererParameter&)
        // Both this and the open below see the same rewrite, so the buffer the
        // title allocates matches the renderer it opens.
        {kWorkSize, [](R r) -> uint64_t {
            Param param;
            param.fetch(r[0]);
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 3) log_param("worksize.param", param);
            if (cpu_manual_mode()) {
                param.set(P_DEVICE, DeviceCpu);
                param.set(P_MODE, ModeManual);
            }            const uint64_t size = call_original(kWorkSize, {uintptr_t(param.bytes)});
            static std::atomic<unsigned> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 3)
                ot2_host_log("AUDREN work buffer %llu", (unsigned long long)size);
            return size;
        }},
        {kOpenEvent, [](R r) -> uint64_t { return open_common(kOpenEvent, r[0], r[1], r[2], r[3], r[4], true); }},
        {kOpenPlain, [](R r) -> uint64_t { return open_common(kOpenPlain, r[0], 0, r[1], r[2], r[3], false); }},
        // ExecuteAudioRendererRendering(AudioRendererHandle)
        // Counted on the untouched path first: the log then says whether anything
        // in the title renders at all, which is the question the deadlock raised.
        {kExecute, [](R r) -> uint64_t {
            const uint64_t n = g_renderer.executes.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 || n % 6000 == 0)
                ot2_host_log("AUDREN execute #%llu handle=%#llx %#llx", (unsigned long long)n,
                             (unsigned long long)r[0], (unsigned long long)r[1]);
            return call_original(kExecute, {r[0], r[1]});
        }},
        // ---- Diagnosis: who advances the graph -------------------------------
        // The manual-mode pair is RequestUpdateAudioRenderer + Execute, and only
        // the advance half was ever called -- from here. These counters answer
        // whether the title drives it too, which decides whether the host has to
        // synthesise the update or step out of the title's way.
        {kAppendWave, [](R r) -> uint64_t {
            g_renderer.appends.fetch_add(1, std::memory_order_relaxed);
            // Per voice, not per session: the first blocks a voice ever gets are
            // exactly the ones that say whether the jingle reached the mixer.
            VoiceTrace* voice = voice_slot(r[0]);
            const uint32_t n = voice ? voice->appends.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
            if (n && n <= 3) {
                // nn::audio::WaveBuffer is {const void* buffer; size_t size;
                // int startSampleOffset; int endSampleOffset; bool loop; ...}
                // Only the first four words are read, and only when the pointer
                // is non-null: this runs on the title's sound thread.
                long long size = 0, start = 0, end = 0;
                if (r[1]) {
                    const auto* w = reinterpret_cast<const uint8_t*>(r[1]);
                    memcpy(&size, w + 8, 8);
                    memcpy(&start, w + 16, 4);
                    memcpy(&end, w + 20, 4);
                }
                ot2_host_log("AUDREN append-wave voice=%#llx #%u size=%lld samples=%lld..%lld",
                             (unsigned long long)r[0], n, size, start, end);
            }
            const uint64_t appended = call_original(kAppendWave, {r[0], r[1]});
            // A refused append is the whole answer when a stream stops advancing:
            // the title keeps the voice fed, the voice refuses the blocks because
            // the mixer never released the previous ones, and the file reader is
            // never asked for the next 128 KiB.
            if (appended && voice) {
                const uint32_t failed = voice->append_failures.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failed <= 3 || failed % 4096 == 0)
                    ot2_host_log("AUDREN append-wave REFUSED voice=%#llx result=%#llx (#%u)",
                                 (unsigned long long)r[0], (unsigned long long)appended, failed);
            }
            return appended;
        }},
        // The five observation probes share one shape: count, log the first few
        // and then rarely, forward, and log the answer for the getters. The
        // answer is the part that matters -- "asked 3000 times and always said
        // NotPlaying" is a completely different diagnosis from "never asked".
        {kGetPlayState, [](R r) -> uint64_t {
            g_renderer.play_state.fetch_add(1, std::memory_order_relaxed);
            const uint64_t result = call_original(kGetPlayState, {r[0]});
            // Announce the transition, not a sample of the steady state: "this
            // voice reported Playing once" is the fact that decides whether the
            // splash's wait can ever be satisfied.
            if (VoiceTrace* voice = voice_slot(r[0])) {
                const uint64_t before = voice->state_max.fetch_add(0, std::memory_order_relaxed);
                if (result > before) {
                    voice->state_max.store(result, std::memory_order_relaxed);
                    ot2_host_log("AUDREN play-state voice=%#llx -> %llu",
                                 (unsigned long long)r[0], (unsigned long long)result);
                }
            }
            return result;
        }},
        {kGetReleasedWave, [](R r) -> uint64_t {
            const uint64_t n = g_renderer.released_wave.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64_t result = call_original(kGetReleasedWave, {r[0]});
            if (n <= 4 || n % 3000 == 0)
                ot2_host_log("AUDREN get-released-wave #%llu voice=%#llx -> %#llx",
                             (unsigned long long)n, (unsigned long long)r[0],
                             (unsigned long long)result);
            return result;
        }},
        {kGetPlayedSamples, [](R r) -> uint64_t {
            g_renderer.played_samples.fetch_add(1, std::memory_order_relaxed);
            const uint64_t result = call_original(kGetPlayedSamples, {r[0]});
            // Same idea: log the first time a voice admits to having played any
            // sample at all, and then only at 1 s, 10 s, 1 min of material. A
            // voice that never gets past zero explains a streamed sound that is
            // never asked for its next block.
            if (VoiceTrace* voice = voice_slot(r[0])) {
                const uint64_t before = voice->played_max.load(std::memory_order_relaxed);
                if (result > before) {
                    static constexpr uint64_t steps[] = {1, 48000, 480000, 2880000};
                    bool announce = before == 0;
                    for (uint64_t step : steps)
                        if (before < step && result >= step) announce = true;
                    voice->played_max.store(result, std::memory_order_relaxed);
                    if (announce)
                        ot2_host_log("AUDREN played-samples voice=%#llx -> %llu",
                                     (unsigned long long)r[0], (unsigned long long)result);
                }
            }
            return result;
        }},
        {kGetWaveCount, [](R r) -> uint64_t {
            const uint64_t n = g_renderer.wave_count.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64_t result = call_original(kGetWaveCount, {r[0]});
            if (n <= 4 || n % 3000 == 0)
                ot2_host_log("AUDREN get-wave-count #%llu voice=%#llx -> %llu",
                             (unsigned long long)n, (unsigned long long)r[0],
                             (unsigned long long)result);
            return result;
        }},
        {kSetPlayState, [](R r) -> uint64_t {
            g_renderer.set_play_state.fetch_add(1, std::memory_order_relaxed);
            // Every distinct state a voice is put into, once. Previously this was
            // capped at eight calls for the whole session, and all eight happened
            // during boot -- so the result jingle's own Play could never appear,
            // which is precisely the event this investigation needs.
            if (VoiceTrace* voice = voice_slot(r[0])) {
                const uint64_t state = r[1];
                const uint64_t bit = state < 64 ? (uint64_t(1) << state) : 0;
                const uint64_t before = voice->states.fetch_or(bit, std::memory_order_relaxed);
                if (bit && !(before & bit))
                    ot2_host_log("AUDREN set-play-state voice=%#llx state=%llu",
                                 (unsigned long long)r[0], (unsigned long long)state);
            }
            return call_original(kSetPlayState, {r[0], r[1]});
        }},
        // Both are member functions: `this` arrives in x0 and has to be forwarded,
        // otherwise the original runs with a null object and faults at +0x8.
        {kSendToDsp, [](R r) -> uint64_t {
            g_renderer.send_to_dsp.fetch_add(1, std::memory_order_relaxed);
            return call_original(kSendToDsp, {r[0]});
        }},
        {kDspProcess, [](R r) -> uint64_t {
            g_renderer.dsp_process.fetch_add(1, std::memory_order_relaxed);
            const uint64_t result = call_original(kDspProcess, {r[0]});
            // The mix is finished at this point, so this is where it leaves the
            // graph for the speaker (see capture_sink).
            capture_sink();
            return result;
        }},
        {kUpdateVoices, [](R r) -> uint64_t {
            g_renderer.update_voices.fetch_add(1, std::memory_order_relaxed);
            return call_original(kUpdateVoices, {r[0], r[1], r[2], r[3]});
        }},
        {kGenCommand, [](R r) -> uint64_t {
            const uint64_t size = call_original(kGenCommand, {r[0], r[1], r[2]});
            g_renderer.cmd_list.store(r[1], std::memory_order_relaxed);
            g_renderer.cmd_len.store(size, std::memory_order_relaxed);
            g_renderer.gen_bytes.fetch_add(size, std::memory_order_relaxed);
            g_renderer.gen_max.store(std::max<uint64_t>(g_renderer.gen_max.load(std::memory_order_relaxed), size),
                                     std::memory_order_relaxed);
            // Walk the generated list: a constant size every frame means the graph
            // holds no per-frame voice work, and the histogram says whether the
            // DeviceSink and the voice commands are in there at all.
            const uint64_t n = g_renderer.gen_calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (r[1] && size >= 0x20 && (n <= 3 || n % 8000 == 0)) {
                const auto* base = reinterpret_cast<const uint8_t*>(r[1]);
                uint32_t ncmd = 0;
                memcpy(&ncmd, base + 4, 4);
                unsigned hist[64] = {0};
                const uint8_t* q = base + 0x20;
                const uint8_t* end = base + size;
                unsigned walked = 0;
                for (; walked < ncmd && q + 8 <= end; ++walked) {
                    int16_t csize = 0;
                    memcpy(&csize, q + 6, 2);
                    if (csize <= 0 || q + csize > end) break;
                    hist[q[5] & 63]++;
                    q += csize;
                }
                char line[320];
                size_t used = 0;
                for (unsigned i = 0; i < 64 && used + 12 < sizeof line; i++)
                    if (hist[i]) used += size_t(snprintf(line + used, sizeof line - used, " %u×%u", i, hist[i]));
                if (!used) snprintf(line, sizeof line, " (no commands walked)");
                ot2_host_log("AUDREN cmds #%llu bytes=%llu ncmd=%u walked=%u rate=%u spf=%u |%s",
                             (unsigned long long)n, (unsigned long long)size, ncmd, walked,
                             [&] { uint32_t v = 0; memcpy(&v, base + 0x18, 4); return v; }(),
                             [&] { uint32_t v = 0; memcpy(&v, base + 0x14, 4); return v; }(), line);
            }
            return size;
        }},
        {kInitConfig, [](R r) -> uint64_t {            if (r[0]) g_renderer.config.store(r[0], std::memory_order_relaxed);
            static std::atomic<unsigned> seen{0};
            if (seen.fetch_add(1, std::memory_order_relaxed) < 4)
                ot2_host_log("AUDREN init-config config=%#llx param=%#llx work=%#llx size=%llu",
                             (unsigned long long)r[0], (unsigned long long)r[1],
                             (unsigned long long)r[2], (unsigned long long)r[3]);
            return call_original(kInitConfig, {r[0], r[1], r[2], r[3]});
        }},
        {kRequestUpdate, [](R r) -> uint64_t {
            const uint64_t n = g_renderer.updates.fetch_add(1, std::memory_order_relaxed) + 1;
            // AudioRendererHandle is 16 bytes, so it occupies x0 and x1 and the
            // config pointer arrives in x2. Reading x1 as the config (the first
            // attempt) forwarded a null x2 to the original and faulted the title
            // inside the SDK at sdk+0x4c684, which is how the layout was settled.
            if (r[2]) g_renderer.config.store(r[2], std::memory_order_relaxed);
            if (n <= 4 || n % 6000 == 0)
                ot2_host_log("AUDREN request-update #%llu handle=%#llx %#llx config=%#llx",
                             (unsigned long long)n, (unsigned long long)r[0],
                             (unsigned long long)r[1], (unsigned long long)r[2]);
            return call_original(kRequestUpdate, {r[0], r[1], r[2]});
        }},
        {kStart, [](R r) -> uint64_t {
            g_renderer.starts.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<unsigned> seen{0};
            if (seen.fetch_add(1, std::memory_order_relaxed) < 4)
                ot2_host_log("AUDREN start handle=%#llx %#llx", (unsigned long long)r[0], (unsigned long long)r[1]);
            return call_original(kStart, {r[0], r[1]});
        }},
        {kState, [](R r) -> uint64_t {
            static std::atomic<unsigned> seen{0};
            const uint64_t value = call_original(kState, {r[0], r[1]});
            if (seen.fetch_add(1, std::memory_order_relaxed) < 4)
                ot2_host_log("AUDREN state handle=%#llx -> %llu", (unsigned long long)r[0], (unsigned long long)value);
            return value;
        }},
        {kElapsedFrames, [](R r) -> uint64_t {
            const uint64_t value = call_original(kElapsedFrames, {r[0], r[1]});
            static std::atomic<unsigned> seen{0};
            const unsigned n = seen.fetch_add(1, std::memory_order_relaxed);
            if (n < 4 || n % 6000 == 0)
                ot2_host_log("AUDREN elapsed-frames #%u config=%#llx -> %llu",
                             n + 1, (unsigned long long)r[0], (unsigned long long)value);
            return value;
        }},
        // CloseAudioRenderer(AudioRendererHandle)
        // The render thread may be inside the renderer the SDK is about to free,
        // so it is stopped and waited for before the close goes through.
        {kClose, [](R r) -> uint64_t {
            if (r[0] == g_renderer.handle[0].load(std::memory_order_relaxed) &&
                g_renderer.open.exchange(0, std::memory_order_acq_rel) == 1) {
                for (int i = 0; i < 400 && g_renderer.render_run.load(std::memory_order_acquire); i++) {
                    timespec pause {0, 1000000};
                    nanosleep(&pause, nullptr);
                }
                ot2_host_log("AUDREN close: render thread %s",
                             g_renderer.render_run.load() ? "did NOT stop" : "stopped");
                // The sink outlives the renderer otherwise: its stream, its two
                // threads and its recycled blocks would stay allocated for the rest
                // of the run, and a reopened renderer would queue into the old one.
                const uint64_t sink = g_sink.exchange(0, std::memory_order_acq_rel);
                if (sink) audren_sink_close(sink);
            }
            return call_original(kClose, {r[0], r[1]});
        }},
    };
    return table;
}

} // namespace ot2
