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
#include "common.h"
#include <atomic>
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

// The system body takes the system's parameters in order; it has 15, forwarding a few more is harmless.
using EventsFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                          void*, void*, void*, void*, void*);

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
enum Phase { kIdle = 0, kClosing = 1, kOpening = 2 };
enum Result { kResNone = 0, kResDone, kResNoTarget, kResTimeout, kResNotOpen };
static const char* kResultNames[] = {"none", "done", "no-target", "timeout", "not-open"};
static std::atomic<int> g_want{-1};  // type the script asked for, claimed by the hook
static std::atomic<uint32_t> g_wantId{0};  // a specific shop (0 = the type's default)
static std::atomic<ULONGLONG> g_wantAt{0};
static std::atomic<int> g_phase{kIdle};
static std::atomic<ULONGLONG> g_phaseAt{0};
static int g_target = -1;         // game thread only
static uint32_t g_targetId = 0;   // game thread only
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

static void EventsFrame(uint8_t* st, const uint8_t* db, const uint8_t* district) {
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

    const bool pending = st[shop::kEventPending] != 0;
    switch (g_phase.load()) {
        case kIdle: {
            const int want = g_want.load();
            if (want < 0 || pending) break;
            g_want = -1;
            if (!open) {
                Finish(kResNotOpen);
                break;
            }
            g_target = want;
            g_targetId = g_wantId.load() ? g_wantId.load() : want == 0 ? g_vendorId.load() : g_formationId.load();
            if (!g_targetId || ShopType(g_targetId) != (uint32_t)want) {
                Finish(kResNoTarget);
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
            if (!open && !pending) {
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
            if (open && type == g_target && id == g_targetId) {
                // The vendor the formation's tab opens next time (a locked one isn't worth returning to).
                if (g_target == 0 && VendorVisited(g_targetId)) SavePairedVendor(g_targetId);
                Finish(kResDone);
            }
            else if (now - g_phaseAt.load() > 1500) Finish(kResTimeout);
            break;
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
            EventsFrame((uint8_t*)a2, (const uint8_t*)a5, (const uint8_t*)a13);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Fault();
        }
    }
    ((EventsFn)g_origEvents)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18);
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
    char buf[512];
    sprintf_s(buf,
              "{\"ok\":true,\"version\":\"" VP_VERSION "\",\"installed\":%s,\"accepted\":%s,\"age\":%lld,\"open\":%s,"
              "\"type\":%d,\"id\":%u,\"formationId\":%u,\"vendorId\":%u,\"paired\":%s,\"shops\":%d,\"district\":\"%08x\","
              "\"busy\":%s,\"serial\":%u,\"result\":\"%s\",\"here\":%u,\"homeLevel\":%s,\"locked\":%s,\"vendors\":[",
              g_installed.load() && !g_faulted.load() ? "true" : "false", accepted ? "true" : "false",
              Age(g_seen.load(), now), open ? "true" : "false", type, id, formation, vendor, paired ? "true" : "false",
              g_shopCount.load(), g_district.load(),
              (g_want.load() >= 0 || g_phase.load() != kIdle) ? "true" : "false", g_serial.load(),
              kResultNames[g_result.load()], g_currentDistrict.load(),
              (open && type == 0 && g_levelInstalled.load() && HomeOf(id)) ? "true" : "false",
              (open && type == 0 && id == g_lockedId.load() && !VendorVisited(id)) ? "true" : "false");
    return std::string(buf) + vendors + "],\"zones\":[" + zones + "],\"visited\":[" + visited + "]}";
}

std::string ShopAction(const std::string& action, const char* query) {
    const bool usable = g_installed.load() && !g_faulted.load();
    bool accepted = false;
    if (action == "swap") {
        // to=0: the vendor, to=1: artifact formation; or id=<shop id> for that shop (the vendor switcher).
        std::string to, idText;
        int want = QueryParam(query, "to", to) && (to == "0" || to == "1") ? to[0] - '0' : -1;
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
        Log("Script asks: swap to %s %u (%s)", want == 0 ? "vendor" : want == 1 ? "formation" : "?", wantId,
            accepted ? "accepted" : usable ? "busy" : "hook unavailable");
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
