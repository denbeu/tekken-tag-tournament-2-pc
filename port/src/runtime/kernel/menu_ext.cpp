// PC menu extensions (plan and reverse-engineering notes: port/MENU_PLAN.md).
//
// TTT2's menus are BX01 binary-XML documents (frames, menus, rows, value lists) whose event
// handlers are precompiled Xtal scripts calling `lib::` natives. This module
//  - edits the Options / main-menu document as the game loads it (menu_doc.cpp): DISPLAY MODE
//    and RESOLUTION rows in Display Settings, EXIT in the main menu with its confirmation;
//  - registers the natives those rows and the dialog call, next to the game's own options natives;
//  - initialises the new rows' value cursors whenever Display Settings initialises MOTION BLUR's;
//  - supplies the confirmation's text and YES / NO labels, and quits on YES;
//  - Controller Setup's KEYBOARD mode: player 1's keyboard keys shown and changed in the button
//    panel (keyui below, keyboard_keys.h).
// It also hosts the system UI (sys_ui.h: Select Profile and the new profile's name dialog).
// Diagnostics: TTT2_MENU_DUMP=1 writes every BX01 document (and the edited one) to
// C:/ttt2/port/menu_bin/; TTT2_TEXT_TRACE=1 logs each text id the game looks up (first time, with
// the caller). Development: TTT2_MENU_OVERRIDE=1 loads a document prepared with
// port/tools/menu_proto.py from C:/ttt2/port/menu_override/<fnv1a>.bin instead of editing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "kernel_shared.h"
#include "guest_heap.h"
#include "display_settings.h"
#include "menu_doc.h"
#include "sys_ui.h"
#include "profiles.h"
#include "keyboard_keys.h"
#include "player_side_select.h"
#include "side_camera_diag.h"
#include "xlive.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82346060);
extern "C" PPC_FUNC(__imp__sub_82A8C170);
extern "C" PPC_FUNC(__imp__sub_823756A8);
extern "C" PPC_FUNC(__imp__sub_8256CD28);
extern "C" PPC_FUNC(__imp__sub_8237F5D0);
extern "C" PPC_FUNC(__imp__sub_8241E7A8);
extern "C" PPC_FUNC(__imp__sub_82623858);
extern "C" PPC_FUNC(__imp__sub_8241B9D0);
extern "C" PPC_FUNC(__imp__sub_8256C708);
extern "C" PPC_FUNC(__imp__sub_82374700);
extern "C" PPC_FUNC(__imp__sub_823747C0);
extern "C" PPC_FUNC(__imp__sub_825ED4F0);
extern "C" PPC_FUNC(__imp__sub_82374738);
extern "C" PPC_FUNC(__imp__sub_826BF778);
extern "C" PPC_FUNC(__imp__sub_826BEF60);
extern void (*g_uiFrameHook)(PPCContext& ctx, uint8_t* base);
void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);
bool XSessionAnyActive();

namespace {
bool EnvFlag(const char* name)
{
    const char* e = std::getenv(name);
    return e && *e == '1';
}
bool MenuDump() { static const bool on = EnvFlag("TTT2_MENU_DUMP"); return on; }
bool MenuOverride() { static const bool on = EnvFlag("TTT2_MENU_OVERRIDE"); return on; }

uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

void SaveFile(const char* path, const void* data, size_t size)
{
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, size, f); std::fclose(f); }
}

// A copy of `bytes` in never-freed guest memory (`prefix` zero bytes in front); guest address.
uint32_t GuestCopy(uint8_t* base, const void* bytes, size_t size, uint32_t prefix = 0)
{
    auto* host = static_cast<uint8_t*>(g_virtualHeap.Alloc(prefix + size + 16));
    std::memset(host, 0, prefix);
    std::memcpy(host + prefix, bytes, size);
    return static_cast<uint32_t>(host + prefix - base);
}
uint32_t GuestCString(uint8_t* base, const char* text, uint32_t prefix = 0)
{
    return GuestCopy(base, text, std::strlen(text) + 1, prefix);
}

// A string as the game's text lookup returns it: data 4 bytes into a table entry whose last
// header byte is the length.
uint32_t TableText(uint8_t* base, const char* text)
{
    const uint32_t addr = GuestCString(base, text, 4);
    GWrite8(base, addr - 1, static_cast<uint8_t>(std::strlen(text)));
    return addr;
}

std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 260)
{
    std::string s;
    for (size_t i = 0; addr && i < max && base[addr + i]; ++i) s.push_back(static_cast<char>(base[addr + i]));
    return s;
}

// A scratch frame below the current guest stack pointer for calling guest functions from a
// hook; restores r1 on destruction. The ABI back chain is written.
struct GuestFrame
{
    PPCContext& ctx;
    uint32_t saved;
    GuestFrame(PPCContext& c, uint8_t* base, uint32_t bytes) : ctx(c), saved(c.r1.u32)
    {
        const uint32_t sp = (saved - bytes) & ~0xFu;
        GWrite32(base, sp, saved);
        ctx.r1.u32 = sp;
    }
    ~GuestFrame() { ctx.r1.u32 = saved; }
    uint32_t sp() const { return ctx.r1.u32; }
};

// Menu widget by name (sub_8256C160, as lib::MenuSelectValueSetCursor looks it up) or 0.
uint32_t FindWidget(PPCContext& ctx, uint8_t* base, uint32_t nameAddr)
{
    ctx.r3.u64 = nameAddr;
    sub_8256C160(ctx, base);
    return ctx.r3.u32;
}

// lib::MenuSwitchFrame(name) (impl sub_8256BF88: name is a C string).
void SwitchFrame(PPCContext& ctx, uint8_t* base, const char* name)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestCString(base, name);
    sub_8256BF88(ctx, base);
}

// ---- system UI (sys_ui.h, PROFILE_PLAN.md) ---------------------------------------------------
// The console-side screens. Select Profile opens when START / A is pressed on the title logo
// while nobody is signed in, drawn with FrameSysMenu<N> (menu_doc.cpp: N = 2..9 rows in Display
// Settings' style; the list uses the frame with exactly its row count); a new profile is named in
// the game's confirmation window. Everything here runs on the game's main thread at the end of
// VdSwap. Two-argument menu natives take the value first and the widget name second.

// Guest copies of strings, one per distinct text (texts change only on input; never freed).
uint32_t GuestStr(uint8_t* base, const std::string& s)
{
    static std::map<std::string, uint32_t> pool;
    const auto it = pool.find(s);
    if (it != pool.end()) return it->second;
    const uint32_t addr = GuestCString(base, s.c_str());
    pool.emplace(s, addr);
    return addr;
}
bool g_hostText = false;                              // MenuSetText is writing (the hook passes it on)
void MenuSetText(PPCContext& ctx, uint8_t* base, const std::string& widget, const std::string& text)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, text);
    ctx.r4.u64 = GuestStr(base, widget);
    g_hostText = true;
    sub_8256C708(ctx, base);                          // lib::MenuEntitySetText(text, widget)
    g_hostText = false;
}
void MenuMovieVisible(PPCContext& ctx, uint8_t* base, const std::string& movie, bool visible)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = visible ? 1 : 0;
    sub_8256D518(ctx, base);                          // lib::MenuIFLumenSetMovieVisible(movie, bool)
}
void MenuTelopTarget(PPCContext& ctx, uint8_t* base, const std::string& widget)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, widget);
    sub_825719A8(ctx, base);                          // lib::MenuTelopSetTargetEntity(widget)
}
void MenuSetCursor(PPCContext& ctx, uint8_t* base, const std::string& menu, int index)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = static_cast<uint32_t>(index);
    ctx.r4.u64 = GuestStr(base, menu);
    sub_8256C928(ctx, base);                          // lib::MenuContainerSetCursor(index, menu)
}

void MenuMoviePlay(PPCContext& ctx, uint8_t* base, const std::string& movie, const std::string& label, bool force)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = GuestStr(base, label);
    ctx.r5.u64 = force ? 1 : 0;
    sub_8256D5C8(ctx, base);                          // lib::MenuIFLumenMoviePlayFromLabelAndStop
}
void MenuTextHide(PPCContext& ctx, uint8_t* base, const std::string& widget, bool hide)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = hide ? 1 : 0;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C758(ctx, base);                          // lib::MenuEntitySetTextHide(bool, widget)
}
void MenuTickCount(PPCContext& ctx, uint8_t* base, const std::string& widget, uint32_t count)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = count;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C2B0(ctx, base);                          // lib::MenuSetTickCount(count, widget)
}
// lib::MenuSoundKeyOn(id) with the menus' SoundIdMenuCursor / Decide / Cancel (natives defined
// by sub_825A5098).
constexpr uint32_t kSoundCursor = 8426, kSoundCancel = 8427, kSoundDecide = 8428;
void MenuSound(PPCContext& ctx, uint8_t* base, uint32_t id)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = id;
    sub_8259E948(ctx, base);
}

