// VendorPlus - every vendor from Artifact Formation in CONTROL Resonant.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>
#include "version.h"

namespace vp {

// ---- globals (dllmain.cpp) ----
extern HMODULE g_self;
extern std::wstring g_modDir;   // folder holding vendorplus.dll, trailing backslash
extern uintptr_t g_gameBase;    // live base of CONTROLResonant.exe

// ---- logging (util.cpp) ----
void LogInit();
void Log(const char* fmt, ...);

// ---- small helpers (util.cpp) ----
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes);
bool WriteWholeFile(const std::wstring& path, const void* data, size_t size);  // temp file + replace
bool EnsureDirectoryFor(const std::wstring& filePath);
std::string UrlDecode(const char* s, size_t n);
// Value of key in a query string ("a=1&b=2"), URL-decoded. Returns false when absent.
bool QueryParam(const char* query, const char* key, std::string& out);
std::string JsonEscape(const std::string& s);

// ---- configuration (config.cpp) ----
struct Config {
    bool enabled = true;
    bool diagnostics = false;  // extra log lines from the DLL and the script
    uint32_t vendorId = 0;     // shop id of the vendor the formation's tab opens, 0 = the last one used
};
extern Config g_cfg;
void LoadConfig();
uint32_t HomeDistrict(uint32_t vendor);               // [Districts] in the INI, 0 = unknown
void SaveHomeDistrict(uint32_t vendor, uint32_t district);
bool VendorVisited(uint32_t vendor);                  // [Visited]: opened at its counter at least once
void SaveVendorVisited(uint32_t vendor);
uint32_t PairedVendor();           // the vendor last browsed to from the formation (saved in the INI)
void SavePairedVendor(uint32_t id);

// ---- pristine game image (image.cpp) ----
struct Section {
    char name[9];
    uint32_t rva, size;
    bool exec, write;
};
struct Image {
    std::vector<uint8_t> mem;  // sections copied to their RVAs
    uint64_t prefBase = 0;
    uint32_t sizeOfImage = 0;
    std::vector<Section> secs;
    const Section* SectionOf(uint32_t rva) const;
    bool Contains(uint32_t rva, uint32_t n) const { return (uint64_t)rva + n <= mem.size(); }
    uint32_t U32(uint32_t rva) const { uint32_t v; memcpy(&v, &mem[rva], 4); return v; }
    int32_t I32(uint32_t rva) const { int32_t v; memcpy(&v, &mem[rva], 4); return v; }
    uint64_t U64(uint32_t rva) const { uint64_t v; memcpy(&v, &mem[rva], 8); return v; }
    float F32(uint32_t rva) const { float v; memcpy(&v, &mem[rva], 4); return v; }
};
bool LoadPristineImage(const std::wstring& exePath, Image& img, std::string& buildId);
struct Pattern {
    std::vector<int> bytes;  // -1 = wildcard
    bool Parse(const char* text);
};
// All matches of pattern in executable sections (up to max).
std::vector<uint32_t> FindPattern(const Image& img, const Pattern& p, size_t max = 16);
bool MatchAt(const Image& img, uint32_t rva, const Pattern& p);
// Unique match or 0 with an error message.
uint32_t FindUnique(const Image& img, const char* what, const char* pattern, std::string& err);

// ---- inline hooks (hook.cpp) ----
bool PatchCode(uint8_t* target, const uint8_t* patch, size_t n);
// Replaces the first 15 (position independent) bytes of target with a jump to hook; *original gets a
// trampoline that runs them and continues at target+15.
bool InstallJmpHook(uint8_t* target, const uint8_t prologue[15], void* hook, void* volatile* original, const char* what,
                    std::string& err);

// ---- UI resource interception (resources.cpp) ----
bool FindResourceSlot(const Image& img, uint32_t& slotRva, std::string& err);
bool InstallResourceHook(const Image& img, std::string& err);
bool BuildInjectedScript(const std::vector<uint8_t>& original, std::string& out);
std::string BuildConfigJs();

// ---- the shop hook (shop.cpp) ----
bool FindShopTargets(const Image& img, uint32_t& events, std::string& err);
bool InstallShopHook(const Image& img, std::string& err);
// The same on a stand-in function with the game's prologue (used by the offline tests).
bool InstallShopHookAt(uint8_t* events, const uint8_t* eventsBytes, std::string& err);
// The vendor level hook on stand-ins (tests): the shop's level function and the per-district one it calls.
bool InstallLevelHookAt(uint8_t* shopLevel, const uint8_t* shopLevelBytes, void* districtLevel, std::string& err);
bool ShopHookInstalled();
std::string ShopAction(const std::string& action, const char* query = nullptr);  // the __vendorplus__.json endpoint

// Structure offsets (pinned by the signatures in shop.cpp), shared with the tests.
namespace shop {
const size_t kEventPayload = 0x00;  // ShopMenuState: event payload (the shop id for Open)
const size_t kEventVariant = 0x04;  // event variant: 0 Open, 1 Close, 2.. focus, select, ...
const size_t kEventPending = 0x08;
const size_t kShopId = 0x290;       // the open (or last) shop's id
const size_t kShopType = 0x294;     // 0 vendor, 1 artifact formation
const size_t kIsOpen = 0x2fc;       // a shop state is on the game_states stack
const size_t kSort = 0x278, kSortSeen = 0x288;  // sort/category and the copy handle_state_change last built for
const uint8_t kOpen = 0, kClose = 1;
const size_t kDbCtrl = 0x30, kDbSlots = 0x38, kDbMask = 0x48;  // ShopDatabase: shop id -> shop (Swiss table)
const size_t kSlotSize = 0x20, kSlotKey = 0x18, kSlotType = 0x1c;  // slot: +0 id, +0x18 (a per-shop hash), +0x1c type
}  // namespace shop

}  // namespace vp
