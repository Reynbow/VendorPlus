// Switching between Artifact Formation and the vendor in its district. Both screens are one game system
// (heron::ui_shop) with one ShopMenuState, so only one of them can be open. The script adds the other one's
// name to the screen's title bar; picking it swaps the open shop the way the game itself would if the player
// walked to the other counter:
//  * Close: the ShopMenuState "Close" event (variant 1), as the menu's Esc button queues it.
//  * Open: once the shop state is off the stack, the "Open" event (variant 0) with the other shop's id. The
//    game looks the id up in its ShopDatabase (vendor = type 0, artifact formation = type 1), pushes that
//    shop's UI state and builds its lists, exactly as the vendor/formation counters do.
// The hook runs inside the game's own ui_shop::process_events, before it reads the event, so every write
// happens where the game itself makes it.
// The Wardrobe tab opens the game's own wardrobe (heron::ui_player_outfit) with its own Open and Back events. A
// wardrobe in the world has you stand on its spot and frames you with its cameras; for the tab, the spot is where
// you stand and the Formation counter's camera is borrowed for the wardrobe's framing (see "the wardrobe's standing
// spot" and "the wardrobe's camera" below). LB / RB and the secondary pair are read from the game's own menu
// actions in ui_shop::handle_input, so every controller, remapped buttons and the keyboard work.
#include "common.h"
#include <atomic>
#include <cmath>
#include <map>

namespace vp {

// ---- signatures (build 25472515); each one pins the structure offsets in common.h ----
// heron::ui_shop::process_events body: rdx = ShopMenuState&, event pending flag at +8.
static const char* kSigEvents =
    "48 89 5C 24 10 48 89 74 24 18 55 41 56 41 57 48 8D AC 24 90 FE FF FF 48 81 EC 70 02 00 00 80 7A 08 00 "
    "4D 8B F1 4D 8B F8 48 8B F2 48 8B D9 0F 84";
// Its Open visitor: the ShopDatabase lookup (table at +0x30, mask at +0x48), then ShopMenuState+0x290 = id and
// +0x294 = the shop's type (slot +0x1c).
static const char* kSigOpen =
    "41 8B 08 48 B8 E9 58 04 63 D2 B9 5F DE 48 8B 1F 4C 8D 45 10 48 F7 E1 89 4D 10 48 8D 4B 30 4C 8D 0C 02 48 8D 55 "
    "C0 E8 ?? ?? ?? ?? 48 8B 43 48 48 03 43 30 48 39 45 C0 0F 84 ?? ?? ?? ?? 48 8B 4F 08 8B 06 48 8B 5D C8 89 81 90 02 "
    "00 00 48 8B 4F 08 8B 43 1C 89 81 94 02 00 00";
// ui_shop::handle_state_change: ShopMenuState+0x2fc = "a shop state is on the stack".
static const char* kSigOpenFlag = "48 8B CE E8 ?? ?? ?? ?? 88 83 FC 02 00 00 0F B6 B3 FB 02 00 00";
// Its list rebuild check: prev = +0x288; cur = +0x278; +0x288 = cur; rebuild when the open flag changed,
// the category changed, or prev != cur.
static const char* kSigRebuild =
    "0F B7 83 86 02 00 00 66 89 83 84 02 00 00 48 8B 8B 88 02 00 00 48 8B 83 78 02 00 00 48 89 83 88 02 00 00 "
    "41 3A F6 75 0E 40 84 FF 75 09 48 3B C8 0F 84";

// The shop's vendor level: VendorLevel(DistrictDatabase, progress, district stack, world_state) -> level. It takes
// the district you're in (the top of the stack, 0x142666190) and asks the per-district function below.
static const char* kSigShopLevel =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 49 8B F9 49 8B C8 48 8B F2 E8 ?? ?? ?? ?? 48 8D 54 24 20 "
    "48 8B CB 44 8B 00 44 89";
// VendorLevelForDistrict(district id, DistrictDatabase, progress, world_state) -> level: the district's vendor
// upgrade (type 2 in its 0x70-byte upgrade list) and its unlocked tier, at least 1.
static const char* kSigDistrictLevel =
    "48 89 5C 24 10 89 4C 24 08 57 48 83 EC 20 48 8B CA 49 8B D9 48 8D 54 24 30 49 8B F8 E8 ?? ?? ?? ?? 48 85 C0 74 "
    "29 48 8B 48 08 8B 40 10 48 6B C0 70";

// ---- the controller (and keyboard) ----
// heron::ui_shop::handle_input body (ShopMenuState&, Query, Query, PurchaseControlData&, UIStateStacks&,
// PinnedOverlayState&, const Input& [7]). It runs every frame; the hook reads the menu actions from its Input.
static const char* kSigInput =
    "48 89 5C 24 08 4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F1 48 81 EC A0 00 00 00 "
    "4D 8B F8 48 8B F9 4C 8B 6D 7F 49 83 7D 00 00 0F 84";
// The wardrobe's handle_input asks ActionPressed(Input+0x268, id) for MENU_BACK, MENU_PREV and MENU_NEXT (the ids
// are words the game fills in at start-up): +22 the call, +29 MENU_PREV, +48 MENU_NEXT (rip-relative).
static const char* kSigMenuKeys =
    "4C 8D A3 68 02 00 00 49 8B CC 44 8B 70 08 41 D1 EE 41 80 E6 01 E8 ?? ?? ?? ?? 0F B7 15 ?? ?? ?? ?? 49 8B CC 88 44 "
    "24 20 E8 ?? ?? ?? ?? 0F B7 15 ?? ?? ?? ?? 49 8B CC 44 0F B6 E8 E8";
// Another menu asks for MENU_NEXT_SECONDARY (+6) and MENU_PREV_SECONDARY (+24).
static const char* kSigSecondaryKeys =
    "88 45 50 0F B7 15 ?? ?? ?? ?? 49 8B CD E8 ?? ?? ?? ?? 88 45 A0 0F B7 15 ?? ?? ?? ?? 49 8B CD E8 ?? ?? ?? ?? 44 0F B6 "
    "C0 48 8B 8B 90 00 00 00";
const size_t kInputActions = 0x268;  // Input: the action set ActionPressed reads

// ---- the wardrobe ----
// The shop's Open pushes its UI state with SetUIState(push, &state, UIStateStacks*, &stack) (+24 the call):
// push = true pushes the state (or pops back to it when it's lower down); false removes it and everything above.
// Inside it, FindStack(&name, UIStateStacks*) and StateIndex(stack, &state) (-1 = not on the stack).
static const char* kSigSetStateCall = "4C 8B 47 10 4C 8D 4D D0 48 8D 55 C0 B1 01 C5 F8 10 00 C5 F8 11 45 C0 E8";
static const char* kSigSetState =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 60 48 8B EA 44 0F B6 F1 C4 C1 78 10 01 "
    "C5 F8 11 44 24 20 49 8B D0 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84 ?? ?? ?? ?? C5 F8 10 45 00 C5 F8 "
    "11 44 24 20 48 8D 54 24 20 48 8B C8 E8";

// The wardrobe's own Open (its process_events visitor, variant 0) pushes it exactly so, with the game's name
// objects for "game_states" (+10) and "player_outfit" (+89).
static const char* kSigWardrobeOpen =
    "48 8B 06 33 DB 89 58 20 8B 0D ?? ?? ?? ?? 81 E1 FF FF FF 00 C1 E1 03 0F B6 05 ?? ?? ?? ?? 3D 80 00 00 00 73 09 8D "
    "04 C5 00 00 00 00 EB 08 C1 E0 08 2D 00 7C 00 00 48 8D 1D ?? ?? ?? ?? 3B C8 48 0F 47 1D ?? ?? ?? ?? 8B 05 ?? ?? ?? "
    "?? 48 89 5C 24 20 48 89 44 24 28 8B 15";

// A state name as the stack functions take it, made from one of the game's name objects the way its code does:
// the text inline at +8 (or a pointer there, for long ones) and the dword at +4.
struct StrView {
    const char* p;
    uint64_t n;
};
static StrView ViewOf(const uint8_t* obj) {
    const uint32_t d0 = *(const uint32_t*)obj;
    const uint32_t size = (d0 & 0xffffff) << 3;
    const uint32_t tag = obj[3];
    const uint32_t inlineSize = tag < 0x80 ? tag << 3 : (tag << 8) - 0x7c00;
    StrView v;
    v.p = size > inlineSize ? *(const char* const*)(obj + 8) : (const char*)(obj + 8);
    v.n = *(const uint32_t*)(obj + 4);
    return v;
}
static const uint8_t* g_gameStatesName = nullptr;  // the game's "game_states" name object
static const uint8_t* g_wardrobeName = nullptr;    // its "player_outfit"

// The system body takes the system's parameters in order; it has 15, forwarding a few more is harmless.
using EventsFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                          void*, void*, void*, void*, void*);
using InputFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
using PressedFn = bool (*)(void* actions, uint32_t id);
using SetStateFn = void (*)(bool push, const StrView* state, void* stacks, const StrView* stack);
using FindStackFn = void* (*)(const StrView* name, void* stacks);
using StateIndexFn = int (*)(void* stack, const StrView* state);
static PressedFn g_pressed = nullptr;
static const uint16_t* g_keyIds[4] = {};  // MENU_NEXT, MENU_PREV, MENU_NEXT_SECONDARY, MENU_PREV_SECONDARY
static SetStateFn g_setState = nullptr;
static FindStackFn g_findStack = nullptr;
static StateIndexFn g_stateIndex = nullptr;
static void* volatile g_origInput = nullptr;
static std::atomic<bool> g_inputInstalled{false};
static std::atomic<uint32_t> g_padCount[4];  // presses of each, for the script
static bool g_padDown[4] = {};               // game thread only
static std::atomic<bool> g_wardrobe{false};  // the wardrobe's state is on the game_states stack

static void* volatile g_origEvents = nullptr;
static std::atomic<bool> g_installed{false};
static std::atomic<bool> g_faulted{false};

// ---- what the hook sees (game thread), read by the endpoint (UI resource thread) ----
static std::atomic<ULONGLONG> g_seen{0};
static std::atomic<uint32_t> g_frames{0};
static std::atomic<bool> g_open{false};
static std::atomic<int> g_type{-1};
static std::atomic<uint32_t> g_id{0};
static std::atomic<uint32_t> g_formationId{0};  // the formation shop
static std::atomic<uint32_t> g_vendorId{0};     // the vendor paired with it (0 = none found)
static std::atomic<uint32_t> g_district{0};     // first dword of ActiveDistrict
static std::atomic<int> g_shopCount{-1};