namespace sysui {

enum class Screen { None, Profiles, Name };
constexpr int kRows = 9;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kStart = 0x0010, kA = 0x1000, kB = 0x2000;
constexpr uint32_t kNameTextId = 0x7F02;              // the name dialog's message (text lookup hook)

struct Row { std::string label, value, help; };
struct Ui
{
    Screen screen = Screen::None;
    int setupFrames = 0;            // frames left before the rows are written (after a switch)
    bool releasing = false;         // closed: keep the controller until every button is up
    bool continueAfter = false;     // then press START for the game (the title continues)
    int cursor = 0;
    std::vector<Row> rows, shown;   // wanted / last written
    int shownCursor = -1;
    int frameRows = 0;              // FrameSysMenu<N> on screen (0 = none yet)
    int settle = 0;                 // frames until the cursor is drawn again (after the menu's OnFocus)
    std::string name, note;         // the name dialog
    bool suggested = false;         // `name` is the suggestion: the first typed character replaces it
    uint16_t prev = 0, repeatKey = 0;
    int64_t repeatAt = 0;
};
Ui g_ui;
std::atomic<bool> g_owns{ false }, g_textEntry{ false }, g_signInRequest{ false };
std::atomic<bool> g_switchProfile{ false };   // Options -> SWITCH PROFILE confirmed: sign out on the logo
std::atomic<int64_t> g_titleLogoMs{ 0 };
std::mutex g_charMutex;
std::vector<uint32_t> g_chars;
// The name dialog's message: a guest buffer the text lookup returns for kNameTextId, and the
// window's text block it was laid out into (captured in sub_8241B9D0), laid out again as the name
// changes.
uint32_t g_nameText = 0;
std::atomic<uint32_t> g_nameBlock{ 0 }, g_nameFont{ 0 }, g_nameSize{ 0 };
// Message boxes (XamShowMessageBoxUI, kernel_ui.cpp): the queued one, the one on screen, its
// message (kBoxTextId, laid out again into g_boxBlock as the listed choice moves) and - while a
// window is being opened - its button labels for the lookups of the window's OK / CANCEL texts.
constexpr uint32_t kBoxTextId = 0x7F03;
enum class BoxStyle { Information, Confirmation, List };
std::mutex g_boxMutex;
std::vector<SysUiMessageBox> g_boxQueue;
struct BoxState
{
    bool showing = false, releasing = false;
    SysUiMessageBox box;
    BoxStyle style = BoxStyle::Information;
    int cursor = 0;
    uint16_t prev = 0;
} g_box;
std::atomic<bool> g_boxOwns{ false };
uint32_t g_boxText = 0;
std::atomic<uint32_t> g_boxBlock{ 0 }, g_boxFont{ 0 }, g_boxSize{ 0 };
std::mutex g_labelMutex;
std::vector<uint32_t> g_labels;             // guest texts, set only around the window's opening

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string Two(int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); }
bool TitleLogo() { return NowMs() - g_titleLogoMs.load() < 150; }

void Notify(uint32_t id, uint32_t param) { XamNotifyEnqueueEvent(id, param); }

void BuildProfiles(Ui& ui)
{
    ui.rows.clear();
    for (const profiles::Profile& p : profiles::List())
        ui.rows.push_back({ p.name, "", "Play as " + p.name + "." });
    if (ui.rows.size() < profiles::kMaxProfiles)
        ui.rows.push_back({ "CREATE NEW PROFILE", "", "Make a new profile with its own save data." });
}

// "PLAYER <n>": the first free number from the profile count up.
std::string SuggestedName()
{
    const std::vector<profiles::Profile> list = profiles::List();
    for (size_t n = list.size() + 1;; ++n)
    {
        const std::string name = "PLAYER " + std::to_string(n);
        bool used = false;
        for (const profiles::Profile& p : list) used |= _stricmp(p.name.c_str(), name.c_str()) == 0;
        if (!used) return name;
    }
}

// ---- the name dialog: the game's confirmation window (OK / CANCEL), answered from the host
std::string NameMessage(const Ui& ui)
{
    std::string hint = ui.note;
    if (hint.empty())
        hint = ui.suggested ? "Type a name, or confirm this one." : "ENTER: confirm   ESC: cancel";
    return "Name your new profile.\n\n" + ui.name + (ui.suggested ? "" : "_") + "\n\n" + hint;
}
// A text the lookup hook returns: a fixed guest buffer (allocated on first use) holding `text` as
// a text-table entry (length byte in front).
void WriteTableText(uint8_t* base, uint32_t& buffer, size_t capacity, const std::string& text)
{
    if (!buffer)
    {
        const std::vector<char> zeros(capacity + 1, 0);
        buffer = GuestCopy(base, zeros.data(), zeros.size(), 4);
    }
    const size_t n = (std::min)(text.size(), capacity);
    std::memcpy(base + buffer, text.data(), n);
    base[buffer + n] = 0;
    GWrite8(base, buffer - 1, static_cast<uint8_t>((std::min<size_t>)(n, 255)));
}
void WriteNameText(uint8_t* base, const std::string& text) { WriteTableText(base, g_nameText, 480, text); }
void ShowNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    g_nameBlock = 0;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kNameTextId;
    ctx.r5.u64 = 0;
    sub_8241FAF0(ctx, base);                          // lib::WindowShowConfirmation(text id)
}
void UpdateNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    const uint32_t block = g_nameBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_nameFont.load();
    ctx.r5.u64 = g_nameSize.load();
    ctx.r6.u64 = g_nameText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void HideDialog(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8241E7C8(ctx, base);                          // lib::WindowHideDialog()
}

void Open(Ui& ui, int cursor)
{
    ui.screen = Screen::Profiles;
    ui.cursor = cursor;
    ui.setupFrames = 1;
    ui.shown.clear();
    ui.shownCursor = -1;
    g_textEntry = false;
    g_owns = true;
}
void Close(PPCContext& ctx, uint8_t* base, Ui& ui, bool continueAfter)
{
    if (ui.screen == Screen::Name) HideDialog(ctx, base);
    SwitchFrame(ctx, base, "FrameSysEmpty");
    ui.screen = Screen::None;
    ui.frameRows = 0;
    ui.releasing = true;
    ui.continueAfter = continueAfter;
    g_textEntry = false;
    Notify(9, 0);                                        // XN_SYS_UI closed
}
void SignInAndClose(PPCContext& ctx, uint8_t* base, Ui& ui, int index)
{
    profiles::SignIn(index);
    Notify(0x0000000Au, 1);                              // XN_SYS_SIGNINCHANGED, user 0
    Close(ctx, base, ui, true);                          // and the title takes START
}

void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force);
void Draw(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    BuildProfiles(ui);
    const int count = std::clamp(int(ui.rows.size()), 2, kRows);
    ui.rows.resize(size_t(count), Row{ "-", "", "-" });
    ui.cursor = std::clamp(ui.cursor, 0, count - 1);
    if (count != ui.frameRows)
    {
        // The frame with exactly this many rows (its bars follow its icon arrays); write the
        // texts once it is up.
        SwitchFrame(ctx, base, ("FrameSysMenu" + std::to_string(count)).c_str());
        ui.frameRows = count;
        ui.shown.clear();
        ui.shownCursor = -1;
        ui.setupFrames = 2;
        return;
    }
    if (ui.shown.empty())
    {
        // As Display Settings' OnFocus does: no large icon (IFMainMenuL would show its first,
        // the globe), and the bottom bar scrolls the game's tips.
        MenuMovieVisible(ctx, base, "rmc_icon_menu_L", false);
        MenuTelopTarget(ctx, base, "StaticTelopSysMenu" + std::to_string(count));
    }
    const std::string n = std::to_string(count) + "_";
    for (int i = 0; i < count; ++i)
    {
        const Row& row = ui.rows[size_t(i)];
        const bool fresh = i >= int(ui.shown.size());
        const Row old = fresh ? Row{} : ui.shown[size_t(i)];
        const std::string k = n + Two(i + 1);
        if (fresh || old.label != row.label) MenuSetText(ctx, base, "MenuItemSysMenu" + k, row.label);
        if (fresh || old.value != row.value) MenuSetText(ctx, base, "StaticValueSysMenu" + k, row.value);
        if (fresh || old.help != row.help) MenuSetText(ctx, base, "StaticSysMenu" + k, row.help);
    }
    if (ui.shown.empty()) ui.settle = 8;
    ui.shown = ui.rows;
    if (ui.cursor != ui.shownCursor)
    {
        DrawCursor(ctx, base, count, ui.cursor, ui.shownCursor < 0);
        ui.shownCursor = ui.cursor;
    }
}

// The cursor as the interface script global::IFMainMenuL_Cursor / _CursorSet draws it (menus get no
// input on the title, so their OnTrigger never runs): the selection bar, the row icons (the
// selected one lit), and only the selected row's help text.
void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force)
{
    const std::string n = std::to_string(count);
    MenuSetCursor(ctx, base, "MenuSysMenu" + n, cursor);
    MenuMoviePlay(ctx, base, "amc_menu_bar_sentaku_L", "menu_bar_sentaku_solo_" + Two(cursor + 1), force);
    for (int i = 0; i < count; ++i)
    {
        MenuMoviePlay(ctx, base, "mc_icon_menu_S_L_" + Two(i + 1), i == cursor ? "icon_s_006" : "icon_s_006_x", force);
        const std::string help = "StaticSysMenu" + n + "_0" + std::to_string(i + 1);
        MenuTextHide(ctx, base, help, i != cursor);
        MenuTickCount(ctx, base, help, 0);
    }
}

// Pressed-this-frame buttons, with auto-repeat for UP / DOWN (400 ms, then every 90 ms).
uint16_t Pressed(Ui& ui)
{
    const uint16_t now = HostControllerButtons();
    uint16_t pressed = now & ~ui.prev;
    const uint16_t dirs = now & (kUp | kDown);
    const int64_t t = NowMs();
    if (dirs && dirs == ui.repeatKey && t >= ui.repeatAt) { pressed |= dirs; ui.repeatAt = t + 90; }
    else if (dirs != ui.repeatKey) { ui.repeatKey = dirs; ui.repeatAt = t + 400; }
    ui.prev = now;
    return pressed;
}

void OpenNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    ui.screen = Screen::Name;
    ui.name = SuggestedName();
    ui.suggested = true;
    ui.note.clear();
    g_textEntry = true;
    ShowNameDialog(ctx, base, ui);
    std::fprintf(stderr, "[sys-ui] new profile: name dialog\n");
}
void CloseNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    HideDialog(ctx, base);
    g_textEntry = false;
    ui.screen = Screen::Profiles;
    ui.shownCursor = -1;
}

