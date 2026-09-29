#include "common.h"
#include <wchar.h>
#include <map>

namespace vp {

Config g_cfg;

static std::wstring IniPath() { return g_modDir + L"VendorPlus.ini"; }
// What the mod remembers (visited vendors, the last vendor, district overrides) lives in its own file, so
// overwriting VendorPlus.ini with a new version's never loses it. Older builds kept it in VendorPlus.ini: still read.
static std::wstring StatePath() { return g_modDir + L"VendorPlus.state.ini"; }

// Every "key=value" of a section, from the state file and (older builds) VendorPlus.ini.
static void ReadSection(const wchar_t* section, std::map<uint32_t, uint32_t>& out) {
    for (const std::wstring& file : {IniPath(), StatePath()}) {
        wchar_t buf[4096];
        DWORD n = GetPrivateProfileSectionW(section, buf, 4096, file.c_str());
        for (const wchar_t* p = buf; n && *p; p += wcslen(p) + 1) {
            const uint32_t k = (uint32_t)wcstoul(p, nullptr, 10);
            const wchar_t* eq = wcschr(p, L'=');
            if (k && eq) out[k] = (uint32_t)wcstoul(eq + 1, nullptr, 10);
        }
    }
}

static std::wstring ReadString(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, L"", buf, 512, IniPath().c_str());
    std::wstring s(buf);
    size_t semi = s.find(L';');
    if (semi != std::wstring::npos) s.resize(semi);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && iswspace(s[start])) ++start;
    return s.substr(start);
}

static bool ReadBool(const wchar_t* key, bool def) {
    std::wstring s = ReadString(L"VendorPlus", key);
    if (s.empty()) return def;
    if (_wcsicmp(s.c_str(), L"true") == 0 || _wcsicmp(s.c_str(), L"on") == 0 || _wcsicmp(s.c_str(), L"yes") == 0)
        return true;
    if (_wcsicmp(s.c_str(), L"false") == 0 || _wcsicmp(s.c_str(), L"off") == 0 || _wcsicmp(s.c_str(), L"no") == 0)
        return false;
    return _wtoi(s.c_str()) != 0;
}

static uint32_t ReadU32(const wchar_t* section, const wchar_t* key) {
    std::wstring s = ReadString(section, key);
    return s.empty() ? 0 : (uint32_t)wcstoul(s.c_str(), nullptr, 0);
}

void LoadConfig() {
    Config c;
    c.enabled = ReadBool(L"Enabled", true);
    c.diagnostics = ReadBool(L"Diagnostics", false);
    c.vendorId = ReadU32(L"VendorPlus", L"VendorId");
    g_cfg = c;
}

// [Districts] vendor id = district id, read once (the level hook asks on the game thread, often).
static SRWLOCK g_homesLock = SRWLOCK_INIT;
static std::map<uint32_t, uint32_t> g_homes;
static bool g_homesRead = false;

uint32_t HomeDistrict(uint32_t vendor) {
    AcquireSRWLockExclusive(&g_homesLock);
    if (!g_homesRead) {
        g_homesRead = true;
        std::map<uint32_t, uint32_t> all;
        ReadSection(L"Districts", all);
        for (auto& kv : all)
            if (kv.second >= 1024) g_homes[kv.first] = kv.second;  // small values: map positions from test builds
    }
    auto it = g_homes.find(vendor);
    const uint32_t d = it == g_homes.end() ? 0 : it->second;
    ReleaseSRWLockExclusive(&g_homesLock);
    return d;
}

void SaveHomeDistrict(uint32_t vendor, uint32_t district) {
    if (HomeDistrict(vendor) == district) return;
    AcquireSRWLockExclusive(&g_homesLock);
    g_homes[vendor] = district;
    ReleaseSRWLockExclusive(&g_homesLock);
    wchar_t key[16], val[16];
    swprintf_s(key, L"%u", vendor);
    swprintf_s(val, L"%u", district);
    WritePrivateProfileStringW(L"Districts", key, val, StatePath().c_str());
}

// [Visited] vendor id = 1, read once and kept in memory.
static SRWLOCK g_visitedLock = SRWLOCK_INIT;
static std::map<uint32_t, bool> g_visited;
static bool g_visitedRead = false;

bool VendorVisited(uint32_t vendor) {
    AcquireSRWLockExclusive(&g_visitedLock);
    if (!g_visitedRead) {
        g_visitedRead = true;
        std::map<uint32_t, uint32_t> all;
        ReadSection(L"Visited", all);
        for (auto& kv : all)
            if (kv.second) g_visited[kv.first] = true;
    }
    const bool yes = g_visited.count(vendor) != 0;
    ReleaseSRWLockExclusive(&g_visitedLock);
    return yes;
}

void SaveVendorVisited(uint32_t vendor) {
    if (VendorVisited(vendor)) return;
    AcquireSRWLockExclusive(&g_visitedLock);
    g_visited[vendor] = true;
    ReleaseSRWLockExclusive(&g_visitedLock);
    wchar_t key[16];
    swprintf_s(key, L"%u", vendor);
    WritePrivateProfileStringW(L"Visited", key, L"1", StatePath().c_str());
}

static uint32_t g_paired = 0;
static bool g_pairedRead = false;

uint32_t PairedVendor() {
    if (!g_pairedRead) {
        wchar_t buf[32];
        GetPrivateProfileStringW(L"State", L"PairedVendor", L"", buf, 32, StatePath().c_str());
        g_paired = buf[0] ? (uint32_t)wcstoul(buf, nullptr, 10) : ReadU32(L"State", L"PairedVendor");
        g_pairedRead = true;
    }
    return g_paired;
}

void SavePairedVendor(uint32_t id) {
    if (id == PairedVendor()) return;
    g_paired = id;
    wchar_t buf[16];
    swprintf_s(buf, L"%u", id);
    WritePrivateProfileStringW(L"State", L"PairedVendor", buf, StatePath().c_str());
}

// The wardrobe's slots (their place in its grid): whether wardrobes in the world show them with their face camera.
// Learned from those wardrobes as you use them ([Wardrobe] in the state file); -1 = not seen yet.
static SRWLOCK g_closeUpLock = SRWLOCK_INIT;
static int g_closeUp[8];
static bool g_closeUpRead = false;

int WardrobeCloseUp(int slot) {
    if (slot < 0 || slot >= 8) return -1;
    AcquireSRWLockExclusive(&g_closeUpLock);
    if (!g_closeUpRead) {
        g_closeUpRead = true;
        for (int i = 0; i < 8; ++i) {
            wchar_t key[16];
            swprintf_s(key, L"Slot%d", i);
            const int v = (int)GetPrivateProfileIntW(L"Wardrobe", key, -1, StatePath().c_str());
            g_closeUp[i] = v == 0 || v == 1 ? v : -1;
        }
    }
    const int v = g_closeUp[slot];
    ReleaseSRWLockExclusive(&g_closeUpLock);
    return v;
}

void SaveWardrobeCloseUp(int slot, bool closeUp) {
    if (slot < 0 || slot >= 8 || WardrobeCloseUp(slot) == (closeUp ? 1 : 0)) return;
    AcquireSRWLockExclusive(&g_closeUpLock);
    g_closeUp[slot] = closeUp ? 1 : 0;
    ReleaseSRWLockExclusive(&g_closeUpLock);
    wchar_t key[16];
    swprintf_s(key, L"Slot%d", slot);
    WritePrivateProfileStringW(L"Wardrobe", key, closeUp ? L"1" : L"0", StatePath().c_str());
}

}  // namespace vp
