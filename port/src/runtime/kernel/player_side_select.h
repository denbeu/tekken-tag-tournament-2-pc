#pragma once
#include <algorithm>
#include <cstdint>

namespace player_side_select {
enum class Mode { Practice, Versus, Online };
enum class Side { Left, Right };
enum class Action { None, Changed, Confirmed, Back, Unverified };

// A visual preference is never a controller assignment. Only a verified mapping
// for the same XAM user can authorize the corresponding camera/input settings.
struct Ownership {
    bool verified = false;
    uint32_t xamUser = 0;
    int battleSlot = -1;
};

struct Selection {
    Mode mode = Mode::Practice;
    Side saved = Side::Left;
    Side pending = Side::Left;
    uint32_t user = 0;
    uint16_t previous = 0;
    bool active = false;
    bool releasing = false;

    bool Open(Mode nextMode, uint32_t xamUser, uint16_t held) {
        if (nextMode == Mode::Online || xamUser > 3 || active || releasing) return false;
        if (mode != nextMode || user != xamUser) saved = Side::Left;
        mode = nextMode;
        user = xamUser;
        pending = saved;
        previous = held;
        active = true;
        return true;
    }
    bool CanApply(const Ownership& owner) const {
        return mode != Mode::Online && owner.verified && owner.xamUser == user &&
               owner.battleSlot == (pending == Side::Right ? 1 : 0);
    }
    Action Poll(uint32_t xamUser, uint16_t held, const Ownership& owner) {
        if (xamUser != user) return Action::None;
        if (releasing) {
            if (!held) releasing = false;
            previous = held;
            return Action::None;
        }
        if (!active) return Action::None;
        const uint16_t pressed = held & ~previous;
        previous = held;
        if (pressed & 0x2000) {
            active = false;
            releasing = true;
            pending = saved;
            return Action::Back;
        }
        if (pressed & 0x1000) {
            if (!CanApply(owner)) return Action::Unverified;
            saved = pending;
            active = false;
            releasing = true;
            return Action::Confirmed;
        }
        if ((held & 0xC) == 0xC) return Action::None;
        const Side before = pending;
        if (pressed & 0x4) pending = Side::Left;
        if (pressed & 0x8) pending = Side::Right;
        return before == pending ? Action::None : Action::Changed;
    }
};

// ---- Phase 9: the real flow --------------------------------------------------------------------
// OFFLINE MODE -> (PRACTICE / OFFLINEENTRY request held back) -> PLAYER SIDE SELECT -> the held
// request goes out unchanged -> character select. The screen records a screen-side preference and
// the XAM user who confirmed it; the battle (side_camera_diag) maps it onto that user's fighter.
// ModeManager ids from sub_825EDC18: OFFLINEENTRY 17 (ARCADE / VS / TEAM / ... entry), PRACTICE 18.
constexpr uint32_t kModeCharaSelect = 8, kModeGame = 9, kModeNameEntry = 10, kModeEnding = 11,
                   kModeMainMenu = 16, kModeOfflineEntry = 17, kModePractice = 18, kModeOfflineResult = 23;
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002;
constexpr uint16_t kPadLeft = 0x0004, kPadRight = 0x0008, kPadA = 0x1000, kPadB = 0x2000;

inline bool IsSideSelectMode(uint32_t mode) { return mode == kModeOfflineEntry || mode == kModePractice; }
// Requests that belong to an offline battle started from the side select (rematch, results) keep the
// preference; anything else (online, tutorial, replays, title) drops it.
inline bool KeepsSidePreference(uint32_t mode) {
    return IsSideSelectMode(mode) || mode == kModeCharaSelect || mode == kModeGame || mode == kModeNameEntry ||
           mode == kModeEnding || mode == kModeMainMenu || mode == kModeOfflineResult;
}

// A side chosen in the Player Match room lives through the online modes (ONLINE_MENU 29 ..
// ONLINE_TERM 37), character select and the battle; leaving for offline modes drops it.
constexpr uint32_t kModeOnlineFirst = 29, kModeOnlineLast = 37;
inline bool KeepsOnlineSidePreference(uint32_t mode) {
    return (mode >= kModeOnlineFirst && mode <= kModeOnlineLast) || mode == kModeCharaSelect || mode == kModeGame;
}

struct ModeRequest { uint32_t holder = 0, mode = 0, arg = 0; };

// ---- Phase 11: Player Match room side select ----------------------------------------------------
// The room (state_room_main, sub_826BF870) is the game's READY period and stays untouched: its
// host-run countdown (room data +32 state, +36 counter to 960, then the fighter pick sub_826BCE58 and
// the counter back to 0) and every client's per-frame check that both fighters (queue room data +8 /
// +10, sub_826BBDC0) are READY (member +20). Once they are, that client switches the room state
// (room +40, dispatched each frame by sub_826BED20) to state_battle_ready (sub_826BF778), which runs
// once and moves on to the synchronised states (battle_ready_wait / _sync, battle_sync, game start ->
// character select).
// Phase 11-D: the side select is that point. state_battle_ready is held (not run; room +40 stays on
// it, so it is called again the next frame) for 10 s of frames on each client (BattleReadyHold), then
// run: both clients reach it within the network delay of each other and the game's own sync states
// that follow absorb the difference. The window sees the hold as a countdown 360 .. 960 (10 .. 0 s).
// (11-B opened it at the room countdown's start and started that countdown at 10 s: the window then
// took the READY input away, nobody was READY at 960, and the room counted again from 16.)
constexpr uint32_t kRoomStateWait = 0, kRoomStateCountdown = 1, kRoomStateBattle = 2;
constexpr uint32_t kRoomCountdownFrames = 960, kRoomSideWindowFrames = 600;
constexpr uint32_t kRoomSideStartCounter = kRoomCountdownFrames - kRoomSideWindowFrames; // 360: 10 s left
constexpr uint32_t kHoldGraceFrames = 60;      // after 10 s: at most 1 s more for the window to apply the side
constexpr int64_t kHoldGapMs = 500;            // not called for this long: the hold was left (a new one starts)

enum class HoldStep { Start, Hold, Release };
struct BattleReadyHold {
    bool active = false;
    uint32_t room = 0;
    uint32_t frames = 0;         // frames held
    int64_t lastMs = 0;
    uint32_t holds = 0;          // holds started
    // Each call of state_battle_ready: hold it (Start / Hold) or let it run (Release, once per hold).
    // `applied`: the window has applied this hold's side. Phase 12-D: released as soon as it is (A
    // confirms at once), else after the 10 s and the grace.
    HoldStep Update(uint32_t roomAddr, int64_t nowMs, bool applied) {
        const bool fresh = !active || roomAddr != room || nowMs - lastMs > kHoldGapMs;
        lastMs = nowMs;
        if (fresh) {
            active = true; room = roomAddr; frames = 1; ++holds;
            return HoldStep::Start;      // `applied` is the previous hold's
        }
        if (!applied && frames < kRoomSideWindowFrames + kHoldGraceFrames) {
            ++frames;
            return HoldStep::Hold;
        }
        active = false;
        return HoldStep::Release;
    }
    // The countdown the window shows: 360 at the start, 960 after 10 s.
    uint32_t Counter() const { return kRoomSideStartCounter + (std::min)(frames, kRoomSideWindowFrames); }
};

// Phase 12-D: the next room state, battle_ready_wait (sub_826BEF60), counts room +180 to 600 frames
// for the peer (sub_826B8640) and then goes on without it. A client that confirmed early waits there
// for a peer still in its side select, so that wait gets the hold time it skipped (+ the grace).
inline uint32_t BattleWaitExtension(uint32_t heldFrames) {
    return heldFrames < kRoomSideWindowFrames ? kRoomSideWindowFrames - heldFrames + kHoldGraceFrames : 0u;
}
// Before each battle_ready_wait call: a counter the game already advanced is stepped back while
// budget is left (the call adds it again), so the 600 frames start after the extension.
inline bool AbsorbBattleWaitFrame(uint32_t& counter, uint32_t& budget) {
    if (budget == 0u || counter == 0u || counter > kRoomSideWindowFrames) return false;
    --counter;
    --budget;
    return true;
}

struct RoomCountdownView {
    bool valid = false;          // a hold frame was recorded (stale view -> invalid)
    uint32_t state = 0;          // 1 while held, 2 once released
    uint32_t counter = 0;        // BattleReadyHold::Counter (360 .. 960)
    uint32_t players = 0;        // members present (sub_826C4940)
    uint32_t nativeSide = 0;     // the room's own side setting (room +228: 0 LEFT, 1 RIGHT)
};
enum class RoomWindowEvent { None, Opened, Tick, Redraw, Confirmed, Unconfirmed, Finished, Cancelled };

struct RoomSideWindow {
    bool open = false;
    bool confirmed = false;
    bool inCountdown = false;
    bool consumed = false;       // this countdown already had its window
    bool armed = true;           // a window ended: only no hold (stale / state 0) re-arms
    uint32_t generation = 0;     // countdowns seen (one window each)
    Side cursor = Side::Left;
    bool havePreference = false;
    Side preference = Side::Left;
    uint16_t prev[4] = {};
    uint32_t user = 0;           // the XAM user who last operated the window
    int seconds = -1;