// ---- message boxes in the game's dialog windows (see SysUiMessageBox)
std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string BoxMessage(const BoxState& b)
{
    if (b.style != BoxStyle::List) return b.box.text;
    std::string m = b.box.text + "\n";
    for (size_t i = 0; i < b.box.buttons.size(); ++i)
    {
        const std::string label = Upper(b.box.buttons[i]);
        m += "\n" + (int(i) == b.cursor ? "> " + label + " <" : label);
    }
    return m;
}
void ShowBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    g_boxBlock = 0;
    {
        static std::map<std::string, uint32_t> pool;
        std::lock_guard l(g_labelMutex);
        g_labels.clear();
        if (b.style != BoxStyle::List)
            for (const std::string& label : b.box.buttons)
            {
                auto it = pool.find(Upper(label));
                if (it == pool.end()) it = pool.emplace(Upper(label), TableText(base, Upper(label).c_str())).first;
                g_labels.push_back(it->second);
            }
    }
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = kBoxTextId;
        ctx.r5.u64 = 1;                               // information: the A button's label
        if (b.style == BoxStyle::Confirmation) sub_8241FAF0(ctx, base);   // lib::WindowShowConfirmation
        else sub_8241FB70(ctx, base);                                    // lib::WindowShowInformation
    }
    std::lock_guard l(g_labelMutex);
    g_labels.clear();
}
void UpdateBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    const uint32_t block = g_boxBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_boxFont.load();
    ctx.r5.u64 = g_boxSize.load();
    ctx.r6.u64 = g_boxText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void BoxFrame(PPCContext& ctx, uint8_t* base)
{
    BoxState& b = g_box;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = now & ~b.prev;
    b.prev = now;
    if (b.releasing)
    {
        if (now == 0) { b.releasing = false; g_boxOwns = false; }   // the answer's press stays ours
        return;
    }
    if (!b.showing)
    {
        {
            std::lock_guard l(g_boxMutex);
            if (g_boxQueue.empty()) return;
            b.box = std::move(g_boxQueue.front());
            g_boxQueue.clear();
        }
        const size_t n = b.box.buttons.size();
        const bool shortLabels = n == 2 && b.box.buttons[0].size() <= 8 && b.box.buttons[1].size() <= 8;
        b.style = n == 1 ? BoxStyle::Information : shortLabels ? BoxStyle::Confirmation : BoxStyle::List;
        b.cursor = std::clamp(b.box.active, 0, int(n) - 1);
        b.showing = true;
        g_boxOwns = true;
        ShowBox(ctx, base, b);
        return;
    }
    int choice = -1;
    switch (b.style)
    {
    case BoxStyle::Information:
        if (pressed & (kA | kB | kStart)) choice = 0;
        break;
    case BoxStyle::Confirmation:
        if (pressed & (kA | kStart)) choice = 0;
        else if (pressed & kB) choice = 1;
        break;
    case BoxStyle::List:
    {
        const int n = int(b.box.buttons.size());
        if (pressed & (kUp | kDown))
        {
            b.cursor = (b.cursor + ((pressed & kUp) ? n - 1 : 1)) % n;
            MenuSound(ctx, base, kSoundCursor);
            UpdateBox(ctx, base, b);
        }
        if (pressed & (kA | kStart)) choice = b.cursor;
        else if ((pressed & kB) && b.box.cancel >= 0) choice = b.box.cancel;
        break;
    }
    }
    if (choice < 0) return;
    MenuSound(ctx, base, choice == b.box.cancel ? kSoundCancel : kSoundDecide);
    HideDialog(ctx, base);
    b.showing = false;
    b.releasing = true;
    std::fprintf(stderr, "[sys-ui] message box answered: %d '%s'\n", choice, b.box.buttons[size_t(choice)].c_str());
    const std::function<void(int)> done = std::move(b.box.done);
    if (done) done(choice);
}

// Confirm: an empty name takes the suggestion; the new profile signs in.
void Finish(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    std::string name = profiles::Normalize(ui.name);
    if (name.empty()) name = SuggestedName();
    if (!profiles::ValidName(name)) { ui.note = "Use letters, digits and spaces."; return; }
    const int index = profiles::Create(name);
    if (index < 0) { ui.note = "That name is already used."; return; }
    MenuSound(ctx, base, kSoundDecide);
    SignInAndClose(ctx, base, ui, index);
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    BoxFrame(ctx, base);
    Ui& ui = g_ui;
    const uint16_t pressed = Pressed(ui);
    if (ui.releasing)
    {
        if (ui.prev == 0)
        {
            ui.releasing = false;
            g_owns = false;
            if (ui.continueAfter) HostPulseStart();
        }
        return;
    }
    if (g_signInRequest.exchange(false) && !profiles::SignedIn())
    {
        // The game asks itself (XamShowSigninUI): its sign-in states draw nothing, so the last
        // used profile signs in. (The title's START no longer gets there: see TitleLogo.)
        const int index = profiles::LastIndex();
        Notify(9, 1);
        profiles::SignIn(index);
        Notify(0x0000000Au, 1);
        Notify(9, 0);
        std::fprintf(stderr, "[sys-ui] the game asked for a sign-in: last profile\n");
        return;
    }
    if (ui.screen == Screen::None)
    {
        if (g_switchProfile.load() && TitleLogo())
        {
            // SWITCH PROFILE: the game is back on the title; the profile signs out here (not in
            // the middle of the mode change) and Select Profile opens by itself.
            g_switchProfile = false;
            profiles::SignOut();
            Notify(0x0000000Au, 1);                      // XN_SYS_SIGNINCHANGED, user 0
            Open(ui, profiles::LastIndex());
            Notify(9, 1);
            std::fprintf(stderr, "[sys-ui] switch profile: Select Profile\n");
            return;
        }
        if (!profiles::SignedIn() && TitleLogo() && (pressed & (kStart | kA)))
        {
            Open(ui, profiles::LastIndex());
            MenuSound(ctx, base, kSoundDecide);
            Notify(9, 1);                                // XN_SYS_UI opened
            std::fprintf(stderr, "[sys-ui] Select Profile\n");
        }
        return;
    }
    if (ui.setupFrames > 0)
    {
        if (--ui.setupFrames == 0) Draw(ctx, base, ui);
        return;
    }

    std::vector<uint32_t> chars;
    {
        std::lock_guard l(g_charMutex);
        chars.swap(g_chars);
    }
    if (ui.screen == Screen::Profiles)
    {
        if (ui.settle > 0 && --ui.settle == 0) ui.shownCursor = -1;   // draw the cursor once more
        const int rows = int(ui.rows.size());
        if (pressed & (kUp | kDown))
        {
            ui.cursor = (ui.cursor + ((pressed & kUp) ? rows - 1 : 1)) % rows;
            MenuSound(ctx, base, kSoundCursor);
        }
        if (pressed & (kA | kStart))
        {
            MenuSound(ctx, base, kSoundDecide);
            if (ui.cursor < int(profiles::List().size())) { SignInAndClose(ctx, base, ui, ui.cursor); return; }
            OpenNameDialog(ctx, base, ui);
            return;
        }
        if (pressed & kB)
        {
            MenuSound(ctx, base, kSoundCancel);
            Close(ctx, base, ui, false);                 // stays on the logo, signed out
            return;
        }
        Draw(ctx, base, ui);
        return;
    }

    // The name dialog: typed text edits the name, ENTER / A / START confirm, ESC / B cancel.
    bool changed = false;
    for (uint32_t c : chars)
    {
        if (c == '\r') { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; continue; }
        if (c == 0x1B) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
        const bool printable = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
        if (c != '\b' && !printable) continue;
        if (ui.suggested) { ui.name.clear(); ui.suggested = false; }
        ui.note.clear();
        if (c == '\b') { if (!ui.name.empty()) ui.name.pop_back(); }
        else if (ui.name.size() >= profiles::kMaxName) ui.note = "A name has at most 15 characters.";
        else if (c != ' ' || (!ui.name.empty() && ui.name.back() != ' ')) ui.name.push_back(char(c));
        changed = true;
    }
    if (pressed & (kA | kStart)) { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; }
    else if (pressed & kB) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
    if (changed) UpdateNameDialog(ctx, base, ui);
}

} // namespace sysui