// ---- the real vendors (build 25472515) ----
// The shop database has 10 vendors; 7 stand in the world. Found by opening each at its counter (the log's
// "vendor opened in person" lines) and matched to the zones in Eurogamer's vendor guide. In the guide's order.
// The district ids came from opening the map in each zone (the map marks the zone, the game's district stack
// says the id); their vendor levels matched the map's. The Gap is in "Unknown".
struct KnownVendor {
    uint32_t id;
    const char* zone;
    uint32_t district;  // the zone's district id (0 = not known yet: learned from the map, saved in the INI)
};
static const KnownVendor kKnownVendors[] = {
    {2954342914u, "Downtown", 1788133588u},
    {2931056327u, "Central", 1689498021u},
    {3602392616u, "West Incursion Zone", 3798067088u},
    {621942590u, "The Park", 1081396876u},
    {4143164435u, "Evacuation Zone", 2624371201u},
    {889868091u, "Underpass", 606444525u},
    {2094905740u, "Unknown", 827007400u},
};

// Shop list from the ShopDatabase, read on the game thread.
struct ShopInfo {
    uint32_t type, key;  // slot +0x1c, slot +0x18
};
static SRWLOCK g_shopsLock = SRWLOCK_INIT;
static std::map<uint32_t, ShopInfo> g_shops;

// The district stack's top (the district you're in), as 0x142666190 reads it: 16-byte entries, id at +8;
// the default at +0x20 when empty.
static uint32_t StackTop(const uint8_t* stack) {
    const uint8_t* entries = *(uint8_t* const*)stack;
    const uint32_t count = *(const uint32_t*)(stack + 8);
    return count ? *(const uint32_t*)(entries + (size_t)count * 16 - 8) : *(const uint32_t*)(stack + 0x20);
}
static std::atomic<uint32_t> g_currentDistrict{0};   // the district the game says you're in
static std::atomic<const uint8_t*> g_stack{nullptr};  // the game's district stack (from the first level call)

// ---- the swap ----
enum Phase { kIdle = 0, kClosing = 1, kOpening = 2, kLeavingWardrobe = 3 };
const int kWantWardrobe = 2;  // g_want: 0 vendor, 1 artifact formation (the shop types), 2 the wardrobe
enum Result { kResNone = 0, kResDone, kResNoTarget, kResTimeout, kResNotOpen };
static const char* kResultNames[] = {"none", "done", "no-target", "timeout", "not-open"};
static std::atomic<int> g_want{-1};  // type the script asked for, claimed by the hook
static std::atomic<uint32_t> g_wantId{0};  // a specific shop (0 = the type's default)
static std::atomic<ULONGLONG> g_wantAt{0};
static std::atomic<int> g_phase{kIdle};
static std::atomic<ULONGLONG> g_phaseAt{0};
static int g_target = -1;         // game thread only
static uint32_t g_targetId = 0;   // game thread only
static uint32_t g_returnId = 0;   // game thread only: the shop a wardrobe switch left
static uint32_t g_spotSeen = 0;   // game thread only: g_spotWrites when leaving the wardrobe began
static std::atomic<uint32_t> g_serial{0};
static std::atomic<int> g_result{kResNone};

static void Finish(Result r) {
    g_phase = kIdle;
    g_result = r;
    ++g_serial;
    Log("Swap result: %s", kResultNames[r]);
}

// Reads every shop in the database. The table is the game's Swiss table: a control byte per slot (high bit
// set = empty or deleted), 32-byte slots.
static void ReadShops(const uint8_t* db) {
    const uint8_t* ctrl = *(uint8_t* const*)(db + shop::kDbCtrl);
    const uint8_t* slots = *(uint8_t* const*)(db + shop::kDbSlots);
    const uint64_t mask = *(const uint64_t*)(db + shop::kDbMask);
    std::map<uint32_t, ShopInfo> found;
    if (ctrl && slots && mask && mask < 0x10000) {
        for (uint64_t i = 0; i <= mask; ++i) {
            if (ctrl[i] & 0x80) continue;
            const uint8_t* s = slots + i * shop::kSlotSize;
            found[*(const uint32_t*)s] = {*(const uint32_t*)(s + shop::kSlotType), *(const uint32_t*)(s + shop::kSlotKey)};
        }
    }
    AcquireSRWLockExclusive(&g_shopsLock);
    bool changed = found.size() != g_shops.size();
    for (auto& kv : found) {
        auto it = g_shops.find(kv.first);
        changed = changed || it == g_shops.end() || it->second.type != kv.second.type ||
                  it->second.key != kv.second.key;
    }
    g_shops.swap(found);
    ReleaseSRWLockExclusive(&g_shopsLock);
    g_shopCount = (int)g_shops.size();
    if (changed) {
        std::string list;
        for (auto& kv : g_shops) {
            char b[64];
            sprintf_s(b, "%s%u:%s@%08x", list.empty() ? "" : " ", kv.first,
                      kv.second.type == 0 ? "vendor" : kv.second.type == 1 ? "formation" : "other", kv.second.key);
            list += b;
        }
        Log("Shops (%d): %s", (int)g_shops.size(), list.c_str());
    }
}

static bool Lookup(uint32_t id, ShopInfo& out) {
    AcquireSRWLockShared(&g_shopsLock);
    auto it = g_shops.find(id);
    const bool ok = it != g_shops.end();
    if (ok) out = it->second;
    ReleaseSRWLockShared(&g_shopsLock);
    return ok;
}
static uint32_t ShopType(uint32_t id) {
    ShopInfo s;
    return Lookup(id, s) ? s.type : 0xffffffffu;
}

// The formation (there's one, in The Gap) and the vendor its tab opens: the INI's VendorId, else the vendor last
// browsed to, else the first. The shop database doesn't say where a vendor stands (the vendor level comes from
// the district the player is in), so any vendor works from anywhere; the vendor screen offers them all.
// The switcher's vendors: every real one, in the guide's order, with their zones. If none of this build's
// vendors is in the database (another game version): every vendor, in id order, unnamed.
static void SwitcherVendors(std::vector<uint32_t>& ids, std::vector<const char*>& zones) {
    ids.clear();
    zones.clear();
    for (const KnownVendor& k : kKnownVendors)
        if (ShopType(k.id) == 0) ids.push_back(k.id), zones.push_back(k.zone);
    if (!ids.empty()) return;
    AcquireSRWLockShared(&g_shopsLock);
    for (auto& kv : g_shops)
        if (kv.second.type == 0) ids.push_back(kv.first), zones.push_back("");
    ReleaseSRWLockShared(&g_shopsLock);
}

// The formation (there's one, in The Gap) and the vendor its tab opens: the INI's VendorId, else the vendor last
// browsed to, else the first you've opened at its counter, else the first (it opens locked).
static void Pair() {
    uint32_t formation = 0;
    AcquireSRWLockShared(&g_shopsLock);
    for (auto& kv : g_shops)
        if (kv.second.type == 1) {
            formation = kv.first;
            break;
        }
    ReleaseSRWLockShared(&g_shopsLock);
    if (g_open.load() && g_type.load() == 1) formation = g_id.load();  // the one that's actually open wins
    g_formationId = formation;

    std::vector<uint32_t> ids;
    std::vector<const char*> zones;
    SwitcherVendors(ids, zones);
    uint32_t vendor = 0;
    if (g_cfg.vendorId && ShopType(g_cfg.vendorId) == 0) vendor = g_cfg.vendorId;
    if (!vendor && std::find(ids.begin(), ids.end(), PairedVendor()) != ids.end()) vendor = PairedVendor();
    for (uint32_t v : ids)
        if (!vendor && VendorVisited(v)) vendor = v;
    if (!vendor && !ids.empty()) vendor = ids[0];
    g_vendorId = vendor;
}

// A vendor we opened (tab or arrows) that you haven't opened at its counter yet: locked. Its level is 0, so the
// game locks every item (and hides the level badge); the script covers the items with "visit to unlock".
static std::atomic<uint32_t> g_lockedId{0};

static void QueueEvent(uint8_t* st, uint8_t variant, uint32_t payload) {
    *(uint32_t*)(st + shop::kEventPayload) = payload;
    st[shop::kEventVariant] = variant;
    st[shop::kEventPending] = 1;
}

// Diagnostics: every 5 s, how often the system ran and what it sees.
static std::atomic<ULONGLONG> g_lastBeat{0};
static void Heartbeat(ULONGLONG now) {
    if (!g_cfg.diagnostics) return;
    ULONGLONG last = g_lastBeat.load();
    if (now - last < 5000 || !g_lastBeat.compare_exchange_strong(last, now)) return;
    static uint32_t frames0;
    const uint32_t frames = g_frames.load();
    Log("Frames in 5 s: %u; open %d type %d id %u, formation %u vendor %u, district %08x, phase %d", frames - frames0,
        (int)g_open.load(), g_type.load(), g_id.load(), g_formationId.load(), g_vendorId.load(), g_district.load(),
        g_phase.load());
    frames0 = frames;
}

// A UI stack keeps its entries (0x28 bytes each at +0xc0, count at +0xc8) and a cursor to the current one (+0xbc):
// closing a state moves the cursor down and leaves its entry above, until a push replaces it. So a state is open
// while its entry is at or below the cursor.
const size_t kStackCursor = 0xbc, kStackCount = 0xc8;
static void StackInfo(void* stacks, int& count, int& cursor, int& idx) {
    count = cursor = idx = -1;
    if (!stacks || !g_findStack || !g_stateIndex || !g_gameStatesName) return;
    const StrView games = ViewOf(g_gameStatesName), wardrobe = ViewOf(g_wardrobeName);
    const uint8_t* stack = (const uint8_t*)g_findStack(&games, stacks);
    if (!stack) return;
    count = *(const int*)(stack + kStackCount);
    cursor = *(const int*)(stack + kStackCursor);
    idx = g_stateIndex((void*)stack, &wardrobe);
}
static bool WardrobeOnStack(void* stacks) {
    int count, cursor, idx;
    StackInfo(stacks, count, cursor, idx);
    return idx >= 0 && idx <= cursor;
}

static void LogStateNames() {
    static bool done = false;
    if (done || !g_gameStatesName || !g_wardrobeName) return;
    done = true;
    const StrView a = ViewOf(g_gameStatesName), b = ViewOf(g_wardrobeName);
    Log("UI state names: \"%.*s\" (%llu), \"%.*s\" (%llu)", (int)strnlen(a.p, 32), a.p, a.n, (int)strnlen(b.p, 32), b.p, b.n);
}

// Directly on or off the stack (off: it and anything above it, its item browser). Only a last resort when the
// wardrobe doesn't take its own Back: its Open and Back run its opening and closing (the lock, the menu's state).
static void SetWardrobe(bool on, void* stacks) {
    const StrView games = ViewOf(g_gameStatesName), wardrobe = ViewOf(g_wardrobeName);
    g_setState(on, &wardrobe, stacks, &games);
}

// ---- the wardrobe's standing spot ----
// The wardrobe's handle_state_change locks the player (CharacterControllerKeyframedState) when it opens, and its
// update_player_orientation then sets the player's transform every frame RELATIVE TO THE PLAYER'S PARENT: the stick's
// turn, at position zero. At a wardrobe in the world the parent is the wardrobe's standing spot. Opened from our tab
// there is no parent, and position zero would be the world's origin (the player falls out of the world). So the
// hook never lets it run without a parent; for our tab it supplies the spot itself: where the player stands, turned
// to face the camera (behind them), plus the stick's turn.
static const char* kSigOrientation =
    "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC F0 00 00 00 8B 05 ?? ?? ?? ?? 4D 8B D9 25 FF FF FF 00 49 8B F8 48 8B F2 "
    "48 8B D9";
