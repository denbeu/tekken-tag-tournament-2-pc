// Optional camera provenance capture for one bounded sequence of GPU frames,
// plus local independent 3D camera side control (LOCAL_CAMERA_SIDE = DEFAULT | OPPOSITE).
// TTT2_SIDE_CAMERA_DIAG=1, TTT2_SIDE_CAMERA_TRIGGER=<existing sentinel path>,
// TTT2_SIDE_CAMERA_OUT=<new output JSONL path>; output is CREATE_NEW (never overwrites).
// Create the trigger only after entering the desired offline battle scene.
#include "side_camera_diag.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>

#if defined(TTT2_GPU_HW_BACKEND)
#include "kernel_shared.h"
#include "sys_ui.h"
extern "C" PPC_FUNC(__imp__sub_823C8B30);
extern "C" PPC_FUNC(__imp__sub_82547540);
extern "C" PPC_FUNC(__imp__sub_82547620);
extern "C" PPC_FUNC(__imp__sub_82546B58);
extern "C" PPC_FUNC(__imp__sub_822CDA88);
extern "C" PPC_FUNC(__imp__sub_823038E0);
extern "C" PPC_FUNC(__imp__sub_82303BB0);
extern "C" PPC_FUNC(__imp__sub_8228DC40);
extern "C" PPC_FUNC(__imp__sub_8230DAB8);
extern "C" PPC_FUNC(__imp__sub_82398448);
extern "C" PPC_FUNC(__imp__sub_82295910);
extern "C" PPC_FUNC(__imp__sub_82528558);
extern "C" PPC_FUNC(__imp__sub_824FF570);
extern "C" PPC_FUNC(__imp__sub_82542C98);
extern "C" PPC_FUNC(__imp__sub_825424B8);
extern "C" PPC_FUNC(__imp__sub_825425C8);
extern "C" PPC_FUNC(__imp__sub_823D4058);
extern "C" PPC_FUNC(__imp__sub_823D4B08);
extern "C" PPC_FUNC(__imp__sub_823D4698);
extern "C" PPC_FUNC(__imp__sub_823D55A8);
extern "C" PPC_FUNC(__imp__sub_823D56D0);
extern "C" PPC_FUNC(__imp__sub_823D6738);
extern "C" PPC_FUNC(__imp__sub_823D6A88);
extern "C" PPC_FUNC(__imp__sub_823D5F28);
extern "C" PPC_FUNC(__imp__sub_823D5BF0);
extern "C" PPC_FUNC(__imp__sub_823D5860);
extern "C" PPC_FUNC(__imp__sub_823D5A10);
extern "C" PPC_FUNC(__imp__sub_82653478);
extern "C" PPC_FUNC(__imp__sub_82632148);
extern "C" PPC_FUNC(__imp__sub_82664CE0);
extern "C" PPC_FUNC(__imp__sub_826EFA48);
extern "C" PPC_FUNC(__imp__sub_826EFA90);
extern "C" PPC_FUNC(__imp__sub_82644588);
extern "C" PPC_FUNC(__imp__sub_82641308);
extern "C" PPC_FUNC(__imp__sub_8263D038);
extern "C" PPC_FUNC(__imp__sub_8263D698);
extern "C" PPC_FUNC(__imp__sub_8256FE28);
extern "C" PPC_FUNC(__imp__sub_8263CF88);
extern "C" PPC_FUNC(__imp__sub_8263CFD8);
extern "C" PPC_FUNC(__imp__sub_8263D008);
extern "C" PPC_FUNC(__imp__sub_82644C98);
extern "C" PPC_FUNC(__imp__sub_8228DA18);
extern "C" PPC_FUNC(__imp__sub_82627040);
extern "C" PPC_FUNC(__imp__sub_82B62BE8);
extern "C" PPC_FUNC(__imp__sub_82B62B30);
bool XSessionAnyActive();   // xsession.cpp
namespace xlive { uint32_t GuestAlloc(uint32_t bytes); }
#endif

namespace ttt2_side_camera_diag {
namespace {
enum class Phase { Uninitialized, Armed, Capturing, Done };
struct Capture {
    Phase phase = Phase::Uninitialized;
    HANDLE file = INVALID_HANDLE_VALUE;
    char trigger[1024] = {};
    char path[1024] = {};
    unsigned frame = 0, maxFrames = 4, draws = 0, writes = 0, producers = 0, inputDiags = 0;
    unsigned omittedDraws = 0, omittedWrites = 0, omittedProducers = 0, omittedInputDiags = 0;
    unsigned long long bytes = 0;
    bool truncated = false;
};
// Owned exclusively by the GPU CP thread (OnSwap, OnConstantWrite, OnDraw, OnSkippedDraw).
// PowerPC guest threads NEVER access `g` directly; they check `g_captureActive` and push
// fixed-size CameraFrameDiagRecord / LocalInputDiagRecord structs into `g_diagQueueMutex`.
Capture g;

constexpr unsigned kMaxFrames = 8;
constexpr unsigned kMaxDrawsPerFrame = 128;
constexpr unsigned kMaxWritesPerFrame = 256;
constexpr unsigned kMaxProducersPerFrame = 8;
constexpr unsigned kMaxInputDiagsPerFrame = 8;
constexpr unsigned long long kMaxBytes = 1024 * 1024;
constexpr unsigned long long kFooterReserve = 1024;

std::atomic<bool> g_captureActive{false};
std::mutex g_diagQueueMutex;
std::array<CameraFrameDiagRecord, kMaxProducersPerFrame> g_diagQueue{};
unsigned g_diagQueueCount = 0;
unsigned g_diagQueueOmitted = 0;

std::array<LocalInputDiagRecord, kMaxInputDiagsPerFrame> g_inputDiagQueue{};
unsigned g_inputDiagQueueCount = 0;
unsigned g_inputDiagQueueOmitted = 0;

std::atomic<uint32_t> g_localCameraSideState{0xFFFFFFFFu};
std::atomic<uint32_t> g_localInputModeState{0xFFFFFFFFu};
std::atomic<uint32_t> g_localCameraUserIndexState{0xFFFFFFFFu};
std::atomic<uint32_t> g_onlineInputOverrideState{0xFFFFFFFFu};
std::atomic<uint32_t> g_allowedSlotMaskState{0xFFFFFFFFu};
std::atomic<uint32_t> g_localInputStrategyState{0xFFFFFFFFu};
std::atomic<uint32_t> g_renderCameraSitesState{0xFFFFFFFFu};
std::atomic<uint32_t> g_hudSideSwapState{0xFFFFFFFFu};

// Phase 9 side preference. g_sidePolicyFast mirrors it for the per-frame getters:
// bit0 active, bit1 decided OPPOSITE, bits 8..9 selector XAM user.
struct SidePolicyState {
    bool active = false;
    PlayerScreenSide wanted = PlayerScreenSide::Left;
    uint32_t selectorUser = 0;
    bool decided = false;
    LocalCameraSide cameraSide = LocalCameraSide::Default;
    uint32_t fighterAddr = 0;
    uint32_t originalSide = 0;
    uint32_t decisions = 0;
    uint32_t teamHint = 0xFFFFFFFFu;   // PRACTICE with BATTLE POSITION: the player's own team
    bool online = false;               // chosen in the Player Match room (Phase 11)
};
constexpr uint32_t kPolicyActiveBit = 1u << 0;
constexpr uint32_t kPolicyOnlineBit = 1u << 2;
constexpr uint32_t kPolicyOppositeBit = 1u << 1;
constexpr uint32_t kPolicyWantRightBit = 1u << 3;
std::mutex g_sidePolicyMutex;
SidePolicyState g_sidePolicy;
std::atomic<uint32_t> g_sidePolicyFast{0};
// Phase 12: the online decision in effect (its decisions count; 0 = none) and the last one whose
// camera / HUD / input application was logged.
std::atomic<uint32_t> g_onlineDecision{0};
std::atomic<uint32_t> g_loggedCameraDecision{0}, g_loggedHudDecision{0}, g_loggedInputDecision{0};
// Logs `what` once per online decision (true the first time for the decision in effect).
bool FirstForOnlineDecision(std::atomic<uint32_t>& logged) {
    const uint32_t d = g_onlineDecision.load(std::memory_order_acquire);
    return d != 0u && logged.exchange(d, std::memory_order_acq_rel) != d;
}
std::atomic<uint64_t> g_sidePolicyLastTickMs{0};

std::atomic<bool> g_oppositeCameraAppliedLatch{false};
std::atomic<uint64_t> g_inputFrameEpoch{1};
std::atomic<uint64_t> g_lastOppositeCameraEpoch{0};
std::atomic<bool> g_battleUnpausedActive{false};
std::atomic<uint64_t> g_lastBattleTickEpoch{0};
std::atomic<bool> g_battleOverlayInhibited{false};
std::atomic<uint64_t> g_lastFighterInputPollEpoch{0};

struct PerUserPadTracking {
    bool initialized = false;
    bool lastTransformApplied = false;
    uint32_t lastRawPacketNumber = 0;
    uint16_t lastEffectiveButtons = 0;
    int16_t lastEffectiveStickX = 0;
    uint32_t syntheticPacketOffset = 0;
    uint32_t lastEffectivePacketNumber = 0;
    bool hasDiagBaseline = false;
    LocalInputDiagRecord lastDiag{};
};
std::mutex g_padTrackMutex;
std::array<PerUserPadTracking, 4> g_padTrack{};

constexpr uint32_t kNoPadOwner = 0xFFFFFFFFu;
constexpr uint64_t kSideLatchMaxStaleFrames = 2;

// Per-fighter result of the last controller poll (4 tag fighters plus spares).
struct FighterSideLatch {
    uint32_t fighterAddr = 0;
    uint32_t ownerUser = kNoPadOwner;
    uint32_t cameraSide = 0;   // what sub_823038E0 latched from +1024
    uint32_t inputSide = 0;    // what input consumers read while active
    uint64_t epoch = 0;
    bool active = false;
    bool diagReported = false;
};
std::mutex g_sideLatchMutex;
std::array<FighterSideLatch, 8> g_sideLatch{};
std::atomic<uint32_t> g_sideRequirementFlips{0};

struct FighterPollTls {
    bool inPoll = false;
    bool lookupSeen = false;
    uint32_t fighterAddr = 0;
    uint32_t ownerUser = kNoPadOwner;
};
thread_local FighterPollTls t_fighterPoll;

constexpr unsigned kMaxSideLatchDiagsPerFrame = 8;
std::array<SideLatchDiagRecord, kMaxSideLatchDiagsPerFrame> g_sideLatchDiagQueue{};
unsigned g_sideLatchDiagQueueCount = 0;
unsigned g_sideLatchDiagQueueOmitted = 0;

std::atomic<uint32_t> g_stageCullCalls{0};
std::atomic<uint32_t> g_stageCullSubstituted{0};
std::atomic<uint32_t> g_stageCullShownOnlyByRendered{0};
std::atomic<uint32_t> g_stageCullHiddenOnlyByRendered{0};
std::atomic<uint32_t> g_paramEyeMirrored{0};
// Per TTT2_RENDER_CAMERA_SITES bit: how many times the opposite camera was handed out.
std::array<std::atomic<uint32_t>, 8> g_stageSideSubstitutions{};

bool EqualsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = char(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = char(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

uint32_t ReadGuestU32(const uint8_t* base, uint32_t ga) {
    if (!base || !IsValidGuestStructRange(ga, sizeof(uint32_t))) return 0;
    uint32_t raw = 0;
    std::memcpy(&raw, base + ga, sizeof(raw));
    return __builtin_bswap32(raw);
}

void WriteGuestU32(uint8_t* base, uint32_t ga, uint32_t v) {
    if (!base || !IsValidGuestStructRange(ga, sizeof(uint32_t))) return;
    const uint32_t raw = __builtin_bswap32(v);
    std::memcpy(base + ga, &raw, sizeof(raw));
}

float ReadGuestF32(const uint8_t* base, uint32_t ga) {
    const uint32_t bits = ReadGuestU32(base, ga);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteGuestF32(uint8_t* base, uint32_t ga, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    WriteGuestU32(base, ga, bits);
}

bool EnvPath(const char* key, char (&dst)[1024]) {
    DWORD n = GetEnvironmentVariableA(key, dst, DWORD(sizeof(dst)));
    return n && n < sizeof(dst);
}

void Emit(const char* fmt, ...) {
    if (g.file == INVALID_HANDLE_VALUE) return;
    char line[4096];
    va_list va; va_start(va, fmt);
    int n = std::vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) ||
        g.bytes + static_cast<unsigned>(n) + kFooterReserve > kMaxBytes) {
        g.truncated = true;
        return;
    }
    DWORD wrote = 0;
    if (!WriteFile(g.file, line, DWORD(n), &wrote, nullptr) || wrote != DWORD(n)) {
        g.truncated = true;
        g_captureActive.store(false, std::memory_order_release);
        CloseHandle(g.file);
        g.file = INVALID_HANDLE_VALUE;
        g.phase = Phase::Done;
        return;
    }
    g.bytes += wrote;
}

int FormatFloatArray(char* dst, size_t cap, const float* vals, size_t count) {
    size_t used = 0;
    int n = std::snprintf(dst, cap, "[");
    if (n < 0 || static_cast<size_t>(n) >= cap) return -1;
    used += static_cast<size_t>(n);
    for (size_t i = 0; i < count; ++i) {
        n = std::snprintf(dst + used, cap - used, "%s%.6f", (i == 0 ? "" : ","), double(vals[i]));
        if (n < 0 || static_cast<size_t>(n) >= cap - used) return -1;
        used += static_cast<size_t>(n);
    }
    n = std::snprintf(dst + used, cap - used, "]");
    if (n < 0 || static_cast<size_t>(n) >= cap - used) return -1;
    used += static_cast<size_t>(n);
    return static_cast<int>(used);
}

void EmitCameraFrameDiagOnGpuThread(unsigned frameNumber, const CameraFrameDiagRecord& rec) {
    char eyeOrigBuf[128] = {};
    char eyeRendBuf[128] = {};
    char targetBuf[128] = {};
    char viewOrigBuf[512] = {};
    char viewRendBuf[512] = {};
    char vpOrigBuf[512] = {};
    char vpRendBuf[512] = {};
    if (FormatFloatArray(eyeOrigBuf, sizeof(eyeOrigBuf), rec.cameraEyeOriginal, 4) < 0 ||
        FormatFloatArray(eyeRendBuf, sizeof(eyeRendBuf), rec.cameraEyeRendered, 4) < 0 ||
        FormatFloatArray(targetBuf, sizeof(targetBuf), rec.cameraTarget, 3) < 0 ||
        FormatFloatArray(viewOrigBuf, sizeof(viewOrigBuf), rec.viewMatrixOriginal, 16) < 0 ||
        FormatFloatArray(viewRendBuf, sizeof(viewRendBuf), rec.viewMatrixRendered, 16) < 0 ||
        FormatFloatArray(vpOrigBuf, sizeof(vpOrigBuf), rec.viewProjOriginal, 16) < 0 ||
        FormatFloatArray(vpRendBuf, sizeof(vpRendBuf), rec.viewProjRendered, 16) < 0) {
        g.truncated = true;
        return;
    }
    const char* sideStr = (rec.cameraSide == LocalCameraSide::Opposite) ? "OPPOSITE" : "DEFAULT";
    Emit("{\"type\":\"camera_frame_diag\",\"timestamp\":%llu,\"frame\":%u,\"camera_side\":\"%s\","
         "\"camMgr\":\"%08X\",\"activeSlot\":%u,\"camInst\":\"%08X\",\"camVtable\":\"%08X\","
         "\"slotAllowed\":%s,\"transformApplied\":%s,\"camInstUnchanged\":%s,"
         "\"cameraEyeOriginal\":%s,\"cameraEyeRendered\":%s,\"cameraTarget\":%s,"
         "\"viewMatrixOriginal\":%s,\"viewMatrixRendered\":%s,"
         "\"viewProjOriginal\":%s,\"viewProjRendered\":%s}\n",
         static_cast<unsigned long long>(rec.timestampMs), frameNumber, sideStr,
         rec.camMgrAddr, rec.activeSlot, rec.camInstAddr, rec.camVtable,
         rec.slotAllowed ? "true" : "false",
         rec.transformApplied ? "true" : "false",
         rec.camInstUnchanged ? "true" : "false",
         eyeOrigBuf, eyeRendBuf, targetBuf,
         viewOrigBuf, viewRendBuf,
         vpOrigBuf, vpRendBuf);
}

void EmitLocalInputDiagOnGpuThread(unsigned frameNumber, const LocalInputDiagRecord& rec) {
    Emit("{\"type\":\"local_input_diag\",\"timestamp\":%llu,\"frame\":%u,\"packet\":%u,"
         "\"camera_side\":\"%s\",\"input_mode\":\"%s\",\"local_user\":%u,"
         "\"raw_buttons\":\"0x%04X\",\"effective_buttons\":\"0x%04X\","
         "\"raw_stick_x\":%d,\"effective_stick_x\":%d,"
         "\"in_battle\":%s,\"transform_applied\":%s}\n",
         static_cast<unsigned long long>(rec.timestampMs),
         frameNumber,
         rec.packetNumber,
         LocalCameraSideName(rec.cameraSide),
         LocalInputModeName(rec.inputMode),
         rec.localUser,
         unsigned(rec.rawButtons),
         unsigned(rec.effectiveButtons),
         int(rec.rawStickX),
         int(rec.effectiveStickX),
         rec.inBattle ? "true" : "false",
         rec.transformApplied ? "true" : "false");
}

void EmitSideLatchDiagOnGpuThread(unsigned frameNumber, const SideLatchDiagRecord& rec) {
    Emit("{\"type\":\"side_latch_diag\",\"timestamp\":%llu,\"frame\":%u,\"fighter\":\"%08X\","
         "\"owner_user\":%d,\"camera_side\":%u,\"input_side\":%u,\"applied\":%s,\"playback\":%s,"
         "\"strategy\":\"%s\"}\n",
         static_cast<unsigned long long>(rec.timestampMs), frameNumber, rec.fighterAddr,
         rec.ownerUser == kNoPadOwner ? -1 : int(rec.ownerUser),
         rec.cameraSide, rec.inputSide,
         rec.applied ? "true" : "false",
         rec.playback ? "true" : "false",
         LocalInputStrategyName(GetLocalInputStrategy()));
}

void DrainAndEmitCameraFrameDiags(unsigned frameNumber) {
    std::array<CameraFrameDiagRecord, kMaxProducersPerFrame> localRecords{};
    unsigned localCount = 0;
    unsigned localOmitted = 0;
    std::array<LocalInputDiagRecord, kMaxInputDiagsPerFrame> localInputRecords{};
    unsigned localInputCount = 0;
    unsigned localInputOmitted = 0;
    std::array<SideLatchDiagRecord, kMaxSideLatchDiagsPerFrame> localSideRecords{};
    unsigned localSideCount = 0;
    unsigned localSideOmitted = 0;
    {
        std::lock_guard<std::mutex> lock(g_diagQueueMutex);
        localCount = g_diagQueueCount;
        localOmitted = g_diagQueueOmitted;
        for (unsigned i = 0; i < localCount; ++i) {
            localRecords[i] = g_diagQueue[i];
        }
        g_diagQueueCount = 0;
        g_diagQueueOmitted = 0;

        localInputCount = g_inputDiagQueueCount;
        localInputOmitted = g_inputDiagQueueOmitted;
        for (unsigned i = 0; i < localInputCount; ++i) {
            localInputRecords[i] = g_inputDiagQueue[i];
        }
        g_inputDiagQueueCount = 0;
        g_inputDiagQueueOmitted = 0;

        localSideCount = g_sideLatchDiagQueueCount;
        localSideOmitted = g_sideLatchDiagQueueOmitted;
        for (unsigned i = 0; i < localSideCount; ++i) {
            localSideRecords[i] = g_sideLatchDiagQueue[i];
        }
        g_sideLatchDiagQueueCount = 0;
        g_sideLatchDiagQueueOmitted = 0;
    }
    for (unsigned i = 0; i < localSideCount; ++i) {
        EmitSideLatchDiagOnGpuThread(frameNumber, localSideRecords[i]);
    }
    if (localSideOmitted > 0) {
        g.truncated = true;
    }
    // One bounded summary line per frame for the Phase 8 consumers.
    Emit("{\"type\":\"phase8_frame_diag\",\"frame\":%u,\"side_requirement_flips\":%u,"
         "\"stage_cull_calls\":%u,\"stage_cull_view_substituted\":%u,"
         "\"stage_meshes_shown_only_by_rendered_view\":%u,\"stage_meshes_hidden_only_by_rendered_view\":%u,"
         "\"param_eye_mirrored\":%u,\"render_sites\":\"0x%X\","
         "\"fade_block_mirrored\":%u,\"mob_camera_shadowed\":%u,\"field_grid_shadowed\":%u,"
         "\"lens_flare_shadowed\":%u,\"light_cull_shadowed\":%u}\n",
         frameNumber,
         g_sideRequirementFlips.exchange(0, std::memory_order_acq_rel),
         g_stageCullCalls.exchange(0, std::memory_order_acq_rel),
         g_stageCullSubstituted.exchange(0, std::memory_order_acq_rel),
         g_stageCullShownOnlyByRendered.exchange(0, std::memory_order_acq_rel),
         g_stageCullHiddenOnlyByRendered.exchange(0, std::memory_order_acq_rel),
         g_paramEyeMirrored.exchange(0, std::memory_order_acq_rel),
         GetRenderCameraSites(),
         g_stageSideSubstitutions[3].exchange(0, std::memory_order_acq_rel),
         g_stageSideSubstitutions[4].exchange(0, std::memory_order_acq_rel),
         g_stageSideSubstitutions[5].exchange(0, std::memory_order_acq_rel),
         g_stageSideSubstitutions[6].exchange(0, std::memory_order_acq_rel),
         g_stageSideSubstitutions[7].exchange(0, std::memory_order_acq_rel));
    for (unsigned i = 0; i < localCount; ++i) {
        if (g.producers < kMaxProducersPerFrame) {
            ++g.producers;
            EmitCameraFrameDiagOnGpuThread(frameNumber, localRecords[i]);
        } else {
            ++g.omittedProducers;
            g.truncated = true;
        }
    }
    if (localOmitted > 0) {
        g.omittedProducers += localOmitted;
        g.truncated = true;
    }
    for (unsigned i = 0; i < localInputCount; ++i) {
        if (g.inputDiags < kMaxInputDiagsPerFrame) {
            ++g.inputDiags;
            EmitLocalInputDiagOnGpuThread(frameNumber, localInputRecords[i]);
        } else {
            ++g.omittedInputDiags;
            g.truncated = true;
        }
    }
    if (localInputOmitted > 0) {
        g.omittedInputDiags += localInputOmitted;
        g.truncated = true;
    }
}

void End() {
    g_captureActive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_diagQueueMutex);
        g_diagQueueCount = 0;
        g_diagQueueOmitted = 0;
        g_inputDiagQueueCount = 0;
        g_inputDiagQueueOmitted = 0;
        g_sideLatchDiagQueueCount = 0;
        g_sideLatchDiagQueueOmitted = 0;
    }
    Emit("{\"type\":\"end\",\"frames\":%u,\"omitted_draws\":%u,\"omitted_writes\":%u,\"truncated\":%s}\n",
         g.frame, g.omittedDraws, g.omittedWrites, g.truncated ? "true" : "false");
    if (g.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g.file);
        CloseHandle(g.file);
        g.file = INVALID_HANDLE_VALUE;
    }
    g.phase = Phase::Done;
    std::fprintf(stderr, "[side-camera] capture complete: %s (frames=%u, bytes=%llu, truncated=%d)\n",
                 g.path, g.frame, g.bytes, int(g.truncated));
}
} // namespace