// ---- Options -> Controller Setup: KEYBOARD mode (KEYBOARD_PLAN.md) ------------------------------
// The screen's artwork has room for three rows, so player 1's BUTTON SETTINGS row carries a mode in
// its value text (menu_doc.cpp): PAD (the game's own button settings) or KEYBOARD, switched with
// LEFT / RIGHT while no other button is held (the game's own change is a held button + LEFT /
// RIGHT). In KEYBOARD mode the button panel's ten slots show each button's key instead of its
// action - the game writes the slots with lib::MenuEntitySetText, so its texts are recorded there
// (the hook below) and put back when the mode or the row is left - and the game sees only UP /
// DOWN on that row. A starts editing in the panel itself: a highlighted slot moves with the
// arrows, LB / RB turn to the second page (UP DOWN LEFT RIGHT START BACK and DEFAULTS), A asks
// for the slot's key (PRESS A KEY; keys then arrive from the window, SysUiOnKey, while the game
// gets neutral input), B or ESC ends editing. No windows open.
namespace keyui {

using namespace keyboard_keys;
constexpr int kButtonSettingsRow = 1;                    // BUTTON SETTINGS in player 1's menu
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010,
                   kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000, kPadB = 0x2000, kDpad = 0x000F;
constexpr int kDefaults = -2, kNone = -1;                 // page 2 cells without a control
const std::string kMenu = "MenuMode007-03-01-1", kValue = "StaticTextMenuItemMode007-03-02-1",
                  kHelp = "StaticTextMode007-03-02-1";
// Panel slot StaticTextMode007-03-SS-1-1 (SS = 01..10): the left column top down, then the right
// (MEASURED on screen 2026-10-14). Slot = column * 5 + row.
const int kPage[2][10] = {
    { LB, LT, X, A, LS, RB, RT, Y, B, RS },
    { Up, Down, Left, Right, kNone, Start, Back, kNone, kNone, kDefaults },
};

struct State
{
    bool here = false;                                   // Controller Setup was up last frame
    bool keyboard = false;                               // BUTTON SETTINGS shows KEYBOARD
    bool editing = false, capturing = false, releasing = false;
    int page = 0, slot = 0;
    std::string value;                                   // the value text as last written
    std::string gameText[10];                            // the game's slot texts (icon + action)
    std::string shown[10];                               // the slots while keys are shown
    bool showing = false;
    std::string help;                                    // the help line while keys are shown
    uint16_t prevButtons = 0;
    bool prevEsc = false;
} g_state;
std::atomic<bool> g_capture{ false };
std::atomic<uint16_t> g_gameMask{ 0xFFFF };              // the buttons the game may see (XamInputGetState)
std::mutex g_keyMutex;
std::vector<int> g_keys;                                 // keys pressed since the last frame

int SlotOf(const std::string& widget)
{
    // "StaticTextMode007-03-SS-1-1"
    static const std::string prefix = "StaticTextMode007-03-", suffix = "-1-1";
    if (widget.size() != prefix.size() + 2 + suffix.size() || widget.compare(0, prefix.size(), prefix) != 0 ||
        widget.compare(prefix.size() + 2, suffix.size(), suffix) != 0)
        return -1;
    const int s = (widget[prefix.size()] - '0') * 10 + (widget[prefix.size() + 1] - '0');
    return s >= 1 && s <= 10 ? s - 1 : -1;
}
std::string SlotWidget(int slot) { return "StaticTextMode007-03-" + sysui::Two(slot + 1) + "-1-1"; }

// A key's name as short as the panel's slots need.
std::string ShortKeyName(int vk)
{
    switch (vk)
    {
    case VK_BACK: return "BKSP";
    case VK_PRIOR: return "PG UP";
    case VK_NEXT: return "PG DN";
    default: return KeyName(vk);
    }
}

// What a slot shows: on page 1 the game's icon part ("~white~<icon> ") and the key, on page 2 the
// control's name and its key (just PRESS A KEY while it waits: the help line names the control).
// The key is in the panel's colour (~default~), the slot being edited in yellow - the colours the
// game's text system knows: red green yellow white gray blue lightgray default; a text has to
// start with one (~white~, as the game's own) for the others to be read, and two tags must not
// touch ("~~" breaks them: the whole text then shows literally).
std::string SlotText(const State& st, int slot)
{
    const int control = kPage[st.page][slot];
    if (control == kNone) return "";
    const bool selected = st.editing && slot == st.slot;
    const std::string colour = selected ? "~yellow~" : "~default~";
    if (control == kDefaults) return "~white~ " + colour + "DEFAULTS";
    const bool asking = st.capturing && selected;
    const std::string key = asking ? std::string("PRESS A KEY") : ShortKeyName(Key(control));
    if (st.page == 1)
        return asking ? "~white~ " + colour + key : std::string("~white~") + ControlName(control) + " " + colour + key;
    const std::string& game = st.gameText[slot];
    const size_t at = game.find("~default~");
    return (at == std::string::npos ? std::string("~white~") : game.substr(0, at)) + colour + key;
}

std::string HelpText(const State& st)
{
    const int control = kPage[st.page][st.slot];
    if (st.capturing) return std::string("Press a key for ") + ControlName(control) + ".   ESC: cancel";
    if (st.editing && control == kDefaults) return "A: restore the default keys   B: done";
    if (st.editing) return "A: change   LB / RB: page   B: done";
    return "A: edit keys";
}

std::string CurrentFrame(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8256C100(ctx, base);                              // lib::MenuGetCurrentFrameName
    return GuestString(base, ctx.r3.u32, 64);
}
int MenuCursor(PPCContext& ctx, uint8_t* base, const std::string& menu)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, menu);
    sub_8256C968(ctx, base);                              // lib::MenuContainerGetCursor(menu)
    return static_cast<int32_t>(ctx.r3.u32);
}
// The game's own help text of BUTTON SETTINGS (textid360 4,635), back after the keyboard hints.
void RestoreHelp(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 4;
    ctx.r4.u64 = 635;
    ctx.r5.u64 = GuestStr(base, kHelp);
    sub_8256C7A8(ctx, base);                              // lib::MenuEntitySetTextID(table, id, widget)
}

bool Foreground()
{
    DWORD process = 0;
    const HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &process);
    return w && process == GetCurrentProcessId();
}
bool AnyKeyDown()
{
    for (int vk = 8; vk < 256; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

// The next slot from `slot` in a direction (page 2 skips its empty cells; rows and columns wrap).
int Move(int page, int slot, uint16_t dir)
{
    int col = slot / 5, row = slot % 5;
    for (int tries = 0; tries < 10; ++tries)
    {
        if (dir == kPadUp) row = (row + 4) % 5;
        else if (dir == kPadDown) row = (row + 1) % 5;
        else col ^= 1;
        if (kPage[page][col * 5 + row] != kNone) return col * 5 + row;
        if (dir & (kPadLeft | kPadRight)) dir = kPadUp;   // an empty cell across: go up to a used one
    }
    return slot;
}

void Edit(PPCContext& ctx, uint8_t* base, State& st, uint16_t pressed, bool esc)
{
    if (st.capturing)
    {
        std::vector<int> keys;
        {
            std::lock_guard l(g_keyMutex);
            keys.swap(g_keys);
        }
        bool done = (pressed & kPadB) != 0;               // a controller's B cancels too
        for (int vk : keys)
        {
            if (done) break;
            if (vk == VK_ESCAPE) { done = true; break; }
            if (!Assignable(vk)) continue;
            Assign(kPage[st.page][st.slot], vk);
            MenuSound(ctx, base, kSoundDecide);
            done = true;
        }
        if (!done) return;
        st.capturing = false;
        st.releasing = true;                              // g_capture stays until the keys are up
        return;
    }
    if (pressed & (kPadB) || esc)
    {
        st.editing = false;
        MenuSound(ctx, base, kSoundCancel);
        return;
    }
    if (pressed & (kPadLB | kPadRB))
    {
        st.page ^= 1;
        st.slot = 0;
        MenuSound(ctx, base, kSoundCursor);
        return;
    }
    for (const uint16_t dir : { kPadUp, kPadDown, kPadLeft, kPadRight })
        if (pressed & dir)
        {
            st.slot = Move(st.page, st.slot, dir);
            MenuSound(ctx, base, kSoundCursor);
            return;
        }
    if (pressed & (kPadA | kPadStart))
    {
        const int control = kPage[st.page][st.slot];
        MenuSound(ctx, base, kSoundDecide);
        if (control == kDefaults) { RestoreDefaults(); return; }
        st.capturing = true;
        {
            std::lock_guard l(g_keyMutex);
            g_keys.clear();
        }
        g_capture = true;
    }
}

void Draw(PPCContext& ctx, uint8_t* base, State& st, bool keys)
{
    if (keys)
    {
        for (int s = 0; s < 10; ++s)
        {
            const std::string text = SlotText(st, s);
            if (!st.showing || st.shown[s] != text) MenuSetText(ctx, base, SlotWidget(s), text);
            st.shown[s] = text;
        }
        const std::string help = HelpText(st);
        if (!st.showing || help != st.help) MenuSetText(ctx, base, kHelp, help);
        st.help = help;
        st.showing = true;
    }
    else if (st.showing)
    {
        for (int s = 0; s < 10; ++s)
            if (!st.gameText[s].empty()) MenuSetText(ctx, base, SlotWidget(s), st.gameText[s]);
        RestoreHelp(ctx, base);
        st.showing = false;
    }
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    State& st = g_state;
    if (CurrentFrame(ctx, base) != "FrameControlerSetup")
    {
        if (st.here) std::fprintf(stderr, "[keyboard] controller setup closed\n");
        State fresh;                                      // the game's slot texts stay recorded
        std::copy(std::begin(st.gameText), std::end(st.gameText), std::begin(fresh.gameText));
        st = fresh;
        g_capture = false;
        g_gameMask = 0xFFFF;
        return;
    }
    const uint16_t buttons = HostControllerButtons();
    const bool esc = Foreground() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!st.here)
    {
        st.here = true;                                   // every visit starts on PAD
        st.prevButtons = buttons;                         // the A that opened the screen is not ours
        st.prevEsc = esc;
    }
    const uint16_t pressed = buttons & ~st.prevButtons;
    const bool escPressed = esc && !st.prevEsc;
    st.prevButtons = buttons;
    st.prevEsc = esc;

    if (st.releasing)
    {
        // The new key is still down: it acts as its button only once released.
        if (!AnyKeyDown() && !(buttons & 0xF3FF)) { st.releasing = false; g_capture = false; st.prevButtons = 0; }
    }
    else if (st.editing) Edit(ctx, base, st, pressed, escPressed);
    else if (MenuCursor(ctx, base, kMenu) == kButtonSettingsRow)
    {
        // Not while a button or trigger is held: that is the game's own change of its action.
        if ((pressed & (kPadLeft | kPadRight)) && !(buttons & ~kDpad) && !HostTriggerHeld())
        {
            st.keyboard = !st.keyboard;
            MenuSound(ctx, base, kSoundCursor);
        }
        else if (st.keyboard && (pressed & kPadA))
        {
            st.editing = true;
            st.page = 0;
            st.slot = 0;
            MenuSound(ctx, base, kSoundDecide);
        }
    }
    const bool onRow = st.editing || MenuCursor(ctx, base, kMenu) == kButtonSettingsRow;
    g_gameMask = st.editing ? uint16_t(0) : (onRow && st.keyboard) ? uint16_t(0x0003) : uint16_t(0xFFFF);

    const std::string value = st.keyboard ? "KEYBOARD" : "PAD";
    if (value != st.value)
    {
        MenuSetText(ctx, base, kValue, value);
        st.value = value;
    }
    Draw(ctx, base, st, onRow && st.keyboard);
}

} // namespace keyui