// Inside it: IsCurrent(&state, UIStateStacks*, &stack) (+14 the call) and SetLocalTransform(&tuple, &query, &xf)
// (+32 the call).
static const char* kSigIsCurrentCall = "8B 05 ?? ?? ?? ?? 49 8B D3 48 89 44 24 48 E8 ?? ?? ?? ?? 84 C0 0F 84";
static const char* kSigSetLocalCall =
    "48 89 44 24 78 48 8B 43 20 C5 FC 11 44 24 40 C5 FC 11 4C 24 20 48 89 84 24 80 00 00 00 C5 F8 77 E8";
const size_t kOrientationPrologue = 18;  // two movs to the home area, push rdi, sub rsp, 0xf0
const size_t kOutfitAngle = 0x68;         // PlayerOutfitMenuState: the stick's turn (radians)
const uint8_t kOutfitOpen = 0, kOutfitClose = 1;  // its events (as the Lua open_player_outfit / its Back queue them)

struct Xf {  // a transform as the game keeps it: rotation (x, y, z, w; y is up), then position
    float q[4];
    float p[4];
};
static void QMul(const float* a, const float* b, float* out) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void QTurn(float angle, float* out) {  // about the up axis, as the wardrobe's own turn
    out[0] = 0.f;
    out[1] = sinf(angle * 0.5f);
    out[2] = 0.f;
    out[3] = cosf(angle * 0.5f);
}

using OrientationFn = void (*)(void*, void*, void*, void*);
using IsCurrentFn = bool (*)(const StrView* state, void* stacks, const StrView* stack);
using SetLocalFn = void (*)(void* tuple, void* query, const Xf* xf);
static void* volatile g_origOrientation = nullptr;
static IsCurrentFn g_isCurrent = nullptr;
static SetLocalFn g_setLocal = nullptr;
static std::atomic<bool> g_orientationInstalled{false};
static std::atomic<bool> g_orientationFaulted{false};
static std::atomic<uint8_t*> g_outfitState{nullptr};  // PlayerOutfitMenuState (from its handle_state_change)
static std::atomic<bool> g_restoreFacing{false};      // leaving through our tabs: face the way you stood first
static std::atomic<uint32_t> g_spotWrites{0};         // frames the hook has placed the player
static bool g_spotCaptured = false;                   // game thread (the orientation system)
static Xf g_spot;

static void QueueOutfit(uint8_t variant) {
    uint8_t* w = g_outfitState.load();
    w[shop::kEventVariant] = variant;
    w[shop::kEventPending] = 1;
}

static bool WardrobeCurrent(void* stacks) {
    const StrView games = ViewOf(g_gameStatesName), wardrobe = ViewOf(g_wardrobeName);
    return g_isCurrent(&wardrobe, stacks, &games);
}

static void LogXf(const char* what, const Xf& x) {
    Log("%s: turn (%.3f %.3f %.3f %.3f) at (%.3f %.3f %.3f)", what, x.q[0], x.q[1], x.q[2], x.q[3], x.p[0], x.p[1], x.p[2]);
}

// ---- the wardrobe's camera ----
// A wardrobe in the world frames you with its own cameras: its script attaches the player camera to a camera entity
// placed in front of its standing spot (camera_attach). Measured at one (the numbers below): the full-body camera
// stands 3.46 m in front of the spot, 2.14 m to its right and 1.14 m up with a 50-degree lens, looking back past you
// so you stand beside the menu; while you browse Hair or Facial Hair it cuts to a face camera, 1.77 m away at face
// height with a 28-degree lens. For our tab the same framing is made where you stand: the camera entity of the counter
// you opened the visit at (attached by its own script, detached when it closed) is moved there, given the wardrobe's
// lens, and the player camera attached to it; after, it's detached and put back exactly. Component ids are FNV-1a
// hashes of the types' names, as the game's own code uses.
constexpr uint32_t TypeId(const char* s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) h = (h ^ (uint8_t)*s++) * 0x01000193u;
    return h;
}
const uint32_t kWorldXf = TypeId("coregame::component::WorldTransformReadOnly");
const uint32_t kLocalXf = TypeId("coregame::component::LocalTransformReadOnly");
const uint32_t kPrevWorldXf = TypeId("coregame::component::PreviousWorldTransformReadOnly");
const uint32_t kPrevLocalXf = TypeId("coregame::component::PreviousLocalTransformReadOnly");
const uint32_t kParent = TypeId("coregame::component::ResolvedParent");
const uint32_t kCameraView = TypeId("coregame::component::CameraView");
const uint32_t kMixerData = TypeId("heron::cameramixer::component::Data");  // the attach stack
const uint32_t kCameraEnv = TypeId("coregame::global::Camera");             // +4 the player camera entity
// The ECS world (build 25472515; checked against the game's own lookups by the self-test).
const size_t kEntCount = 0x58510, kEntGen = 0x584e8, kEntLoc = 0x58530;   // entities: count, generations, locations
const size_t kArchTypes = 0x18450, kArchOffsets = 0x18458, kArchCount = 0x18464;  // + 32 * chunk
const size_t kChunkBase = 0x50;                                            // + 8 * chunk
const size_t kEnvHashes = 0x585b0, kEnvCount = 0x585b8, kEnvPtrs = 0x585c0;
const size_t kMixerDataSize = 0x110;
// A wardrobe's cameras in its standing spot's frame (y up, z forward): turn, position and lens (CameraView +0x30,
// radians; the counter's own camera has 80 degrees).
struct Framing {
    float q[4], p[3], fov;
};
static const Framing kBodyFraming = {{0.00488f, -0.98322f, 0.02675f, 0.18039f}, {2.1440f, 1.1420f, 3.4610f}, 0.872665f};
static const Framing kFaceFraming = {{0.00000f, -0.98358f, 0.00000f, 0.18046f}, {0.8052f, 1.6170f, 1.5753f}, 0.488692f};
const size_t kCameraViewFov = 0x30;
// The wardrobe's slots that get the face camera, by their place in its grid (Top Layer, Hair / Base Layer, Facial
// Hair / Accessories, Body and Face): as wardrobes in the world show them (learned, see LearnFrame), else Hair,
// Facial Hair and Accessories. The script reports the slot whose items are open.
static bool FaceSlot(int slot) {
    const int learned = WardrobeCloseUp(slot);
    return learned >= 0 ? learned == 1 : slot == 1 || slot == 3 || slot == 4;
}
static std::atomic<int> g_openSlot{-1};  // the slot whose item browser is open (the script's report), -1 = none

using TypeSlotFn = int (*)(const void* types, uint32_t count, uint32_t type);
using AttachFn = void (*)(void* mixer, uint64_t entity, float blend);
using DetachFn = void (*)(void* mixer, float blend);
static TypeSlotFn g_typeSlot = nullptr;
static AttachFn g_attach = nullptr;
static DetachFn g_detach = nullptr;
static std::atomic<bool> g_cameraReady{false};         // everything found, on the known build
static std::atomic<uint64_t> g_lastAttach{0};          // the last camera_attach(entity) any script made
static std::atomic<ULONGLONG> g_lastAttachAt{0};
static std::atomic<uint32_t> g_attachSeq{0};
static std::atomic<uint64_t> g_counterCamera{0};       // the camera of the counter this visit started at

static void QConj(const float* q, float* out) { out[0] = -q[0], out[1] = -q[1], out[2] = -q[2], out[3] = q[3]; }
static void QRotate(const float* q, const float* v, float* out) {
    const float p[4] = {v[0], v[1], v[2], 0.f};
    float c[4], t[4], r[4];
    QConj(q, c);
    QMul(q, p, t);
    QMul(t, c, r);
    out[0] = r[0], out[1] = r[1], out[2] = r[2];
}

static bool Locate(const uint8_t* world, uint64_t handle, uint32_t& chunk, uint32_t& row) {
    const uint32_t index = (uint32_t)handle;
    if (index == 0xffffffffu || index >= *(const uint64_t*)(world + kEntCount)) return false;
    const uint64_t gen = (*(const uint64_t* const*)(world + kEntGen))[index];
    if ((uint32_t)gen != (uint32_t)(handle >> 32)) return false;
    const uint64_t loc = (*(const uint64_t* const*)(world + kEntLoc))[index];
    if ((loc & 0xffff) == 0xffff) return false;
    chunk = (uint32_t)(loc & 0xffff);
    row = (uint32_t)(loc >> 32);
    return true;
}
static uint8_t* Comp(const uint8_t* world, uint32_t chunk, uint32_t row, uint32_t type, size_t size) {
    const void* types = *(void* const*)(world + kArchTypes + 32 * (size_t)chunk);
    const uint32_t count = *(const uint32_t*)(world + kArchCount + 32 * (size_t)chunk);
    const int slot = g_typeSlot(types, count, type);
    if (slot < 0) return nullptr;
    const uint32_t* offsets = *(const uint32_t* const*)(world + kArchOffsets + 32 * (size_t)chunk);
    uint8_t* base = *(uint8_t* const*)(world + kChunkBase + 8 * (size_t)chunk);
    return base + offsets[slot] + (size_t)row * size;
}
static uint8_t* EntityComp(const uint8_t* world, uint64_t handle, uint32_t type, size_t size) {
    uint32_t chunk, row;
    return Locate(world, handle, chunk, row) ? Comp(world, chunk, row, type, size) : nullptr;
}
static uint8_t* Env(const uint8_t* world, uint32_t hash) {
    const uint32_t* hashes = *(const uint32_t* const*)(world + kEnvHashes);
    const uint32_t n = *(const uint32_t*)(world + kEnvCount);
    for (uint32_t i = 0; hashes && i < n && i < 4096; ++i)
        if (hashes[i] == hash) return (*(uint8_t* const* const*)(world + kEnvPtrs))[i];
    return nullptr;
}
static uint8_t* Mixer(const uint8_t* world) {
    const uint8_t* env = Env(world, kCameraEnv);
    return env ? EntityComp(world, *(const uint64_t*)(env + 4), kMixerData, kMixerDataSize) : nullptr;
}

struct Mount {
    bool tried = false, active = false, attached = false, face = false;
    uint64_t handle = 0;
    uint8_t *world = nullptr, *local = nullptr, *prevWorld = nullptr, *prevLocal = nullptr, *view = nullptr;
    Xf saved[4];      // world, local, previous world, previous local, as they were
    float savedFov = 0.f;
    Xf spot;          // the standing spot this visit frames (turned to face the way you face)
    Xf target;        // where the camera stands now (world)
    float fov = 0.f;  // and its lens
    Xf parent;        // its parent's world transform (identity when it has none)
};
static Mount g_mount;  // the orientation system only
static const uint8_t* g_mountWorld = nullptr;