bool IsValidGuestStructRange(uint32_t guestAddr, uint32_t byteSize) {
    if (guestAddr < kMinValidGuestAddr || guestAddr > kMaxValidGuestAddr) return false;
    if (byteSize > (kMaxValidGuestAddr - guestAddr)) return false;
    if ((guestAddr & 0x3u) != 0u) return false;
    return true;
}

uint64_t HashGuestBytesFNV1a(const uint8_t* base, uint32_t guestAddr, uint32_t byteSize) {
    if (!base || !IsValidGuestStructRange(guestAddr, byteSize)) return 0;
    uint64_t h = 14695981039346656037ull;
    const uint8_t* ptr = base + guestAddr;
    for (uint32_t i = 0; i < byteSize; ++i) {
        h ^= uint64_t(ptr[i]);
        h *= 1099511628211ull;
    }
    return h;
}

LocalCameraSide ParseLocalCameraSide(const char* value) {
    if (!value) return LocalCameraSide::Default;
    while (*value == ' ' || *value == '\t') ++value;
    if (EqualsIgnoreCase(value, "OPPOSITE") ||
        EqualsIgnoreCase(value, "1") ||
        EqualsIgnoreCase(value, "RIGHT") ||
        EqualsIgnoreCase(value, "2P_LEFT")) {
        return LocalCameraSide::Opposite;
    }
    return LocalCameraSide::Default;
}

const char* LocalCameraSideName(LocalCameraSide side) {
    return (side == LocalCameraSide::Opposite) ? "OPPOSITE" : "DEFAULT";
}

LocalCameraSide ReloadLocalCameraSideFromEnv() {
    char buf[64] = {};
    LocalCameraSide side = LocalCameraSide::Default;
    DWORD n = GetEnvironmentVariableA("LOCAL_CAMERA_SIDE", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        side = ParseLocalCameraSide(buf);
    } else {
        n = GetEnvironmentVariableA("TTT2_LOCAL_CAMERA_SIDE", buf, DWORD(sizeof(buf)));
        if (n > 0 && n < sizeof(buf)) {
            side = ParseLocalCameraSide(buf);
        }
    }
    g_localCameraSideState.store(static_cast<uint32_t>(side), std::memory_order_relaxed);
    return side;
}

LocalCameraSide GetLocalCameraSide() {
    const uint32_t policy = g_sidePolicyFast.load(std::memory_order_acquire);
    if (policy & kPolicyActiveBit) {
        return (policy & kPolicyOppositeBit) ? LocalCameraSide::Opposite : LocalCameraSide::Default;
    }
    const uint32_t raw = g_localCameraSideState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadLocalCameraSideFromEnv();
    }
    return raw == static_cast<uint32_t>(LocalCameraSide::Opposite)
        ? LocalCameraSide::Opposite
        : LocalCameraSide::Default;
}

void SetLocalCameraSide(LocalCameraSide side) {
    g_localCameraSideState.store(static_cast<uint32_t>(side), std::memory_order_relaxed);
    if (side != LocalCameraSide::Opposite) {
        g_oppositeCameraAppliedLatch.store(false, std::memory_order_release);
    }
}

LocalInputMode ParseLocalInputMode(const char* value) {
    if (!value) return LocalInputMode::Original;
    while (*value == ' ' || *value == '\t') ++value;
    if (EqualsIgnoreCase(value, "CAMERA_RELATIVE") ||
        EqualsIgnoreCase(value, "CAMERA") ||
        EqualsIgnoreCase(value, "RELATIVE") ||
        EqualsIgnoreCase(value, "1")) {
        return LocalInputMode::CameraRelative;
    }
    return LocalInputMode::Original;
}

const char* LocalInputModeName(LocalInputMode mode) {
    return (mode == LocalInputMode::CameraRelative) ? "CAMERA_RELATIVE" : "ORIGINAL";
}

LocalInputMode ReloadLocalInputModeFromEnv() {
    char buf[64] = {};
    LocalInputMode mode = LocalInputMode::Original;
    DWORD n = GetEnvironmentVariableA("LOCAL_INPUT_MODE", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        mode = ParseLocalInputMode(buf);
    } else {
        n = GetEnvironmentVariableA("TTT2_LOCAL_INPUT_MODE", buf, DWORD(sizeof(buf)));
        if (n > 0 && n < sizeof(buf)) {
            mode = ParseLocalInputMode(buf);
        }
    }
    g_localInputModeState.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
    return mode;
}

LocalInputMode GetLocalInputMode() {
    const uint32_t policy = g_sidePolicyFast.load(std::memory_order_acquire);
    if (policy & kPolicyActiveBit) {
        return (policy & kPolicyOppositeBit) ? LocalInputMode::CameraRelative : LocalInputMode::Original;
    }
    const uint32_t raw = g_localInputModeState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadLocalInputModeFromEnv();
    }
    return raw == static_cast<uint32_t>(LocalInputMode::CameraRelative)
        ? LocalInputMode::CameraRelative
        : LocalInputMode::Original;
}

void SetLocalInputMode(LocalInputMode mode) {
    g_localInputModeState.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
}

LocalInputStrategy ParseLocalInputStrategy(const char* value) {
    if (!value) return LocalInputStrategy::SideLatch;
    while (*value == ' ' || *value == '\t') ++value;
    if (EqualsIgnoreCase(value, "XINPUT_LR") ||
        EqualsIgnoreCase(value, "XINPUT") ||
        EqualsIgnoreCase(value, "PHASE6") ||
        EqualsIgnoreCase(value, "1")) {
        return LocalInputStrategy::XInputMirror;
    }
    return LocalInputStrategy::SideLatch;
}

const char* LocalInputStrategyName(LocalInputStrategy strategy) {
    return (strategy == LocalInputStrategy::XInputMirror) ? "XINPUT_LR" : "SIDE_LATCH";
}

LocalInputStrategy ReloadLocalInputStrategyFromEnv() {
    char buf[64] = {};
    LocalInputStrategy strategy = LocalInputStrategy::SideLatch;
    const DWORD n = GetEnvironmentVariableA("TTT2_CAMERA_INPUT_STRATEGY", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        strategy = ParseLocalInputStrategy(buf);
    }
    g_localInputStrategyState.store(static_cast<uint32_t>(strategy), std::memory_order_relaxed);
    return strategy;
}

LocalInputStrategy GetLocalInputStrategy() {
    const uint32_t raw = g_localInputStrategyState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadLocalInputStrategyFromEnv();
    }
    return raw == static_cast<uint32_t>(LocalInputStrategy::XInputMirror)
        ? LocalInputStrategy::XInputMirror
        : LocalInputStrategy::SideLatch;
}

void SetLocalInputStrategy(LocalInputStrategy strategy) {
    g_localInputStrategyState.store(static_cast<uint32_t>(strategy), std::memory_order_relaxed);
}

uint32_t ReloadRenderCameraSitesFromEnv() {
    char buf[64] = {};
    uint32_t mask = kDefaultRenderCameraSites;
    const DWORD n = GetEnvironmentVariableA("TTT2_RENDER_CAMERA_SITES", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(buf, &end, 0);
        if (end != buf && *end == '\0') {
            mask = static_cast<uint32_t>(parsed) & kRenderCameraSitesMask;
        }
    }
    g_renderCameraSitesState.store(mask, std::memory_order_relaxed);
    return mask;
}

uint32_t GetRenderCameraSites() {
    const uint32_t raw = g_renderCameraSitesState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadRenderCameraSitesFromEnv();
    }
    return raw & kRenderCameraSitesMask;
}

void SetRenderCameraSites(uint32_t mask) {
    g_renderCameraSitesState.store(mask & kRenderCameraSitesMask, std::memory_order_relaxed);
}

uint32_t ReloadLocalCameraUserIndexFromEnv() {
    char buf[64] = {};
    uint32_t userIdx = 0u;
    DWORD n = GetEnvironmentVariableA("LOCAL_CAMERA_USER", buf, DWORD(sizeof(buf)));
    if (n == 0 || n >= sizeof(buf)) {
        n = GetEnvironmentVariableA("TTT2_LOCAL_CAMERA_USER", buf, DWORD(sizeof(buf)));
    }
    if (n > 0 && n < sizeof(buf)) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(buf, &end, 0);
        if (end != buf && *end == '\0' && parsed <= 3ul) {
            userIdx = static_cast<uint32_t>(parsed);
        }
    }
    g_localCameraUserIndexState.store(userIdx, std::memory_order_relaxed);
    return userIdx;
}

uint32_t GetLocalCameraUserIndex() {
    const uint32_t policy = g_sidePolicyFast.load(std::memory_order_acquire);
    if (policy & kPolicyActiveBit) {
        return (policy >> 8) & 3u;
    }
    const uint32_t raw = g_localCameraUserIndexState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadLocalCameraUserIndexFromEnv();
    }
    return (raw <= 3u) ? raw : 0u;
}

void SetLocalCameraUserIndex(uint32_t userIndex) {
    g_localCameraUserIndexState.store((userIndex <= 3u) ? userIndex : 0u, std::memory_order_relaxed);
}

bool ReloadOnlineInputOverrideAllowedFromEnv() {
    char buf[32] = {};
    bool allowed = false;
    const DWORD n = GetEnvironmentVariableA("TTT2_ALLOW_ONLINE_CAMERA_INPUT", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        allowed = EqualsIgnoreCase(buf, "1") || EqualsIgnoreCase(buf, "TRUE");
    }
    g_onlineInputOverrideState.store(allowed ? 1u : 0u, std::memory_order_relaxed);
    return allowed;
}

bool IsOnlineInputOverrideAllowed() {
    const uint32_t raw = g_onlineInputOverrideState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadOnlineInputOverrideAllowedFromEnv();
    }
    return raw != 0u;
}

void SetOnlineInputOverrideAllowed(bool allowed) {
    g_onlineInputOverrideState.store(allowed ? 1u : 0u, std::memory_order_relaxed);
}

LocalCameraSide DecideCameraSideForPreference(PlayerScreenSide wanted, uint32_t team) {
    // Phase 10: by team. The Phase 9 "+1024 0 = right" calibration came from decisions latched on
    // menu / cinematic-camera polls (the logs decide before "mode 18 requested" and flip between
    // battles for the same player), so it is not used any more.
    const PlayerScreenSide native = (team == 0u) ? PlayerScreenSide::Left : PlayerScreenSide::Right;
    return native == wanted ? LocalCameraSide::Default : LocalCameraSide::Opposite;
}

uint32_t BattleTeamOfFighter(const uint8_t* base, uint32_t fighterAddr) {
    if (!base || fighterAddr == 0u || !IsValidGuestStructRange(kBattleFighterArrayAddr, 16u)) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t entry = ReadGuestU32(base, kBattleFighterArrayAddr + i * 4u);
        if (entry != 0u && entry == fighterAddr) return i & 1u;
    }
    for (uint32_t t = 0; t < 2u; ++t) {
        const uint32_t entry = ReadGuestU32(base, kBattleFighterArrayAddr + t * 4u);
        if (entry != 0u && IsValidGuestStructRange(entry + kFighterPartnerOffset, 4u) &&
            ReadGuestU32(base, entry + kFighterPartnerOffset) == fighterAddr) {
            return t;
        }
    }
    return 0xFFFFFFFFu;
}

uint32_t CurrentGameMode(const uint8_t* base) {
    if (!base || !IsValidGuestStructRange(kModeManagerImplAddr, 4u)) return 0xFFFFFFFFu;
    const uint32_t impl = ReadGuestU32(base, kModeManagerImplAddr);
    if (!IsValidGuestStructRange(impl, kModeManagerCurrentModeOffset + 4u)) return 0xFFFFFFFFu;
    return ReadGuestU32(base, impl + kModeManagerCurrentModeOffset);
}

static void PublishSidePolicyLocked() {
    uint32_t packed = 0;
    if (g_sidePolicy.active) {
        packed = kPolicyActiveBit | ((g_sidePolicy.selectorUser & 3u) << 8);
        if (g_sidePolicy.decided && g_sidePolicy.cameraSide == LocalCameraSide::Opposite) {
            packed |= kPolicyOppositeBit;
        }
        if (g_sidePolicy.online) packed |= kPolicyOnlineBit;
        if (g_sidePolicy.wanted == PlayerScreenSide::Right) packed |= kPolicyWantRightBit;
    }
    g_onlineDecision.store(g_sidePolicy.active && g_sidePolicy.online && g_sidePolicy.decided ? g_sidePolicy.decisions : 0u,
                           std::memory_order_release);
    const uint32_t before = g_sidePolicyFast.exchange(packed, std::memory_order_acq_rel);
    if ((before & kPolicyOppositeBit) && !(packed & kPolicyOppositeBit)) {
        g_oppositeCameraAppliedLatch.store(false, std::memory_order_release);
    }
}

void SetPlayerSidePreference(PlayerScreenSide wanted, uint32_t selectorUser, uint32_t teamHint, bool online) {
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    const uint32_t decisions = g_sidePolicy.decisions;
    g_sidePolicy = SidePolicyState{};
    g_sidePolicy.active = true;
    g_sidePolicy.online = online;
    g_sidePolicy.wanted = wanted;
    g_sidePolicy.selectorUser = selectorUser & 3u;
    g_sidePolicy.teamHint = teamHint <= 1u ? teamHint : 0xFFFFFFFFu;
    g_sidePolicy.decisions = decisions;
    PublishSidePolicyLocked();
}

void ClearPlayerSidePreference() {
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    const uint32_t decisions = g_sidePolicy.decisions;
    g_sidePolicy = SidePolicyState{};
    g_sidePolicy.decisions = decisions;
    PublishSidePolicyLocked();
}