// Runs on the game's main thread once per frame (end of VdSwap, registers restored after): the
// system UI, and a diagnostics trigger - C:/ttt2/port/ui.trigger holding a frame name switches
// the menu to that frame.
namespace sideselect {
// Phase 9 (player_side_select.h): PLAYER SIDE SELECT between OFFLINE MODE and character select.
// The PRACTICE / OFFLINEENTRY request of the OFFLINE MODE items is held back (sub_825ED4F0 hook
// below), FramePlayerSideSelect (menu_doc.cpp) is shown, and on A the same request goes out
// unchanged once every pad is released. TTT2_PLAYER_SIDE_SELECT=0 turns the screen off.
player_side_select::SideSelectFlow flow;
std::atomic<bool> owns{false};
bool loaded = false;
uint32_t holderCell = 0;                      // guest cell holding the ModeManager impl pointer
uint32_t practiceOption = 0xFFFFFFFFu;        // BATTLE POSITION when PRACTICE was held back

// Phase 10: TTT2_PRACTICE_BATTLE_POSITION=0 keeps PRACTICE exactly as requested (side then only
// moves the camera / HUD in battle, the Phase 9 behaviour).
bool PracticeBattlePositionLink()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_PRACTICE_BATTLE_POSITION"); return !(e && std::strcmp(e, "0") == 0); }();
    return on;
}

// lib::GameOptionGetBattlePosition / SetBattlePosition natives (options+16). The setter's port
// hook (INPUT DELAY from the Game Options widget) is bypassed: only the option itself is stored.
uint32_t ReadBattlePosition(PPCContext& ctx, uint8_t* base)
{
    const PPCContext saved = ctx;
    uint32_t value = 0xFFFFFFFFu;
    {
        GuestFrame frame(ctx, base, 0x100);
        __imp__sub_82374738(ctx, base);
        value = ctx.r3.u32;
    }
    ctx = saved;
    return value;
}
void WriteBattlePosition(PPCContext& ctx, uint8_t* base, uint32_t value)
{
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = value;
        __imp__sub_82374700(ctx, base);
    }
    ctx = saved;
}

bool Enabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_PLAYER_SIDE_SELECT"); return !(e && std::strcmp(e, "0") == 0); }();
    return on;
}

// The last confirmed side, in the save root the launcher isolates (never the user's saves).
std::string PrefPath()
{
    const char* root = std::getenv("TTT2_SAVE_ROOT");
    return (root && *root) ? std::string(root) + "\\player_side.ini" : std::string();
}
void LoadPreference()
{
    if (loaded) return;
    loaded = true;
    const std::string path = PrefPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char line[64] = {};
    std::fread(line, 1, sizeof(line) - 1, f);
    std::fclose(f);
    if (std::strstr(line, "Side=Right")) flow.saved = flow.cursor = player_side_select::Side::Right;
}
void SavePreference()
{
    const std::string path = PrefPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "[PlayerSide]\r\nSide=%s\r\n", flow.saved == player_side_select::Side::Right ? "Right" : "Left");
    std::fclose(f);
}

void HeldButtons(uint16_t held[4])
{
    for (uint32_t u = 0; u < 4; ++u) held[u] = HostControllerButtonsForUser(u);
}

void Draw(PPCContext& ctx, uint8_t* base)
{
    const bool right = flow.cursor == player_side_select::Side::Right;
    MenuMovieVisible(ctx, base, "rmc_icon_menu_L", false);
    MenuTextHide(ctx, base, "StaticIconPlayerSideSelect", false);
    MenuSetText(ctx, base, "StaticIconPlayerSideSelect", "PLAYER SIDE SELECT");
    MenuSetText(ctx, base, "StaticTelopPlayerSideSelect", "UP/DOWN: SELECT   A: CONFIRM   B: BACK");
    MenuTelopTarget(ctx, base, "StaticTelopPlayerSideSelect");
    MenuSetCursor(ctx, base, "MenuPlayerSideSelect", right ? 1 : 0);
    MenuMoviePlay(ctx, base, "amc_menu_bar_sentaku_L",
                  right ? "menu_bar_sentaku_solo_02" : "menu_bar_sentaku_solo_01", true);
    for (int i = 0; i < 2; ++i) {
        const std::string suffix = i ? "_02" : "_01";
        MenuSetText(ctx, base, "StaticValuePlayerSideSelect" + suffix, (right == bool(i)) ? "SELECTED" : "");
        // The long Phase 9 description overlaid the row's SELECTED marker. Keep
        // the original game's compact two-line menu labels unobstructed.
        MenuTextHide(ctx, base, "StaticPlayerSideSelect" + suffix, true);
        MenuMoviePlay(ctx, base, "mc_icon_menu_S_L" + suffix,
                      right == bool(i) ? "icon_s_006" : "icon_s_006_x", true);
    }
}

// From the sub_825ED4F0 hook: true when the request is held back for the side select.
bool Intercept(PPCContext& ctx, uint8_t* base)
{
    using namespace player_side_select;
    if (!Enabled() || !IsSideSelectMode(ctx.r4.u32) || flow.state != FlowState::Idle) return false;
    if (XSessionAnyActive()) return false;
    const PPCContext saved = ctx;
    bool ok = false;
    {
        // Only the OFFLINE MODE items (rematch and result menus request modes too), and only when
        // the main menu document carries the side select frame.
        ok = keyui::CurrentFrame(ctx, base) == "FrameMode002-01" &&
             FindWidget(ctx, base, GuestStr(base, "MenuPlayerSideSelect")) != 0;
    }
    ctx = saved;
    const uint32_t impl = ok ? GRead32(base, ctx.r3.u32) : 0;
    if (!ok || !impl) return false;
    LoadPreference();
    // The script's ModeManager wrapper is not kept; the impl it points at is the singleton.
    if (!holderCell) holderCell = GuestCopy(base, "\0\0\0\0", 4);
    GWrite32(base, holderCell, impl);
    ModeRequest request{holderCell, ctx.r4.u32, ctx.r5.u32};
    if (!flow.Request(request, true, false, true)) return false;
    practiceOption = 0xFFFFFFFFu;
    if (request.mode == kModePractice && PracticeBattlePositionLink()) {
        // The screen opens on the side the game's own BATTLE POSITION already gives.
        practiceOption = ReadBattlePosition(ctx, base);
        flow.cursor = SideForBattlePosition(practiceOption, flow.cursor);
    }
    owns = true;
    std::fprintf(stderr, "[side-select] mode %u (arg %u) held for PLAYER SIDE SELECT\n", request.mode, request.arg);
    return true;
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    using namespace player_side_select;
    ttt2_side_camera_diag::MaintainSidePolicy(GetTickCount64());
    if (flow.state == FlowState::Idle) { owns = false; return; }
    if (XSessionAnyActive()) {                 // never on line: drop the screen and the request
        const Side keep = flow.saved;
        flow = SideSelectFlow{};
        flow.saved = flow.cursor = keep;
        owns = false;
        return;
    }
    uint16_t held[4];
    HeldButtons(held);
    if (flow.state == FlowState::Opening) {
        SwitchFrame(ctx, base, "FramePlayerSideSelect");
        flow.BeginOpen(held);
        return;
    }
    switch (flow.Poll(held)) {
    case FlowEvent::Opened:
        Draw(ctx, base);
        break;
    case FlowEvent::Redraw:
        Draw(ctx, base);
        MenuSound(ctx, base, kSoundCursor);
        break;
    case FlowEvent::Confirmed: {
        uint32_t teamHint = 0xFFFFFFFFu;
        if (flow.pending.mode == kModePractice && PracticeBattlePositionLink()) {
            // Put the player on the chosen side the way Game Options does it: the option and the
            // argument the game derives from it. Skipped (request untouched) unless the held
            // argument is exactly the game's own derivation of the current option.
            const PracticePositionPlan plan = PlanPracticeBattlePosition(practiceOption, flow.pending.arg, flow.saved);
            if (plan.apply) {
                WriteBattlePosition(ctx, base, plan.position);
                std::fprintf(stderr, "[side-select] PRACTICE: BATTLE POSITION %u -> %u, request arg %u -> %u (game mapping)\n",
                             practiceOption, plan.position, flow.pending.arg, plan.arg);
                flow.pending.arg = plan.arg;
                teamHint = plan.position;   // LEFT -> team 0 (1P), RIGHT -> team 1 (2P)
            } else {
                std::fprintf(stderr, "[side-select] PRACTICE: option %u / arg %u not the known mapping; request left unchanged\n",
                             practiceOption, flow.pending.arg);
            }
        }
        ttt2_side_camera_diag::SetPlayerSidePreference(
            flow.saved == Side::Right ? ttt2_side_camera_diag::PlayerScreenSide::Right
                                      : ttt2_side_camera_diag::PlayerScreenSide::Left,
            flow.confirmUser, teamHint);
        SavePreference();
        SwitchFrame(ctx, base, "FrameMode002-01");
        MenuSound(ctx, base, kSoundDecide);
        std::fprintf(stderr, "[side-select] %s confirmed by XAM user %u\n",
                     flow.saved == Side::Right ? "RIGHT" : "LEFT", flow.confirmUser);
        break;
    }
    case FlowEvent::Cancelled:
        SwitchFrame(ctx, base, "FrameMode002-01");
        MenuSound(ctx, base, kSoundCancel);
        break;
    case FlowEvent::Dispatch: {
        // The held request. Its argument is the menu's own, except PRACTICE after a confirmed
        // BATTLE POSITION change (PlanPracticeBattlePosition: 1 or 2, never 0). Forcing arg 0
        // (no human side) is what sent the 2026-10-25 trial into the attract demo.
        const PPCContext saved = ctx;
        {
            GuestFrame frame(ctx, base, 0x100);
            ctx.r3.u64 = flow.pending.holder;
            ctx.r4.u64 = flow.pending.mode;
            ctx.r5.u64 = flow.pending.arg;
            __imp__sub_825ED4F0(ctx, base);
        }
        ctx = saved;
        std::fprintf(stderr, "[side-select] mode %u requested\n", flow.pending.mode);
        break;
    }
    default:
        break;
    }
    owns = flow.OwnsInput();
}
} // namespace sideselect

