#pragma once
// Opt-in, bounded PM4 camera-constant provenance and local independent 3D camera side control.
#include <cstdint>

namespace ttt2_side_camera_diag {

enum class LocalCameraSide : uint32_t {
    Default = 0,
    Opposite = 1,
};

enum class LocalInputMode : uint32_t {
    Original = 0,
    CameraRelative = 1,
};

// Where CAMERA_RELATIVE input is applied (TTT2_CAMERA_INPUT_STRATEGY).
// SideLatch (Phase 8, default): the raw pad is untouched; the local fighter's latched input side
//   (fighter+1028) is read as the opposite side by the game's own consumers that interpret input
//   (sub_82303BB0 -> sub_82303400 L/R table, record/playback, history; requirements 144..149 that pick
//   the sidestep direction). Up/down/jump/crouch bits are never rewritten.
// XInputMirror (Phase 6 legacy): D-pad LEFT/RIGHT and LX mirrored in XamInputGetState only.
enum class LocalInputStrategy : uint32_t {
    SideLatch = 0,
    XInputMirror = 1,
};

constexpr uint16_t kXInputDpadUp     = 0x0001u;
constexpr uint16_t kXInputDpadDown   = 0x0002u;
constexpr uint16_t kXInputDpadLeft   = 0x0004u;
constexpr uint16_t kXInputDpadRight  = 0x0008u;
constexpr uint16_t kXInputDpadLRMask = kXInputDpadLeft | kXInputDpadRight;

struct LocalInputGateContext {
    LocalCameraSide cameraSide = LocalCameraSide::Default;
    LocalInputMode inputMode = LocalInputMode::Original;
    uint32_t targetUserIndex = 0;
    uint32_t localCameraUserIndex = 0;
    bool oppositeCameraActiveInBattle = false;
    bool inActiveUnpausedBattle = false;
    bool battleOverlayInhibited = false;
    bool sysUiOrMenuActive = false;
    bool onlineSessionActive = false;
    bool allowOnlineInputOverride = false;
    // Exactly one live strategy is enabled by BuildLiveInputGateContext, so the raw-pad mirror and
    // the side latch can never both convert the same input.
    bool rawPadMirrorEnabled = true;
    bool sideLatchEnabled = false;
};

// Phase 8-A guest layout (static analysis of the 0.1.7 image, see the Phase 8 report).
// sub_82542D18 writes fighter+1024 (and +1020) from the ACTIVE camera eye every poll;
// sub_823038E0 latches +1024 into +1028, which input interpretation reads.
constexpr uint32_t kFighterCameraSideOffset = 1024u;
constexpr uint32_t kFighterInputSideOffset = 1028u;
// sub_823069C8 / sub_823069E0: *(0x82ECE840) == 1 records, == 2 plays back (side restored from data).
constexpr uint32_t kInputRecordStateAddr = 0x82ECE840u;
constexpr uint32_t kInputRecordStatePlayback = 2u;
// sub_8230DAB8 requirement ids whose cases read fighter+1028 (1P/2P side, with/without +196 facing).
constexpr uint32_t kSideRequirementFirst = 144u;
constexpr uint32_t kSideRequirementLast = 149u;
// sub_8228DC40(padMgr, logicalId): record index = *(padMgr+84)[logicalId-9]; records 0..3 are built
// for XAM users 0..3 (sub_8228E870), record 4 (id 13) for the user at padMgr+848.
constexpr uint32_t kPadLookupReturnInPadButtons = 0x82274664u;

struct SideLatchDiagRecord {
    uint64_t timestampMs = 0;
    uint32_t fighterAddr = 0;
    uint32_t ownerUser = 0xFFFFFFFFu;
    uint32_t cameraSide = 0;
    uint32_t inputSide = 0;
    bool applied = false;
    bool playback = false;
};

// Phase 8-B: render-only consumers of the ORIGINAL camera.
// sub_82525CD0 (stage mesh submit) culls each mesh with sub_82295910(sphere, sub_82398448(camMgr))
// (= activeCamInst+224 view, x camInst+160 projection set by sub_82547DE0) and a distance test
// from the eye in the render parameter block filled by sub_82528558.
constexpr uint32_t kStageMeshCullViewReturn = 0x82525FFCu;
constexpr uint32_t kParticleBillboardViewReturn = 0x823E2440u;
constexpr uint32_t kRenderParamBlockAddr = 0x82DE6AF0u;   // lis -32034, addi 27376
constexpr uint32_t kRenderParamTargetOffset = 48u;        // sub_825426C0: target (x,y,z,1)
constexpr uint32_t kRenderParamEyeOffset = 64u;           // sub_82542618: eye (x,y,z,1)
// Phase 8 stage fix: stage object fade camera block filled by sub_824FF570 every frame
// (callers sub_82422918, sub_8226E488): +0 forward (x,y,z,0), +16 eye (x,y,z,1), +32 target,
// +48 fov term, +52 |eye-target|, +64.. 4 fighter positions. Only the fade module
// sub_824FF0B0..sub_824FFFE0 reads it (stage mesh loops sub_825269D0/sub_82524578/sub_825250A8
// and mobs via sub_825ADCB8): front-of-camera occluder fade, eye-distance fade, facing fade.
constexpr uint32_t kStageFadeCameraBlockAddr = 0x82DE69A0u;  // lis -32034, addi 27040
constexpr uint32_t kStageFadeForwardOffset = 0u;
constexpr uint32_t kStageFadeEyeOffset = 16u;
constexpr uint32_t kStageFadeTargetOffset = 32u;
// sub_822569E0 returns this singleton; sub_82565678 -> *(p)+724 = camera system; +436 = camMgr.
constexpr uint32_t kCameraSystemHolderAddr = 0x82E6B05Cu;
// sub_82542C98 (camMgr getter) return addresses whose callers read activeCamInst inline
// (view +224, viewProj +96) only to decide what to draw.
constexpr uint32_t kMobVisibleReturn = 0x825AC8F8u;         // sub_825AC8B8: sphere vs view -> +2370
constexpr uint32_t kMobDepthReturn = 0x825ACF04u;           // sub_825ACEF0: view depth -> +2272
constexpr uint32_t kMobFighterDepthMinReturn = 0x825B1DC4u; // sub_825B1DB0: fighters' view depth
constexpr uint32_t kMobFighterDepthMaxReturn = 0x825B1EDCu; // sub_825B1EC8
constexpr uint32_t kFieldGridViewReturn = 0x82522914u;      // sub_825228B8: cascade frusta from view
constexpr uint32_t kFieldGridView2Return = 0x82523C0Cu;     // sub_825239D0 -> sub_82271358 view copy
constexpr uint32_t kLensFlareViewProjReturn = 0x824F5E44u;  // sub_824F5DD8 -> sub_82422A98 (+96)
constexpr uint32_t kLightCullViewReturn = 0x8239B77Cu;      // sub_8239B230: light sphere vs view
// Camera-system vector getters (sub_825424B8 forward, sub_825425C8 eye) return addresses.
constexpr uint32_t kFieldGridForwardReturn = 0x825217E0u;   // sub_825212E8
constexpr uint32_t kFieldGridEyeReturn = 0x8252181Cu;
constexpr uint32_t kLensFlareEyeReturn = 0x824F5F24u;       // sub_824F5DD8
constexpr uint32_t kLensFlareForwardReturn = 0x824F5F34u;
// Shadow camera copies handed to the callers above (camMgr bytes / camInst bytes).
constexpr uint32_t kShadowCamMgrBytes = 400u;
constexpr uint32_t kShadowCamInstBytes = 384u;
constexpr uint32_t kCamMgrActiveSlotOffset = 316u;
constexpr uint32_t kCamMgrSlotArrayOffset = 340u;
// TTT2_RENDER_CAMERA_SITES bit mask.
constexpr uint32_t kRenderSiteStageCullView = 1u << 0;
constexpr uint32_t kRenderSiteParamBlockEye = 1u << 1;
constexpr uint32_t kRenderSiteParticleView = 1u << 2;
constexpr uint32_t kRenderSiteStageFadeBlock = 1u << 3;
constexpr uint32_t kRenderSiteMobCamera = 1u << 4;
constexpr uint32_t kRenderSiteFieldGrid = 1u << 5;
constexpr uint32_t kRenderSiteLensFlare = 1u << 6;
constexpr uint32_t kRenderSiteLightCull = 1u << 7;
constexpr uint32_t kRenderCameraSitesMask = 0xFFu;
// Particle billboards and dynamic light culling also feed character visuals; opt-in only.
constexpr uint32_t kDefaultRenderCameraSites =
    kRenderSiteStageCullView | kRenderSiteParamBlockEye | kRenderSiteStageFadeBlock |
    kRenderSiteMobCamera | kRenderSiteFieldGrid | kRenderSiteLensFlare;

struct LocalInputDiagRecord {
    uint64_t timestampMs = 0;
    uint32_t packetNumber = 0;
    uint32_t localUser = 0;
    LocalCameraSide cameraSide = LocalCameraSide::Default;
    LocalInputMode inputMode = LocalInputMode::Original;
    uint16_t rawButtons = 0;
    uint16_t effectiveButtons = 0;
    int16_t rawStickX = 0;
    int16_t effectiveStickX = 0;
    bool inBattle = false;
    bool transformApplied = false;
};

// Valid guest structure address bounds for TTT2 (excludes NULL/low page and GPU MMIO/unmapped ranges).
constexpr uint32_t kMinValidGuestAddr = 0x00100000u;
constexpr uint32_t kMaxValidGuestAddr = 0xDFFFFFFFu;

// Default bitmask of camera slots (0..11) allowed for OPPOSITE transform:
// Bit 1 (Slot 1 = 2P Battle Camera) through Bit 11 enabled; Bit 0 (Slot 0 = Base fallback) disabled.
// Phase 12-D: the staged cameras keep the original view - slots 2 / 3 (sequence / motion script:
// intro and win poses, user report 2026-10-26: backs shown under OPPOSITE) and 6 (stage staging).
// TTT2_CAMERA_SLOT_MASK=0x0FFE restores the Phase 5..12 mask.
constexpr uint32_t kStagedCameraSlotMask = (1u << 2) | (1u << 3) | (1u << 6);
constexpr uint32_t kDefaultAllowedCameraSlotMask = 0x0FFEu & ~kStagedCameraSlotMask;
// Logs the active camera slot when it changes while OPPOSITE is requested (bounded).
void NoteActiveCameraSlot(uint32_t slot, bool applied, uint32_t mode);

struct CameraProducerSnapshot {
    uint32_t camMgrAddr = 0;
    uint32_t activeSlot = 0;
    uint32_t camInstAddr = 0;
    uint32_t camVtable = 0;
    uint32_t viewAddr = 0;      // camInstAddr + 224 (0xE0)
    uint32_t projAddr = 0;      // camInstAddr + 160 (0xA0)
    uint32_t viewProjAddr = 0;  // camInstAddr + 96  (0x60) -> GPU Matrix B
    float eye[3] = {};          // camInstAddr + 16  (0x10)
    float target[3] = {};       // camInstAddr + 32  (0x20) -> Fighter midpoint pivot
    float up[3] = {};           // camInstAddr + 48  (0x30)
    float zNear = 0.0f;         // camInstAddr + 64  (0x40)
    float zFar = 0.0f;          // camInstAddr + 68  (0x44)
    float fovY = 0.0f;          // camInstAddr + 72  (0x48)
    LocalCameraSide appliedSide = LocalCameraSide::Default;
    bool slotAllowed = false;
    bool geometryValid = false;
};

struct CameraFrameDiagRecord {
    uint64_t timestampMs = 0;
    uint32_t camMgrAddr = 0;
    uint32_t activeSlot = 0;
    uint32_t camInstAddr = 0;
    uint32_t camVtable = 0;
    LocalCameraSide cameraSide = LocalCameraSide::Default;
    bool slotAllowed = false;
    bool transformApplied = false;
    bool camInstUnchanged = true;
    float cameraEyeOriginal[4] = {};
    float cameraEyeRendered[4] = {};
    float cameraTarget[3] = {};
    float cameraUp[3] = {};
    float zNear = 0.0f;
    float zFar = 0.0f;
    float fovY = 0.0f;
    float viewMatrixOriginal[16] = {};
    float viewMatrixRendered[16] = {};
    float viewProjOriginal[16] = {};
    float viewProjRendered[16] = {};
};

LocalCameraSide ParseLocalCameraSide(const char* value);
LocalCameraSide GetLocalCameraSide();
void SetLocalCameraSide(LocalCameraSide side);
LocalCameraSide ReloadLocalCameraSideFromEnv();
const char* LocalCameraSideName(LocalCameraSide side);

LocalInputMode ParseLocalInputMode(const char* value);
LocalInputMode GetLocalInputMode();
void SetLocalInputMode(LocalInputMode mode);
LocalInputMode ReloadLocalInputModeFromEnv();
const char* LocalInputModeName(LocalInputMode mode);

LocalInputStrategy ParseLocalInputStrategy(const char* value);
LocalInputStrategy GetLocalInputStrategy();
void SetLocalInputStrategy(LocalInputStrategy strategy);
LocalInputStrategy ReloadLocalInputStrategyFromEnv();
const char* LocalInputStrategyName(LocalInputStrategy strategy);

uint32_t GetRenderCameraSites();
void SetRenderCameraSites(uint32_t mask);
uint32_t ReloadRenderCameraSitesFromEnv();

uint32_t GetLocalCameraUserIndex();
void SetLocalCameraUserIndex(uint32_t userIndex);
uint32_t ReloadLocalCameraUserIndexFromEnv();

bool IsOnlineInputOverrideAllowed();
void SetOnlineInputOverrideAllowed(bool allowed);
bool ReloadOnlineInputOverrideAllowedFromEnv();

uint32_t GetAllowedCameraSlotMask();
void SetAllowedCameraSlotMask(uint32_t mask);
uint32_t ReloadAllowedCameraSlotMaskFromEnv();

bool IsValidGuestStructRange(uint32_t guestAddr, uint32_t byteSize);
uint64_t HashGuestBytesFNV1a(const uint8_t* base, uint32_t guestAddr, uint32_t byteSize);

// Pure 3D camera 180-degree world-Y pivot rotation around pivot = (px, py, pz).
// Left-multiplies row-vector 4x4 View or View-Projection matrix by M_pivot:
//   Row 0 -> -Row 0
//   Row 1 -> +Row 1
//   Row 2 -> -Row 2
//   Row 3 ->  Row 3 + 2*px*Row 0 + 2*pz*Row 2
bool ApplyOppositeCameraMatrix4x4(const float inMatrix[16], const float pivot[3], float outMatrix[16]);
bool ApplyOppositeCameraEye(const float inEye[4], const float pivot[3], float outEye[4]);

// Camera-relative directional input gating & transformation (Phase 6).
bool ShouldApplyCameraRelativeInputGate(const LocalInputGateContext& gate);
bool ApplyCameraRelativePadTransform(
    uint16_t rawButtons,
    int16_t rawThumbLX,
    bool shouldTransform,
    uint16_t& outEffectiveButtons,
    int16_t& outEffectiveThumbLX);
bool TransformPadStateWithGate(
    const LocalInputGateContext& gate,
    uint32_t& inOutPacketNumber,
    uint16_t& inOutButtons,
    int16_t& inOutThumbLX,
    LocalInputDiagRecord* outDiag = nullptr);

void NotifyOppositeCameraFrameState(bool oppositeTransformApplied);
void NotifyBattleSimulationTick(bool unpaused);
void NotifyFighterInputPoll(bool overlayInhibited);
bool IsGuestBattlePausedOrOverlayActive(const uint8_t* base);
LocalInputGateContext BuildLiveInputGateContext(
    const uint8_t* base,
    uint32_t targetUserIndex,
    bool sysUiOrMenuActive,
    bool onlineSessionActive);
bool TransformPadStateForLocalCamera(
    const uint8_t* base,
    uint32_t targetUserIndex,
    bool sysUiOrMenuActive,
    bool onlineSessionActive,
    uint32_t& inOutPacketNumber,
    uint16_t& inOutButtons,
    int16_t& inOutThumbLX);

// Phase 8-A side latch. All guest accesses are bounds-checked; base may be a fake guest image.
bool ShouldApplySideLatchGate(const LocalInputGateContext& gate, bool playbackActive);
uint32_t ResolvePadOwnerUser(const uint8_t* base, uint32_t padMgrAddr, uint32_t logicalId);
void OnFighterInputPollBegin(uint32_t fighterAddr);
void OnFighterPadLookup(const uint8_t* base, uint32_t returnAddr, uint32_t padMgrAddr, uint32_t logicalId);
// After sub_823038E0 latched +1028. Returns true when +1028 now holds the opposite (virtual) side.
bool OnFighterInputPollEnd(
    uint8_t* base,
    uint32_t fighterAddr,
    bool skipped,
    bool sysUiOrMenuActive,
    bool onlineSessionActive,
    SideLatchDiagRecord* outDiag = nullptr);
// After sub_82303BB0: put the camera side back for every non-input consumer.
void OnFighterInputProcessEnd(uint8_t* base, uint32_t fighterAddr);
// Around sub_8230DAB8(req, fighter, opponent): side requirements see the virtual side.
bool BeginSideRequirementEval(uint8_t* base, uint32_t reqAddr, uint32_t fighterAddr, uint32_t& savedSide);
void EndSideRequirementEval(uint8_t* base, uint32_t fighterAddr, uint32_t savedSide);
bool IsSideLatchActiveForFighter(uint32_t fighterAddr);

// Phase 8-B render-only camera data. camInst is only read.
bool ComputeOppositeViewForRender(const uint8_t* base, uint32_t camMgrAddr, float outView[16]);
bool MirrorRenderParamBlockEye(uint8_t* base, uint32_t blockAddr, const CameraProducerSnapshot& snap);
void NotifyStageCullSample(bool substituted, int originalResult, int renderedResult);

// Phase 8 stage fix (render-only, camInst/camMgr are only read).
// Rewrites forward/eye of the stage fade block when it was filled from snap's camera this frame.
bool MirrorStageFadeCameraBlock(uint8_t* base, uint32_t blockAddr, const CameraProducerSnapshot& snap);
// Rewrites a 3-float eye / unit-forward written by a camera-system getter, if it matches snap.
bool MirrorCameraEyeOut(uint8_t* base, uint32_t outAddr, const CameraProducerSnapshot& snap);
bool MirrorCameraForwardOut(uint8_t* base, uint32_t outAddr, const CameraProducerSnapshot& snap);
// Copies camMgr/active camInst into the shadow buffers with the opposite view/viewProj/eye and
// points the shadow slot entry at the shadow camInst. Returns false (shadow unused) otherwise.
bool BuildShadowCameraManager(uint8_t* base, uint32_t camMgrAddr, uint32_t shadowMgrAddr, uint32_t shadowInstAddr);
// Site bit for a sub_82542C98 / camera-vector getter return address, 0 when not a stage consumer.
uint32_t RenderSiteForCamMgrReturn(uint32_t returnAddr);
uint32_t RenderSiteForCameraEyeReturn(uint32_t returnAddr);
uint32_t RenderSiteForCameraForwardReturn(uint32_t returnAddr);
// camMgr resolved the same way sub_82542C98 does, 0 when any link is invalid.
uint32_t ResolveCameraManager(const uint8_t* base);
uint32_t CameraManagerFromSystem(const uint8_t* base, uint32_t camSysAddr);
void NotifyStageSideSubstitution(uint32_t siteBit);

// ---- Phase 9: player side selected before character select -----------------------------------
// The menu stores a screen-side preference and the XAM user who confirmed it. Nothing is assumed
// about slots before battle: the first battle poll whose pad lookup resolves to that user decides
// DEFAULT/OPPOSITE from that fighter's internal camera side (+1024; live-observed 0 = RIGHT
// on screen, 1 = LEFT), and the decision is held for the battle.
// While a preference is active it replaces LOCAL_CAMERA_SIDE / LOCAL_INPUT_MODE / LOCAL_CAMERA_USER.
enum class PlayerScreenSide : uint32_t { Left = 0, Right = 1 };
struct SidePolicySnapshot {
    bool active = false;
    PlayerScreenSide wanted = PlayerScreenSide::Left;
    uint32_t selectorUser = 0;
    bool decided = false;
    LocalCameraSide cameraSide = LocalCameraSide::Default;
    uint32_t fighterAddr = 0;
    uint32_t originalSide = 0;
    uint32_t decisions = 0;
    bool online = false;
};
// Battle without a tick for this long is over: the decision is dropped (menus run unconverted).
constexpr uint64_t kSidePolicyBattleIdleMs = 1500;
// 0x82E88878: the four battle fighters (lis -32023, addi -30600). Index = team + 2 * member;
// the HUD reads it by index (gauges sub_823D4058 & co, portraits sub_823D55A8, win marks
// sub_823D56D0) and takes screen positions from the same index.
constexpr uint32_t kBattleFighterArrayAddr = 0x82E88878u;

// Phase 10: the decision uses the local fighter's TEAM, not +1024. Live Phase 9 logs showed the
// same left-standing practice player latched as LEFT in one battle and RIGHT in the next: +1024 is
// computed from whatever camera is active at that poll (menus, intro cinematics). Team 0 (1P)
// starts on screen LEFT and team 1 (2P) on screen RIGHT under the original camera.
// Polled fighters are sub_824CB300(t) / sub_824CB340(t): fighters[t] or its partner (+172).
constexpr uint32_t kFighterPartnerOffset = 172u;
// ModeManager::Impl singleton (sub_825ED930 stores it at 0x82F30B94); +12 = current mode
// (getCurrentMode sub_825EBFD0). Decisions are taken only in GAME (9) or PRACTICE (18).
constexpr uint32_t kModeManagerImplAddr = 0x82F30B94u;
constexpr uint32_t kModeManagerCurrentModeOffset = 12u;
constexpr uint32_t kGameModeGame = 9u;
constexpr uint32_t kGameModePractice = 18u;
// 0/1, or 0xFFFFFFFF when the fighter is not one of the battle's four.
uint32_t BattleTeamOfFighter(const uint8_t* base, uint32_t fighterAddr);
// Current ModeManager mode, 0xFFFFFFFF when the singleton is not readable.
uint32_t CurrentGameMode(const uint8_t* base);
inline bool IsBattleGameMode(uint32_t mode) { return mode == kGameModeGame || mode == kGameModePractice; }
// ModeManager's online modes (its constant table sub_825EDC18, BOOT = 2 ... ONLINE_MENU 29,
// CREATEMATCH, CUSTOMMATCH, QUICKMATCH, RANKEDMATCH, INVITEMATCH, LOBBY, ONLINE_TERM 36). An online
// match runs inside its online mode (the room's own state_battle_ready .. state_battle), not in GAME.
constexpr uint32_t kGameModeOnlineFirst = 29u, kGameModeOnlineLast = 36u;
inline bool IsOnlineGameMode(uint32_t mode) { return mode >= kGameModeOnlineFirst && mode <= kGameModeOnlineLast; }

// OPPOSITE when the team's native screen side (team 0 left, team 1 right) differs from the wanted one.
LocalCameraSide DecideCameraSideForPreference(PlayerScreenSide wanted, uint32_t team);
// teamHint (0/1): the team the game itself put the player on (PRACTICE via BATTLE POSITION).
// online: chosen in the Player Match room; decided from the local battle slot, camera / HUD /
// raw-pad left-right only (no +1028 side latch online).
void SetPlayerSidePreference(PlayerScreenSide wanted, uint32_t selectorUser, uint32_t teamHint = 0xFFFFFFFFu,
                             bool online = false);

// ---- Phase 11: Player Match ---------------------------------------------------------------------
// Online manager (sub_82274750 -> 0x82E6C5B8), its block at +7488 (accessors sub_8266BFF8 ..):
//  +7488 (+0, sub_8266C5E8) = the online layout mode (-1 offline; character select sub_825CB328 takes
//        0 / 1 / 2 / 3 - NOT a side: 11-A read it as the slot and both clients of a match saw 2);
//  +7492 (+4, sub_8266C5F8 / set by sub_82668178) = the LOCAL player's battle side, 0 = 1P, 1 = 2P
//        (-1 / -2 none / reassigning), set by the online match setup sub_82654CE8 from the player's
//        side byte (+666), flipped when +13 is set; the room compares it with its 1P / 2P fighters;
//  +7496 (+8) top bit set = not playing online (sub_8266C748).
// sub_822745B0 reads the battle input of logical ids 9/11 from slot 0 and 10/12 from slot 1.
constexpr uint32_t kOnlineManagerAddr = 0x82E6C5B8u;
constexpr uint32_t kOnlineLocalSlotOffset = 7488u;          // the block (+0 layout mode)
constexpr uint32_t kOnlineLocalSideOffset = 7492u;          // +4: the local battle side
// Phase 12: neither is the owner in a Player Match (MEASURED 2026-10-26, two loopback instances:
// +7488 = 2 and +7492 = -2 on both through the battle, +7492 = 0 on BOTH at the end-of-match save).
// Phase 12 (first build): the match player table the name labels read (sub_8256FE28: G = 0x82E70FB0,
// side s first entry u32 G + 49608 + 4 s, identity G + 16 + 248 i) - MEASURED empty in an online
// battle (side entries 0 / 50, XUID 0, type 0 on both clients): not used to decide, logged only.
constexpr uint32_t kMatchPlayersAddr = 0x82E70FB0u;
constexpr uint32_t kMatchSideIndexOffset = 12402u * 4u;     // 49608
constexpr uint32_t kMatchPlayerStride = 248u;
constexpr uint32_t kMatchPlayerIdentityOffset = 16u;
constexpr uint32_t kMatchPlayerMaxIndex = (kMatchSideIndexOffset - 8u) / kMatchPlayerStride - 1u;
// Phase 12-B: the owner is the network sync engine's own answer. NACE = online manager + 16
// (0x82E6C5C8), its sync engine kSync = NACE + 64: node array at kSync + 4828, count at +4856.
//  - node 0 is THIS machine (sub_826F5AD0: "local node" = the first; character select asks it
//    through sub_8266C740 -> sub_826EF378 = node0 + 744 to tell which side is local);
//  - node +744 = that machine's battle input slot (sub_826F6140(kSync, slot) finds the node with
//    +744 == slot; the record builder sub_826F0180 takes slot i's input from it);
//  - slot = battle team: sub_822745B0 reads logical ids 9/11 from slot 0 and 10/12 from slot 1.
// Local team = node0 + 744, only with >= 2 nodes, 0 / 1, and no other node on the same slot.
constexpr uint32_t kNaceAddr = 0x82E6C5C8u;
constexpr uint32_t kSyncAddr = kNaceAddr + 64u;
constexpr uint32_t kSyncNodesOffset = 4828u, kSyncNodeCountOffset = 4856u, kSyncNodeSlotOffset = 744u;
constexpr uint32_t kSyncMaxNodes = 7u;                      // 4828 + 4 * 7 = 4856
struct OnlineOwner {
    int team = -1;                          // 0 / 1, or -1 = UNKNOWN
    uint32_t nodes = 0;                     // sync engine node count
    uint32_t node[2] = {};                  // node 0 (local) and node 1 objects
    int32_t slot[2] = {-9, -9};             // their +744 battle slots
    uint32_t index[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};   // the name table (logged only)
    uint64_t xuid[2] = {};
    uint32_t type[2] = {};
    const char* reason = "";                // why UNKNOWN (or "matched")
};
OnlineOwner IdentifyOnlineOwner(const uint8_t* base, uint64_t localXuid);
// The local online XUID for the online side policy (set with the room's choice).
void SetOnlineLocalXuid(uint64_t xuid);
uint64_t OnlineLocalXuid();
uint32_t OnlineLocalBattleSlot(const uint8_t* base);

// Phase 12-C online W/S. The sidestep direction (requirements 144..149 on +1028) is simulation
// state, so it may only change when BOTH clients change it on the same frame. The OPPOSITE-view
// player says so inside its own networked input word:
//  - sub_8266D398 hands this machine's pad word to sub_82664CE0 -> sub_826F0CD0, which stores the
//    low 16 bits for the local slot (NACE + 6008 + 2 slot); sub_826EFAD8 memcpy's those 2 bytes into
//    the frame sent to the peer (sub_826F6190 = size 2), so all 16 bits travel;
//  - every fighter reads the agreed frame through sub_826EFA48 (current, NACE + 5984 + 2 slot) and
//    sub_826EFA90 (previous, NACE + 5996 + 2 slot), both clients alike.
// The flag is 0x0800, a pad bit nothing produces (XInput leaves 0x0400 / 0x0800 unused; the game's
// button masks 0x82172398.. list none of them). It is stripped from both getters, so the game never
// sees it, and a side requirement of a fighter whose team's slot carries it reads the other side.
constexpr uint32_t kSyncCurrentInputOffset = 5984u;        // NACE + 5984 + 2 slot (sub_826EFA48)
constexpr uint32_t kSyncPreviousInputOffset = 5996u;       // NACE + 5996 + 2 slot (sub_826EFA90)
constexpr uint32_t kOnlineOppositeViewFlag = 0x0800u;
// True while battle input comes from the network: sub_822745B0's own test (+7488 not -1, +7496 top
// bit clear) and a sync session with >= 2 nodes.
bool OnlineBattleInputLive(const uint8_t* base);
// sub_82664CE0(mgr, buttons): the word to send (the flag added while this machine's online
// decision is OPPOSITE in a battle mode).
uint32_t OnlineInputToSend(const uint8_t* base, uint32_t buttons);
// sub_826EFA48 / sub_826EFA90 result for `slot`: the flag removed (and its first sighting logged).
uint32_t OnlineInputAsRead(uint32_t slot, uint32_t value, bool previous);
// The agreed current frame of battle slot `team` carries the flag.
bool OnlineTeamSidestepFlipped(const uint8_t* base, uint32_t team);

// Phase 12-D online character select. The Lumen select controller draws side s's select state
// (select data +48 / +56) and profile on its p1 / p2 elements; while this machine's chosen side is
// not its team's native one, its update / draw run with the two sides traded (side_camera_diag.cpp
// ScopedCharaSelectSwap), so the local team shows on the chosen side. Display only - the selection
// logic, cursors and the network are untouched. TTT2_CHARASELECT_SIDE_SWAP=0 disables it.
// (A first try traded p1 / p2 element names in sub_82B62B30: the big portraits sit in per-side
// subtrees whose names exist only on their own side, so they stayed put - "P1DUMMY" panels.)
constexpr uint32_t kGameModeCharaSelect = 8u;
// This frame's character select is drawn swapped (online, chosen side != the local team's native).
bool CharaSelectSideSwapActive(const uint8_t* base);
void SetCharaSelectSideSwapEnabled(bool enabled);
// Phase 12-D8: the name plates (script binding sub_8256FE28) are filled while the character select
// is built, before the mode reads 8: online side choice, not a battle mode, owner known, chosen side
// != native. outMode / outTeam (may be null) report what was read.
bool OnlineNamePlateSwapWanted(const uint8_t* base, uint32_t* outMode = nullptr, int* outTeam = nullptr);
// An online character select with a room side choice (swapped or not): its diagnostics log.
bool CharaSelectOnlineDiagActive(const uint8_t* base);
// The room timer sub_826BB470(room): P = *(*(room+52) + 328) (sub_826B81F8), room data = P + 668
// (sub_826C4938: +32 state, +36 host counter), members present = bytes P+24 + 108*i (sub_826C4940),
// the room's own side setting = room+228 (OnlinePlayerMatchOperationMenuGetValue(0)).
struct PlayerMatchRoomCountdown {
    uint32_t state = 0, counter = 0, players = 0, nativeSide = 0;
};
bool ReadPlayerMatchRoomCountdown(const uint8_t* base, uint32_t roomObj, PlayerMatchRoomCountdown& out);
bool ObserveOnlineTeamForSidePolicy(uint32_t localSlot, uint32_t fighterAddr);
bool IsOnlineSidePolicyActive();
void ClearPlayerSidePreference();
SidePolicySnapshot GetSidePolicySnapshot();
// Battle poll observed (owner = pad lookup's XAM user, originalSide = +1028 as just latched).
bool ObserveFighterForSidePolicy(uint32_t fighterAddr, uint32_t ownerUser, uint32_t originalSide,
                                 bool onlineSessionActive = false);
void NoteBattleTickForSidePolicy(uint64_t nowMs);
// Drops the decision once no battle tick arrived for kSidePolicyBattleIdleMs.
void MaintainSidePolicy(uint64_t nowMs);

// Display-only HUD team swap: the HUD sees fighters [1,0,3,2] while the local camera is OPPOSITE,
// so each side of the HUD shows the team standing on that side. TTT2_HUD_SIDE_SWAP=0 disables it.
bool ShouldSwapHudTeams(bool onlineSessionActive);
bool ReloadHudSideSwapEnabledFromEnv();
void SetHudSideSwapEnabled(bool enabled);
// Swaps entries 0<->1 and 2<->3; false (nothing written) unless both team leaders are present.
bool SwapHudFighterSlots(uint8_t* base, uint32_t arrayAddr);
// The Lumen win marks (sub_82632148(team, wins)) take the team index; mirrored for display.
uint32_t HudTeamForDisplay(uint32_t team, bool swap);

// Guest big-endian memory helpers for camera structure inspection and render-context slot transform.
bool ReadCameraProducerSnapshot(const uint8_t* base, uint32_t camMgrAddr, CameraProducerSnapshot& out);
bool ShouldApplyOppositeToCamera(const CameraProducerSnapshot& snap);
bool TransformGuestCameraMatrix4x4(uint8_t* base, uint32_t srcMatrixAddr, uint32_t pivotAddr, uint32_t dstMatrixAddr);
bool TransformGuestCameraEye4(uint8_t* base, uint32_t pivotAddr, uint32_t inOutEyeAddr);

// Thread-safe producer diagnostic handoff: PPC thread enqueues fixed-size records;
// GPU CP thread drains and emits them during OnSwap().
bool IsCaptureActiveFast();
bool EnqueueCameraFrameDiag(const CameraFrameDiagRecord& record);
bool EnqueueLocalInputDiag(const LocalInputDiagRecord& record);
void OnProducerSample(const CameraProducerSnapshot& snap);
void ResetDiagStateForTesting();

void OnSwap();
bool Active();
bool WantsDrawHash();
void OnSkippedDraw();
bool IsCameraRegister(uint32_t reg);
void OnConstantWrite(uint32_t reg, uint32_t raw, uint32_t packet,
                     uint32_t source, uint32_t packetKind);
void OnDraw(const uint32_t* regs, uint32_t initiator, uint32_t vsAddr,
            uint32_t vsSize, uint64_t vsHash, uint32_t psAddr,
            uint32_t psSize, uint64_t psHash);
}