SidePolicySnapshot GetSidePolicySnapshot() {
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    SidePolicySnapshot s{};
    s.active = g_sidePolicy.active;
    s.wanted = g_sidePolicy.wanted;
    s.selectorUser = g_sidePolicy.selectorUser;
    s.decided = g_sidePolicy.decided;
    s.cameraSide = g_sidePolicy.cameraSide;
    s.fighterAddr = g_sidePolicy.fighterAddr;
    s.originalSide = g_sidePolicy.originalSide;
    s.decisions = g_sidePolicy.decisions;
    s.online = g_sidePolicy.online;
    return s;
}

uint32_t OnlineLocalBattleSlot(const uint8_t* base) {
    if (!base || !IsValidGuestStructRange(kOnlineManagerAddr + kOnlineLocalSlotOffset, 12u)) return 0xFFFFFFFFu;
    const uint32_t layout = ReadGuestU32(base, kOnlineManagerAddr + kOnlineLocalSlotOffset);
    const uint32_t side = ReadGuestU32(base, kOnlineManagerAddr + kOnlineLocalSideOffset);
    const uint32_t flags = ReadGuestU32(base, kOnlineManagerAddr + kOnlineLocalSlotOffset + 8u);
    // sub_822745B0: online only when +7488 is not -1 and sub_8266C748 (top bit clear); the local
    // player's side is +7492 (Phase 11-D; 0 / 1, anything else: none).
    if (layout == 0xFFFFFFFFu || (flags & 0x80000000u) != 0u) return 0xFFFFFFFFu;
    return side <= 1u ? side : 0xFFFFFFFFu;
}

static std::atomic<uint64_t> g_onlineLocalXuid{0};
void SetOnlineLocalXuid(uint64_t xuid) { g_onlineLocalXuid.store(xuid, std::memory_order_release); }
uint64_t OnlineLocalXuid() { return g_onlineLocalXuid.load(std::memory_order_acquire); }

OnlineOwner IdentifyOnlineOwner(const uint8_t* base, uint64_t localXuid) {
    OnlineOwner o;
    if (!base) {
        o.reason = "no guest memory";
        return o;
    }
    // The name table, for the log only (MEASURED empty in an online battle).
    if (IsValidGuestStructRange(kMatchPlayersAddr + kMatchSideIndexOffset, 8u)) {
        for (uint32_t s = 0; s < 2u; ++s) {
            o.index[s] = ReadGuestU32(base, kMatchPlayersAddr + kMatchSideIndexOffset + 4u * s);
            const uint32_t identity = kMatchPlayersAddr + kMatchPlayerIdentityOffset + kMatchPlayerStride * o.index[s];
            if (o.index[s] <= kMatchPlayerMaxIndex && IsValidGuestStructRange(identity, 64u)) {
                o.xuid[s] = (uint64_t(ReadGuestU32(base, identity)) << 32) | ReadGuestU32(base, identity + 4u);
                o.type[s] = uint32_t(base[identity + 60u]) << 8 | base[identity + 61u];
            }
        }
    }
    (void)localXuid;
    // The owner: the sync engine's local node (node 0) and its battle slot (+744).
    if (!IsValidGuestStructRange(kSyncAddr + kSyncNodesOffset, kSyncNodeCountOffset - kSyncNodesOffset + 4u)) {
        o.reason = "sync engine not readable";
        return o;
    }
    o.nodes = ReadGuestU32(base, kSyncAddr + kSyncNodeCountOffset);
    if (o.nodes < 2u || o.nodes > kSyncMaxNodes) {
        o.reason = "no online sync session (fewer than 2 nodes)";
        return o;
    }
    for (uint32_t i = 0; i < 2u; ++i) {
        o.node[i] = ReadGuestU32(base, kSyncAddr + kSyncNodesOffset + 4u * i);
        if (!IsValidGuestStructRange(o.node[i], kSyncNodeSlotOffset + 4u)) {
            o.reason = "sync node not readable";
            return o;
        }
        o.slot[i] = int32_t(ReadGuestU32(base, o.node[i] + kSyncNodeSlotOffset));
    }
    if (o.slot[0] != 0 && o.slot[0] != 1) {
        o.reason = "local node has no battle slot (0 / 1)";
        return o;
    }
    for (uint32_t i = 1; i < o.nodes; ++i) {
        const uint32_t node = ReadGuestU32(base, kSyncAddr + kSyncNodesOffset + 4u * i);
        if (node == o.node[0]) {
            o.reason = "local node listed twice";
            return o;
        }
        if (IsValidGuestStructRange(node, kSyncNodeSlotOffset + 4u) &&
            int32_t(ReadGuestU32(base, node + kSyncNodeSlotOffset)) == o.slot[0]) {
            o.reason = "another node has the local node's slot";
            return o;
        }
    }
    o.team = o.slot[0];
    o.reason = "matched";
    return o;
}

// Owner lookups logged when the answer changes.
static void NoteOnlineOwner(const OnlineOwner& o, uint64_t localXuid, uint32_t mode) {
    static std::mutex m;
    static uint64_t lastKey = ~0ull;
    static int logged = 0;
    const uint64_t key = (uint64_t(uint32_t(o.team + 1)) << 56) ^ (uint64_t(o.nodes) << 48) ^ (uint64_t(uint32_t(o.slot[0] + 9)) << 40) ^
                         (uint64_t(uint32_t(o.slot[1] + 9)) << 32) ^ o.node[0] ^ (uint64_t(o.node[1]) << 8) ^ localXuid;
    std::lock_guard<std::mutex> lock(m);
    if (key == lastKey || logged >= 24) return;
    lastKey = key;
    ++logged;
    std::fprintf(stderr, "[side-owner] local XUID %016llX, mode %u; sync engine %u nodes\n", (unsigned long long)localXuid, mode, o.nodes);
    std::fprintf(stderr, "[side-owner] node 0 (this machine) %08X: battle slot %d; node 1 %08X: battle slot %d\n", o.node[0],
                 int(o.slot[0]), o.node[1], int(o.slot[1]));
    std::fprintf(stderr, "[side-owner] (name table, not used: side 0 entry %d XUID %016llX type %u; side 1 entry %d XUID %016llX type %u)\n",
                 int(o.index[0]), (unsigned long long)o.xuid[0], o.type[0], int(o.index[1]), (unsigned long long)o.xuid[1], o.type[1]);
    if (o.team >= 0)
        std::fprintf(stderr, "[side-owner] local member matched: team %d (%s)\n", o.team, o.team == 0 ? "1P, native LEFT" : "2P, native RIGHT");
    else
        std::fprintf(stderr, "[side-owner] UNKNOWN: %s\n", o.reason);
}

bool OnlineBattleInputLive(const uint8_t* base) {
    if (!base || !IsValidGuestStructRange(kOnlineManagerAddr + kOnlineLocalSlotOffset, 12u) ||
        !IsValidGuestStructRange(kSyncAddr + kSyncNodeCountOffset, 4u)) {
        return false;
    }
    const uint32_t layout = ReadGuestU32(base, kOnlineManagerAddr + kOnlineLocalSlotOffset);
    const uint32_t flags = ReadGuestU32(base, kOnlineManagerAddr + kOnlineLocalSlotOffset + 8u);
    const uint32_t nodes = ReadGuestU32(base, kSyncAddr + kSyncNodeCountOffset);
    return layout != 0xFFFFFFFFu && (flags & 0x80000000u) == 0u && nodes >= 2u && nodes <= kSyncMaxNodes;
}

static std::atomic<uint32_t> g_loggedSendDecision{0};
// Teams whose first online sidestep flip was logged since their slot's flag last changed.
static std::atomic<uint32_t> g_loggedSidestepFlipTeams{0};
// The online session state of the last fighter poll (XSessionAnyActive), so offline requirement
// evaluations never look at the online manager.
static std::atomic<bool> g_pollOnlineSession{false};

uint32_t OnlineInputToSend(const uint8_t* base, uint32_t buttons) {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    constexpr uint32_t kWanted = kPolicyActiveBit | kPolicyOnlineBit | kPolicyOppositeBit;
    if ((fast & kWanted) != kWanted || !IsBattleGameMode(CurrentGameMode(base)) || !OnlineBattleInputLive(base)) {
        return buttons;
    }
    if (FirstForOnlineDecision(g_loggedSendDecision))
        std::fprintf(stderr, "[side-input] W/S: this machine sends the OPPOSITE-view flag in its battle input (online decision %u)\n",
                     g_onlineDecision.load());
    return buttons | kOnlineOppositeViewFlag;
}

uint32_t OnlineInputAsRead(uint32_t slot, uint32_t value, bool previous) {
    static std::atomic<uint32_t> seen[2] = {0u, 0u};   // 1 + whether the slot carried the flag
    static std::atomic<int> logged{0};
    const bool flagged = (value & kOnlineOppositeViewFlag) != 0u;
    if (!previous && slot <= 1u) {
        const uint32_t now = flagged ? 2u : 1u;
        const uint32_t before = seen[slot].exchange(now, std::memory_order_acq_rel);
        if (before != now) {
            g_loggedSidestepFlipTeams.fetch_and(~(1u << slot), std::memory_order_acq_rel);
            if ((flagged || before == 2u) && logged.fetch_add(1, std::memory_order_relaxed) < 32)
                std::fprintf(stderr, "[side-input] W/S: battle slot %u %s the OPPOSITE-view flag (both clients read the same frame)\n", slot,
                             flagged ? "carries" : "no longer carries");
        }
    }
    return value & ~kOnlineOppositeViewFlag;
}

bool OnlineTeamSidestepFlipped(const uint8_t* base, uint32_t team) {
    if (team > 1u || !OnlineBattleInputLive(base) || !IsValidGuestStructRange(kNaceAddr + kSyncCurrentInputOffset, 4u)) return false;
    const uint32_t addr = kNaceAddr + kSyncCurrentInputOffset + 2u * team;
    const uint32_t word = (uint32_t(base[addr]) << 8) | base[addr + 1u];
    return (word & kOnlineOppositeViewFlag) != 0u;
}

bool CharaSelectOnlineDiagActive(const uint8_t* base) {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    return (fast & kPolicyActiveBit) && (fast & kPolicyOnlineBit) && CurrentGameMode(base) == kGameModeCharaSelect;
}

static std::atomic<bool> g_charaSelectSwapEnabled{true};
static std::atomic<int> g_charaSelectSwapEnvState{-1};
void SetCharaSelectSideSwapEnabled(bool enabled) {
    g_charaSelectSwapEnabled.store(enabled, std::memory_order_relaxed);
    g_charaSelectSwapEnvState.store(1, std::memory_order_relaxed);
}

static bool CharaSelectSwapEnabled() {
    if (g_charaSelectSwapEnvState.load(std::memory_order_relaxed) < 0) {
        char buf[8] = {};
        const DWORD len = GetEnvironmentVariableA("TTT2_CHARASELECT_SIDE_SWAP", buf, DWORD(sizeof(buf)));
        g_charaSelectSwapEnabled.store(!(len == 1u && buf[0] == '0'), std::memory_order_relaxed);
        g_charaSelectSwapEnvState.store(1, std::memory_order_relaxed);
    }
    return g_charaSelectSwapEnabled.load(std::memory_order_relaxed);
}

bool OnlineNamePlateSwapWanted(const uint8_t* base, uint32_t* outMode, int* outTeam) {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    const uint32_t mode = CurrentGameMode(base);
    int team = -1;
    bool swap = false;
    if ((fast & kPolicyActiveBit) && (fast & kPolicyOnlineBit) && CharaSelectSwapEnabled() && !IsBattleGameMode(mode)) {
        team = IdentifyOnlineOwner(base, OnlineLocalXuid()).team;
        swap = team >= 0 && (team == 1) != ((fast & kPolicyWantRightBit) != 0u);
    }
    if (outMode) *outMode = mode;
    if (outTeam) *outTeam = team;
    return swap;
}

bool CharaSelectSideSwapActive(const uint8_t* base) {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    if (!(fast & kPolicyActiveBit) || !(fast & kPolicyOnlineBit)) return false;
    if (!CharaSelectSwapEnabled()) return false;
    // Once per presented frame (the lookups come by the hundred).
    static std::atomic<uint64_t> cachedEpoch{0};
    static std::atomic<bool> cached{false};
    const uint64_t epoch = g_inputFrameEpoch.load(std::memory_order_acquire);
    if (cachedEpoch.load(std::memory_order_acquire) == epoch) return cached.load(std::memory_order_acquire);
    bool swap = false;
    int team = -1;
    if (CurrentGameMode(base) == kGameModeCharaSelect) {
        team = IdentifyOnlineOwner(base, OnlineLocalXuid()).team;
        const bool wantRight = (fast & kPolicyWantRightBit) != 0u;
        swap = team >= 0 && (team == 1) != wantRight;
    }
    cached.store(swap, std::memory_order_release);
    cachedEpoch.store(epoch, std::memory_order_release);
    static std::atomic<int> last{-1};
    const int now = swap ? 1 : 0;
    const int prev = last.exchange(now);
    if (prev != now && (swap || prev >= 0))
        std::fprintf(stderr, "[side-charsel] character select %s (local team %d, chosen %s)\n",
                     swap ? "drawn swapped: the two sides' select states traded in its UI" : "drawn as the game lays it out", team,
                     (fast & kPolicyWantRightBit) ? "RIGHT" : "LEFT");
    return swap;
}

bool ObserveOnlineTeamForSidePolicy(uint32_t localSlot, uint32_t fighterAddr) {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    if (!(fast & kPolicyActiveBit) || !(fast & kPolicyOnlineBit)) return false;
    if (localSlot > 1u) return false;                  // 2: watching from the room, nothing to turn
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    if (!g_sidePolicy.active || !g_sidePolicy.online || g_sidePolicy.decided) return false;
    g_sidePolicy.decided = true;
    g_sidePolicy.fighterAddr = fighterAddr;
    g_sidePolicy.originalSide = localSlot;
    g_sidePolicy.cameraSide = DecideCameraSideForPreference(g_sidePolicy.wanted, localSlot);
    ++g_sidePolicy.decisions;
    g_sidePolicyLastTickMs.store(GetTickCount64(), std::memory_order_release);
    PublishSidePolicyLocked();
    std::fprintf(stderr, "[side-owner] owner verified: local team %u (fighter %08X polled as team %u)\n", localSlot, fighterAddr,
                 localSlot);
    std::fprintf(stderr, "[side-select] requested %s / native %s -> camera %s (local view only)\n",
                 g_sidePolicy.wanted == PlayerScreenSide::Left ? "LEFT" : "RIGHT", localSlot == 0u ? "LEFT" : "RIGHT",
                 LocalCameraSideName(g_sidePolicy.cameraSide));
    return true;
}

bool ReadPlayerMatchRoomCountdown(const uint8_t* base, uint32_t roomObj, PlayerMatchRoomCountdown& out) {
    out = PlayerMatchRoomCountdown{};
    if (!base || !IsValidGuestStructRange(roomObj, 232u)) return false;
    const uint32_t a = ReadGuestU32(base, roomObj + 52u);
    if (!IsValidGuestStructRange(a, 332u)) return false;
    const uint32_t p = ReadGuestU32(base, a + 328u);
    if (!IsValidGuestStructRange(p, 668u + 40u)) return false;
    out.state = ReadGuestU32(base, p + 668u + 32u);
    out.counter = ReadGuestU32(base, p + 668u + 36u);
    for (uint32_t i = 0; i < 6u; ++i) out.players += base[p + 24u + 108u * i] != 0u ? 1u : 0u;
    out.nativeSide = ReadGuestU32(base, roomObj + 228u);
    return true;
}

bool IsOnlineSidePolicyActive() {
    const uint32_t fast = g_sidePolicyFast.load(std::memory_order_acquire);
    return (fast & kPolicyActiveBit) && (fast & kPolicyOnlineBit);
}

bool ObserveFighterForSidePolicy(uint32_t fighterAddr, uint32_t ownerUser, uint32_t originalSide,
                                 bool onlineSessionActive) {
    // Offline only (Phase 10 decides what may differ between clients online).
    if (onlineSessionActive) return false;
    if (!(g_sidePolicyFast.load(std::memory_order_acquire) & kPolicyActiveBit)) return false;
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    if (!g_sidePolicy.active || g_sidePolicy.decided || g_sidePolicy.online) return false;
    // Only a fighter whose pad lookup resolved to the user who confirmed the side.
    if (fighterAddr == 0u || ownerUser != g_sidePolicy.selectorUser || originalSide > 1u) return false;
    // The practice dummy may share the player's pad: with a known own team, only that team counts.
    if (g_sidePolicy.teamHint <= 1u && originalSide != g_sidePolicy.teamHint) return false;
    g_sidePolicy.decided = true;
    g_sidePolicy.fighterAddr = fighterAddr;
    g_sidePolicy.originalSide = originalSide;
    g_sidePolicy.cameraSide = DecideCameraSideForPreference(g_sidePolicy.wanted, originalSide);
    ++g_sidePolicy.decisions;
    g_sidePolicyLastTickMs.store(GetTickCount64(), std::memory_order_release);
    PublishSidePolicyLocked();
    std::fprintf(stderr, "[side-select] battle: fighter %08X of XAM user %u is team %u (native %s); wanted %s -> camera %s\n",
                 fighterAddr, ownerUser, originalSide, originalSide == 0u ? "LEFT" : "RIGHT",
                 g_sidePolicy.wanted == PlayerScreenSide::Left ? "LEFT" : "RIGHT",
                 LocalCameraSideName(g_sidePolicy.cameraSide));
    return true;
}

void NoteBattleTickForSidePolicy(uint64_t nowMs) {
    g_sidePolicyLastTickMs.store(nowMs, std::memory_order_release);
}

void MaintainSidePolicy(uint64_t nowMs) {
    if (!(g_sidePolicyFast.load(std::memory_order_acquire) & kPolicyActiveBit)) return;
    std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
    if (!g_sidePolicy.decided) return;
    const uint64_t last = g_sidePolicyLastTickMs.load(std::memory_order_acquire);
    if (nowMs >= last && nowMs - last > kSidePolicyBattleIdleMs) {
        // Battle over (result, menus, loading): undecided again, the next battle decides anew.
        g_sidePolicy.decided = false;
        g_sidePolicy.cameraSide = LocalCameraSide::Default;
        g_sidePolicy.fighterAddr = 0;
        PublishSidePolicyLocked();
        std::fprintf(stderr, "[side-select] battle left: camera and input back to DEFAULT/ORIGINAL\n");
    }
}

bool ReloadHudSideSwapEnabledFromEnv() {
    char buf[16] = {};
    bool enabled = true;
    const DWORD n = GetEnvironmentVariableA("TTT2_HUD_SIDE_SWAP", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) enabled = !EqualsIgnoreCase(buf, "0");
    g_hudSideSwapState.store(enabled ? 1u : 0u, std::memory_order_relaxed);
    return enabled;
}

