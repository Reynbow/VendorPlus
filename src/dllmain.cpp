// Entry point. crloader (winmm.dll) loads every DLL in crmods\ at start-up; we do our setup on a worker
// thread so the loader lock is never held while we read and scan the game image.
#include "common.h"

namespace vp {

HMODULE g_self = nullptr;
std::wstring g_modDir;
uintptr_t g_gameBase = 0;
bool g_knownBuild = false;

static const char* kKnownBuildId = "6ab107a0-06301000-05eedcbd";  // build 25472515

static void Setup() {
    LoadConfig();
    LogInit();
    Log("VendorPlus " VP_VERSION " folder=%s", Utf8(g_modDir).c_str());
    if (!g_cfg.enabled) {
        Log("Disabled by INI (Enabled=0)");
        return;
    }
    wchar_t name[64];
    swprintf_s(name, L"Local\\VendorPlus.Instance.%lu", GetCurrentProcessId());
    HANDLE instance = CreateMutexW(nullptr, TRUE, name);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("Another VendorPlus copy is already active in this process; this copy stays idle");
        return;
    }
    // Intentionally never closed: marks this process as served for its lifetime.

    g_gameBase = (uintptr_t)GetModuleHandleW(nullptr);
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (!n || n >= sizeof(exe) / sizeof(exe[0])) {
        Log("Cannot resolve the game executable path; not installing");
        return;
    }
    Image img;
    std::string build;
    if (!LoadPristineImage(exe, img, build)) {
        Log("Cannot read the game image; not installing");
        return;
    }
    g_knownBuild = build == kKnownBuildId;
    Log("Game build %s (%s)", build.c_str(), g_knownBuild ? "known build 25472515" : "other build; running on signatures");
    Log("Settings: VendorId=%u PairedVendor=%u", g_cfg.vendorId, PairedVendor());

    // The game hook first, so the script's config already says whether it works when the UI asks for it.
    std::string err;
    if (!InstallShopHook(img, err)) Log("Shop hook off: %s. The title bar switch stays off on this game version.", err.c_str());
    err.clear();
    if (!InstallResourceHook(img, err)) {
        Log("Resource interception failed (%s); VendorPlus stays off for this game version", err.c_str());
        return;
    }
    Log("Setup done: hook=%d diagnostics=%d", ShopHookInstalled(), g_cfg.diagnostics);
}

static DWORD WINAPI SetupThread(void*) {
    try {
        Setup();
    } catch (...) {
        Log("Initialization exception; no further setup attempted");
    }
    return 0;
}

}  // namespace vp

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        vp::g_self = module;
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        std::wstring p(path, n);
        size_t slash = p.find_last_of(L"\\/");
        vp::g_modDir = slash == std::wstring::npos ? L".\\" : p.substr(0, slash + 1);
        HANDLE h = CreateThread(nullptr, 0, vp::SetupThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