namespace roomside {
// Phase 11 (player_side_select.h RoomSideWindow): PLAYER SIDE SELECT once both players are READY.
// The battle-ready hold (sub_826BF778, below; BattleReadyHold) records its countdown each frame;
// here, once per frame, the window opens in the game's dialog window, counts the 10 s down and ends
// with the hold's 10 s (the side becomes this client's local view; `applied` lets the hold go) or
// with the hold left (nothing kept). TTT2_ONLINE_SIDE_SELECT=0 turns it off.
player_side_select::RoomSideWindow window;
player_side_select::BattleReadyHold hold;       // the game thread's (sub_826BF778)
std::atomic<bool> applied{false};               // this hold's side is set
std::atomic<bool> owns{false};
bool releasing = false;
bool loaded = false;
std::mutex viewMutex;
player_side_select::RoomCountdownView latest;
int64_t latestMs = 0;

bool Enabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_ONLINE_SIDE_SELECT"); return !(e && std::strcmp(e, "0") == 0); }();
    return on;
}

// The last online side, next to the offline one in the isolated save root.
std::string PrefPath()
{
    const char* root = std::getenv("TTT2_SAVE_ROOT");
    return (root && *root) ? std::string(root) + "\\player_side_online.ini" : std::string();
}
void LoadPreference()
{
    if (loaded) return;
    loaded = true;
    const std::string path = PrefPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char line[64] = {};
    std::fread(line, 1, sizeof(line) - 1, f);
    std::fclose(f);
    if (std::strstr(line, "Side=")) {
        window.havePreference = true;
        window.preference = std::strstr(line, "Side=Right") ? player_side_select::Side::Right : player_side_select::Side::Left;
    }
}
void SavePreference()
{
    const std::string path = PrefPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "[PlayerSideOnline]\r\nSide=%s\r\n", window.preference == player_side_select::Side::Right ? "Right" : "Left");
    std::fclose(f);
}

void Record(const player_side_select::RoomCountdownView& view)
{
    std::lock_guard l(viewMutex);
    latest = view;
    latestMs = sysui::NowMs();
}

sysui::BoxState Box()
{
    sysui::BoxState b;
    b.style = sysui::BoxStyle::List;
    b.box.text = "PLAYER SIDE SELECT\nTIME LEFT  " + std::to_string((std::max)(window.seconds, 0)) + "\nUP/DOWN: SELECT   A: CONFIRM";
    b.box.buttons = { "LEFT SIDE  [1P]", "RIGHT SIDE  [2P]" };
    b.cursor = window.cursor == player_side_select::Side::Right ? 1 : 0;
    return b;
}

// Phase 12-D: after an early A, until the game leaves the room for character select (mode 8).
std::atomic<uint32_t> waitBudget{0};            // battle_ready_wait frames still to absorb
bool waiting = false;
int64_t waitingSinceMs = 0;
constexpr int64_t kWaitingBoxMaxMs = 25000;
sysui::BoxState WaitingBox()
{
    sysui::BoxState b;
    b.style = sysui::BoxStyle::List;
    b.box.text = "PLAYER SIDE SELECT\nWAITING FOR THE OTHER PLAYER";
    b.box.buttons = { window.preference == player_side_select::Side::Right ? "RIGHT SIDE  [2P]" : "LEFT SIDE  [1P]" };
    b.cursor = 0;
    return b;
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    using namespace player_side_select;
    if (!Enabled()) return;
    if (waiting) {
        const uint32_t mode = ttt2_side_camera_diag::CurrentGameMode(base);
        if (mode == kModeCharaSelect || !XSessionAnyActive() || sysui::NowMs() - waitingSinceMs > kWaitingBoxMaxMs) {
            waiting = false;
            sysui::HideDialog(ctx, base);
            std::fprintf(stderr, "[side-select] waiting box closed (mode %u, %lld ms)\n", mode,
                         (long long)(sysui::NowMs() - waitingSinceMs));
        }
        return;
    }
    if (releasing) {
        if (HostControllerButtons() == 0) { releasing = false; owns = false; }
        return;
    }
    RoomCountdownView view;
    {
        std::lock_guard l(viewMutex);
        view = latest;
        // The room timer runs every frame in the room; anything older is not the room any more.
        if (sysui::NowMs() - latestMs > 500) view.valid = false;
    }
    if (!window.open && (!XSessionAnyActive() || sysui::g_box.showing)) view.valid = false;
    LoadPreference();
    uint16_t held[4];
    for (uint32_t u = 0; u < 4; ++u) held[u] = HostControllerButtonsForUser(u);
    switch (window.Update(view, held)) {
    case RoomWindowEvent::Opened:
        owns = true;
        sysui::ShowBox(ctx, base, Box());
        MenuSound(ctx, base, kSoundCursor);
        std::fprintf(stderr, "[side-select] room countdown %u: PLAYER SIDE SELECT (%d s, hold counter %u)\n",
                     window.generation, window.seconds, view.counter);
        break;
    case RoomWindowEvent::Tick:
        sysui::UpdateBox(ctx, base, Box());
        break;
    case RoomWindowEvent::Redraw:
        sysui::UpdateBox(ctx, base, Box());
        MenuSound(ctx, base, kSoundCursor);
        break;
    case RoomWindowEvent::Confirmed:
        sysui::UpdateBox(ctx, base, Box());
        MenuSound(ctx, base, kSoundDecide);
        break;
    case RoomWindowEvent::Unconfirmed:
        sysui::UpdateBox(ctx, base, Box());
        MenuSound(ctx, base, kSoundCancel);
        break;
    case RoomWindowEvent::Finished:
        if (window.confirmed) MenuSound(ctx, base, kSoundDecide);
        sysui::UpdateBox(ctx, base, WaitingBox());
        waiting = true;
        waitingSinceMs = sysui::NowMs();
        ttt2_side_camera_diag::SetPlayerSidePreference(
            window.preference == Side::Right ? ttt2_side_camera_diag::PlayerScreenSide::Right
                                             : ttt2_side_camera_diag::PlayerScreenSide::Left,
            window.user, 0xFFFFFFFFu, true);
        // Phase 12: the owner is matched by this machine's online XUID in the battle (side_camera_diag).
        ttt2_side_camera_diag::SetOnlineLocalXuid(xlive::OnlineXuid());
        SavePreference();
        applied = true;
        releasing = true;
        std::fprintf(stderr, "[side-select] room countdown %u ended (%s, %d s left): %s by XAM user %u - local view only\n",
                     window.generation, window.confirmed ? "A confirmed" : "time up, highlighted side", window.seconds,
                     window.preference == Side::Right ? "RIGHT" : "LEFT", window.user);
        {
            // For reference only (this match's table may not be filled yet; the battle decides).
            const ttt2_side_camera_diag::OnlineOwner o =
                ttt2_side_camera_diag::IdentifyOnlineOwner(base, xlive::OnlineXuid());
            std::fprintf(stderr, "[side-owner] at READY: local XUID %016llX; sync engine %u nodes, local node slot %d, node 1 slot %d: %s\n",
                         (unsigned long long)xlive::OnlineXuid(), o.nodes, int(o.slot[0]), int(o.slot[1]),
                         o.team >= 0 ? (o.team == 0 ? "team 0" : "team 1") : o.reason);
        }
        break;
    case RoomWindowEvent::Cancelled:
        sysui::HideDialog(ctx, base);
        releasing = true;
        std::fprintf(stderr, "[side-select] room countdown %u cancelled (state %u, %u members): side select closed\n",
                     window.generation, view.state, view.players);
        break;
    default:
        break;
    }
}
} // namespace roomside

void UiFrame(PPCContext& ctx, uint8_t* base)
{
    sysui::Frame(ctx, base);
    keyui::Frame(ctx, base);
    sideselect::Frame(ctx, base);
    roomside::Frame(ctx, base);
    static uint32_t tick = 0;
    if (++tick % 30) return;
    FILE* f = std::fopen("C:/ttt2/port/ui.trigger", "rb");
    if (!f) return;
    char name[128] = {};
    const size_t n = std::fread(name, 1, sizeof(name) - 1, f);
    std::fclose(f);
    DeleteFileA("C:/ttt2/port/ui.trigger");
    name[n] = 0;
    for (char* p = name; *p; ++p) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    std::fprintf(stderr, "[menu-ext] ui trigger: '%s'\n", name);
    if (*name) SwitchFrame(ctx, base, name);
}
struct UiHookInstall { UiHookInstall() { g_uiFrameHook = UiFrame; } } g_uiHookInstall;
} // namespace

bool SysUiRequestSignIn() { sysui::g_signInRequest = true; return true; }
bool SysUiOwnsInput() { return sysui::g_owns.load() || sysui::g_boxOwns.load() || keyui::g_capture.load() || sideselect::owns.load() || roomside::owns.load(); }
bool SysUiKeyCaptureActive() { return keyui::g_capture.load(); }
uint16_t SysUiGameButtonMask() { return keyui::g_gameMask.load(); }
void SysUiOnKey(int vk)
{
    if (!keyui::g_capture.load()) return;
    std::lock_guard l(keyui::g_keyMutex);
    if (keyui::g_keys.size() < 16) keyui::g_keys.push_back(vk);
}
bool SysUiTitleWantsProfile() { return !profiles::SignedIn() && sysui::TitleLogo(); }
bool SysUiTextEntryActive() { return sysui::g_textEntry.load(); }
bool SysUiShowMessageBox(SysUiMessageBox box)
{
    std::lock_guard l(sysui::g_boxMutex);
    if (sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty() || box.buttons.empty()) return false;
    sysui::g_boxQueue.push_back(std::move(box));
    return true;
}
void SysUiOnChar(uint32_t ch)
{
    if (!sysui::g_textEntry.load()) return;
    std::lock_guard l(sysui::g_charMutex);
    if (sysui::g_chars.size() < 64) sysui::g_chars.push_back(ch);
}