void SetHudSideSwapEnabled(bool enabled) {
    g_hudSideSwapState.store(enabled ? 1u : 0u, std::memory_order_relaxed);
}

bool ShouldSwapHudTeams(bool onlineSessionActive) {
    const uint32_t raw = g_hudSideSwapState.load(std::memory_order_relaxed);
    const bool enabled = raw == 0xFFFFFFFFu ? ReloadHudSideSwapEnabledFromEnv() : raw == 1u;
    // Online only for a side chosen in the Player Match room (display data only, main thread).
    const bool swap = enabled && (!onlineSessionActive || IsOnlineSidePolicyActive()) &&
                      GetLocalCameraSide() == LocalCameraSide::Opposite;
    if (swap && onlineSessionActive && FirstForOnlineDecision(g_loggedHudDecision))
        std::fprintf(stderr, "[side-hud] local HUD mapping applied (teams swapped on this screen, online decision %u)\n",
                     g_onlineDecision.load());
    return swap;
}

bool SwapHudFighterSlots(uint8_t* base, uint32_t arrayAddr) {
    if (!base || !IsValidGuestStructRange(arrayAddr, 16u)) return false;
    const uint32_t v[4] = {ReadGuestU32(base, arrayAddr), ReadGuestU32(base, arrayAddr + 4u),
                           ReadGuestU32(base, arrayAddr + 8u), ReadGuestU32(base, arrayAddr + 12u)};
    if (v[0] == 0u || v[1] == 0u || v[0] == v[1]) return false;
    WriteGuestU32(base, arrayAddr, v[1]);
    WriteGuestU32(base, arrayAddr + 4u, v[0]);
    WriteGuestU32(base, arrayAddr + 8u, v[3]);
    WriteGuestU32(base, arrayAddr + 12u, v[2]);
    return true;
}

uint32_t HudTeamForDisplay(uint32_t team, bool swap) {
    return (swap && team <= 1u) ? (team ^ 1u) : team;
}

uint32_t ReloadAllowedCameraSlotMaskFromEnv() {
    char buf[64] = {};
    uint32_t mask = kDefaultAllowedCameraSlotMask;
    const DWORD n = GetEnvironmentVariableA("TTT2_CAMERA_SLOT_MASK", buf, DWORD(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(buf, &end, 0);
        if (end != buf && *end == '\0') {
            mask = static_cast<uint32_t>(parsed) & 0x0FFFu;
        }
    }
    g_allowedSlotMaskState.store(mask, std::memory_order_relaxed);
    return mask;
}

uint32_t GetAllowedCameraSlotMask() {
    const uint32_t raw = g_allowedSlotMaskState.load(std::memory_order_relaxed);
    if (raw == 0xFFFFFFFFu) {
        return ReloadAllowedCameraSlotMaskFromEnv();
    }
    return raw & 0x0FFFu;
}

void SetAllowedCameraSlotMask(uint32_t mask) {
    g_allowedSlotMaskState.store(mask & 0x0FFFu, std::memory_order_relaxed);
}

bool ApplyOppositeCameraMatrix4x4(const float inMatrix[16], const float pivot[3], float outMatrix[16]) {
    if (!inMatrix || !pivot || !outMatrix) return false;
    const float px = pivot[0];
    const float py = pivot[1];
    const float pz = pivot[2];
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) {
        if (inMatrix != outMatrix) {
            std::memcpy(outMatrix, inMatrix, 16 * sizeof(float));
        }
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(inMatrix[i])) {
            if (inMatrix != outMatrix) {
                std::memcpy(outMatrix, inMatrix, 16 * sizeof(float));
            }
            return false;
        }
    }
    for (int j = 0; j < 4; ++j) {
        const float r0 = inMatrix[0 * 4 + j];
        const float r1 = inMatrix[1 * 4 + j];
        const float r2 = inMatrix[2 * 4 + j];
        const float r3 = inMatrix[3 * 4 + j];
        outMatrix[0 * 4 + j] = -r0;
        outMatrix[1 * 4 + j] =  r1;
        outMatrix[2 * 4 + j] = -r2;
        outMatrix[3 * 4 + j] =  r3 + 2.0f * px * r0 + 2.0f * pz * r2;
    }
    return true;
}

bool ApplyOppositeCameraEye(const float inEye[4], const float pivot[3], float outEye[4]) {
    if (!inEye || !pivot || !outEye) return false;
    const float px = pivot[0];
    const float py = pivot[1];
    const float pz = pivot[2];
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) {
        if (inEye != outEye) {
            std::memcpy(outEye, inEye, 4 * sizeof(float));
        }
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(inEye[i])) {
            if (inEye != outEye) {
                std::memcpy(outEye, inEye, 4 * sizeof(float));
            }
            return false;
        }
    }
    outEye[0] = 2.0f * px - inEye[0];
    outEye[1] = inEye[1];
    outEye[2] = 2.0f * pz - inEye[2];
    outEye[3] = inEye[3];
    return true;
}

bool ReadCameraProducerSnapshot(const uint8_t* base, uint32_t camMgrAddr, CameraProducerSnapshot& out) {
    out = CameraProducerSnapshot{};
    if (!base || !IsValidGuestStructRange(camMgrAddr, 400u)) return false;
    const uint32_t activeSlot = ReadGuestU32(base, camMgrAddr + 316u);
    if (activeSlot >= 12u) return false;
    const uint32_t camInstAddr = ReadGuestU32(base, camMgrAddr + (activeSlot + 85u) * 4u);
    if (!IsValidGuestStructRange(camInstAddr, 384u)) return false;

    out.camMgrAddr = camMgrAddr;
    out.activeSlot = activeSlot;
    out.camInstAddr = camInstAddr;
    out.camVtable = ReadGuestU32(base, camInstAddr + 0u);
    out.viewProjAddr = camInstAddr + 96u;
    out.projAddr = camInstAddr + 160u;
    out.viewAddr = camInstAddr + 224u;
    for (uint32_t i = 0; i < 3u; ++i) {
        out.eye[i] = ReadGuestF32(base, camInstAddr + 16u + i * 4u);
        out.target[i] = ReadGuestF32(base, camInstAddr + 32u + i * 4u);
        out.up[i] = ReadGuestF32(base, camInstAddr + 48u + i * 4u);
    }
    out.zNear = ReadGuestF32(base, camInstAddr + 64u);
    out.zFar = ReadGuestF32(base, camInstAddr + 68u);
    out.fovY = ReadGuestF32(base, camInstAddr + 72u);
    out.appliedSide = GetLocalCameraSide();
    out.slotAllowed = ((GetAllowedCameraSlotMask() >> activeSlot) & 1u) != 0u;

    bool finiteAll = std::isfinite(out.zNear) && std::isfinite(out.zFar) && std::isfinite(out.fovY);
    for (int i = 0; i < 3; ++i) {
        finiteAll = finiteAll && std::isfinite(out.eye[i]) &&
                    std::isfinite(out.target[i]) && std::isfinite(out.up[i]);
    }
    for (uint32_t i = 0; i < 16u && finiteAll; ++i) {
        finiteAll = finiteAll &&
                    std::isfinite(ReadGuestF32(base, out.viewProjAddr + i * 4u)) &&
                    std::isfinite(ReadGuestF32(base, out.viewAddr + i * 4u));
    }
    const float dx = out.eye[0] - out.target[0];
    const float dz = out.eye[2] - out.target[2];
    const float horizDist = std::hypot(dx, dz);
    out.geometryValid = finiteAll && (horizDist > 1.0f);
    return true;
}

bool ShouldApplyOppositeToCamera(const CameraProducerSnapshot& snap) {
    return snap.appliedSide == LocalCameraSide::Opposite &&
           snap.slotAllowed &&
           snap.geometryValid &&
           IsValidGuestStructRange(snap.camInstAddr, 384u);
}

bool TransformGuestCameraMatrix4x4(uint8_t* base, uint32_t srcMatrixAddr, uint32_t pivotAddr, uint32_t dstMatrixAddr) {
    if (!base ||
        !IsValidGuestStructRange(srcMatrixAddr, 64u) ||
        !IsValidGuestStructRange(pivotAddr, 12u) ||
        !IsValidGuestStructRange(dstMatrixAddr, 64u)) {
        return false;
    }
    float inMat[16] = {};
    float outMat[16] = {};
    float pivot[3] = {
        ReadGuestF32(base, pivotAddr + 0u),
        ReadGuestF32(base, pivotAddr + 4u),
        ReadGuestF32(base, pivotAddr + 8u),
    };
    for (uint32_t i = 0; i < 16u; ++i) {
        inMat[i] = ReadGuestF32(base, srcMatrixAddr + i * 4u);
    }
    if (!ApplyOppositeCameraMatrix4x4(inMat, pivot, outMat)) return false;
    for (uint32_t i = 0; i < 16u; ++i) {
        WriteGuestF32(base, dstMatrixAddr + i * 4u, outMat[i]);
    }
    return true;
}

bool TransformGuestCameraEye4(uint8_t* base, uint32_t pivotAddr, uint32_t inOutEyeAddr) {
    if (!base ||
        !IsValidGuestStructRange(pivotAddr, 12u) ||
        !IsValidGuestStructRange(inOutEyeAddr, 16u)) {
        return false;
    }
    float inEye[4] = {
        ReadGuestF32(base, inOutEyeAddr + 0u),
        ReadGuestF32(base, inOutEyeAddr + 4u),
        ReadGuestF32(base, inOutEyeAddr + 8u),
        ReadGuestF32(base, inOutEyeAddr + 12u),
    };
    float pivot[3] = {
        ReadGuestF32(base, pivotAddr + 0u),
        ReadGuestF32(base, pivotAddr + 4u),
        ReadGuestF32(base, pivotAddr + 8u),
    };
    float outEye[4] = {};
    if (!ApplyOppositeCameraEye(inEye, pivot, outEye)) return false;
    for (uint32_t i = 0; i < 4u; ++i) {
        WriteGuestF32(base, inOutEyeAddr + i * 4u, outEye[i]);
    }
    return true;
}

static bool CameraRelativeGateOpen(const LocalInputGateContext& gate) {
    if (gate.inputMode != LocalInputMode::CameraRelative) return false;
    if (gate.cameraSide != LocalCameraSide::Opposite) return false;
    if (!gate.oppositeCameraActiveInBattle) return false;
    if (!gate.inActiveUnpausedBattle) return false;
    if (gate.battleOverlayInhibited) return false;
    if (gate.sysUiOrMenuActive) return false;
    if (gate.onlineSessionActive && !gate.allowOnlineInputOverride) return false;
    if (gate.targetUserIndex > 3u || gate.targetUserIndex != gate.localCameraUserIndex) return false;
    return true;
}

bool ShouldApplyCameraRelativeInputGate(const LocalInputGateContext& gate) {
    return gate.rawPadMirrorEnabled && CameraRelativeGateOpen(gate);
}

bool ShouldApplySideLatchGate(const LocalInputGateContext& gate, bool playbackActive) {
    // Playback restores the recorded +1028 itself; converting again would double-convert.
    // The latch changes local simulation state that the peer never sees, so no online override.
    return gate.sideLatchEnabled && !playbackActive && !gate.onlineSessionActive &&
           CameraRelativeGateOpen(gate);
}

bool ApplyCameraRelativePadTransform(
    uint16_t rawButtons,
    int16_t rawThumbLX,
    bool shouldTransform,
    uint16_t& outEffectiveButtons,
    int16_t& outEffectiveThumbLX) {
    if (!shouldTransform) {
        outEffectiveButtons = rawButtons;
        outEffectiveThumbLX = rawThumbLX;
        return false;
    }

    const bool leftHeld  = (rawButtons & kXInputDpadLeft) != 0u;
    const bool rightHeld = (rawButtons & kXInputDpadRight) != 0u;
    if (leftHeld != rightHeld) {
        outEffectiveButtons = static_cast<uint16_t>(rawButtons ^ kXInputDpadLRMask);
    } else {
        outEffectiveButtons = rawButtons;
    }

    if (rawThumbLX == 0) {
        outEffectiveThumbLX = 0;
    } else if (rawThumbLX == -32768) {
        outEffectiveThumbLX = 32767;
    } else {
        outEffectiveThumbLX = static_cast<int16_t>(-static_cast<int32_t>(rawThumbLX));
    }
    return true;
}

bool EnqueueLocalInputDiag(const LocalInputDiagRecord& record) {
    if (!IsCaptureActiveFast()) return false;
    std::lock_guard<std::mutex> lock(g_diagQueueMutex);
    if (!IsCaptureActiveFast()) return false;
    if (g_inputDiagQueueCount < kMaxInputDiagsPerFrame) {
        g_inputDiagQueue[g_inputDiagQueueCount++] = record;
        return true;
    }
    ++g_inputDiagQueueOmitted;
    return false;
}

bool TransformPadStateWithGate(
    const LocalInputGateContext& gate,
    uint32_t& inOutPacketNumber,
    uint16_t& inOutButtons,
    int16_t& inOutThumbLX,
    LocalInputDiagRecord* outDiag) {
    const uint32_t rawPacket = inOutPacketNumber;
    const uint16_t rawButtons = inOutButtons;
    const int16_t rawStickX = inOutThumbLX;

    const bool shouldTransform = ShouldApplyCameraRelativeInputGate(gate);
    uint16_t effectiveButtons = rawButtons;
    int16_t effectiveStickX = rawStickX;
    const bool applied = ApplyCameraRelativePadTransform(
        rawButtons, rawStickX, shouldTransform, effectiveButtons, effectiveStickX);

    inOutButtons = effectiveButtons;
    inOutThumbLX = effectiveStickX;
    if (applied && gate.onlineSessionActive && FirstForOnlineDecision(g_loggedInputDecision))
        std::fprintf(stderr, "[side-input] local direction transform active (raw pad left/right, XAM user %u, online decision %u)\n",
                     gate.targetUserIndex, g_onlineDecision.load());

    bool shouldEnqueueDiag = false;
    LocalInputDiagRecord rec{};
    rec.timestampMs = GetTickCount64();
    rec.localUser = gate.targetUserIndex;
    rec.cameraSide = gate.cameraSide;
    rec.inputMode = gate.inputMode;
    rec.rawButtons = rawButtons;
    rec.effectiveButtons = effectiveButtons;
    rec.rawStickX = rawStickX;
    rec.effectiveStickX = effectiveStickX;
    rec.inBattle = gate.oppositeCameraActiveInBattle &&
                   gate.inActiveUnpausedBattle &&
                   !gate.battleOverlayInhibited &&
                   !gate.sysUiOrMenuActive;
    rec.transformApplied = applied;

    if (gate.targetUserIndex < 4u) {
        std::lock_guard<std::mutex> lock(g_padTrackMutex);
        auto& trk = g_padTrack[gate.targetUserIndex];
        if (trk.initialized) {
            const bool effectiveChanged = effectiveButtons != trk.lastEffectiveButtons ||
                                          effectiveStickX != trk.lastEffectiveStickX;
            if (rawPacket == trk.lastRawPacketNumber && effectiveChanged) {
                ++trk.syntheticPacketOffset;
            }
            // A raw counter reset/jump can collide with the preceding synthetic packet.
            if ((rawPacket != trk.lastRawPacketNumber || effectiveChanged) &&
                rawPacket + trk.syntheticPacketOffset == trk.lastEffectivePacketNumber) {
                ++trk.syntheticPacketOffset;
            }
        }
        trk.initialized = true;
        trk.lastTransformApplied = applied;
        trk.lastRawPacketNumber = rawPacket;
        trk.lastEffectiveButtons = effectiveButtons;
        trk.lastEffectiveStickX = effectiveStickX;
        inOutPacketNumber = rawPacket + trk.syntheticPacketOffset;
        trk.lastEffectivePacketNumber = inOutPacketNumber;
        rec.packetNumber = inOutPacketNumber;

        if (IsCaptureActiveFast() && gate.targetUserIndex == gate.localCameraUserIndex) {
            if (!trk.hasDiagBaseline ||
                trk.lastDiag.cameraSide != rec.cameraSide ||
                trk.lastDiag.inputMode != rec.inputMode ||
                trk.lastDiag.rawButtons != rec.rawButtons ||
                trk.lastDiag.effectiveButtons != rec.effectiveButtons ||
                trk.lastDiag.rawStickX != rec.rawStickX ||
                trk.lastDiag.effectiveStickX != rec.effectiveStickX ||
                trk.lastDiag.inBattle != rec.inBattle ||
                trk.lastDiag.transformApplied != rec.transformApplied) {
                trk.hasDiagBaseline = true;
                trk.lastDiag = rec;
                shouldEnqueueDiag = true;
            }
        }
    } else {
        rec.packetNumber = inOutPacketNumber;
    }

    if (outDiag) {
        *outDiag = rec;
    }
    if (shouldEnqueueDiag) {
        EnqueueLocalInputDiag(rec);
    }
    return applied;
}

void NoteActiveCameraSlot(uint32_t slot, bool applied, uint32_t mode) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    static std::atomic<int> logged{0};
    if (GetLocalCameraSide() != LocalCameraSide::Opposite) {
        last.store(0xFFFFFFFFu, std::memory_order_relaxed);
        return;
    }
    const uint32_t key = (slot & 0xFFu) | (applied ? 0x100u : 0u);
    if (last.exchange(key, std::memory_order_relaxed) == key || logged.fetch_add(1, std::memory_order_relaxed) >= 200) return;
    std::fprintf(stderr, "[side-camera] active camera slot %u (mode %u): %s\n", slot, mode,
                 applied ? "OPPOSITE" : "original view (staged camera or not allowed)");
}

void NotifyOppositeCameraFrameState(bool oppositeTransformApplied) {
    g_oppositeCameraAppliedLatch.store(oppositeTransformApplied, std::memory_order_release);
    if (oppositeTransformApplied) {
        g_lastOppositeCameraEpoch.store(g_inputFrameEpoch.load(), std::memory_order_release);
        if (FirstForOnlineDecision(g_loggedCameraDecision))
            std::fprintf(stderr, "[side-camera] OPPOSITE applied to local view (online decision %u)\n", g_onlineDecision.load());
    }
}

void NotifyBattleSimulationTick(bool unpaused) {
    NoteBattleTickForSidePolicy(GetTickCount64());
    if (unpaused) {
        g_battleUnpausedActive.store(true, std::memory_order_release);
        g_lastBattleTickEpoch.store(g_inputFrameEpoch.load(), std::memory_order_release);
    } else {
        g_battleUnpausedActive.store(false, std::memory_order_release);
        g_oppositeCameraAppliedLatch.store(false, std::memory_order_release);
    }
}

void NotifyFighterInputPoll(bool overlayInhibited) {
    g_battleOverlayInhibited.store(overlayInhibited, std::memory_order_release);
    g_lastFighterInputPollEpoch.store(g_inputFrameEpoch.load(), std::memory_order_release);
}