static void MountWrite() {
    Xf local;
    float inv[4], d[3];
    QConj(g_mount.parent.q, inv);
    QMul(inv, g_mount.target.q, local.q);
    for (int i = 0; i < 3; ++i) d[i] = g_mount.target.p[i] - g_mount.parent.p[i];
    QRotate(inv, d, local.p);
    local.p[3] = g_mount.target.p[3];
    memcpy(g_mount.world, &g_mount.target, sizeof(Xf));
    if (g_mount.prevWorld) memcpy(g_mount.prevWorld, &g_mount.target, sizeof(Xf));
    if (g_mount.local) memcpy(g_mount.local, &local, sizeof(Xf));
    if (g_mount.prevLocal) memcpy(g_mount.prevLocal, &local, sizeof(Xf));
    if (g_mount.view) memcpy(g_mount.view + kCameraViewFov, &g_mount.fov, sizeof(float));
}

// The framing for now: the face camera while a face slot's items are open, else the full body.
static void MountAim() {
    const Framing& f = g_mount.face ? kFaceFraming : kBodyFraming;
    float off[3];
    QMul(g_mount.spot.q, f.q, g_mount.target.q);
    QRotate(g_mount.spot.q, f.p, off);
    for (int i = 0; i < 3; ++i) g_mount.target.p[i] = g_mount.spot.p[i] + off[i];
    g_mount.target.p[3] = g_mount.saved[0].p[3];
    g_mount.fov = f.fov;
}

// The camera for this visit: the wardrobe's framing where you stand (the spot, turned to face the way you face).
static void MountBegin(const uint8_t* world, const Xf& spot) {
    g_mount = Mount();
    g_mount.tried = true;
    if (!g_cameraReady.load()) return;
    const uint64_t cam = g_counterCamera.load();
    uint32_t chunk, row;
    if (!cam || !Locate(world, cam, chunk, row)) {
        Log("Wardrobe camera: no counter camera to use (%llx)", (unsigned long long)cam);
        return;
    }
    g_mount.handle = cam;
    g_mount.world = Comp(world, chunk, row, kWorldXf, sizeof(Xf));
    g_mount.local = Comp(world, chunk, row, kLocalXf, sizeof(Xf));
    g_mount.prevWorld = Comp(world, chunk, row, kPrevWorldXf, sizeof(Xf));
    g_mount.prevLocal = Comp(world, chunk, row, kPrevLocalXf, sizeof(Xf));
    uint8_t* mixer = Mixer(world);
    if (!g_mount.world || !mixer) {
        Log("Wardrobe camera: the counter camera or the player camera can't be used");
        return;
    }
    g_mount.view = Comp(world, chunk, row, kCameraView, 64);
    if (g_mount.view) memcpy(&g_mount.savedFov, g_mount.view + kCameraViewFov, sizeof(float));
    memcpy(&g_mount.saved[0], g_mount.world, sizeof(Xf));
    if (g_mount.local) memcpy(&g_mount.saved[1], g_mount.local, sizeof(Xf));
    if (g_mount.prevWorld) memcpy(&g_mount.saved[2], g_mount.prevWorld, sizeof(Xf));
    if (g_mount.prevLocal) memcpy(&g_mount.saved[3], g_mount.prevLocal, sizeof(Xf));
    g_mount.parent = Xf{{0, 0, 0, 1}, {0, 0, 0, 0}};
    if (const uint8_t* par = Comp(world, chunk, row, kParent, 8)) {
        const uint64_t ph = *(const uint64_t*)par;
        if ((uint32_t)ph != 0xffffffffu) {
            const uint8_t* pw = EntityComp(world, ph, kWorldXf, sizeof(Xf));
            if (!pw) {
                Log("Wardrobe camera: the counter camera's parent can't be read");
                return;
            }
            memcpy(&g_mount.parent, pw, sizeof(Xf));
        }
    }
    // The spot faces the way you face (turned around), as a wardrobe's spot faces its camera.
    float turn[4];
    QTurn(3.14159265f, turn);
    QMul(spot.q, turn, g_mount.spot.q);
    memcpy(g_mount.spot.p, spot.p, sizeof(spot.p));
    g_mount.face = false;
    MountAim();
    MountWrite();
    g_mount.active = true;
    g_mountWorld = world;
    g_attach(mixer, cam, 0.f);
    g_mount.attached = true;
    if (g_cfg.diagnostics) {
        LogXf("Wardrobe camera: the counter camera was", g_mount.saved[0]);
        LogXf("Wardrobe camera: moved to", g_mount.target);
    }
    Log("Wardrobe camera: the wardrobe's close-up, on the counter camera %llx", (unsigned long long)cam);
}

static void MountEnd() {
    if (!g_mount.active) {
        g_mount.tried = false;
        return;
    }
    const uint8_t* world = g_mountWorld;
    if (g_mount.attached) {
        if (uint8_t* mixer = Mixer(world)) g_detach(mixer, 0.f);
    }
    // Back where it stood, if it's still the same entity in the same place in memory.
    uint32_t chunk, row;
    if (Locate(world, g_mount.handle, chunk, row) && Comp(world, chunk, row, kWorldXf, sizeof(Xf)) == g_mount.world) {
        memcpy(g_mount.world, &g_mount.saved[0], sizeof(Xf));
        if (g_mount.local) memcpy(g_mount.local, &g_mount.saved[1], sizeof(Xf));
        if (g_mount.prevWorld) memcpy(g_mount.prevWorld, &g_mount.saved[2], sizeof(Xf));
        if (g_mount.prevLocal) memcpy(g_mount.prevLocal, &g_mount.saved[3], sizeof(Xf));
        if (g_mount.view) memcpy(g_mount.view + kCameraViewFov, &g_mount.savedFov, sizeof(float));
    }
    Log("Wardrobe camera: back to the game's camera");
    g_mount = Mount();
}