namespace {

menu_doc::PcLists PcLists()
{
    menu_doc::PcLists lists;
    lists.modes = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
    for (const display_settings::Resolution& r : display_settings::Resolutions())
        lists.resolutions.push_back(std::to_string(r.width) + "x" + std::to_string(r.height));
    return lists;
}

// ---- natives -------------------------------------------------------------------------------
// Signature descriptors of the game's own natives (port/tools/xtal_natives.py). Implementations
// use the plain ABI (r3 in, r3 out).
constexpr uint32_t kSetterInt = 0x8203624C;   // void f(int)
constexpr uint32_t kGetterInt = 0x8205B9A4;   // int f()
constexpr uint32_t kGetterBool = 0x820025B8;  // bool f()

PPC_FUNC(NativeSetWindowMode) { (void)base; display_settings::SetMode(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetWindowMode) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetMode()); }
PPC_FUNC(NativeSetResolution) { (void)base; display_settings::SetResolutionIndex(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetResolution) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetResolutionIndex()); }
PPC_FUNC(NativeTrue) { (void)base; ctx.r3.u64 = 1; }

// lib::ExitGameWindow - the exit dialog's lib::WindowGetValue: -1 while it waits, 0 for the
// confirm button (A, YES), otherwise NO. Settings are saved as they change; the guest threads
// are not unwound (TerminateProcess, as for a closed window), so nothing can hang on the way out.
PPC_FUNC(NativeExitGameWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0)
    {
        std::fprintf(stderr, "[menu] exit confirmed - quitting\n");
        std::fflush(stderr);
        std::fflush(stdout);
        LogDrain(); TerminateProcess(GetCurrentProcess(), 0);
    }
}

// lib::SwitchProfileWindow - the SWITCH PROFILE dialog's lib::WindowGetValue: 0 (YES) arms the
// sign-out for when the title is back (the script then requests the SIGNOUT mode).
PPC_FUNC(NativeSwitchProfileWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0 && !sysui::g_switchProfile.exchange(true))
        std::fprintf(stderr, "[menu] switch profile confirmed\n");
}

struct HostNative { const char* name; uint32_t signature; PPCFunc* fn; };
const HostNative kNatives[] = {
    // main menu: the 9th item (STORE in the game, EXIT here) is shown when this is true
    { "IsExitMenuItemVisible", kGetterBool, NativeTrue },
    // the PC rows' OnTrigger is 3D DEPTH's with renamed natives: the capability gate, the
    // preview call and the store call (both apply live)
    { "DisplaySettingIsPCDisplayCapable", kGetterBool, NativeTrue },
    { "MenuOptionDisplayWindowMode", kSetterInt, NativeSetWindowMode },
    { "DisplaySettingSetWinMode", kSetterInt, NativeSetWindowMode },
    { "MenuOptionDisplayResolution", kSetterInt, NativeSetResolution },
    { "DisplaySettingSetDispRes", kSetterInt, NativeSetResolution },
    { "DisplaySettingGetWindowMode", kGetterInt, NativeGetWindowMode },
    { "DisplaySettingGetResolution", kGetterInt, NativeGetResolution },
    // the exit confirmation's answer
    { "ExitGameWindow", kGetterInt, NativeExitGameWindow },
    // the switch-profile confirmation's answer
    { "SwitchProfileWindow", kGetterInt, NativeSwitchProfileWindow },
};
// Guest code addresses routed to the host natives: the interior of the development menu loader
// sub_823450B0 (never called in retail; nothing indirect-calls into a function's middle).
constexpr uint32_t kNativeSlotBase = 0x823450C0;

// Define the natives in `lib` exactly as sub_823756A8 defines the game's own:
// sub_82A802F0(&native, signature, &impl), sub_82A6F5E8(&id, name, length, 0),
// sub_82A69330(&copy, &native), sub_822501A0(lib, &id, &copy), sub_82A69360(&native).
void RegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t lib)
{
    GuestFrame frame(ctx, base, 0x200);
    const uint32_t sp = frame.sp();
    const uint32_t implCell = sp + 0x80, native = sp + 0xA0, id = sp + 0xB0, copy = sp + 0xC0;
    for (size_t i = 0; i < sizeof(kNatives) / sizeof(kNatives[0]); ++i)
    {
        const HostNative& n = kNatives[i];
        const uint32_t slot = kNativeSlotBase + uint32_t(i) * 4;
        PPC_LOOKUP_FUNC(base, slot) = n.fn;
        GWrite32(base, implCell, slot);
        ctx.r3.u64 = native; ctx.r4.u64 = n.signature; ctx.r5.u64 = implCell;
        sub_82A802F0(ctx, base);
        ctx.r3.u64 = id; ctx.r4.u64 = GuestCString(base, n.name); ctx.r5.u64 = std::strlen(n.name); ctx.r6.u64 = 0;
        sub_82A6F5E8(ctx, base);
        ctx.r3.u64 = copy; ctx.r4.u64 = native;
        sub_82A69330(ctx, base);
        ctx.r5.u64 = ctx.r3.u32; ctx.r3.u64 = lib; ctx.r4.u64 = id;
        sub_822501A0(ctx, base);
        ctx.r3.u64 = native;
        sub_82A69360(ctx, base);
    }
    std::fprintf(stderr, "[menu] registered %zu PC natives\n", sizeof(kNatives) / sizeof(kNatives[0]));
}

// The exit and switch-profile confirmations (lib::WindowShowConfirmation(id) in the cloned
// dialogs). The window looks up its button labels right after its message: text 0x3D8 "OK" and
// 0x3DC "CANCEL" (MEASURED 2026-10-12); for these dialogs they read YES / NO.
constexpr uint32_t kExitTextId = 0x7F01, kSwitchTextId = 0x7F04, kOkTextId = 0x03D8, kCancelTextId = 0x03DC;
std::atomic<int> g_exitLabelsPending{ 0 };
} // namespace

// ---- hooks ---------------------------------------------------------------------------------

// Options natives registration; the PC natives are added right after, into the same `lib`:
// sub_823756A8 takes it as [sub_82582860(sub_82254450()) + 56] (script environment singleton).
PPC_FUNC(sub_823756A8)
{
    __imp__sub_823756A8(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        sub_82254450(ctx, base);
        sub_82582860(ctx, base);
    }
    const uint32_t lib = GRead32(base, ctx.r3.u32 + 56);
    RegisterNatives(ctx, base, lib);
    ctx = saved;
}

// lib::MenuSelectValueSetCursor(cursor, name): when Display Settings initialises MOTION BLUR's
// value (on open and on DEFAULT), the PC rows get their current values (cursor at widget+368); when
// Game Options initialises BATTLE POSITION's, INPUT DELAY gets its own (OFF = 0, ON = 1).
PPC_FUNC(sub_8256CD28)
{
    const uint32_t nameAddr = ctx.r4.u32;
    __imp__sub_8256CD28(ctx, base);
    const std::string name = GuestString(base, nameAddr, 64);
    const bool display = name == "SelectValueMenuItemMode007-04-02", game = name == "SelectValueMenuItemMode007-01-05";
    if (!display && !game) return;
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t modeName = GuestCString(base, "SelectValueMenuItemMode007-04-06");
        static const uint32_t resName = GuestCString(base, "SelectValueMenuItemMode007-04-07");
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (display)
        {
            if (const uint32_t w = FindWidget(ctx, base, modeName)) GWrite32(base, w + 368, uint32_t(display_settings::GetMode()));
            if (const uint32_t w = FindWidget(ctx, base, resName))
                GWrite32(base, w + 368, uint32_t(display_settings::GetResolutionIndex()));
        }
        else if (const uint32_t w = FindWidget(ctx, base, delayName))
            GWrite32(base, w + 368, display_settings::GetInputDelayOn() ? 1u : 0u);
    }
    ctx = saved;
}

// lib::GameOptionSetBattlePosition (impl sub_82374700): Game Options stores its rows as it closes,
// BATTLE POSITION last - INPUT DELAY's cursor is stored with it.
PPC_FUNC(sub_82374700)
{
    __imp__sub_82374700(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (const uint32_t w = FindWidget(ctx, base, delayName))
            display_settings::SetInputDelayOn(GRead32(base, w + 368) == 1);
    }
    ctx = saved;
}

// Player Match state_battle_ready (sub_826BF778): room +40 once both fighters are READY, called each
// frame by the room update (sub_826BED20) until it moves on to the synchronised states. Phase 11-D
// holds it for the side select (player_side_select.h BattleReadyHold): not run while held, so room
// +40 stays here and the game's battle sync starts only after the 10 s. The room itself, its READY
// countdown and its packets are not touched. Off with TTT2_ONLINE_SIDE_SELECT=0 or outside an online
// session.
PPC_FUNC(sub_826BF778)
{
    const uint32_t room = ctx.r3.u32;
    if (roomside::Enabled() && XSessionAnyActive())
    {
        using namespace player_side_select;
        const HoldStep step = roomside::hold.Update(room, sysui::NowMs(), roomside::applied.load());
        if (step == HoldStep::Start)
        {
            roomside::applied = false;
            std::fprintf(stderr, "[side-select] both READY: battle ready held for PLAYER SIDE SELECT (10 s, hold %u)\n",
                         roomside::hold.holds);
        }
        ttt2_side_camera_diag::PlayerMatchRoomCountdown c;
        const bool read = ttt2_side_camera_diag::ReadPlayerMatchRoomCountdown(base, room, c);
        RoomCountdownView view;
        view.valid = true;
        view.state = step == HoldStep::Release ? kRoomStateBattle : kRoomStateCountdown;
        view.counter = roomside::hold.Counter();
        view.players = read ? c.players : 2u;
        view.nativeSide = read ? c.nativeSide : 0u;
        roomside::Record(view);
        if (step != HoldStep::Release)
        {
            ctx.r3.u64 = 0;
            return;
        }
        roomside::waitBudget = BattleWaitExtension(roomside::hold.frames);
        std::fprintf(stderr, "[side-select] battle ready released after %u frames (side %s) - the game's battle sync goes on"
                             " (its peer wait extended by %u frames)\n",
                     roomside::hold.frames, roomside::applied.load() ? "applied" : "NOT applied: window did not finish",
                     roomside::waitBudget.load());
    }
    __imp__sub_826BF778(ctx, base);
}