bool IsGuestBattlePausedOrOverlayActive(const uint8_t* base) {
    if (!base) return false;
    // sub_82274750 returns (0x82E70000 - 14920); sub_823038E0 reads byte +8706.
    if (base[0x82E6E7BAu] != 0u) {
        return true;
    }
    // sub_822D36C8 writes byte +640 through *(0x82E70000 - 18696).
    const uint32_t pauseObjAddr = ReadGuestU32(base, 0x82E6B6F8u);
    if (IsValidGuestStructRange(pauseObjAddr, 644u)) {
        if (base[pauseObjAddr + 640u] != 0u) {
            return true;
        }
    }
    return false;
}

LocalInputGateContext BuildLiveInputGateContext(
    const uint8_t* base,
    uint32_t targetUserIndex,
    bool sysUiOrMenuActive,
    bool onlineSessionActive) {
    LocalInputGateContext gate{};
    gate.cameraSide = GetLocalCameraSide();
    gate.inputMode = GetLocalInputMode();
    gate.targetUserIndex = targetUserIndex;
    gate.localCameraUserIndex = GetLocalCameraUserIndex();
    gate.sysUiOrMenuActive = sysUiOrMenuActive;
    gate.onlineSessionActive = onlineSessionActive;
    gate.allowOnlineInputOverride = IsOnlineInputOverrideAllowed();
    const LocalInputStrategy strategy = GetLocalInputStrategy();
    gate.rawPadMirrorEnabled = (strategy == LocalInputStrategy::XInputMirror);
    gate.sideLatchEnabled = (strategy == LocalInputStrategy::SideLatch);
    if (onlineSessionActive && IsOnlineSidePolicyActive()) {
        // Phase 11: online, only the raw pad of the local player is mirrored (left/right) before
        // the game reads it, so the peer receives already-converted input and both simulations
        // stay identical. The +1028 side latch changes local simulation state: never online.
        gate.rawPadMirrorEnabled = true;
        gate.sideLatchEnabled = false;
        gate.allowOnlineInputOverride = true;
    }

    // Bound stale evidence by presented frames, not elapsed time: a slow frame
    // must not reverse held input while the opposite view remains on screen.
    const auto recent = [](const std::atomic<uint64_t>& last) {
        const uint64_t value = last.load(std::memory_order_acquire);
        const uint64_t epoch = g_inputFrameEpoch.load(std::memory_order_acquire);
        return value != 0 && value <= epoch && epoch - value <= 2;
    };
    const bool recentOpp = recent(g_lastOppositeCameraEpoch);
    const bool recentBattle = recent(g_lastBattleTickEpoch);
    const bool recentFighter = recent(g_lastFighterInputPollEpoch);
    const bool guestPausedOrOverlay = IsGuestBattlePausedOrOverlayActive(base);

    gate.oppositeCameraActiveInBattle =
        (gate.cameraSide == LocalCameraSide::Opposite) &&
        g_oppositeCameraAppliedLatch.load(std::memory_order_acquire) &&
        recentOpp;
    gate.inActiveUnpausedBattle =
        g_battleUnpausedActive.load(std::memory_order_acquire) &&
        recentBattle &&
        recentFighter &&
        !guestPausedOrOverlay;
    gate.battleOverlayInhibited =
        g_battleOverlayInhibited.load(std::memory_order_acquire) ||
        guestPausedOrOverlay;
    return gate;
}

bool TransformPadStateForLocalCamera(
    const uint8_t* base,
    uint32_t targetUserIndex,
    bool sysUiOrMenuActive,
    bool onlineSessionActive,
    uint32_t& inOutPacketNumber,
    uint16_t& inOutButtons,
    int16_t& inOutThumbLX) {
    const LocalInputGateContext gate = BuildLiveInputGateContext(
        base, targetUserIndex, sysUiOrMenuActive, onlineSessionActive);
    return TransformPadStateWithGate(
        gate, inOutPacketNumber, inOutButtons, inOutThumbLX, nullptr);
}

uint32_t ResolvePadOwnerUser(const uint8_t* base, uint32_t padMgrAddr, uint32_t logicalId) {
    if (!base || logicalId < 9u || !IsValidGuestStructRange(padMgrAddr, 852u)) return kNoPadOwner;
    const uint32_t begin = ReadGuestU32(base, padMgrAddr + 84u);
    const uint32_t end = ReadGuestU32(base, padMgrAddr + 88u);
    const uint32_t slot = logicalId - 9u;
    if (end < begin || slot >= (end - begin) / 4u ||
        !IsValidGuestStructRange(begin + slot * 4u, 4u)) {
        return kNoPadOwner;
    }
    const uint32_t record = ReadGuestU32(base, begin + slot * 4u);
    if (record < 4u) return record;
    if (record == 4u) {
        const uint32_t mainUser = ReadGuestU32(base, padMgrAddr + 848u);
        return (mainUser < 4u) ? mainUser : kNoPadOwner;
    }
    return kNoPadOwner;
}

void OnFighterInputPollBegin(uint32_t fighterAddr) {
    t_fighterPoll = FighterPollTls{};
    t_fighterPoll.inPoll = true;
    t_fighterPoll.fighterAddr = fighterAddr;
}

void OnFighterPadLookup(const uint8_t* base, uint32_t returnAddr, uint32_t padMgrAddr, uint32_t logicalId) {
    // Only the first lookup of the poll (sub_822745B0 with the fighter's primary id in r28) owns it.
    if (!t_fighterPoll.inPoll || t_fighterPoll.lookupSeen ||
        returnAddr != kPadLookupReturnInPadButtons) {
        return;
    }
    t_fighterPoll.lookupSeen = true;
    t_fighterPoll.ownerUser = ResolvePadOwnerUser(base, padMgrAddr, logicalId);
}

static FighterSideLatch* FindSideLatchLocked(uint32_t fighterAddr, bool create) {
    FighterSideLatch* freeSlot = nullptr;
    for (auto& e : g_sideLatch) {
        if (e.fighterAddr == fighterAddr) return &e;
        if (!freeSlot && (e.fighterAddr == 0 || !e.active)) freeSlot = &e;
    }
    if (!create || !freeSlot) return nullptr;
    *freeSlot = FighterSideLatch{};
    freeSlot->fighterAddr = fighterAddr;
    return freeSlot;
}

static bool SideLatchFresh(const FighterSideLatch& e) {
    const uint64_t epoch = g_inputFrameEpoch.load(std::memory_order_acquire);
    return e.active && e.epoch <= epoch && epoch - e.epoch <= kSideLatchMaxStaleFrames;
}

static void EnqueueSideLatchDiag(const SideLatchDiagRecord& rec) {
    if (!IsCaptureActiveFast()) return;
    std::lock_guard<std::mutex> lock(g_diagQueueMutex);
    if (!IsCaptureActiveFast()) return;
    if (g_sideLatchDiagQueueCount < kMaxSideLatchDiagsPerFrame) {
        g_sideLatchDiagQueue[g_sideLatchDiagQueueCount++] = rec;
    } else {
        ++g_sideLatchDiagQueueOmitted;
    }
}

bool OnFighterInputPollEnd(
    uint8_t* base,
    uint32_t fighterAddr,
    bool skipped,
    bool sysUiOrMenuActive,
    bool onlineSessionActive,
    SideLatchDiagRecord* outDiag) {
    const FighterPollTls poll = t_fighterPoll;
    t_fighterPoll = FighterPollTls{};
    g_pollOnlineSession.store(onlineSessionActive, std::memory_order_release);
    if (!base || !IsValidGuestStructRange(fighterAddr, kFighterInputSideOffset + 4u)) return false;

    const uint32_t cameraSide = ReadGuestU32(base, fighterAddr + kFighterInputSideOffset);
    const bool playback = ReadGuestU32(base, kInputRecordStateAddr) == kInputRecordStatePlayback;
    bool apply = false;
    // Phase 11: online polls read the network buffer (sub_822745B0 -> sub_82664CD8), not a pad, so
    // there is no XAM owner; the local battle slot (online manager +7488) names our team.
    // Phase 12: online, the fighter's owner is the match player table's side whose online XUID is
    // ours (IdentifyOnlineOwner). The online battle runs in GAME (MEASURED: mode 9); the online
    // manager's +7488 / +7492 are not the owner (logged for reference only).
    if (!skipped && onlineSessionActive && IsOnlineSidePolicyActive()) {
        const uint32_t team = BattleTeamOfFighter(base, fighterAddr);
        const uint32_t mode = CurrentGameMode(base);
        if (team <= 1u && IsBattleGameMode(mode)) {
            const uint64_t localXuid = OnlineLocalXuid();
            const OnlineOwner owner = IdentifyOnlineOwner(base, localXuid);
            NoteOnlineOwner(owner, localXuid, mode);
            if (owner.team >= 0 && uint32_t(owner.team) == team) ObserveOnlineTeamForSidePolicy(team, fighterAddr);
        }
    }
    if (!skipped && poll.fighterAddr == fighterAddr && poll.ownerUser <= 3u && cameraSide <= 1u) {
        // Phase 9/10: the side chosen before character select is settled here, on a verified
        // owner, by its team, and only inside a battle mode (menus and character select poll too).
        // Guest lookups only while a side preference waits for its battle.
        if (g_sidePolicyFast.load(std::memory_order_acquire) & kPolicyActiveBit) {
            const uint32_t team = BattleTeamOfFighter(base, fighterAddr);
            if (team <= 1u && IsBattleGameMode(CurrentGameMode(base))) {
                ObserveFighterForSidePolicy(fighterAddr, poll.ownerUser, team, onlineSessionActive);
            }
        }
        const LocalInputGateContext gate =
            BuildLiveInputGateContext(base, poll.ownerUser, sysUiOrMenuActive, onlineSessionActive);
        apply = ShouldApplySideLatchGate(gate, playback);
    }
    // Written from the fresh latch each poll, never toggled, so a re-run poll cannot flip twice.
    const uint32_t inputSide = apply ? (cameraSide ^ 1u) : cameraSide;
    if (apply) {
        WriteGuestU32(base, fighterAddr + kFighterInputSideOffset, inputSide);
    }

    bool report = false;
    {
        std::lock_guard<std::mutex> lock(g_sideLatchMutex);
        const bool capturing = IsCaptureActiveFast() && !skipped;
        FighterSideLatch* e = FindSideLatchLocked(fighterAddr, apply || capturing);
        if (e) {
            const bool changed = e->active != apply || e->cameraSide != cameraSide ||
                                 e->inputSide != inputSide || e->ownerUser != poll.ownerUser;
            report = capturing && (changed || !e->diagReported);
            if (report) e->diagReported = true;
            e->ownerUser = poll.ownerUser;
            e->cameraSide = cameraSide;
            e->inputSide = inputSide;
            e->epoch = g_inputFrameEpoch.load(std::memory_order_acquire);
            e->active = apply;
        }
    }

    SideLatchDiagRecord rec{};
    rec.timestampMs = GetTickCount64();
    rec.fighterAddr = fighterAddr;
    rec.ownerUser = poll.ownerUser;
    rec.cameraSide = cameraSide;
    rec.inputSide = inputSide;
    rec.applied = apply;
    rec.playback = playback;
    if (outDiag) *outDiag = rec;
    if (report) EnqueueSideLatchDiag(rec);
    return apply;
}

void OnFighterInputProcessEnd(uint8_t* base, uint32_t fighterAddr) {
    if (!base || !IsValidGuestStructRange(fighterAddr, kFighterInputSideOffset + 4u)) return;
    std::lock_guard<std::mutex> lock(g_sideLatchMutex);
    FighterSideLatch* e = FindSideLatchLocked(fighterAddr, false);
    if (!e || !e->active) return;
    // The cinematic camera (sub_8254AD48), effects (sub_824CAEB0) and AI helpers read +1028 too;
    // outside input interpretation they keep the camera's side. Leave anything the game rewrote.
    if (ReadGuestU32(base, fighterAddr + kFighterInputSideOffset) == e->inputSide) {
        WriteGuestU32(base, fighterAddr + kFighterInputSideOffset, e->cameraSide);
    }
}

bool BeginSideRequirementEval(uint8_t* base, uint32_t reqAddr, uint32_t fighterAddr, uint32_t& savedSide) {
    if (!base || !IsValidGuestStructRange(reqAddr, 4u)) return false;
    const uint32_t reqId = ReadGuestU32(base, reqAddr);
    if (reqId < kSideRequirementFirst || reqId > kSideRequirementLast) return false;
    if (!IsValidGuestStructRange(fighterAddr, kFighterInputSideOffset + 4u)) return false;
    // Phase 12-C online: decided only by the agreed network frame, so both clients flip alike. Left /
    // right is already mirrored in the raw pad before it is sent; only the sidestep side changes here.
    if (g_pollOnlineSession.load(std::memory_order_acquire) && OnlineBattleInputLive(base)) {
        const uint32_t current = ReadGuestU32(base, fighterAddr + kFighterInputSideOffset);
        const uint32_t team = BattleTeamOfFighter(base, fighterAddr);
        if (current > 1u || !OnlineTeamSidestepFlipped(base, team)) return false;
        savedSide = current;
        WriteGuestU32(base, fighterAddr + kFighterInputSideOffset, current ^ 1u);
        g_sideRequirementFlips.fetch_add(1, std::memory_order_relaxed);
        if (!(g_loggedSidestepFlipTeams.fetch_or(1u << team, std::memory_order_acq_rel) & (1u << team)))
            std::fprintf(stderr, "[side-input] W/S: team %u sidestep side flipped (requirement %u, side %u -> %u)\n", team, reqId,
                         current, current ^ 1u);
        return true;
    }
    std::lock_guard<std::mutex> lock(g_sideLatchMutex);
    FighterSideLatch* e = FindSideLatchLocked(fighterAddr, false);
    if (!e || !SideLatchFresh(*e)) return false;
    const uint32_t current = ReadGuestU32(base, fighterAddr + kFighterInputSideOffset);
    if (current != e->cameraSide) return false;
    savedSide = current;
    WriteGuestU32(base, fighterAddr + kFighterInputSideOffset, e->inputSide);
    g_sideRequirementFlips.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void EndSideRequirementEval(uint8_t* base, uint32_t fighterAddr, uint32_t savedSide) {
    if (!base || !IsValidGuestStructRange(fighterAddr, kFighterInputSideOffset + 4u)) return;
    if (ReadGuestU32(base, fighterAddr + kFighterInputSideOffset) == (savedSide ^ 1u)) {
        WriteGuestU32(base, fighterAddr + kFighterInputSideOffset, savedSide);
    }
}

bool IsSideLatchActiveForFighter(uint32_t fighterAddr) {
    std::lock_guard<std::mutex> lock(g_sideLatchMutex);
    const FighterSideLatch* e = FindSideLatchLocked(fighterAddr, false);
    return e && SideLatchFresh(*e);
}

bool ComputeOppositeViewForRender(const uint8_t* base, uint32_t camMgrAddr, float outView[16]) {
    CameraProducerSnapshot snap{};
    if (!outView || !ReadCameraProducerSnapshot(base, camMgrAddr, snap) ||
        !ShouldApplyOppositeToCamera(snap)) {
        return false;
    }
    float view[16] = {};
    for (uint32_t i = 0; i < 16u; ++i) {
        view[i] = ReadGuestF32(base, snap.viewAddr + i * 4u);
    }
    return ApplyOppositeCameraMatrix4x4(view, snap.target, outView);
}

bool MirrorRenderParamBlockEye(uint8_t* base, uint32_t blockAddr, const CameraProducerSnapshot& snap) {
    if (!base || !ShouldApplyOppositeToCamera(snap) ||
        !IsValidGuestStructRange(blockAddr, kRenderParamEyeOffset + 16u)) {
        return false;
    }
    float target[3] = {};
    float eye[4] = {};
    for (uint32_t i = 0; i < 3u; ++i) {
        target[i] = ReadGuestF32(base, blockAddr + kRenderParamTargetOffset + i * 4u);
    }
    for (uint32_t i = 0; i < 4u; ++i) {
        eye[i] = ReadGuestF32(base, blockAddr + kRenderParamEyeOffset + i * 4u);
    }
    // Only rewrite a block that was filled from this camera this frame.
    for (int i = 0; i < 3; ++i) {
        const float tolT = 1e-3f * std::fmax(1.0f, std::fabs(snap.target[i]));
        const float tolE = 1e-3f * std::fmax(1.0f, std::fabs(snap.eye[i]));
        if (!(std::fabs(target[i] - snap.target[i]) <= tolT) ||
            !(std::fabs(eye[i] - snap.eye[i]) <= tolE)) {
            return false;
        }
    }
    float outEye[4] = {};
    if (!ApplyOppositeCameraEye(eye, snap.target, outEye)) return false;
    for (uint32_t i = 0; i < 4u; ++i) {
        WriteGuestF32(base, blockAddr + kRenderParamEyeOffset + i * 4u, outEye[i]);
    }
    if (IsCaptureActiveFast()) g_paramEyeMirrored.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void NotifyStageCullSample(bool substituted, int originalResult, int renderedResult) {
    if (!IsCaptureActiveFast()) return;
    g_stageCullCalls.fetch_add(1, std::memory_order_relaxed);
    if (!substituted) return;
    g_stageCullSubstituted.fetch_add(1, std::memory_order_relaxed);
    if (originalResult < 0) return;
    // sub_822956C0 returns 0 when the sphere is outside; sub_82525CD0 then skips the mesh.
    if (originalResult == 0 && renderedResult != 0) {
        g_stageCullShownOnlyByRendered.fetch_add(1, std::memory_order_relaxed);
    } else if (originalResult != 0 && renderedResult == 0) {
        g_stageCullHiddenOnlyByRendered.fetch_add(1, std::memory_order_relaxed);
    }
}

static bool NearlyEqualVec3(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; ++i) {
        const float tol = 1e-3f * std::fmax(1.0f, std::fabs(b[i]));
        if (!(std::fabs(a[i] - b[i]) <= tol)) return false;
    }
    return true;
}

// Unit forward (target - eye); sub_82AC3840 normalizes with vrsqrtefp, so compare loosely.
static bool SnapshotForward(const CameraProducerSnapshot& snap, float out[3]) {
    const float d[3] = {snap.target[0] - snap.eye[0], snap.target[1] - snap.eye[1], snap.target[2] - snap.eye[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) out[i] = d[i] / len;
    return true;
}

static bool MatchesForward(const float v[3], const float fwd[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(v[i] - fwd[i]) <= 4e-3f)) return false;
    }
    return true;
}

static void ReadGuestVec3(const uint8_t* base, uint32_t addr, float out[3]) {
    for (uint32_t i = 0; i < 3u; ++i) out[i] = ReadGuestF32(base, addr + i * 4u);
}