    // Shown seconds: 10 .. 0 over the hold (at most 10).
    static int SecondsLeft(uint32_t counter) {
        const uint32_t left = counter >= kRoomCountdownFrames ? 0u : kRoomCountdownFrames - counter;
        return (std::min)(int((left + 59u) / 60u), int(kRoomSideWindowFrames / 60u));
    }
    RoomWindowEvent Update(const RoomCountdownView& v, const uint16_t held[4]) {
        const bool counting = v.valid && v.state == kRoomStateCountdown && v.players >= 2u;
        if (!v.valid || v.state == kRoomStateWait) armed = true;
        if (counting && !inCountdown && armed) { ++generation; consumed = false; }
        inCountdown = counting;
        RoomWindowEvent event = RoomWindowEvent::None;
        if (open) {
            if ((v.valid && v.state == kRoomStateBattle) || (counting && v.counter >= kRoomCountdownFrames)) {
                open = false;
                armed = false;
                preference = cursor;
                havePreference = true;
                event = RoomWindowEvent::Finished;
            } else if (!counting) {
                open = false;
                armed = false;
                event = RoomWindowEvent::Cancelled;
            } else {
                for (uint32_t u = 0; u < 4 && event == RoomWindowEvent::None; ++u) {
                    const uint16_t pressed = uint16_t(held[u] & ~prev[u]);
                    if (!pressed) continue;
                    user = u;
                    if (pressed & kPadA) {
                        // Phase 12-D: final - the window ends here and the hold lets the game go on.
                        confirmed = true;
                        open = false;
                        armed = false;
                        preference = cursor;
                        havePreference = true;
                        event = RoomWindowEvent::Finished;
                    } else if ((held[u] & (kPadUp | kPadDown)) != (kPadUp | kPadDown) &&
                               (held[u] & (kPadLeft | kPadRight)) != (kPadLeft | kPadRight)) {
                        const Side before = cursor;
                        if (pressed & (kPadUp | kPadLeft)) cursor = Side::Left;
                        if (pressed & (kPadDown | kPadRight)) cursor = Side::Right;
                        if (cursor != before) event = RoomWindowEvent::Redraw;
                    }
                }
                const int s = SecondsLeft(v.counter);
                if (event == RoomWindowEvent::None && s != seconds) event = RoomWindowEvent::Tick;
                seconds = s;
            }
        } else if (counting && !consumed && v.counter < kRoomCountdownFrames) {
            open = true;
            consumed = true;
            confirmed = false;
            cursor = havePreference ? preference : (v.nativeSide == 1u ? Side::Right : Side::Left);
            seconds = SecondsLeft(v.counter);
            event = RoomWindowEvent::Opened;
        }
        for (int u = 0; u < 4; ++u) prev[u] = held[u];
        return event;
    }
};

// ---- Phase 10: PRACTICE uses the game's own BATTLE POSITION ------------------------------------
// Game Options -> BATTLE POSITION ("キャラクターの立ち位置", values LEFT = 0 default, RIGHT = 1;
// sub_82374700 stores the row cursor at options+16, sub_823747C0 resets it to 0). The PRACTICE
// item (MenuItemMode002-07 OnEnter) and the practice pause menu's re-entry (FramePracticeMenu01-14)
// both send requestChangeMode(PRACTICE, LEFT -> 1 / RIGHT -> 2): the bytecode pushes the constants
// 1 and 2 on the two branches, and the default option logged "mode 18 (arg 1)". The argument is a
// side-presence mask: sub_825CB328 splits CharaSelectMode+140 bit0 / bit1 into the two sides'
// human flags and takes the no-human path (loc_825CB864) when both are clear - the arg 0 trial's
// attract-like demo. So 2P side in Practice is what the game itself does for BATTLE POSITION RIGHT:
// option 1 and argument 2, together (the pause-menu re-entry reads the option again).
constexpr uint32_t kBattlePositionLeft = 0, kBattlePositionRight = 1;
inline uint32_t PracticeArgForBattlePosition(uint32_t position) {
    return position == kBattlePositionLeft ? 1u : position == kBattlePositionRight ? 2u : 0u;
}
inline Side SideForBattlePosition(uint32_t position, Side fallback) {
    return position == kBattlePositionLeft ? Side::Left : position == kBattlePositionRight ? Side::Right : fallback;
}
struct PracticePositionPlan {
    bool apply = false;          // false: nothing is changed, the request goes out as it came
    uint32_t position = 0;       // BATTLE POSITION to store before the request
    uint32_t arg = 0;            // the request argument the game derives from that position
};
// Applies only when the held request is exactly what the game derived from the current option
// (confirms the mapping at run time); never yields 0 (no human side).
inline PracticePositionPlan PlanPracticeBattlePosition(uint32_t optionBefore, uint32_t requestedArg, Side wanted) {
    PracticePositionPlan plan{false, optionBefore, requestedArg};
    const uint32_t expected = PracticeArgForBattlePosition(optionBefore);
    if (expected == 0u || expected != requestedArg) return plan;
    plan.apply = true;
    plan.position = wanted == Side::Right ? kBattlePositionRight : kBattlePositionLeft;
    plan.arg = PracticeArgForBattlePosition(plan.position);
    return plan;
}
enum class FlowState { Idle, Opening, Open, ConfirmRelease, BackRelease };
enum class FlowEvent { None, Opened, Redraw, Confirmed, Cancelled, Dispatch, Closed };

struct SideSelectFlow {
    FlowState state = FlowState::Idle;
    Side cursor = Side::Left;          // kept between visits: the last confirmed side
    Side saved = Side::Left;
    ModeRequest pending{};
    uint16_t prev[4] = {};
    int setup = 0;
    uint32_t confirmUser = 0;