// Player Match battle_ready_wait (sub_826BEF60, the state after state_battle_ready): room +180
// counts frames to 600 while sub_826B8640 waits for the peer, then it goes on anyway. Phase 12-D: a
// client that confirmed its side early has the frames it did not hold added (BattleWaitExtension).
PPC_FUNC(sub_826BEF60)
{
    const uint32_t room = ctx.r3.u32;
    uint32_t budget = roomside::waitBudget.load();
    if (budget != 0u)
    {
        uint32_t counter = GRead32(base, room + 180);
        if (player_side_select::AbsorbBattleWaitFrame(counter, budget))
        {
            GWrite32(base, room + 180, counter);
            roomside::waitBudget = budget;
            if (budget == 0u)
                std::fprintf(stderr, "[side-select] peer wait extension used up (the game's own 600 frames go on)\n");
        }
        else if (counter > player_side_select::kRoomSideWindowFrames)
            roomside::waitBudget = 0;
    }
    __imp__sub_826BEF60(ctx, base);
}

// ModeManager.requestChangeMode as Xtal binds it (sub_825ED4F0: wrapper r3, mode r4, arg r5 ->
// (*wrapper)->vtable[1](mode, arg, 0); method table sub_825EDC18). PRACTICE (18, arg = the
// BATTLE POSITION option) and OFFLINEENTRY (17) from OFFLINE MODE wait for PLAYER SIDE SELECT;
// other modes than an offline battle's drop the side preference.
PPC_FUNC(sub_825ED4F0)
{
    if (sideselect::Intercept(ctx, base))
    {
        ctx.r3.u64 = 1;
        return;
    }
    const bool online = ttt2_side_camera_diag::IsOnlineSidePolicyActive();
    if (online ? !player_side_select::KeepsOnlineSidePreference(ctx.r4.u32)
               : !player_side_select::KeepsSidePreference(ctx.r4.u32)) {
        if (online) std::fprintf(stderr, "[side-select] mode %u requested: the room's side choice is dropped\n", ctx.r4.u32);
        ttt2_side_camera_diag::ClearPlayerSidePreference();
    }
    __imp__sub_825ED4F0(ctx, base);
}

// lib::GameOptionResetToDefaults (impl sub_823747C0): Game Options' DEFAULT resets INPUT DELAY too
// (its cursor follows when the page re-initialises BATTLE POSITION's).
PPC_FUNC(sub_823747C0)
{
    __imp__sub_823747C0(ctx, base);
    display_settings::SetInputDelayOn(false);
}

// TitleMode::Impl::state_title_main: the logo waiting for START (sub_82622B70); impl+40 counts the
// frames in the state and the attract demo starts when it reaches impl+128 (MEASURED 2026-10-12).
// The logo stays up while the system UI is open.
PPC_FUNC(sub_82623858)
{
    sysui::g_titleLogoMs = sysui::NowMs();
    if (SysUiOwnsInput())
    {
        GWrite32(base, ctx.r3.u32 + 40, 0);
        GWrite32(base, ctx.r3.u32 + 44, 0);
    }
    __imp__sub_82623858(ctx, base);
}

// A dialog window's text block takes a text (block, font, size, text: copied and laid out). The
// name dialog's and the message box's blocks are remembered to lay their texts out again.
PPC_FUNC(sub_8241B9D0)
{
    if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_nameText)
    {
        sysui::g_nameBlock = ctx.r3.u32;
        sysui::g_nameFont = ctx.r4.u32;
        sysui::g_nameSize = ctx.r5.u32;
    }
    else if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_boxText)
    {
        sysui::g_boxBlock = ctx.r3.u32;
        sysui::g_boxFont = ctx.r4.u32;
        sysui::g_boxSize = ctx.r5.u32;
    }
    __imp__sub_8241B9D0(ctx, base);
}

// lib::MenuEntitySetText(text, widget): Controller Setup's button panel (player 1) - the game's
// texts are recorded, and while the KEYBOARD mode shows keys the game's writes keep the key.
PPC_FUNC(sub_8256C708)
{
    if (!g_hostText && ctx.r4.u32 && ctx.r3.u32)
    {
        const int slot = keyui::SlotOf(GuestString(base, ctx.r4.u32, 40));
        if (slot >= 0)
        {
            keyui::g_state.gameText[slot] = GuestString(base, ctx.r3.u32, 96);
            if (keyui::g_state.showing) ctx.r3.u64 = GuestStr(base, keyui::SlotText(keyui::g_state, slot));
        }
    }
    __imp__sub_8256C708(ctx, base);
}

// Text lookup (text manager, id): the exit confirmation's message and button labels, the name
// dialog's message.
PPC_FUNC(sub_8237F5D0)
{
    const uint32_t id = ctx.r4.u32 & 0xFFFF;
    if (id == kExitTextId || id == kSwitchTextId)
    {
        static const uint32_t exitText = TableText(base, "Exit the game?");
        static const uint32_t switchText = TableText(base, "Switch to another profile?\nThe game returns to the title screen.");
        ctx.r3.u64 = id == kExitTextId ? exitText : switchText;
        g_exitLabelsPending = 2;
        return;
    }
    if ((id == kOkTextId || id == kCancelTextId) && g_exitLabelsPending.load() > 0)
    {
        static const uint32_t yes = TableText(base, "YES"), no = TableText(base, "NO");
        g_exitLabelsPending.fetch_sub(1);
        ctx.r3.u64 = id == kOkTextId ? yes : no;
        return;
    }
    if (id == sysui::kNameTextId && sysui::g_nameText)
    {
        ctx.r3.u64 = sysui::g_nameText;
        return;
    }
    if (id == sysui::kBoxTextId && sysui::g_boxText)
    {
        ctx.r3.u64 = sysui::g_boxText;
        return;
    }
    if (id == kOkTextId || id == kCancelTextId)
    {
        std::lock_guard l(sysui::g_labelMutex);           // a message box's button labels
        const size_t slot = id == kOkTextId ? 0 : 1;
        if (slot < sysui::g_labels.size())
        {
            ctx.r3.u64 = sysui::g_labels[slot];
            return;
        }
    }
    const uint32_t lr = uint32_t(ctx.lr);
    const bool corner = (id == 0x09 || id == 0x0A || id == 0x0B) && lr >= 0x82277000 && lr < 0x82278000;
    const bool logo = id == 0x05 && lr >= 0x82646000 && lr < 0x82647000;
    if ((corner || logo) && SysUiOwnsInput())
    {
        // The title's PRESS START / FREE PLAY texts (the arcade credit display in the corners,
        // looked up from 0x82277664 / 0x82277818, and the logo's from 0x8264656C - MEASURED
        // 2026-10-12) would sit on the system UI's key hints; they are blank while it is open.
        static const uint32_t empty = TableText(base, "");
        ctx.r3.u64 = empty;
        return;
    }
    __imp__sub_8237F5D0(ctx, base);
    static const bool trace = std::getenv("TTT2_TEXT_TRACE") != nullptr;
    if (trace)
    {
        static std::mutex m;
        static std::map<uint32_t, int> seen;
        std::lock_guard l(m);
        if (seen[id]++ == 0)
            std::fprintf(stderr, "[text] id %04X lr %08X '%s'\n", id, lr, GuestString(base, ctx.r3.u32, 48).c_str());
    }
}

// Retail menu document loader: r3 menu manager, r4 BX01 bytes, r5 size, r6 root widget. The
// Options / main-menu document gets the PC edits; the edited copy lives in a never-freed guest
// allocation, as the reader keeps pointing into its bytes.
PPC_FUNC(sub_82346060)
{
    const uint8_t* doc = base + ctx.r4.u32;
    const uint32_t size = ctx.r5.u32;
    if (MenuOverride())
    {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_override/%08X.bin", Fnv1a(doc, size));
        if (FILE* f = std::fopen(path, "rb"))
        {
            std::string bytes;
            char chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.append(chunk, n);
            std::fclose(f);
            ctx.r4.u64 = GuestCopy(base, bytes.data(), bytes.size());
            ctx.r5.u64 = bytes.size();
            std::fprintf(stderr, "[menu] document replaced by %s (%zu bytes)\n", path, bytes.size());
        }
    }
    else
    {
        std::string why, note;
        const std::string edited = menu_doc::ApplyPcEdits(doc, size, PcLists(), &why, &note);
        if (!note.empty()) std::fprintf(stderr, "[menu] %s\n", note.c_str());
        if (!edited.empty())
        {
            if (MenuDump())
            {
                CreateDirectoryA("C:/ttt2/port/menu_bin", nullptr);
                char path[MAX_PATH];
                std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_bin/edited_%08X.bin", Fnv1a(doc, size));
                SaveFile(path, edited.data(), edited.size());
            }
            ctx.r4.u64 = GuestCopy(base, edited.data(), edited.size());
            ctx.r5.u64 = edited.size();
            std::fprintf(stderr, "[menu] Options / main menu: PC rows and EXIT added (%u -> %zu bytes)\n", size, edited.size());
        }
        else if (!why.empty())
            std::fprintf(stderr, "[menu] Options / main menu left unchanged: %s\n", why.c_str());
    }
    __imp__sub_82346060(ctx, base);
}

// BX01 document reader: r3 document, r4 bytes, r5 size. TTT2_MENU_DUMP=1 saves each document.
PPC_FUNC(sub_82A8C170)
{
    if (MenuDump() && ctx.r4.u32 && ctx.r5.u32 && ctx.r5.u32 < (64u << 20))
    {
        static std::atomic<uint32_t> index{ 0 };
        CreateDirectoryA("C:/ttt2/port/menu_bin", nullptr);
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_bin/%03u_%08X.bin", index.fetch_add(1),
                      Fnv1a(base + ctx.r4.u32, ctx.r5.u32));
        SaveFile(path, base + ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_82A8C170(ctx, base);
}