bool MirrorStageFadeCameraBlock(uint8_t* base, uint32_t blockAddr, const CameraProducerSnapshot& snap) {
    if (!base || !ShouldApplyOppositeToCamera(snap) || !IsValidGuestStructRange(blockAddr, 48u)) {
        return false;
    }
    float forward[3] = {};
    float eye[4] = {};
    float target[3] = {};
    float snapForward[3] = {};
    ReadGuestVec3(base, blockAddr + kStageFadeForwardOffset, forward);
    for (uint32_t i = 0; i < 4u; ++i) eye[i] = ReadGuestF32(base, blockAddr + kStageFadeEyeOffset + i * 4u);
    ReadGuestVec3(base, blockAddr + kStageFadeTargetOffset, target);
    // Only a block filled from this camera this frame; a mirrored block no longer matches.
    if (!SnapshotForward(snap, snapForward) || !NearlyEqualVec3(eye, snap.eye) ||
        !NearlyEqualVec3(target, snap.target) || !MatchesForward(forward, snapForward)) {
        return false;
    }
    float outEye[4] = {};
    if (!ApplyOppositeCameraEye(eye, snap.target, outEye)) return false;
    for (uint32_t i = 0; i < 4u; ++i) {
        WriteGuestF32(base, blockAddr + kStageFadeEyeOffset + i * 4u, outEye[i]);
    }
    // 180 degrees about the vertical axis through the target.
    WriteGuestF32(base, blockAddr + kStageFadeForwardOffset + 0u, -forward[0]);
    WriteGuestF32(base, blockAddr + kStageFadeForwardOffset + 8u, -forward[2]);
    return true;
}

bool MirrorCameraEyeOut(uint8_t* base, uint32_t outAddr, const CameraProducerSnapshot& snap) {
    if (!base || !ShouldApplyOppositeToCamera(snap) || !IsValidGuestStructRange(outAddr, 12u)) {
        return false;
    }
    float eye[4] = {};
    ReadGuestVec3(base, outAddr, eye);
    eye[3] = 1.0f;
    if (!NearlyEqualVec3(eye, snap.eye)) return false;
    float outEye[4] = {};
    if (!ApplyOppositeCameraEye(eye, snap.target, outEye)) return false;
    WriteGuestF32(base, outAddr + 0u, outEye[0]);
    WriteGuestF32(base, outAddr + 8u, outEye[2]);
    return true;
}

bool MirrorCameraForwardOut(uint8_t* base, uint32_t outAddr, const CameraProducerSnapshot& snap) {
    if (!base || !ShouldApplyOppositeToCamera(snap) || !IsValidGuestStructRange(outAddr, 12u)) {
        return false;
    }
    float forward[3] = {};
    float snapForward[3] = {};
    ReadGuestVec3(base, outAddr, forward);
    if (!SnapshotForward(snap, snapForward) || !MatchesForward(forward, snapForward)) return false;
    WriteGuestF32(base, outAddr + 0u, -forward[0]);
    WriteGuestF32(base, outAddr + 8u, -forward[2]);
    return true;
}

bool BuildShadowCameraManager(uint8_t* base, uint32_t camMgrAddr, uint32_t shadowMgrAddr, uint32_t shadowInstAddr) {
    CameraProducerSnapshot snap{};
    if (!base || !ReadCameraProducerSnapshot(base, camMgrAddr, snap) || !ShouldApplyOppositeToCamera(snap) ||
        !IsValidGuestStructRange(camMgrAddr, kShadowCamMgrBytes) ||
        !IsValidGuestStructRange(snap.camInstAddr, kShadowCamInstBytes) ||
        !IsValidGuestStructRange(shadowMgrAddr, kShadowCamMgrBytes) ||
        !IsValidGuestStructRange(shadowInstAddr, kShadowCamInstBytes) ||
        (shadowInstAddr & 15u) != 0u ||
        kCamMgrSlotArrayOffset + (snap.activeSlot + 1u) * 4u > kShadowCamMgrBytes) {
        return false;
    }
    // Byte copies keep the guest big-endian layout; the real camMgr/camInst are only read.
    std::memmove(base + shadowMgrAddr, base + camMgrAddr, kShadowCamMgrBytes);
    std::memmove(base + shadowInstAddr, base + snap.camInstAddr, kShadowCamInstBytes);
    const uint32_t pivotAddr = snap.camInstAddr + 32u;
    if (!TransformGuestCameraMatrix4x4(base, snap.viewAddr, pivotAddr, shadowInstAddr + 224u) ||
        !TransformGuestCameraMatrix4x4(base, snap.viewProjAddr, pivotAddr, shadowInstAddr + 96u) ||
        !TransformGuestCameraEye4(base, pivotAddr, shadowInstAddr + 16u)) {
        return false;
    }
    WriteGuestU32(base, shadowMgrAddr + kCamMgrSlotArrayOffset + snap.activeSlot * 4u, shadowInstAddr);
    return true;
}

uint32_t RenderSiteForCamMgrReturn(uint32_t returnAddr) {
    switch (returnAddr) {
    case kMobVisibleReturn:
    case kMobDepthReturn:
    case kMobFighterDepthMinReturn:
    case kMobFighterDepthMaxReturn:
        return kRenderSiteMobCamera;
    case kFieldGridViewReturn:
    case kFieldGridView2Return:
        return kRenderSiteFieldGrid;
    case kLensFlareViewProjReturn:
        return kRenderSiteLensFlare;
    case kLightCullViewReturn:
        return kRenderSiteLightCull;
    default:
        return 0u;
    }
}

uint32_t RenderSiteForCameraEyeReturn(uint32_t returnAddr) {
    if (returnAddr == kFieldGridEyeReturn) return kRenderSiteFieldGrid;
    if (returnAddr == kLensFlareEyeReturn) return kRenderSiteLensFlare;
    return 0u;
}

uint32_t RenderSiteForCameraForwardReturn(uint32_t returnAddr) {
    if (returnAddr == kFieldGridForwardReturn) return kRenderSiteFieldGrid;
    if (returnAddr == kLensFlareForwardReturn) return kRenderSiteLensFlare;
    return 0u;
}

uint32_t CameraManagerFromSystem(const uint8_t* base, uint32_t camSysAddr) {
    if (!base || !IsValidGuestStructRange(camSysAddr, 440u)) return 0u;
    const uint32_t camMgrAddr = ReadGuestU32(base, camSysAddr + 436u);
    return IsValidGuestStructRange(camMgrAddr, kShadowCamMgrBytes) ? camMgrAddr : 0u;
}

uint32_t ResolveCameraManager(const uint8_t* base) {
    if (!base) return 0u;
    const uint32_t holder = ReadGuestU32(base, kCameraSystemHolderAddr);
    if (!IsValidGuestStructRange(holder, 728u)) return 0u;
    return CameraManagerFromSystem(base, ReadGuestU32(base, holder + 724u));
}

void NotifyStageSideSubstitution(uint32_t siteBit) {
    if (!IsCaptureActiveFast() || siteBit == 0u) return;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (siteBit == (1u << i)) {
            g_stageSideSubstitutions[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

bool IsCaptureActiveFast() {
    return g_captureActive.load(std::memory_order_acquire);
}

bool EnqueueCameraFrameDiag(const CameraFrameDiagRecord& record) {
    if (!IsCaptureActiveFast()) return false;
    std::lock_guard<std::mutex> lock(g_diagQueueMutex);
    if (!IsCaptureActiveFast()) return false;
    if (g_diagQueueCount < kMaxProducersPerFrame) {
        g_diagQueue[g_diagQueueCount++] = record;
        return true;
    }
    ++g_diagQueueOmitted;
    return false;
}

void OnProducerSample(const CameraProducerSnapshot& snap) {
    if (!IsCaptureActiveFast()) return;
    CameraFrameDiagRecord rec{};
    rec.timestampMs = GetTickCount64();
    rec.camMgrAddr = snap.camMgrAddr;
    rec.activeSlot = snap.activeSlot;
    rec.camInstAddr = snap.camInstAddr;
    rec.camVtable = snap.camVtable;
    rec.cameraSide = snap.appliedSide;
    rec.slotAllowed = snap.slotAllowed;
    rec.transformApplied = ShouldApplyOppositeToCamera(snap);
    rec.camInstUnchanged = true;
    for (int i = 0; i < 3; ++i) {
        rec.cameraEyeOriginal[i] = snap.eye[i];
        rec.cameraEyeRendered[i] = snap.eye[i];
        rec.cameraTarget[i] = snap.target[i];
        rec.cameraUp[i] = snap.up[i];
    }
    rec.cameraEyeOriginal[3] = 1.0f;
    rec.cameraEyeRendered[3] = 1.0f;
    if (rec.transformApplied) {
        ApplyOppositeCameraEye(rec.cameraEyeOriginal, rec.cameraTarget, rec.cameraEyeRendered);
    }
    rec.zNear = snap.zNear;
    rec.zFar = snap.zFar;
    rec.fovY = snap.fovY;
    EnqueueCameraFrameDiag(rec);
}

void ResetDiagStateForTesting() {
    g_captureActive.store(false, std::memory_order_release);
    g_pollOnlineSession.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_diagQueueMutex);
        g_diagQueueCount = 0;
        g_diagQueueOmitted = 0;
        g_inputDiagQueueCount = 0;
        g_inputDiagQueueOmitted = 0;
        g_sideLatchDiagQueueCount = 0;
        g_sideLatchDiagQueueOmitted = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_padTrackMutex);
        g_padTrack = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_sideLatchMutex);
        g_sideLatch = {};
    }
    t_fighterPoll = FighterPollTls{};
    g_sideRequirementFlips.store(0, std::memory_order_release);
    g_stageCullCalls.store(0, std::memory_order_release);
    g_stageCullSubstituted.store(0, std::memory_order_release);
    g_stageCullShownOnlyByRendered.store(0, std::memory_order_release);
    g_stageCullHiddenOnlyByRendered.store(0, std::memory_order_release);
    g_paramEyeMirrored.store(0, std::memory_order_release);
    for (auto& c : g_stageSideSubstitutions) c.store(0, std::memory_order_release);
    g_oppositeCameraAppliedLatch.store(false, std::memory_order_release);
    g_inputFrameEpoch.store(1, std::memory_order_release);
    g_lastOppositeCameraEpoch.store(0, std::memory_order_release);
    g_battleUnpausedActive.store(false, std::memory_order_release);
    g_lastBattleTickEpoch.store(0, std::memory_order_release);
    g_battleOverlayInhibited.store(false, std::memory_order_release);
    g_lastFighterInputPollEpoch.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_sidePolicyMutex);
        g_sidePolicy = SidePolicyState{};
        g_sidePolicyFast.store(0, std::memory_order_release);
    }
    g_sidePolicyLastTickMs.store(0, std::memory_order_release);
    g_hudSideSwapState.store(0xFFFFFFFFu, std::memory_order_relaxed);
    if (g.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g.file);
    }
    g = Capture{};
}

bool Active() { return g.phase == Phase::Capturing && g.file != INVALID_HANDLE_VALUE; }
bool WantsDrawHash() { return Active() && g.draws < kMaxDrawsPerFrame; }
void OnSkippedDraw() {
    if (!Active()) return;
    ++g.omittedDraws;
    g.truncated = true;
}
bool IsCameraRegister(uint32_t reg) { return reg >= 0x4020 && reg <= 0x402F; }

void OnSwap() {
    g_inputFrameEpoch.fetch_add(1, std::memory_order_acq_rel);
    if (g.phase == Phase::Uninitialized) {
        char enabled[8] = {};
        if (GetEnvironmentVariableA("TTT2_SIDE_CAMERA_DIAG", enabled, DWORD(sizeof(enabled))) != 1 ||
            enabled[0] != '1') {
            g.phase = Phase::Done;
            return;
        }
        if (!EnvPath("TTT2_SIDE_CAMERA_TRIGGER", g.trigger) ||
            !EnvPath("TTT2_SIDE_CAMERA_OUT", g.path)) {
            std::fputs("[side-camera] enable requires TRIGGER and OUT absolute paths\n", stderr);
            g.phase = Phase::Done;
            return;
        }
        g.phase = Phase::Armed;
        char count[32] = {};
        if (GetEnvironmentVariableA("TTT2_SIDE_CAMERA_FRAMES", count, DWORD(sizeof(count))) > 0) {
            char* end = nullptr;
            long n = std::strtol(count, &end, 10);
            if (end != count && *end == '\0' && n >= 1 && n <= static_cast<long>(kMaxFrames))
                g.maxFrames = unsigned(n);
        }
    }
    if (g.phase == Phase::Armed) {
        DWORD attr = GetFileAttributesA(g.trigger);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
        g.file = CreateFileA(g.path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g.file == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[side-camera] cannot create NEW output: %s (err=%lu)\n",
                         g.path, GetLastError());
            g.phase = Phase::Done;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_diagQueueMutex);
            g_diagQueueCount = 0;
            g_diagQueueOmitted = 0;
            g_inputDiagQueueCount = 0;
            g_inputDiagQueueOmitted = 0;
            g_sideLatchDiagQueueCount = 0;
            g_sideLatchDiagQueueOmitted = 0;
        }
        {
            std::lock_guard<std::mutex> lock(g_padTrackMutex);
            for (auto& trk : g_padTrack) {
                trk.hasDiagBaseline = false;
            }
        }
        g_sideRequirementFlips.store(0, std::memory_order_release);
        for (auto& c : g_stageSideSubstitutions) c.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_sideLatchMutex);
            for (auto& e : g_sideLatch) {
                e.diagReported = false;
            }
        }
        g.phase = Phase::Capturing;
        g_captureActive.store(true, std::memory_order_release);
        Emit("{\"type\":\"start\",\"format\":1,\"tick_ms\":%llu,\"frames_requested\":%u,\"candidates\":[8,9,10,11,20,21,22,23]}\n",
             static_cast<unsigned long long>(GetTickCount64()), g.maxFrames);
        std::fprintf(stderr, "[side-camera] capturing %u GPU frames to %s\n",g.maxFrames,g.path);
        return; // the next complete swap-to-swap interval is frame 1
    }
    if (!Active()) return;
    ++g.frame;
    DrainAndEmitCameraFrameDiags(g.frame);
    Emit("{\"type\":\"frame_end\",\"tick_ms\":%llu,\"frame\":%u,\"draws\":%u,\"writes\":%u}\n",
         static_cast<unsigned long long>(GetTickCount64()), g.frame, g.draws, g.writes);
    if (g.frame >= g.maxFrames) { End(); return; }
    g.draws = 0; g.writes = 0; g.producers = 0; g.inputDiags = 0;
}
void OnConstantWrite(uint32_t reg, uint32_t raw, uint32_t packet,
                     uint32_t source, uint32_t packetKind) {
    if (!Active() || !IsCameraRegister(reg)) return;
    if (g.writes++ >= kMaxWritesPerFrame) { ++g.omittedWrites; g.truncated = true; return; }
    Emit("{\"type\":\"write\",\"frame\":%u,\"reg\":%u,\"row\":%u,\"component\":%u,\"raw\":\"%08X\",\"packet\":\"%08X\",\"source\":\"%08X\",\"kind\":%u}\n",
         g.frame + 1, reg, (reg-0x4000)/4, (reg-0x4000)%4, raw, packet, source, packetKind);
}
void OnDraw(const uint32_t* regs, uint32_t initiator, uint32_t vsAddr,
            uint32_t vsSize, uint64_t vsHash, uint32_t psAddr,
            uint32_t psSize, uint64_t psHash) {
    if (!Active() || !regs) return;
    if (g.draws++ >= kMaxDrawsPerFrame) { ++g.omittedDraws; g.truncated = true; return; }
    char rows[620] = {};
    size_t used = 0;
    for (unsigned row : {8u, 9u, 10u, 11u, 20u, 21u, 22u, 23u}) {
        int n = std::snprintf(rows + used, sizeof(rows) - used,
              "%sc%u:%08X,%08X,%08X,%08X", used ? ";" : "", row,
              regs[0x4000 + row*4], regs[0x4001 + row*4],
              regs[0x4002 + row*4], regs[0x4003 + row*4]);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(rows)-used) return;
        used += static_cast<size_t>(n);
    }
    Emit("{\"type\":\"draw\",\"frame\":%u,\"index\":%u,\"initiator\":\"%08X\",\"mode\":%u,\"target\":\"%08X\",\"depth\":\"%08X\",\"cull\":\"%08X\",\"vs\":\"%08X\",\"vs_count\":%u,\"vs_hash\":\"%016llX\",\"ps\":\"%08X\",\"ps_count\":%u,\"ps_hash\":\"%016llX\",\"constants\":\"%s\"}\n",
         g.frame+1, g.draws, initiator, regs[0x2208]&7u, regs[0x2001], regs[0x2002], regs[0x2205],
         vsAddr,vsSize,static_cast<unsigned long long>(vsHash),psAddr,psSize,
         static_cast<unsigned long long>(psHash), rows);
}
} // namespace ttt2_side_camera_diag