// Diagnostics: every camera a script attaches, where it stands against you and your parent (a wardrobe's spot),
// and its lens (CameraView), for the wardrobe's other cameras.
static void AttachDiagFrame(uint8_t* tuple, void* query) {
    static uint32_t seen = 0;
    const uint32_t seq = g_attachSeq.load();
    if (seq == seen) return;
    seen = seq;
    const uint8_t* world = *(const uint8_t* const*)query;
    const uint64_t cam = g_lastAttach.load();
    const uint64_t idx = *(const uint64_t*)(tuple + 0x20);
    const uint8_t* worldArr = *(const uint8_t* const*)(tuple + 0x0);
    const uint64_t* parents = *(const uint64_t* const*)(tuple + 0x10);
    Log("Camera attached to %llx:", (unsigned long long)cam);
    if (const uint8_t* cw = EntityComp(world, cam, kWorldXf, sizeof(Xf))) LogXf("  the camera", *(const Xf*)cw);
    if (worldArr) LogXf("  the player", *(const Xf*)(worldArr + idx * sizeof(Xf)));
    if (parents && (uint32_t)parents[idx] != 0xffffffffu) {
        if (const uint8_t* pw = EntityComp(world, parents[idx], kWorldXf, sizeof(Xf))) LogXf("  the player's parent", *(const Xf*)pw);
    }
    if (const uint8_t* menu = g_outfitState.load())
        Log("  the open slot: %d (the wardrobe's selected slot value %d)", g_openSlot.load(), *(const int*)(menu + 0x20));
    if (const float* v = (const float*)EntityComp(world, cam, kCameraView, 64))
        Log("  its view: %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
            v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
}

// A wardrobe in the world (the player has its spot as parent): when a slot's items open, its script attaches the
// slot's camera; its lens says which (the face camera's is 28 degrees, the full body's 50). Remembered per slot.
static void LearnFrame(void* query) {
    static int slot0 = -1;
    static ULONGLONG since = 0;
    static bool done = false;
    const int slot = g_openSlot.load();
    const ULONGLONG now = GetTickCount64();
    if (slot != slot0) {
        slot0 = slot;
        since = now;
        done = false;
    }
    if (slot < 0 || done || now - since < 400) return;  // the script attaches as the slot opens; the report lags
    done = true;
    const ULONGLONG at = g_lastAttachAt.load();
    if (!at || at + 1500 < since) return;  // no camera switch around it
    const uint8_t* view = EntityComp(*(const uint8_t* const*)query, g_lastAttach.load(), kCameraView, 64);
    if (!view) return;
    float fov;
    memcpy(&fov, view + kCameraViewFov, sizeof(fov));
    const bool face = fov < 0.7f;
    if (WardrobeCloseUp(slot) != (face ? 1 : 0)) {
        SaveWardrobeCloseUp(slot, face);
        Log("Wardrobe slot %d: %s (as a wardrobe in the world shows it, lens %.0f degrees)", slot,
            face ? "the face camera" : "the full body", fov * 57.29578f);
    }
}

// No parent: the spot. tuple = the player's (WorldTransform array +0, LocalTransform +8, ResolvedParent +0x10,
// +0x18 the chunk's entities, index +0x20), as the original hands it to SetLocalTransform.
static void SpotFrame(uint8_t* tuple, uint8_t* menu, void* query, void* stacks) {
    if (!WardrobeOnStack(stacks)) {
        g_spotCaptured = false;
        MountEnd();
        return;
    }
    if (g_mount.active) {
        // The camera stays put for the whole visit; the face camera while a face slot's items are open (its item
        // browser is on the stack above the wardrobe; the game still counts the wardrobe as current then).
        int count, cursor, idx;
        StackInfo(stacks, count, cursor, idx);
        const bool face = idx >= 0 && cursor > idx && FaceSlot(g_openSlot.load());
        if (face != g_mount.face) {
            g_mount.face = face;
            MountAim();
            if (g_cfg.diagnostics) Log("Wardrobe camera: %s", face ? "the face" : "the full body");
        }
        MountWrite();
    }
    if (!WardrobeCurrent(stacks)) return;  // an item's browser on top: the lock holds the player where they are
    const uint64_t idx = *(const uint64_t*)(tuple + 0x20);
    const uint8_t* world = *(const uint8_t* const*)(tuple + 0x0);
    if (!world) return;
    if (!g_spotCaptured) {
        memcpy(&g_spot, world + idx * sizeof(Xf), sizeof(Xf));
        g_spotCaptured = true;
        if (g_cfg.diagnostics) LogXf("Wardrobe without a spot: the player stands", g_spot);
    }
    if (!g_mount.tried) MountBegin(*(const uint8_t* const*)query, g_spot);
    float turn[4];
    QTurn(g_restoreFacing.load() ? 0.f : 3.14159265f + *(const float*)(menu + kOutfitAngle), turn);
    Xf x;
    QMul(g_spot.q, turn, x.q);
    memcpy(x.p, g_spot.p, sizeof(x.p));
    uint64_t t[5] = {*(const uint64_t*)(tuple + 0x10), *(const uint64_t*)(tuple + 0x8), *(const uint64_t*)(tuple + 0x0),
                     *(const uint64_t*)(tuple + 0x18), idx};
    alignas(32) uint8_t q[32];
    memcpy(q, query, sizeof(q));
    g_setLocal(t, q, &x);
    ++g_spotWrites;
}



// update_player_orientation(player tuple, PlayerOutfitMenuState&, Query<WorldTransformReadOnly>, UIStateStacks&)
static void HookOrientation(void* a1, void* a2, void* a3, void* a4) {
    bool parentless = true;
    __try {
        const uint64_t* parents = *(const uint64_t* const*)((const uint8_t*)a1 + 0x10);
        const uint64_t idx = *(const uint64_t*)((const uint8_t*)a1 + 0x20);
        parentless = !parents || (uint32_t)parents[idx] == 0xffffffffu;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        parentless = true;  // in doubt, never the write at position zero
    }
    if (g_cfg.diagnostics && g_cameraReady.load() && a3) {
        __try {
            AttachDiagFrame((uint8_t*)a1, a3);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!parentless) {
        if (g_cameraReady.load() && a3) {
            __try {
                LearnFrame(a3);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ((OrientationFn)g_origOrientation)(a1, a2, a3, a4);
        return;
    }
    if (g_orientationFaulted.load() || !a2 || !a3 || !a4) return;
    __try {
        SpotFrame((uint8_t*)a1, (uint8_t*)a2, a3, a4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_orientationFaulted.exchange(true)) Log("Exception placing the player in the wardrobe; the player is left as is");
    }
}

static void EventsFrame(uint8_t* st, const uint8_t* db, const uint8_t* district, void* stacks) {
    const ULONGLONG now = GetTickCount64();
    g_seen = now;
    const uint32_t frame = ++g_frames;
    Heartbeat(now);
    if (db && (g_shopCount.load() < 0 || frame % 600 == 0)) ReadShops(db);
    if (district) {
        const uint32_t d = *(const uint32_t*)district;
        if (g_district.exchange(d) != d && g_cfg.diagnostics) {
            const uint32_t* q = (const uint32_t*)district;
            Log("ActiveDistrict: %08x %08x %08x %08x %08x %08x %08x %08x", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        }
    }

    if (const uint8_t* stack = g_stack.load()) {
        const uint32_t here = StackTop(stack);
        if (g_currentDistrict.exchange(here) != here && g_cfg.diagnostics) Log("You're in district %u", here);
    }

    const bool open = st[shop::kIsOpen] != 0;
    const int type = (int)*(const uint32_t*)(st + shop::kShopType);
    const uint32_t id = *(const uint32_t*)(st + shop::kShopId);
    const bool wasOpen = g_open.exchange(open);
    g_type = type;
    g_id = id;
    if (open) {
        if (!wasOpen) {
            Log("Shop open: id %u type %d, district %08x", id, type, g_district.load());
            // The formation opened at its counter: its script just attached its camera (the wardrobe tab borrows it).
            if (type == 1 && g_phase.load() == kIdle) {
                const uint64_t cam = g_lastAttach.load();
                const bool recent = cam && now - g_lastAttachAt.load() < 3000;
                g_counterCamera = recent ? cam : 0;
                if (g_cfg.diagnostics) Log("Counter camera: %llx%s", (unsigned long long)cam, recent ? "" : " (not recent: none)");
            }
            // A vendor you opened yourself at its counter (our switches open it mid-swap, or in place).
            if (type == 0 && g_phase.load() == kIdle) {
                g_lockedId = 0;
                if (!VendorVisited(id)) {
                    SaveVendorVisited(id);
                    Log("Vendor %u opened at its counter: unlocked in the switcher", id);
                }
            }
        }
    } else if (wasOpen) {
        Log("Shop closed");
        if (g_phase.load() == kIdle) g_lockedId = 0;
    }
    Pair();
    if (g_cfg.diagnostics) LogStateNames();
    const bool wardrobe = WardrobeOnStack(stacks);
    if (g_wardrobe.exchange(wardrobe) != wardrobe) Log("Wardrobe %s", wardrobe ? "open" : "closed");

    const bool pending = st[shop::kEventPending] != 0;
    switch (g_phase.load()) {
        case kIdle: {
            const int want = g_want.load();
            if (want < 0 || pending) break;
            g_want = -1;
            g_target = want;
            if (want == kWantWardrobe) {
                if (wardrobe) {
                    Finish(kResDone);
                } else if (!open || !g_setState || !g_outfitState.load() || !g_orientationInstalled.load()) {
                    Finish(kResNotOpen);
                } else {
                    // Close the shop the way its Esc does; the wardrobe goes on once the shop state is off.
                    g_targetId = 0;
                    g_returnId = id;  // reopened if the wardrobe doesn't come up
                    QueueEvent(st, shop::kClose, 0);
                    g_phase = kClosing;
                    g_phaseAt = now;
                    Log("Swap to the wardrobe: closing %s %u", type == 0 ? "vendor" : "formation", id);
                }
                break;
            }
            g_targetId = g_wantId.load() ? g_wantId.load() : want == 0 ? g_vendorId.load() : g_formationId.load();
            if (!g_targetId || ShopType(g_targetId) != (uint32_t)want) {
                Finish(kResNoTarget);
                break;
            }
            if (!open) {
                if (!wardrobe || !g_setState || !g_outfitState.load()) {
                    Finish(kResNotOpen);
                    break;
                }
                // From the wardrobe: turn the player back, close it with its own Back (once more from an item's
                // browser), then the shop as its counter would open it.
                g_restoreFacing = true;
                g_spotSeen = g_spotWrites.load();
                g_phase = kLeavingWardrobe;
                g_phaseAt = now;
                Log("Swap to %s %u: closing the wardrobe", want == 0 ? "vendor" : "formation", g_targetId);
                break;
            }
            if (type == want && id == g_targetId) {
                Finish(kResDone);  // already there
                break;
            }
            if (type == 0 && want == 0) {
                // Vendor to vendor: no Close, so the screen never fades out. Open with the other id (the vendor
                // state is already on top, so the game pushes nothing) and mark the list stale, so
                // handle_state_change rebuilds it for the new id as it does after a sort change.
                g_lockedId = VendorVisited(g_targetId) ? 0 : g_targetId;
                QueueEvent(st, shop::kOpen, g_targetId);
                *(uint64_t*)(st + shop::kSortSeen) = ~*(const uint64_t*)(st + shop::kSort);
                g_phase = kOpening;
                g_phaseAt = now;
                Log("Swap to vendor %u: in place from vendor %u", g_targetId, id);
                break;
            }
            QueueEvent(st, shop::kClose, 0);
            g_phase = kClosing;
            g_phaseAt = now;
            Log("Swap to %s %u: closing %s %u", want == 0 ? "vendor" : "formation", g_targetId,
                type == 0 ? "vendor" : "formation", id);
            break;
        }
        case kClosing:
            if (!open && !pending && g_target == kWantWardrobe) {
                // The game's own wardrobe, opened with its own Open (what the Lua open_player_outfit queues), so it
                // locks the player and runs its opening; HookOrientation gives it the standing spot.
                g_lockedId = 0;
                g_restoreFacing = false;
                QueueOutfit(kOutfitOpen);
                Log("Swap: wardrobe on (%llu ms after close)", now - g_phaseAt.load());
                g_phase = kOpening;
                g_phaseAt = now;
            } else if (!open && !pending) {
                g_lockedId = g_target == 0 && !VendorVisited(g_targetId) ? g_targetId : 0;
                QueueEvent(st, shop::kOpen, g_targetId);
                Log("Swap: opening %u (%llu ms after close)", g_targetId, now - g_phaseAt.load());
                g_phase = kOpening;
                g_phaseAt = now;
            } else if (now - g_phaseAt.load() > 1500) {
                Finish(kResTimeout);
            }
            break;
        case kOpening:
            if (g_target == kWantWardrobe) {
                if (wardrobe) {
                    Finish(kResDone);
                } else if (now - g_phaseAt.load() > 500) {
                    // The wardrobe didn't come up: back to the shop it left (as one of our switches, so it isn't
                    // taken for a visit at its counter), rather than no menu at all.
                    const uint32_t back = g_returnId, backType = ShopType(back);
                    if (back && backType <= 1 && !open && !pending) {
                        Log("The wardrobe didn't open; back to shop %u", back);
                        g_target = (int)backType;
                        g_targetId = back;
                        g_lockedId = backType == 0 && !VendorVisited(back) ? back : 0;
                        QueueEvent(st, shop::kOpen, back);
                        g_phaseAt = now;
                    } else {
                        Log("The wardrobe didn't open");
                        Finish(kResTimeout);
                    }
                }
            } else if (open && type == g_target && id == g_targetId) {
                // The vendor the formation's tab opens next time (a locked one isn't worth returning to).
                if (g_target == 0 && VendorVisited(g_targetId)) SavePairedVendor(g_targetId);
                Finish(kResDone);
            }
            else if (now - g_phaseAt.load() > 1500) Finish(kResTimeout);
            break;
        case kLeavingWardrobe: {
            if (wardrobe) {
                if (now - g_phaseAt.load() > 1500) {
                    // Its Back didn't take: off the stack directly (it and its browser), so the switch completes.
                    Log("The wardrobe didn't close with its Back; taken off the stack");
                    SetWardrobe(false, stacks);
                    break;
                }
                // One Back at a time; from the wardrobe's own page, only once the player has been turned back.
                const bool turnedBack = g_spotWrites.load() != g_spotSeen || now - g_phaseAt.load() > 250;
                uint8_t* w = g_outfitState.load();
                if (w && !w[shop::kEventPending] && (turnedBack || !WardrobeCurrent(stacks))) QueueOutfit(kOutfitClose);
                break;
            }
            g_restoreFacing = false;
            g_lockedId = g_target == 0 && !VendorVisited(g_targetId) ? g_targetId : 0;
            QueueEvent(st, shop::kOpen, g_targetId);
            Log("Swap: opening %u (%llu ms after the wardrobe)", g_targetId, now - g_phaseAt.load());
            g_phase = kOpening;
            g_phaseAt = now;
            break;
        }
    }
}

static void Fault() {
    if (!g_faulted.exchange(true)) Log("Exception in the shop hook; VendorPlus stops touching the game");
}

// ---- the vendor level ----
// The game gives a vendor the level of the district you're standing in. A vendor opened through us stands
// somewhere else, so it gets its own zone's level: VendorLevelForDistrict with its home district. Every level
// check (the header, the item locks, a purchase) goes through here, so they all agree.
using ShopLevelFn = int (*)(void*, void*, void*, void*);
using DistrictLevelFn = int (*)(uint32_t, void*, void*, void*);
static void* volatile g_origShopLevel = nullptr;
static DistrictLevelFn g_districtLevel = nullptr;
static std::atomic<bool> g_levelInstalled{false};
static std::atomic<uint8_t*> g_statePtr{nullptr};  // ShopMenuState (set by the events hook)
static std::atomic<uint32_t> g_homeUsed{0};          // the vendor whose home level was used last
static std::atomic<bool> g_tableLogged{false};

// DistrictDatabase: 0x18-byte records (id at +0) at +8, count at +0x10 (as 0x142667740 reads it).
static bool DistrictAt(const uint8_t* db, uint32_t index, uint32_t& id) {
    const uint8_t* recs = *(uint8_t* const*)(db + 8);
    const uint32_t count = *(const uint32_t*)(db + 0x10);
    if (!recs || index >= count || count > 256) return false;
    id = *(const uint32_t*)(recs + (size_t)index * 0x18);
    return true;
}
static bool DistrictExists(const uint8_t* db, uint32_t id) {
    uint32_t d;
    for (uint32_t i = 0; DistrictAt(db, i, d); ++i)
        if (d == id) return true;
    return false;
}

static uint32_t HomeOf(uint32_t vendor) {
    for (const KnownVendor& k : kKnownVendors)
        if (k.id == vendor && k.district) return k.district;
    return HomeDistrict(vendor);
}


static void LevelFault() {
    if (!g_faulted.exchange(true)) Log("Exception in the vendor level hook; VendorPlus stops touching the game");
}

// One level call: the logging, the districts the map showed, and the override. true = use `out`.
static bool LevelFrame(void* districts, void* progress, void* stack, void* world, int& out) {
    const uint32_t here = StackTop((const uint8_t*)stack);
    if (g_currentDistrict.exchange(here) != here && g_cfg.diagnostics) Log("Vendor level: you're in district %u", here);
    const uint8_t* db = (const uint8_t*)districts;
    if (db && g_cfg.diagnostics && !g_tableLogged.exchange(true)) {
        std::string list;
        uint32_t d;
        for (uint32_t i = 0; DistrictAt(db, i, d); ++i) {
            char b[48];
            sprintf_s(b, "%s#%u %u (level %d)", list.empty() ? "" : ", ", i, d, g_districtLevel(d, districts, progress, world));
            list += b;
        }
        Log("Districts: %s", list.c_str());
    }
    g_stack = (const uint8_t*)stack;
    const uint8_t* st = g_statePtr.load();
    if (st && st[shop::kIsOpen] && *(const uint32_t*)(st + shop::kShopType) == 0) {
        const uint32_t vendor = *(const uint32_t*)(st + shop::kShopId);
        if (vendor == g_lockedId.load() && !VendorVisited(vendor)) {
            out = 0;  // locked: every item is above level 0
            return true;
        }
        const uint32_t home = HomeOf(vendor);
        if (home && home != here && DistrictExists(db, home)) {
            const int level = g_districtLevel(home, districts, progress, world);
            if (g_homeUsed.exchange(vendor) != vendor)
                Log("Vendor %u: its own district %u, level %d (you're in %u)", vendor, home, level, here);
            out = level;
            return true;
        }
        g_homeUsed = 0;
    }
    return false;
}

static int HookShopLevel(void* districts, void* progress, void* stack, void* world) {
    if (!g_faulted.load() && stack) {
        int level = 0;
        bool use = false;
        __try {
            use = LevelFrame(districts, progress, stack, world, level);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LevelFault();
            use = false;
        }
        if (use) return level;
    }
    return ((ShopLevelFn)g_origShopLevel)(districts, progress, stack, world);
}

bool InstallLevelHookAt(uint8_t* shopLevel, const uint8_t* shopLevelBytes, void* districtLevel, std::string& err) {
    g_districtLevel = (DistrictLevelFn)districtLevel;
    if (!InstallJmpHook(shopLevel, shopLevelBytes, (void*)&HookShopLevel, &g_origShopLevel, "vendor level", err))
        return false;
    g_levelInstalled = true;
    return true;
}

// process_events(Query [1], ShopMenuState& [2], Query, Query, ShopDatabase& [5], PinnedItemsDatabase&,
//                TutorialState&, world_state Database&, CharmDatabase, OutfitItemDatabase, CurrencyDatabase,
//                DistrictDatabase, ActiveDistrict [13], UIStateStacks& [14], AudioEventRequests& [15])
static void HookEvents(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9,
                       void* a10, void* a11, void* a12, void* a13, void* a14, void* a15, void* a16, void* a17,
                       void* a18) {
    if (!g_faulted.load() && a2) {
        __try {
            g_statePtr = (uint8_t*)a2;
            EventsFrame((uint8_t*)a2, (const uint8_t*)a5, (const uint8_t*)a13, a14);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault();
        }
    }
    ((EventsFn)g_origEvents)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18);
}

// The menu actions this frame, from the game's own input (so every pad, remapped buttons and the keyboard all
// work): each press of LB / RB (MENU_PREV / MENU_NEXT) and the secondary pair is counted; the script acts on them
// while its tabs show. Only reads: the shop itself ignores these actions.
static void InputFrame(uint8_t* input) {
    if (!*(void* const*)input) return;  // no input this frame (as the game's own check)
    void* actions = input + kInputActions;
    for (int i = 0; i < 4; ++i) {
        if (!g_keyIds[i]) continue;
        const bool down = g_pressed(actions, *g_keyIds[i]);
        if (down && !g_padDown[i]) ++g_padCount[i];
        g_padDown[i] = down;
    }
}

static void HookInput(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, void* a9,
                      void* a10) {
    if (!g_faulted.load() && a7) {
        __try {
            InputFrame((uint8_t*)a7);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!g_faulted.exchange(true)) Log("Exception in the input hook; VendorPlus stops touching the game");
        }
    }
    ((InputFn)g_origInput)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

// rip-relative target of the 4-byte displacement at rva + off, for an instruction ending at rva + end.
static uint32_t RipTarget(const Image& img, uint32_t rva, uint32_t off, uint32_t end) {
    return rva + end + (uint32_t)img.I32(rva + off);
}

bool FindInputTargets(const Image& img, InputTargets& t, std::string& err) {
    t = InputTargets();
    t.input = FindUnique(img, "shop input", kSigInput, err);
    const uint32_t keys = t.input ? FindUnique(img, "menu keys", kSigMenuKeys, err) : 0;
    if (!keys) return false;
    t.pressed = RipTarget(img, keys, 22, 26);
    t.keys[0] = RipTarget(img, keys, 48, 52);  // MENU_NEXT
    t.keys[1] = RipTarget(img, keys, 29, 33);  // MENU_PREV
    std::string err2;
    if (const uint32_t second = FindUnique(img, "secondary keys", kSigSecondaryKeys, err2)) {
        t.keys[2] = RipTarget(img, second, 6, 10);   // MENU_NEXT_SECONDARY
        t.keys[3] = RipTarget(img, second, 24, 28);  // MENU_PREV_SECONDARY
    }
    return true;
}

bool FindWardrobeTargets(const Image& img, WardrobeTargets& t, std::string& err) {
    t = WardrobeTargets();
    const uint32_t call = FindUnique(img, "UI state call", kSigSetStateCall, err);
    const uint32_t setState = call ? RipTarget(img, call, 24, 28) : 0;
    Pattern p;
    p.Parse(kSigSetState);
    if (!setState || !MatchAt(img, setState, p)) {
        if (err.empty()) err = "the UI state function changed";
        return false;
    }
    const uint32_t n = (uint32_t)p.bytes.size();  // the pattern ends with the StateIndex call
    const uint32_t open = FindUnique(img, "wardrobe Open", kSigWardrobeOpen, err);
    if (!open) return false;
    Pattern o;
    o.Parse(kSigWardrobeOpen);
    const uint32_t m = (uint32_t)o.bytes.size();  // it ends with the load of "player_outfit"
    t.setState = setState;
    t.findStack = RipTarget(img, setState, 53, 57);
    t.stateIndex = RipTarget(img, setState, n, n + 4);
    t.gameStatesName = RipTarget(img, open, 10, 14);
    t.wardrobeName = RipTarget(img, open, m, m + 4);
    return true;
}

// ---- diagnostics: what the wardrobe in the world does when you use it ----
// Its script opens the screen with the Lua function open_player_outfit, which only queues the wardrobe's Open; the
// close-up and the player's placing come from the interaction around it. With Diagnostics=1 (known build only) the
// game's Lua bindings that could do that are logged with their arguments, read from the Lua stack (Lua 5.1: base at
// lua_State+24, top at +16, 16-byte values with the type at +8), so the game's own argument checks see nothing.
struct LuaBinding {
    const char* name;
    size_t prologue;  // whole instructions, position independent, at least 15 bytes (checked offline)
};
static const LuaBinding kLuaBindings[] = {
    {"open_player_outfit", 15}, {"camera_set", 19}, {"clear_camera_set", 16},
    {"camera_detach", 20}, {"camera_attach_bone", 15}, {"camera_detach_bone", 20}, {"camera_target", 15},
    {"camera_reset_target", 20}, {"camera_look_direction", 17}, {"camera_reset_look_direction", 15},
    {"camera_side", 15}, {"attach_to_character", 16}, {"spawn_and_attach_to_character", 16},
    {"set_reattach_parent", 15}, {"set_interactable_enabled", 17}, {"zone_transition", 16},
    {"keyframed_movement_get_position", 17}, {"is_execution_blocked", 17},
};
const int kLuaCount = sizeof(kLuaBindings) / sizeof(kLuaBindings[0]);
using LuaFn = int (*)(void* L);
static void* volatile g_luaOrig[kLuaCount] = {};

// The binding's function, from the game's registration table: `lea rax, [name]` then `lea rax, [function]`.
static uint32_t FindLuaBinding(const Image& img, const char* name) {
    const size_t n = strlen(name);
    uint32_t str = 0;
    for (const Section& sec : img.secs) {
        if (sec.exec || sec.write || sec.size < n + 2) continue;
        const uint8_t* p = &img.mem[sec.rva];
        const uint8_t* end = p + sec.size - n - 1;
        for (const uint8_t* q = p; q < end && (q = (const uint8_t*)memchr(q, name[0], end - q)) != nullptr; ++q) {
            if (memcmp(q, name, n) != 0 || q[n] != 0 || (q != p && q[-1] != 0)) continue;
            if (str) return 0;  // twice: not this one
            str = sec.rva + (uint32_t)(q - p);
        }
    }
    if (!str) return 0;
    uint32_t found = 0;
    for (const Section& sec : img.secs) {
        if (!sec.exec) continue;
        const uint8_t* p = &img.mem[sec.rva];
        for (uint32_t off = 0; off + 7 <= sec.size; ++off) {
            if (p[off] != 0x48 || p[off + 1] != 0x8D || p[off + 2] != 0x05) continue;
            int32_t d;
            memcpy(&d, p + off + 3, 4);
            if (sec.rva + off + 7 + (uint32_t)d != str) continue;
            for (uint32_t k = off + 7; k + 7 <= sec.size && k < off + 27; ++k) {
                if (p[k] != 0x48 || p[k + 1] != 0x8D || p[k + 2] != 0x05) continue;
                int32_t d2;
                memcpy(&d2, p + k + 3, 4);
                const uint32_t fn = sec.rva + k + 7 + (uint32_t)d2;
                if (found && found != fn) return 0;
                found = fn;
                break;
            }
        }
    }
    return found;
}

// This Lua's values are 24 bytes with the type at +0x10 (as its lua_type reads them), strings are type 5, and the
// stack's base is at lua_State+0x10 with its top at +0x8. Strings go through the game's lua_tolstring (no
// conversion for a string); everything else is shown raw.
struct LuaValue {
    uint64_t v, v2;
    int tt, pad;
};
using LuaToLStringFn = const char* (*)(void* L, int idx, size_t* len);
static LuaToLStringFn g_luaToLString = nullptr;
static void LuaArgs(void* L, char* out, size_t cap) {
    const uint8_t* Ls = (const uint8_t*)L;
    const LuaValue* base = *(const LuaValue* const*)(Ls + 0x10);
    const LuaValue* top = *(const LuaValue* const*)(Ls + 0x8);
    size_t used = 0;
    for (int i = 0; base && base + i < top && i < 8 && used + 100 < cap; ++i) {
        const LuaValue& a = base[i];
        char b[96];
        if (a.tt == 5 && g_luaToLString) {
            size_t len = 0;
            const char* str = g_luaToLString(L, i + 1, &len);
            sprintf_s(b, "\"%.*s\"", (int)(len < 60 ? len : 60), str ? str : "");
        } else if (a.tt == 3) {
            double d;
            memcpy(&d, &a.v, 8);
            sprintf_s(b, "%g", d);
        } else if (a.tt == 0) {
            strcpy_s(b, "nil");
        } else if (a.tt == 1) {
            strcpy_s(b, (uint32_t)a.v ? "true" : "false");
        } else {
            sprintf_s(b, "t%d:%llx", a.tt, (unsigned long long)a.v);
        }
        used += (size_t)sprintf_s(out + used, cap - used, "%s%s", i ? ", " : "", b);
    }
}
static void LogLua(int i, void* L) {
    char args[400] = "";
    __try {
        LuaArgs(L, args, sizeof(args));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(args, "?");
    }
    Log("Lua %s(%s)", kLuaBindings[i].name, args);
}
template <int I>
static int HookLua(void* L) {
    LogLua(I, L);
    return ((LuaFn)g_luaOrig[I])(L);
}

// camera_attach(entity, blend), always hooked: the camera a counter (or a wardrobe) frames you with.
static void* volatile g_origCameraAttach = nullptr;
static void CaptureAttach(void* L) {
    const uint8_t* Ls = (const uint8_t*)L;
    const LuaValue* base = *(const LuaValue* const*)(Ls + 0x10);
    const LuaValue* top = *(const LuaValue* const*)(Ls + 0x8);
    if (base && base < top && base[0].tt == 2) {
        g_lastAttach = base[0].v;
        g_lastAttachAt = GetTickCount64();
        ++g_attachSeq;
    }
}
static int HookCameraAttach(void* L) {
    __try {
        CaptureAttach(L);
        if (g_cfg.diagnostics) {
            char args[400] = "";
            LuaArgs(L, args, sizeof(args));
            Log("Lua camera_attach(%s)", args);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ((LuaFn)g_origCameraAttach)(L);
}
static const LuaFn kLuaHooks[kLuaCount] = {
    HookLua<0>, HookLua<1>, HookLua<2>, HookLua<3>, HookLua<4>, HookLua<5>, HookLua<6>, HookLua<7>, HookLua<8>,
    HookLua<9>, HookLua<10>, HookLua<11>, HookLua<12>, HookLua<13>, HookLua<14>, HookLua<15>, HookLua<16>,
    HookLua<17>,
};

// ui_player_outfit::handle_state_change body (entity tuple, Query, PlayerOutfitMenuState&, BroadcastState&,
// OutfitItemDatabase, UIStateStacks): the tuple holds the player's component pointers, +0x28 its ResolvedParent
// array (optional), +0x38 the index. Logged when the wardrobe's state comes or goes: whether the player is attached
// to something (the wardrobe in the world) at that moment.
static const char* kSigOutfitStateChange =
    "48 8B C4 48 89 50 10 48 89 48 08 53 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 C5 F8 29 70 B8 4D 8B F9 "
    "4D 8B F0 C5 FC 10 09";
using OutfitStateChangeFn = void (*)(void*, void*, void*, void*, void*, void*);
static void* volatile g_origOutfitStateChange = nullptr;
static void OutfitStateChangeFrame(const uint8_t* tuple, void* stacks) {
    static int last = -1;
    const int on = WardrobeOnStack(stacks) ? 1 : 0;
    if (on == last) return;
    last = on;
    const uint8_t* parents = *(const uint8_t* const*)(tuple + 0x28);
    const uint64_t index = *(const uint64_t*)(tuple + 0x38);
    const uint64_t handle = parents ? *(const uint64_t*)(parents + index * 8) : 0;
    const uint8_t* keyframed = *(const uint8_t* const*)(tuple + 8);
    int count, cursor, idx;
    StackInfo(stacks, count, cursor, idx);
    Log("Wardrobe state %s: player parent %s (%llx), keyframed %d; game_states: %d entries, cursor %d, wardrobe at %d",
        on ? "on" : "off", parents ? "present" : "none", (unsigned long long)handle, keyframed ? keyframed[index * 2] : -1,
        count, cursor, idx);
}
static void HookOutfitStateChange(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    if (a3) g_outfitState = (uint8_t*)a3;  // PlayerOutfitMenuState: its events are queued there
    if (g_cfg.diagnostics) {
        __try {
            if (a1) OutfitStateChangeFrame((const uint8_t*)a1, a6);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    ((OutfitStateChangeFn)g_origOutfitStateChange)(a1, a2, a3, a4, a5, a6);
}

uint32_t LuaBindingRva(const Image& img, const char* name) { return FindLuaBinding(img, name); }

static void InstallWardrobeDiagnostics(const Image& img) {
    if (!g_knownBuild) {
        Log("Diagnostics hooks: only on the known build");
        return;
    }
    std::string err;
    int hooked = 0;
    for (int i = 0; i < kLuaCount; ++i) {
        const uint32_t fn = FindLuaBinding(img, kLuaBindings[i].name);
        if (fn && InstallJmpHook((uint8_t*)g_gameBase + fn, &img.mem[fn], (void*)kLuaHooks[i], &g_luaOrig[i],
                                 kLuaBindings[i].name, err, kLuaBindings[i].prologue))
            ++hooked;
        else
            Log("Diagnostics: no hook on Lua %s (%s)", kLuaBindings[i].name, fn ? err.c_str() : "not found");
    }
    Log("Diagnostics: logging %d Lua bindings", hooked);
    // lua_tolstring: the call at +54 of camera_set (after its luaL_checktype).
    const uint32_t cs = FindLuaBinding(img, "camera_set");
    if (cs && img.Contains(cs, 60) && img.mem[cs + 54] == 0xE8) g_luaToLString = (LuaToLStringFn)(g_gameBase + RipTarget(img, cs, 55, 59));
    else Log("Diagnostics: lua_tolstring not found; strings show raw");
}

static void InstallInputAndWardrobe(const Image& img) {
    if (g_cfg.diagnostics) InstallWardrobeDiagnostics(img);
    std::string err;
    InputTargets in;
    if (FindInputTargets(img, in, err)) {
        g_pressed = (PressedFn)(g_gameBase + in.pressed);
        for (int i = 0; i < 4; ++i) g_keyIds[i] = in.keys[i] ? (const uint16_t*)(g_gameBase + in.keys[i]) : nullptr;
        if (InstallJmpHook((uint8_t*)g_gameBase + in.input, &img.mem[in.input], (void*)&HookInput, &g_origInput,
                           "shop input", err)) {
            g_inputInstalled = true;
            Log("Game hook active: shop input +0x%x (controller: tabs%s)", in.input, in.keys[2] ? ", vendors" : "");
        }
    }
    if (!g_inputInstalled.load()) Log("Controller support off (%s)", err.c_str());

    err.clear();
    WardrobeTargets w;
    if (FindWardrobeTargets(img, w, err)) {
        g_findStack = (FindStackFn)(g_gameBase + w.findStack);
        g_stateIndex = (StateIndexFn)(g_gameBase + w.stateIndex);
        g_gameStatesName = (const uint8_t*)(g_gameBase + w.gameStatesName);
        g_wardrobeName = (const uint8_t*)(g_gameBase + w.wardrobeName);
        g_setState = (SetStateFn)(g_gameBase + w.setState);
        Log("Wardrobe UI state +0x%x", w.setState);
    } else {
        Log("Wardrobe tab off (%s)", err.c_str());
        return;
    }
    // The wardrobe's own systems: its state change (its menu state, for its events) and its orientation (the spot).
    // Both or neither: without the spot its Open would put the player at the world's origin.
    err.clear();
    OrientationTargets o;
    if (!FindOrientationTargets(img, o, err)) {
        Log("Wardrobe tab off (%s)", err.c_str());
        return;
    }
    g_isCurrent = (IsCurrentFn)(g_gameBase + o.isCurrent);
    g_setLocal = (SetLocalFn)(g_gameBase + o.setLocal);
    if (!InstallJmpHook((uint8_t*)g_gameBase + o.orientation, &img.mem[o.orientation], (void*)&HookOrientation,
                        &g_origOrientation, "wardrobe orientation", err, kOrientationPrologue)) {
        Log("Wardrobe tab off (%s)", err.c_str());
        return;
    }
    g_orientationInstalled = true;
    if (!InstallJmpHook((uint8_t*)g_gameBase + o.stateChange, &img.mem[o.stateChange], (void*)&HookOutfitStateChange,
                        &g_origOutfitStateChange, "wardrobe state change", err, 16)) {
        Log("Wardrobe tab off (%s); the player is still never placed at the origin", err.c_str());
        return;
    }
    Log("Wardrobe tab ready: orientation +0x%x, state change +0x%x", o.orientation, o.stateChange);

    // The close-up: known build only (the world's layout is checked against this build).
    err.clear();
    CameraTargets ct;
    if (!g_knownBuild) {
        Log("Wardrobe camera off (other game build): the wardrobe shows with the normal camera");
    } else if (!FindCameraTargets(img, o, ct, err)) {
        Log("Wardrobe camera off (%s)", err.c_str());
    } else if (!InstallJmpHook((uint8_t*)g_gameBase + ct.luaAttach, &img.mem[ct.luaAttach], (void*)&HookCameraAttach,
                               &g_origCameraAttach, "Lua camera_attach", err, 17)) {
        Log("Wardrobe camera off (%s)", err.c_str());
    } else {
        g_typeSlot = (TypeSlotFn)(g_gameBase + ct.typeSlot);
        g_attach = (AttachFn)(g_gameBase + ct.attach);
        g_detach = (DetachFn)(g_gameBase + ct.detach);
        g_cameraReady = true;
        Log("Wardrobe camera ready: attach +0x%x, detach +0x%x", ct.attach, ct.detach);
    }
}

bool FindCameraTargets(const Image& img, const OrientationTargets& o, CameraTargets& t, std::string& err) {
    t = CameraTargets();
    t.luaAttach = FindLuaBinding(img, "camera_attach");
    t.luaDetach = FindLuaBinding(img, "camera_detach");
    if (!t.luaAttach || !t.luaDetach || !img.Contains(t.luaAttach, 0x50) || !img.Contains(t.luaDetach, 0x40) ||
        img.mem[t.luaAttach + 0x47] != 0xE8 || img.mem[t.luaDetach + 0x2c] != 0xE8) {
        err = "the Lua camera functions changed";
        return false;
    }
    t.attach = RipTarget(img, t.luaAttach, 0x48, 0x4c);
    t.detach = RipTarget(img, t.luaDetach, 0x2d, 0x31);
    // The type search the parent lookup uses (+0xf5), and the world offsets it reads, as this build has them.
    if (!o.lookup || !img.Contains(o.lookup, 0x120) || img.mem[o.lookup + 0xf5] != 0xE8) {
        err = "the transform lookup changed";
        return false;
    }
    t.typeSlot = RipTarget(img, o.lookup, 0xf6, 0xfa);
    if (img.U32(o.lookup + 0x5e) != kEntCount || img.U32(o.lookup + 0x6b) != kEntGen || img.U32(o.lookup + 0xbb) != kEntLoc ||
        img.U32(t.luaAttach + 0) == 0) {
        err = "the ECS world layout changed";
        return false;
    }
    return true;
}

// The orientation hook on a stand-in with the game's prologue, with stand-ins for the calls it makes (tests).
bool InstallOrientationHookAt(uint8_t* target, const uint8_t* prologue, const OrientationTestFns& f, std::string& err) {
    g_isCurrent = (IsCurrentFn)f.isCurrent;
    g_setLocal = (SetLocalFn)f.setLocal;
    g_findStack = (FindStackFn)f.findStack;
    g_stateIndex = (StateIndexFn)f.stateIndex;
    g_gameStatesName = f.gameStatesName;
    g_wardrobeName = f.wardrobeName;
    if (!InstallJmpHook(target, prologue, (void*)&HookOrientation, &g_origOrientation, "wardrobe orientation", err,
                        kOrientationPrologue))
        return false;
    g_orientationInstalled = true;
    return true;
}
void SetRestoreFacingForTest(bool on) { g_restoreFacing = on; }
void InstallCameraForTest(void* typeSlot, void* attach, void* detach) {
    g_typeSlot = (TypeSlotFn)typeSlot;
    g_attach = (AttachFn)attach;
    g_detach = (DetachFn)detach;
    g_cameraReady = true;
}
void SetCounterCameraForTest(uint64_t handle) { g_counterCamera = handle; }
void SetLastAttachForTest(uint64_t handle) {
    g_lastAttach = handle;
    g_lastAttachAt = GetTickCount64();
    ++g_attachSeq;
}

bool FindOrientationTargets(const Image& img, OrientationTargets& t, std::string& err) {
    t = OrientationTargets();
    if (!(t.orientation = FindUnique(img, "wardrobe orientation", kSigOrientation, err))) return false;
    if (!(t.stateChange = FindUnique(img, "wardrobe state change", kSigOutfitStateChange, err))) return false;
    const uint32_t cur = FindUnique(img, "wardrobe current check", kSigIsCurrentCall, err);
    const uint32_t set = FindUnique(img, "wardrobe transform call", kSigSetLocalCall, err);
    if (!cur || !set) return false;
    if (cur < t.orientation || cur > t.orientation + 0x400 || set < t.orientation || set > t.orientation + 0x400) {
        err = "the wardrobe orientation changed";
        return false;
    }
    t.isCurrent = RipTarget(img, cur, 15, 19);
    t.setLocal = RipTarget(img, set, 33, 37);
    // SetLocalTransform looks the parent's transform up with the call at +0xdc.
    if (!img.Contains(t.setLocal, 0xe1) || img.mem[t.setLocal + 0xdc] != 0xE8) {
        err = "the wardrobe transform function changed";
        return false;
    }
    t.lookup = RipTarget(img, t.setLocal, 0xdd, 0xe1);
    return true;
}

// ---- setup ----
bool FindShopTargets(const Image& img, uint32_t& events, std::string& err) {
    events = 0;
    if (!(events = FindUnique(img, "shop events", kSigEvents, err))) return false;
    if (!FindUnique(img, "shop Open", kSigOpen, err)) return false;
    if (!FindUnique(img, "shop open flag", kSigOpenFlag, err)) return false;
    if (!FindUnique(img, "shop list rebuild", kSigRebuild, err)) return false;
    return true;
}

bool InstallShopHookAt(uint8_t* events, const uint8_t* eventsBytes, std::string& err) {
    if (!InstallJmpHook(events, eventsBytes, (void*)&HookEvents, &g_origEvents, "shop events", err)) return false;
    g_installed = true;
    return true;
}

bool InstallShopHook(const Image& img, std::string& err) {
    uint32_t events = 0;
    if (!FindShopTargets(img, events, err)) return false;
    if (!InstallShopHookAt((uint8_t*)g_gameBase + events, &img.mem[events], err)) return false;
    Log("Game hook active: shop events +0x%x", events);
    InstallInputAndWardrobe(img);
    // The vendor level: optional; without it vendors keep the game's rule (the level where you stand).
    std::string lerr;
    const uint32_t shopLevel = FindUnique(img, "vendor level", kSigShopLevel, lerr);
    const uint32_t districtLevel = shopLevel ? FindUnique(img, "district vendor level", kSigDistrictLevel, lerr) : 0;
    if (shopLevel && districtLevel &&
        InstallLevelHookAt((uint8_t*)g_gameBase + shopLevel, &img.mem[shopLevel], (uint8_t*)g_gameBase + districtLevel, lerr))
        Log("Game hook active: vendor level +0x%x (district level +0x%x)", shopLevel, districtLevel);
    else
        Log("Vendor level hook off (%s): vendors keep the level of the zone you're in", lerr.c_str());
    return true;
}

bool ShopHookInstalled() { return g_installed.load(); }

// ---- endpoint ----
static long long Age(ULONGLONG t, ULONGLONG now) { return t ? (long long)(now - t) : -1; }

static std::string StatusJson(bool accepted) {
    const ULONGLONG now = GetTickCount64();
    const bool open = g_open.load();
    const int type = g_type.load();
    const uint32_t id = g_id.load(), formation = g_formationId.load(), vendor = g_vendorId.load();
    std::vector<uint32_t> ids;
    std::vector<const char*> zoneNames;
    SwitcherVendors(ids, zoneNames);
    std::string vendors, zones, visited;
    for (size_t i = 0; i < ids.size(); ++i) {
        vendors += (i ? "," : "") + std::to_string(ids[i]);
        zones += std::string(i ? "," : "") + "\"" + zoneNames[i] + "\"";
        visited += std::string(i ? "," : "") + (VendorVisited(ids[i]) ? "true" : "false");
    }
    // One of the pair is open (the formation, or any vendor), so the title bar can offer the rest.
    const bool paired = open && formation && vendor && ((type == 1 && id == formation) || (type == 0 && ShopType(id) == 0));
    char buf[768];
    sprintf_s(buf,
              "{\"ok\":true,\"version\":\"" VP_VERSION "\",\"installed\":%s,\"accepted\":%s,\"age\":%lld,\"open\":%s,"
              "\"type\":%d,\"id\":%u,\"formationId\":%u,\"vendorId\":%u,\"paired\":%s,\"shops\":%d,\"district\":\"%08x\","
              "\"busy\":%s,\"serial\":%u,\"result\":\"%s\",\"here\":%u,\"homeLevel\":%s,\"locked\":%s,"
              "\"wardrobe\":%s,\"canWardrobe\":%s,\"pad\":[%u,%u,%u,%u],\"vendors\":[",
              g_installed.load() && !g_faulted.load() ? "true" : "false", accepted ? "true" : "false",
              Age(g_seen.load(), now), open ? "true" : "false", type, id, formation, vendor, paired ? "true" : "false",
              g_shopCount.load(), g_district.load(),
              (g_want.load() >= 0 || g_phase.load() != kIdle) ? "true" : "false", g_serial.load(),
              kResultNames[g_result.load()], g_currentDistrict.load(),
              (open && type == 0 && g_levelInstalled.load() && HomeOf(id)) ? "true" : "false",
              (open && type == 0 && id == g_lockedId.load() && !VendorVisited(id)) ? "true" : "false",
              g_wardrobe.load() ? "true" : "false",
              g_setState && g_outfitState.load() && g_orientationInstalled.load() ? "true" : "false", g_padCount[0].load(),
              g_padCount[1].load(), g_padCount[2].load(), g_padCount[3].load());
    return std::string(buf) + vendors + "],\"zones\":[" + zones + "],\"visited\":[" + visited + "]}";
}

std::string ShopAction(const std::string& action, const char* query) {
    const bool usable = g_installed.load() && !g_faulted.load();
    bool accepted = false;
    if (action == "swap") {
        // to=0: the vendor, to=1: artifact formation, to=2: the wardrobe; or id=<shop id> for that shop (the
        // vendor switcher).
        std::string to, idText;
        int want = QueryParam(query, "to", to) && (to == "0" || to == "1" || to == "2") ? to[0] - '0' : -1;
        uint32_t wantId = 0;
        if (QueryParam(query, "id", idText) && !idText.empty() && idText.size() <= 10 &&
            idText.find_first_not_of("0123456789") == std::string::npos) {
            wantId = (uint32_t)strtoul(idText.c_str(), nullptr, 10);
            const uint32_t t = ShopType(wantId);
            want = t <= 1 ? (int)t : -1;
        }
        // A request nobody claimed for 3 s (the game was paused or loading) is replaced.
        if (g_want.load() >= 0 && GetTickCount64() - g_wantAt.load() > 3000) g_want = -1;
        if (usable && want >= 0 && g_phase.load() == kIdle && g_want.load() < 0) {
            g_wantAt = GetTickCount64();
            g_wantId = wantId;
            g_want = want;
            accepted = true;
        }
        Log("Script asks: swap to %s %u (%s)", want == 0 ? "vendor" : want == 1 ? "formation" : want == 2 ? "wardrobe" : "?", wantId,
            accepted ? "accepted" : usable ? "busy" : "hook unavailable");
    }
    if (action == "slot") {
        // The wardrobe's slot whose items are open (its place in the grid), -1 = none: the face camera's cue.
        std::string n;
        if (QueryParam(query, "slot", n) && !n.empty() && n.size() <= 3) {
            const int slot = atoi(n.c_str());
            if (g_openSlot.exchange(slot) != slot && g_cfg.diagnostics) Log("Wardrobe slot open: %d", slot);
            accepted = true;
        }
    }
    if (action == "here") {
        // The map is open and marks you in <zone>: the district you're in now is that zone's. Its vendors get it
        // as their home district.
        std::string zone;
        const uint32_t here = g_currentDistrict.load();
        if (QueryParam(query, "zone", zone) && here && g_stack.load())
            for (const KnownVendor& k : kKnownVendors)
                if (_stricmp(k.zone, zone.c_str()) == 0 && !k.district) {
                    if (HomeDistrict(k.id) != here) Log("Learned: %s is district %u (vendor %u)", k.zone, here, k.id);
                    SaveHomeDistrict(k.id, here);
                    accepted = true;
                }
    }
    return StatusJson(accepted);
}

std::string BuildConfigJs() {
    std::string js = "window.__VendorPlusConfig={version:\"" VP_VERSION "\",diagnostics:";
    js += g_cfg.diagnostics ? "1" : "0";
    js += ",hook:";
    js += g_installed.load() ? "1" : "0";
    js += "};";
    return js;
}

}  // namespace vp