    // A mode request from the OFFLINE MODE frame: held back (true) or passed on (false).
    bool Request(const ModeRequest& request, bool enabled, bool online, bool fromOfflineMenu) {
        if (!enabled || online || !fromOfflineMenu || state != FlowState::Idle || !IsSideSelectMode(request.mode))
            return false;
        pending = request;
        cursor = saved;
        state = FlowState::Opening;
        return true;
    }
    // The frame was switched to: buttons already held do not count.
    void BeginOpen(const uint16_t held[4]) {
        for (int u = 0; u < 4; ++u) prev[u] = held[u];
        setup = 3;
        state = FlowState::Open;
    }
    FlowEvent Poll(const uint16_t held[4]) {
        switch (state) {
        case FlowState::Open: {
            if (setup > 0) {
                for (int u = 0; u < 4; ++u) prev[u] = held[u];
                return --setup == 0 ? FlowEvent::Opened : FlowEvent::None;
            }
            FlowEvent event = FlowEvent::None;
            for (uint32_t u = 0; u < 4 && event == FlowEvent::None; ++u) {
                const uint16_t pressed = uint16_t(held[u] & ~prev[u]);
                if (pressed & kPadB) {
                    cursor = saved;
                    state = FlowState::BackRelease;
                    event = FlowEvent::Cancelled;
                } else if (pressed & kPadA) {
                    saved = cursor;
                    confirmUser = u;
                    state = FlowState::ConfirmRelease;
                    event = FlowEvent::Confirmed;
                } else if ((held[u] & (kPadUp | kPadDown)) != (kPadUp | kPadDown) &&
                           (held[u] & (kPadLeft | kPadRight)) != (kPadLeft | kPadRight)) {
                    const Side before = cursor;
                    // The BX01 menu has TWO VERTICAL rows. Support up/down as well as
                    // left/right so keyboard D-pad, thumb stick and UI agree.
                    if (pressed & (kPadUp | kPadLeft)) cursor = Side::Left;
                    if (pressed & (kPadDown | kPadRight)) cursor = Side::Right;
                    if (cursor != before) event = FlowEvent::Redraw;
                }
            }
            for (int u = 0; u < 4; ++u) prev[u] = held[u];
            return event;
        }
        case FlowState::ConfirmRelease:
        case FlowState::BackRelease: {
            // The confirming A must not reach character select: wait until every pad is released.
            for (int u = 0; u < 4; ++u) if (held[u]) return FlowEvent::None;
            const bool confirmed = state == FlowState::ConfirmRelease;
            state = FlowState::Idle;
            return confirmed ? FlowEvent::Dispatch : FlowEvent::Closed;
        }
        default:
            return FlowEvent::None;
        }
    }
    bool OwnsInput() const { return state != FlowState::Idle; }
};
} // namespace player_side_select