#if defined(TTT2_GPU_HW_BACKEND)
namespace {
struct TlsRenderSlot0Trace {
    bool inSub823C8B30 = false;
    bool hasValidSnapshot = false;
    bool view0Transformed = false;
    bool view1Transformed = false;
    bool viewProjTransformed = false;
    bool eyeTransformed = false;
    uint32_t sceneRenderCtxAddr = 0;
    uint64_t camInstHashBefore = 0;
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
};
thread_local TlsRenderSlot0Trace g_tlsSlot0;

// Camera manager singleton seen by the camera hooks; the render parameter block hook has no r3 for it.
std::atomic<uint32_t> g_lastCamMgrAddr{0};

struct TlsStageCull {
    bool pending = false;
    uint32_t originalViewAddr = 0;
};
thread_local TlsStageCull g_tlsStageCull;
// Per-thread 64-byte guest matrix handed to render-only view consumers (lvx128 needs 16 alignment).
thread_local uint32_t g_tlsRenderViewShadow = 0;

uint32_t RenderViewShadow() {
    if (g_tlsRenderViewShadow == 0) {
        g_tlsRenderViewShadow = xlive::GuestAlloc(64);
    }
    return g_tlsRenderViewShadow;
}

// Per-thread shadow camMgr/camInst for stage consumers that read activeCamInst inline.
// Each caller uses the copy only until it returns, so one pair per thread is enough.
thread_local uint32_t g_tlsShadowCamMgr = 0;
thread_local uint32_t g_tlsShadowCamInst = 0;

bool ShadowCameraBuffers(uint32_t& mgr, uint32_t& inst) {
    if (g_tlsShadowCamMgr == 0) {
        g_tlsShadowCamMgr = xlive::GuestAlloc(ttt2_side_camera_diag::kShadowCamMgrBytes);
    }
    if (g_tlsShadowCamInst == 0) {
        g_tlsShadowCamInst = xlive::GuestAlloc(ttt2_side_camera_diag::kShadowCamInstBytes);
    }
    mgr = g_tlsShadowCamMgr;
    inst = g_tlsShadowCamInst;
    return mgr != 0u && inst != 0u;
}

bool RenderSiteEnabled(uint32_t siteBit) {
    return siteBit != 0u && (ttt2_side_camera_diag::GetRenderCameraSites() & siteBit) != 0u;
}

// Phase 9 HUD team swap around the HUD's own code (main thread; nested calls swap once). The
// four entries swapped in are remembered and put back only if the array still holds them.
thread_local int t_hudSwapDepth = 0;
thread_local bool t_hudSwapped = false;
thread_local uint32_t t_hudOriginal[4] = {};

struct ScopedHudTeamSwap {
    uint8_t* base;
    explicit ScopedHudTeamSwap(uint8_t* b) : base(b) {
        if (t_hudSwapDepth++ != 0) return;
        if (!ttt2_side_camera_diag::ShouldSwapHudTeams(XSessionAnyActive())) return;
        const uint32_t a = ttt2_side_camera_diag::kBattleFighterArrayAddr;
        for (uint32_t i = 0; i < 4u; ++i) t_hudOriginal[i] = GRead32(base, a + i * 4u);
        t_hudSwapped = ttt2_side_camera_diag::SwapHudFighterSlots(base, a);
    }
    ~ScopedHudTeamSwap() {
        if (--t_hudSwapDepth != 0 || !t_hudSwapped) return;
        t_hudSwapped = false;
        const uint32_t a = ttt2_side_camera_diag::kBattleFighterArrayAddr;
        const uint32_t swappedIn[4] = {t_hudOriginal[1], t_hudOriginal[0], t_hudOriginal[3], t_hudOriginal[2]};
        for (uint32_t i = 0; i < 4u; ++i) {
            if (GRead32(base, a + i * 4u) != swappedIn[i]) {
                std::fprintf(stderr,
                             "[side-select] HUD fighter array changed during the HUD; left as the game wrote it\n");
                return;
            }
        }
        for (uint32_t i = 0; i < 4u; ++i) GWrite32(base, a + i * 4u, t_hudOriginal[i]);
    }
};

bool LocalSysUiOrMenuActive() {
    // Same signals kernel_xam.cpp feeds the Phase 6 gate; any of them blocks the side latch.
    return SysUiOwnsInput() || SysUiTitleWantsProfile() || SysUiGameButtonMask() != 0xFFFF ||
           SysUiTextEntryActive() || SysUiKeyCaptureActive();
}
} // namespace

// SceneRenderContext::PopulateCameraPassSlots (sub_823C8B30 in ppc_recomp.43.cpp).
// Wraps __imp__sub_823C8B30 to capture a complete, post-execution comparison between
// the untouched guest camera object (activeCamInst) and the final rendered Slot 0
// matrices (sceneRenderCtx + 624, +752, +944) after all of sub_823C8B30 finishes.
PPC_FUNC(sub_823C8B30) {
    g_tlsSlot0 = TlsRenderSlot0Trace{};
    g_tlsSlot0.inSub823C8B30 = true;
    g_tlsSlot0.sceneRenderCtxAddr = ctx.r3.u32;

    __imp__sub_823C8B30(ctx, base);

    const bool oppositeApplied = g_tlsSlot0.hasValidSnapshot &&
                                 g_tlsSlot0.view0Transformed &&
                                 g_tlsSlot0.view1Transformed &&
                                 g_tlsSlot0.viewProjTransformed &&
                                 g_tlsSlot0.eyeTransformed;
    ttt2_side_camera_diag::NotifyOppositeCameraFrameState(oppositeApplied);
    if (g_tlsSlot0.hasValidSnapshot) {
        ttt2_side_camera_diag::NoteActiveCameraSlot(g_tlsSlot0.snap.activeSlot, oppositeApplied,
                                                    ttt2_side_camera_diag::CurrentGameMode(base));
    }

    if (ttt2_side_camera_diag::IsCaptureActiveFast() &&
        g_tlsSlot0.hasValidSnapshot &&
        ttt2_side_camera_diag::IsValidGuestStructRange(g_tlsSlot0.sceneRenderCtxAddr, 1088u)) {
        const auto& snap = g_tlsSlot0.snap;
        const uint32_t sceneCtx = g_tlsSlot0.sceneRenderCtxAddr;
        const uint64_t hashAfter = ttt2_side_camera_diag::HashGuestBytesFNV1a(base, snap.camInstAddr, 384u);

        ttt2_side_camera_diag::CameraFrameDiagRecord rec{};
        rec.timestampMs = GetTickCount64();
        rec.camMgrAddr = snap.camMgrAddr;
        rec.activeSlot = snap.activeSlot;
        rec.camInstAddr = snap.camInstAddr;
        rec.camVtable = snap.camVtable;
        rec.cameraSide = snap.appliedSide;
        rec.slotAllowed = snap.slotAllowed;
        rec.transformApplied = oppositeApplied;
        rec.camInstUnchanged = (hashAfter != 0 && hashAfter == g_tlsSlot0.camInstHashBefore);

        for (uint32_t i = 0; i < 3u; ++i) {
            rec.cameraEyeOriginal[i] = snap.eye[i];
            rec.cameraTarget[i] = snap.target[i];
            rec.cameraUp[i] = snap.up[i];
        }
        rec.cameraEyeOriginal[3] = 1.0f;
        rec.zNear = snap.zNear;
        rec.zFar = snap.zFar;
        rec.fovY = snap.fovY;

        for (uint32_t i = 0; i < 4u; ++i) {
            const uint32_t rawEye = GRead32(base, sceneCtx + 944u + i * 4u);
            std::memcpy(&rec.cameraEyeRendered[i], &rawEye, sizeof(float));
        }
        for (uint32_t i = 0; i < 16u; ++i) {
            const uint32_t rawVO = GRead32(base, snap.viewAddr + i * 4u);
            const uint32_t rawVR = GRead32(base, sceneCtx + 624u + i * 4u);
            const uint32_t rawB_O = GRead32(base, snap.viewProjAddr + i * 4u);
            const uint32_t rawB_R = GRead32(base, sceneCtx + 752u + i * 4u);
            std::memcpy(&rec.viewMatrixOriginal[i], &rawVO, sizeof(float));
            std::memcpy(&rec.viewMatrixRendered[i], &rawVR, sizeof(float));
            std::memcpy(&rec.viewProjOriginal[i], &rawB_O, sizeof(float));
            std::memcpy(&rec.viewProjRendered[i], &rawB_R, sizeof(float));
        }
        ttt2_side_camera_diag::EnqueueCameraFrameDiag(rec);
    }
    g_tlsSlot0.inSub823C8B30 = false;
}

// BattleSimulation::TickFrame (sub_822CDA88 in ppc_recomp.14.cpp:23771)
// r3 enables the simulation branch; it is not a round-state or controller-slot ID.
PPC_FUNC(sub_822CDA88) {
    const bool unpaused = (ctx.r3.u32 != 0u);
    ttt2_side_camera_diag::NotifyBattleSimulationTick(unpaused);
    __imp__sub_822CDA88(ctx, base);
}

// Fighter::PollBattleControllerInput (sub_823038E0 in ppc_recomp.18.cpp:20495)
// r3 = fighterObj, r4 = analogOutPtr, low byte of r5 skips polling when nonzero.
PPC_FUNC(sub_823038E0) {
    const uint32_t fighterAddr = ctx.r3.u32;
    const uint32_t skipFlag = ctx.r5.u32 & 0xFFu;
    ttt2_side_camera_diag::OnFighterInputPollBegin(fighterAddr);
    __imp__sub_823038E0(ctx, base);
    if (skipFlag == 0u) {
        // The returned input may have traversed a delay FIFO and is not menu state.
        ttt2_side_camera_diag::NotifyFighterInputPoll(
            ttt2_side_camera_diag::IsGuestBattlePausedOrOverlayActive(base));
    }
    // +1028 was just latched from +1024; r3 (the returned pad word) is left untouched.
    ttt2_side_camera_diag::OnFighterInputPollEnd(
        base, fighterAddr, skipFlag != 0u, LocalSysUiOrMenuActive(), XSessionAnyActive());
}

// Fighter::ProcessControllerInput (sub_82303BB0): polls (sub_823038E0), builds commands with the
// +1028 table row (sub_82303400), records/plays back and stores input history (sub_823028E0).
PPC_FUNC(sub_82303BB0) {
    const uint32_t fighterAddr = ctx.r3.u32;
    __imp__sub_82303BB0(ctx, base);
    ttt2_side_camera_diag::OnFighterInputProcessEnd(base, fighterAddr);
}

// PadManager::GetButtons(padMgr, logicalId) (sub_8228DC40), offline branch of sub_822745B0.
PPC_FUNC(sub_8228DC40) {
    ttt2_side_camera_diag::OnFighterPadLookup(
        base, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32);
    __imp__sub_8228DC40(ctx, base);
}

// Requirement evaluator (sub_8230DAB8: r3 = requirement {id, param}, r4 = fighter, r5 = opponent).
// Only ids 144..149 read +1028; every other id runs untouched.
PPC_FUNC(sub_8230DAB8) {
    const uint32_t fighterAddr = ctx.r4.u32;
    uint32_t savedSide = 0;
    const bool flipped = ttt2_side_camera_diag::BeginSideRequirementEval(
        base, ctx.r3.u32, fighterAddr, savedSide);
    __imp__sub_8230DAB8(ctx, base);
    if (flipped) {
        ttt2_side_camera_diag::EndSideRequirementEval(base, fighterAddr, savedSide);
    }
}

// Phase 12-C online W/S. This machine's battle input on its way to the sync engine
// (sub_8266D398 -> sub_82664CE0(mgr, buttons) -> sub_826F0CD0 stores the low 16 bits for its slot).
PPC_FUNC(sub_82664CE0) {
    ctx.r4.u64 = ttt2_side_camera_diag::OnlineInputToSend(base, ctx.r4.u32);
    __imp__sub_82664CE0(ctx, base);
}

// The agreed frame's input of slot r4 (r3 = NACE): current (sub_826EFA48) and previous
// (sub_826EFA90). The OPPOSITE-view flag never reaches the game.
PPC_FUNC(sub_826EFA48) {
    const uint32_t slot = ctx.r4.u32;
    __imp__sub_826EFA48(ctx, base);
    ctx.r3.u64 = ttt2_side_camera_diag::OnlineInputAsRead(slot, ctx.r3.u32, false);
}

PPC_FUNC(sub_826EFA90) {
    const uint32_t slot = ctx.r4.u32;
    __imp__sub_826EFA90(ctx, base);
    ctx.r3.u64 = ttt2_side_camera_diag::OnlineInputAsRead(slot, ctx.r3.u32, true);
}

// CameraManager::GetActiveViewMatrix, second getter (sub_82398448 -> activeCamInst + 224).
// Not reached by sub_823C8B30; render-only callers listed in side_camera_diag.h get the
// opposite view from a per-thread guest copy. camInst stays untouched.
PPC_FUNC(sub_82398448) {
    const uint32_t camMgrAddr = ctx.r3.u32;
    __imp__sub_82398448(ctx, base);
    g_lastCamMgrAddr.store(camMgrAddr, std::memory_order_relaxed);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    const uint32_t sites = ttt2_side_camera_diag::GetRenderCameraSites();
    const bool stageCull = lr == ttt2_side_camera_diag::kStageMeshCullViewReturn &&
                           (sites & ttt2_side_camera_diag::kRenderSiteStageCullView) != 0u;
    const bool particle = lr == ttt2_side_camera_diag::kParticleBillboardViewReturn &&
                          (sites & ttt2_side_camera_diag::kRenderSiteParticleView) != 0u;
    if (lr == ttt2_side_camera_diag::kStageMeshCullViewReturn) {
        g_tlsStageCull = TlsStageCull{};
    }
    if (!stageCull && !particle) {
        return;
    }
    float view[16] = {};
    if (!ttt2_side_camera_diag::ComputeOppositeViewForRender(base, camMgrAddr, view)) {
        return;
    }
    const uint32_t shadow = RenderViewShadow();
    if (!ttt2_side_camera_diag::IsValidGuestStructRange(shadow, 64u)) {
        return;
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, &view[i], sizeof(bits));
        GWrite32(base, shadow + i * 4u, bits);
    }
    if (stageCull) {
        g_tlsStageCull.pending = true;
        g_tlsStageCull.originalViewAddr = ctx.r3.u32;
    }
    ctx.r3.u64 = shadow;
}

// Stage mesh sphere-vs-view test (sub_82295910 -> sub_822956C0), only called by sub_82525CD0.
PPC_FUNC(sub_82295910) {
    const TlsStageCull cull = g_tlsStageCull;
    g_tlsStageCull = TlsStageCull{};
    int originalResult = -1;
    if (cull.pending && ttt2_side_camera_diag::IsCaptureActiveFast()) {
        // Diagnostic only: the same pure test against the original view, on a scratch context.
        PPCContext original = ctx;
        original.r4.u64 = cull.originalViewAddr;
        __imp__sub_82295910(original, base);
        originalResult = static_cast<int>(original.r3.s32);
    }
    __imp__sub_82295910(ctx, base);
    ttt2_side_camera_diag::NotifyStageCullSample(cull.pending, originalResult, static_cast<int>(ctx.r3.s32));
}

// Render parameter block (sub_82528558 fills 0x82DE6AF0: target +48, eye +64). sub_82525CD0
// skips far meshes by distance from this eye; it now matches the rendered (opposite) eye.
PPC_FUNC(sub_82528558) {
    const uint32_t blockAddr = ctx.r3.u32;
    __imp__sub_82528558(ctx, base);
    if ((ttt2_side_camera_diag::GetRenderCameraSites() & ttt2_side_camera_diag::kRenderSiteParamBlockEye) == 0u ||
        blockAddr != ttt2_side_camera_diag::kRenderParamBlockAddr) {
        return;
    }
    const uint32_t camMgrAddr = g_lastCamMgrAddr.load(std::memory_order_relaxed);
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    if (camMgrAddr != 0u && ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap)) {
        ttt2_side_camera_diag::MirrorRenderParamBlockEye(base, blockAddr, snap);
    }
}

// Stage object fade camera block (sub_824FF570 fills 0x82DE69A0 from the active camera).
// Every stage mesh loop and mob fades/hides objects by front-of-camera, eye distance and facing
// tests on this block, so it follows the rendered (opposite) camera.
PPC_FUNC(sub_824FF570) {
    __imp__sub_824FF570(ctx, base);
    if (!RenderSiteEnabled(ttt2_side_camera_diag::kRenderSiteStageFadeBlock)) {
        return;
    }
    uint32_t camMgrAddr = ttt2_side_camera_diag::ResolveCameraManager(base);
    if (camMgrAddr == 0u) {
        camMgrAddr = g_lastCamMgrAddr.load(std::memory_order_relaxed);
    }
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    if (camMgrAddr != 0u && ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap) &&
        ttt2_side_camera_diag::MirrorStageFadeCameraBlock(base, ttt2_side_camera_diag::kStageFadeCameraBlockAddr, snap)) {
        ttt2_side_camera_diag::NotifyStageSideSubstitution(ttt2_side_camera_diag::kRenderSiteStageFadeBlock);
    }
}

// CameraManager getter (sub_82542C98 -> camSys+436). Mob, field grid, lens flare and light
// culling callers read activeCamInst inline; only those listed return addresses get a shadow
// camMgr whose active slot holds a copy of camInst with the opposite view/viewProj/eye.
PPC_FUNC(sub_82542C98) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    __imp__sub_82542C98(ctx, base);
    const uint32_t site = ttt2_side_camera_diag::RenderSiteForCamMgrReturn(lr);
    if (!RenderSiteEnabled(site)) {
        return;
    }
    uint32_t shadowMgr = 0;
    uint32_t shadowInst = 0;
    if (ShadowCameraBuffers(shadowMgr, shadowInst) &&
        ttt2_side_camera_diag::BuildShadowCameraManager(base, ctx.r3.u32, shadowMgr, shadowInst)) {
        ctx.r3.u64 = shadowMgr;
        ttt2_side_camera_diag::NotifyStageSideSubstitution(site);
    }
}

// Camera-system forward getter (sub_825424B8: r3 camSys, r4 out = normalize(target - eye)).
PPC_FUNC(sub_825424B8) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    const uint32_t camSysAddr = ctx.r3.u32;
    const uint32_t outAddr = ctx.r4.u32;
    __imp__sub_825424B8(ctx, base);
    if (!RenderSiteEnabled(ttt2_side_camera_diag::RenderSiteForCameraForwardReturn(lr))) {
        return;
    }
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    const uint32_t camMgrAddr = ttt2_side_camera_diag::CameraManagerFromSystem(base, camSysAddr);
    if (camMgrAddr != 0u && ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap)) {
        ttt2_side_camera_diag::MirrorCameraForwardOut(base, outAddr, snap);
    }
}

// Camera-system eye getter (sub_825425C8: r3 camSys, r4 out = eye xyz).
PPC_FUNC(sub_825425C8) {
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    const uint32_t camSysAddr = ctx.r3.u32;
    const uint32_t outAddr = ctx.r4.u32;
    __imp__sub_825425C8(ctx, base);
    if (!RenderSiteEnabled(ttt2_side_camera_diag::RenderSiteForCameraEyeReturn(lr))) {
        return;
    }
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    const uint32_t camMgrAddr = ttt2_side_camera_diag::CameraManagerFromSystem(base, camSysAddr);
    if (camMgrAddr != 0u && ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap)) {
        ttt2_side_camera_diag::MirrorCameraEyeOut(base, outAddr, snap);
    }
}

// CameraManager::GetActiveViewMatrix (sub_82547540 -> returns activeCamInst + 224)
PPC_FUNC(sub_82547540) {
    const uint32_t camMgrAddr = ctx.r3.u32;
    __imp__sub_82547540(ctx, base);
    g_lastCamMgrAddr.store(camMgrAddr, std::memory_order_relaxed);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr != 0x823C8BE0u && lr != 0x823C8CF0u) {
        return;
    }
    const uint32_t viewAddr = ctx.r3.u32;
    const uint32_t sceneCtxAddr = ctx.r31.u32;
    if (viewAddr < 224u || !ttt2_side_camera_diag::IsValidGuestStructRange(sceneCtxAddr, 1088u)) {
        return;
    }
    const uint32_t camInstAddr = viewAddr - 224u;
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    if (!ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap) ||
        snap.camInstAddr != camInstAddr) {
        return;
    }
    if (g_tlsSlot0.inSub823C8B30 && !g_tlsSlot0.hasValidSnapshot) {
        g_tlsSlot0.hasValidSnapshot = true;
        g_tlsSlot0.snap = snap;
        g_tlsSlot0.camInstHashBefore = ttt2_side_camera_diag::HashGuestBytesFNV1a(base, camInstAddr, 384u);
    }
    if (!ttt2_side_camera_diag::ShouldApplyOppositeToCamera(snap)) {
        return;
    }
    const uint32_t dstAddr = (lr == 0x823C8BE0u) ? (sceneCtxAddr + 624u) : (sceneCtxAddr + 960u);
    if (ttt2_side_camera_diag::TransformGuestCameraMatrix4x4(base, viewAddr, camInstAddr + 32u, dstAddr)) {
        ctx.r3.u64 = dstAddr;
        if (g_tlsSlot0.inSub823C8B30) {
            if (lr == 0x823C8BE0u) g_tlsSlot0.view0Transformed = true;
            else g_tlsSlot0.view1Transformed = true;
        }
    }
}

// CameraManager::GetActiveViewProjMatrix (sub_82547620 -> returns activeCamInst + 96 = Matrix B)
PPC_FUNC(sub_82547620) {
    const uint32_t camMgrAddr = ctx.r3.u32;
    __imp__sub_82547620(ctx, base);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr != 0x823C8C2Cu) {
        return;
    }
    const uint32_t viewProjAddr = ctx.r3.u32;
    const uint32_t sceneCtxAddr = ctx.r31.u32;
    if (viewProjAddr < 96u || !ttt2_side_camera_diag::IsValidGuestStructRange(sceneCtxAddr, 1088u)) {
        return;
    }
    const uint32_t camInstAddr = viewProjAddr - 96u;
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    if (!ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap) ||
        snap.camInstAddr != camInstAddr) {
        return;
    }
    if (g_tlsSlot0.inSub823C8B30 && !g_tlsSlot0.hasValidSnapshot) {
        g_tlsSlot0.hasValidSnapshot = true;
        g_tlsSlot0.snap = snap;
        g_tlsSlot0.camInstHashBefore = ttt2_side_camera_diag::HashGuestBytesFNV1a(base, camInstAddr, 384u);
    }
    if (!ttt2_side_camera_diag::ShouldApplyOppositeToCamera(snap)) {
        return;
    }
    const uint32_t dstAddr = sceneCtxAddr + 752u;
    if (ttt2_side_camera_diag::TransformGuestCameraMatrix4x4(base, viewProjAddr, camInstAddr + 32u, dstAddr)) {
        ctx.r3.u64 = dstAddr;
        if (g_tlsSlot0.inSub823C8B30) {
            g_tlsSlot0.viewProjTransformed = true;
        }
    }
}

// CameraManager::GetActiveEyePositionVec4 (sub_82546B58 -> writes c40 (ex, ey, ez, 1.0f) to r4)
PPC_FUNC(sub_82546B58) {
    const uint32_t camMgrAddr = ctx.r3.u32;
    const uint32_t outVec4Addr = ctx.r4.u32;
    __imp__sub_82546B58(ctx, base);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr != 0x823C8CE8u || !ttt2_side_camera_diag::IsValidGuestStructRange(outVec4Addr, 16u)) {
        return;
    }
    ttt2_side_camera_diag::CameraProducerSnapshot snap{};
    if (!ttt2_side_camera_diag::ReadCameraProducerSnapshot(base, camMgrAddr, snap)) {
        return;
    }
    if (!ttt2_side_camera_diag::ShouldApplyOppositeToCamera(snap)) {
        return;
    }
    if (ttt2_side_camera_diag::TransformGuestCameraEye4(base, snap.camInstAddr + 32u, outVec4Addr)) {
        if (g_tlsSlot0.inSub823C8B30) {
            g_tlsSlot0.eyeTransformed = true;
        }
    }
}
// ---- Phase 9 HUD: each side of the HUD shows the team standing on that side -------------------
// Battle2D (task vtable 0x820508E0) draws every per-team element from fighters[index] and the
// team profile Array<Profile,2>[index] (sub_82653478) while the screen position comes from the
// same index. Only these element functions run with the swapped view, so positions, bar
// direction and layout stay where the game puts them and only the data moves.
#define TTT2_HUD_SWAP_HOOK(addr)     PPC_FUNC(sub_##addr) { ScopedHudTeamSwap swap(base); __imp__sub_##addr(ctx, base); }
TTT2_HUD_SWAP_HOOK(823D4058)   // gauge draw (four gauges, via sub_823D4690)
TTT2_HUD_SWAP_HOOK(823D4B08)   // gauge update (HP, recoverable, tag swap of gauge+8)
TTT2_HUD_SWAP_HOOK(823D4698)   // gauge reset from battle logic (sub_823D59C0 <- sub_822CDD98)
TTT2_HUD_SWAP_HOOK(823D55A8)   // per-team portrait / character name
TTT2_HUD_SWAP_HOOK(823D56D0)   // round win marks (sub_82395638 wins of fighters[0] / [1])
TTT2_HUD_SWAP_HOOK(823D6738)   // per-team panel (profile, rank)
TTT2_HUD_SWAP_HOOK(823D6A88)   // per-team panel (PairFighterCS)
TTT2_HUD_SWAP_HOOK(823D5F28)   // per-team status
TTT2_HUD_SWAP_HOOK(823D5BF0)   // per-team helper (called by the panels)
TTT2_HUD_SWAP_HOOK(823D5860)   // profile-indexed text
TTT2_HUD_SWAP_HOOK(823D5A10)   // profile-indexed text
#undef TTT2_HUD_SWAP_HOOK

// Team profile Array<Profile,2>::operator[](i): inside a swapped HUD element (or the swapped online
// character select UI) the team index follows the fighters / select states. Everywhere else it is
// untouched.
// ---- Phase 12-D online character select: the local team on the chosen side ----------------------
// The Lumen character select controller (vtable 0x820DBE10, constructed by sub_82643770 with the
// select data at +144 / +148) only displays: update (sub_82644588, this) and draw (sub_82641308,
// this + 4) read each side's select state through data +48 (side 0) / +56 (side 1) and the side's
// profile (sub_82653478) and draw it on that side's p1 / p2 elements. While the screen is to be
// swapped, both run with the two states (and profiles) traded, so the game itself draws the local
// team in the chosen side's elements. Nothing else reads the trade: it is restored on return.
// Phase 12-D3: the TAG / SOLO panel (one object per side, side index at +52, names solo_tag_p1 /
// tag_icon_p1_1 ... from table 0x82E0CE20) runs as its own Lumen states sub_8263D038 / sub_8263D698
// (they set each other; no pad reads, Lumen helpers only) and reads the same per-side select states
// through *(this + 48) + 152. Each select data object is traded at most once at a time per thread.
thread_local uint32_t t_charSelSwappedData[4] = {};
thread_local int t_charSelSwappedCount = 0;
// Phase 12-D7: the select UI shows a side's browsing (hover portrait, frame) only when that side's
// bit (side 0 = 1, side 1 = 2; sub_82643A08 loc_82643D78) is set in the local-human mask
// *(*(*(0x82E6C5A8) + 240) + 84) - online only the local slot's bit. MEASURED (diag log, charsel6):
// the native client toggles chara_face_yoko_main_p1 / p1_chara_waku_main while browsing, the traded
// one never did. While any select data is traded, bits 0 / 1 are traded too.
constexpr uint32_t kSelectModeHolderAddr = 0x82E6C5A8u;   // lis -32025, -14936
static uint32_t SelectHumanMaskAddr(uint8_t* base) {
    if (!ttt2_side_camera_diag::IsValidGuestStructRange(kSelectModeHolderAddr, 4u)) return 0u;
    const uint32_t holder = GRead32(base, kSelectModeHolderAddr);
    if (!ttt2_side_camera_diag::IsValidGuestStructRange(holder, 244u)) return 0u;
    const uint32_t obj = GRead32(base, holder + 240u);
    return ttt2_side_camera_diag::IsValidGuestStructRange(obj, 88u) ? obj + 84u : 0u;
}
static uint32_t TradeLowSideBits(uint32_t v) { return (v & ~3u) | ((v & 1u) << 1) | ((v >> 1) & 1u); }
thread_local uint32_t t_charSelMaskAddr = 0;
struct ScopedCharaSelectSwap {
    uint8_t* base;
    uint32_t data = 0;
    uint32_t side0 = 0, side1 = 0;
    ScopedCharaSelectSwap(uint8_t* b, uint32_t dataAddr) : base(b) {
        if (!ttt2_side_camera_diag::IsValidGuestStructRange(dataAddr, 64u) || t_charSelSwappedCount >= 4) return;
        for (int i = 0; i < t_charSelSwappedCount; ++i)
            if (t_charSelSwappedData[i] == dataAddr) return;
        if (!ttt2_side_camera_diag::CharaSelectSideSwapActive(base)) return;
        data = dataAddr;
        side0 = GRead32(base, data + 48u);
        side1 = GRead32(base, data + 56u);
        GWrite32(base, data + 48u, side1);
        GWrite32(base, data + 56u, side0);
        if (t_charSelSwappedCount == 0) {
            t_charSelMaskAddr = SelectHumanMaskAddr(base);
            if (t_charSelMaskAddr) GWrite32(base, t_charSelMaskAddr, TradeLowSideBits(GRead32(base, t_charSelMaskAddr)));
        }
        t_charSelSwappedData[t_charSelSwappedCount++] = data;
    }
    ~ScopedCharaSelectSwap() {
        if (data == 0u) return;
        if (--t_charSelSwappedCount == 0 && t_charSelMaskAddr) {
            GWrite32(base, t_charSelMaskAddr, TradeLowSideBits(GRead32(base, t_charSelMaskAddr)));
            t_charSelMaskAddr = 0;
        }
        if (GRead32(base, data + 48u) != side1 || GRead32(base, data + 56u) != side0) {
            std::fprintf(stderr, "[side-charsel] select states changed during the UI; left as the game wrote them\n");
            return;
        }
        GWrite32(base, data + 48u, side0);
        GWrite32(base, data + 56u, side1);
    }
};
static uint32_t CharaSelectDataAt(uint8_t* base, uint32_t holder, uint32_t offset) {
    return ttt2_side_camera_diag::IsValidGuestStructRange(holder, offset + 4u) ? GRead32(base, holder + offset) : 0u;
}
PPC_FUNC(sub_82644588) {
    ScopedCharaSelectSwap swap(base, CharaSelectDataAt(base, ctx.r3.u32, 148u));
    __imp__sub_82644588(ctx, base);
}
PPC_FUNC(sub_82641308) {
    ScopedCharaSelectSwap swap(base, CharaSelectDataAt(base, ctx.r3.u32 - 4u, 148u));
    __imp__sub_82641308(ctx, base);
}
#define TTT2_SOLOTAG_SWAP_HOOK(addr)                                                                   \
    PPC_FUNC(sub_##addr) {                                                                              \
        ScopedCharaSelectSwap swap(base, CharaSelectDataAt(base, CharaSelectDataAt(base, ctx.r3.u32, 48u), 152u)); \
        __imp__sub_##addr(ctx, base);                                                                   \
    }
TTT2_SOLOTAG_SWAP_HOOK(8263D038)   // TAG / SOLO panel state (selecting)
TTT2_SOLOTAG_SWAP_HOOK(8263D698)   // TAG / SOLO panel state (shown / hidden)
#undef TTT2_SOLOTAG_SWAP_HOOK
// Phase 12-D5: every other reader of the per-side select states (static scan of 0x82600000..
// 0x82650000 for data +148 / +152 -> +48 / +56; user report: the grid cursor vanished after TAG /
// SOLO, portraits and names mixed). The owner's side getters (r3 = owner, data at +152, r4 = side):
// state 2 / 32 (sub_8263CF88), +420 (sub_8263CFD8), +424 (sub_8263D008); and sub_82644C98 (r4 = the
// select controller, data at +148, r5 = side).
#define TTT2_SELECT_GETTER_SWAP_HOOK(addr)                                                             \
    PPC_FUNC(sub_##addr) {                                                                              \
        ScopedCharaSelectSwap swap(base, CharaSelectDataAt(base, ctx.r3.u32, 152u));                    \
        __imp__sub_##addr(ctx, base);                                                                   \
    }
TTT2_SELECT_GETTER_SWAP_HOOK(8263CF88)
TTT2_SELECT_GETTER_SWAP_HOOK(8263CFD8)
TTT2_SELECT_GETTER_SWAP_HOOK(8263D008)
#undef TTT2_SELECT_GETTER_SWAP_HOOK
PPC_FUNC(sub_82644C98) {
    ScopedCharaSelectSwap swap(base, CharaSelectDataAt(base, ctx.r4.u32, 148u));
    __imp__sub_82644C98(ctx, base);
}

// Phase 12-D6: PadManager logical id -> display pad id (sub_8228DA18(padMgr, id): id 9..12 ->
// table[id - 9] + 9). The select UI (sub_82642AE8, in the controller's update) asks it for side 0
// with id 9 and side 1 with id 10; inside a traded UI call the id follows the traded side.
PPC_FUNC(sub_8228DA18) {
    if (t_charSelSwappedCount > 0 && ctx.r4.u32 >= 9u && ctx.r4.u32 <= 12u) {
        ctx.r4.u64 = ((ctx.r4.u32 - 9u) ^ 1u) + 9u;
    }
    __imp__sub_8228DA18(ctx, base);
}

// Phase 12-D9 diagnostics (name plates): every text set (sub_82B62BE8(handle, text)) and every
// element lookup by a name containing "name" (sub_82B62B30(out, parent, name)) while an online
// side choice is active outside a battle mode, with the caller's return address; deduplicated.
static bool NamePlateDiagActive(uint8_t* base) {
    return ttt2_side_camera_diag::IsOnlineSidePolicyActive() &&
           !ttt2_side_camera_diag::IsBattleGameMode(ttt2_side_camera_diag::CurrentGameMode(base));
}
static void NamePlateDiagLog(const char* what, uint8_t* base, uint32_t textAddr, uint32_t lr) {
    if (textAddr < 0x10000u || textAddr >= 0xFFFF0000u) return;
    char text[48];
    uint32_t n = 0;
    while (n + 1u < sizeof(text)) {
        const char c = char(base[textAddr + n]);
        if (c == '\0') break;
        text[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    text[n] = '\0';
    static std::mutex m;
    static std::map<std::string, int> seen;
    static int logged = 0;
    char key[96];
    std::snprintf(key, sizeof(key), "%s|%08X|%s", what, lr, text);
    std::lock_guard<std::mutex> lock(m);
    if (logged >= 400 || !seen.emplace(key, 1).second) return;
    ++logged;
    std::fprintf(stderr, "[side-charsel-diag] %s \"%s\" from %08X (mode %u, traded %d)\n", what, text, lr,
                 ttt2_side_camera_diag::CurrentGameMode(base), t_charSelSwappedCount);
}
PPC_FUNC(sub_82B62BE8) {
    if (NamePlateDiagActive(base)) NamePlateDiagLog("text", base, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
    __imp__sub_82B62BE8(ctx, base);
}
PPC_FUNC(sub_82B62B30) {
    if (NamePlateDiagActive(base) && ctx.r5.u32 >= 0x10000u && ctx.r5.u32 < 0xFFFF0000u) {
        char probe[64];
        uint32_t n = 0;
        while (n + 1u < sizeof(probe) && (probe[n] = char(base[ctx.r5.u32 + n])) != '\0') ++n;
        probe[n] = '\0';
        if (std::strstr(probe, "name") || std::strstr(probe, "NAME")) NamePlateDiagLog("lookup", base, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
    }
    __imp__sub_82B62B30(ctx, base);
}

// Phase 12-D6 diagnostics: a per-side element's visibility as the select UI sets it
// (sub_82627040(lumen, name, visible)) on an online character select, both clients, on change.
PPC_FUNC(sub_82627040) {
    const uint32_t nameAddr = ctx.r4.u32;
    const uint32_t value = ctx.r5.u32;
    if (nameAddr >= 0x10000u && nameAddr < 0xFFFF0000u && ttt2_side_camera_diag::CharaSelectOnlineDiagActive(base)) {
        char name[64];
        uint32_t n = 0;
        while (n + 1u < sizeof(name) && (name[n] = char(base[nameAddr + n])) != '\0') ++n;
        name[n] = '\0';
        if (std::strstr(name, "p1") || std::strstr(name, "p2") || std::strstr(name, "P1") || std::strstr(name, "P2")) {
            static std::mutex m;
            static std::map<std::string, uint32_t> last;
            static int logged = 0;
            std::lock_guard<std::mutex> lock(m);
            auto it = last.find(name);
            if ((it == last.end() || it->second != value) && logged < 600) {
                last[name] = value;
                ++logged;
                std::fprintf(stderr, "[side-charsel-diag] visible %s = %u (traded %d)\n", name, value, t_charSelSwappedCount);
            }
        }
    }
    __imp__sub_82627040(ctx, base);
}

// Phase 12-D4: the name plates above each side (PLAYER 2 / gamertag) come from the script binding
// sub_8256FE28(out, side, member, kind) -> the match player table entry of `side` -> the label
// (sub_824165C8). On the swapped character select the side follows the traded select states.
PPC_FUNC(sub_8256FE28) {
    const uint32_t side = ctx.r4.u32, member = ctx.r5.u32, kind = ctx.r6.u32;
    uint32_t mode = 0;
    int team = -1;
    const bool swap = side <= 1u && ttt2_side_camera_diag::OnlineNamePlateSwapWanted(base, &mode, &team);
    if (swap) ctx.r4.u64 = side ^ 1u;
    static std::atomic<int> logged{0};
    if ((ttt2_side_camera_diag::IsOnlineSidePolicyActive() || swap) && logged.fetch_add(1, std::memory_order_relaxed) < 40)
        std::fprintf(stderr, "[side-charsel] name plate side %u (member %u, kind %u, mode %u, local team %d) -> side %u\n", side,
                     member, kind, mode, team, swap ? side ^ 1u : side);
    __imp__sub_8256FE28(ctx, base);
}

PPC_FUNC(sub_82653478) {
    if ((t_hudSwapped || t_charSelSwappedCount > 0) && ctx.r4.u32 <= 1u) {
        ctx.r4.u64 = ctx.r4.u32 ^ 1u;
    }
    __imp__sub_82653478(ctx, base);
}

// Lumen round win lights: sub_82632148(team = fighter+7936, wins) -> sub_826316E8 (p1 / p2 clip).
PPC_FUNC(sub_82632148) {
    ctx.r3.u64 = ttt2_side_camera_diag::HudTeamForDisplay(
        ctx.r3.u32, ttt2_side_camera_diag::ShouldSwapHudTeams(XSessionAnyActive()));
    __imp__sub_82632148(ctx, base);
}
#endif
