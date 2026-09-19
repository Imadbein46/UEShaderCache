// UEShaderCache.asi - Unreal Engine 4/5 shader & PSO caching plugin for Ultimate ASI Loader
//
// Entry: Ultimate ASI Loader calls LoadLibrary on every *.asi next to the game exe (or in
// scripts\ / plugins\) from the proxy DLL's DllMain -> we run before the game's WinMain.
//
// Startup sequence:
//   1. Load UEShaderCache.ini, open log.
//   2. Detect Unreal (project name, version, Saved\Config path) and inject CVars into Engine.ini.
//   3. Initialise MinHook; hook D3D12CreateDevice (d3d12.dll is force-loaded, that's cheap).
//   4. Hook LoadLibraryExW so we can hook dstorage.dll / d3d11.dll when the game loads them later.
//   5. Set driver-side shader cache hints for DX11 paths.

#include "common.h"
#include "d3d12_cache.h"
#include "ue_cvars.h"
#include "dstorage_tune.h"
#include <MinHook.h>
#include <atomic>

using namespace usc;

static HMODULE g_self = nullptr;
static std::atomic<bool> g_started{false};

// ------------------------------------------------------------------ LoadLibrary watcher
typedef HMODULE (WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static PFN_LoadLibraryExW g_origLoadLibraryExW = nullptr;

static bool NameIs(LPCWSTR path, const wchar_t* dll) {
    if (!path) return false;
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') leaf = p + 1;
    return _wcsicmp(leaf, dll) == 0;
}

static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE h = g_origLoadLibraryExW(name, file, flags);
    if (h && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))) {
        if (NameIs(name, L"dstorage.dll")) usc::dstorage::Install(h);
    }
    return h;
}

// ------------------------------------------------------------------ D3D11 driver cache hints
static void ApplyD3D11DriverCacheHints() {
    if (!GetConfig().d3d11HintDriverCache) return;
    // NVIDIA: the driver honours a per-process shader cache size cap through the environment
    // (used by e.g. DXVK / Special K). 0xFFFFFFFF = unlimited. Harmless on other vendors.
    SetEnvironmentVariableW(L"__GL_SHADER_DISK_CACHE", L"1");
    SetEnvironmentVariableW(L"__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", L"1");
    SetEnvironmentVariableW(L"__GL_SHADER_DISK_CACHE_SIZE", L"4294967295");
    // Put the driver cache next to our own cache so it survives driver "clean" installs
    std::wstring nvPath = GetConfig().cacheDir + L"\\NV";
    if (EnsureDir(nvPath)) SetEnvironmentVariableW(L"__GL_SHADER_DISK_CACHE_PATH", nvPath.c_str());
    // AMD / Intel honour the standard D3D shader cache; nothing to do beyond not disabling it.
    LOGV("D3D11: driver shader-cache hints applied");
}

// ------------------------------------------------------------------ startup
static void Startup() {
    if (g_started.exchange(true)) return;

    LoadConfig(g_self);
    LogOpen(g_self);
    LOGI("UEShaderCache %s starting (pid %lu, exe '%s')", USC_VERSION, GetCurrentProcessId(), Narrow(GetExeName()).c_str());
    if (!GetConfig().enabled) { LOGI("Disabled via ini"); return; }

    const UEInfo& ue = DetectUnreal();
    if (ue.isUnreal)
        LOGI("Unreal Engine %d.%d detected - project '%s', root '%s'", ue.majorVersion, ue.minorVersion,
             Narrow(ue.projectName).c_str(), Narrow(ue.projectRoot).c_str());
    else
        LOGI("Not an Unreal Engine layout - running in generic D3D12 PSO-cache mode");

    usc::ue::InjectCVars();

    if (MH_Initialize() != MH_OK) { LOGI("MinHook init failed - hooks disabled"); return; }

    if (GetConfig().d3d12PipelineLibrary) usc::d3d12::Install();
    ApplyD3D11DriverCacheHints();

    // Watch for late-loaded modules (dstorage.dll)
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* ll = kb ? (void*)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
    if (!ll) ll = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryExW");
    if (ll && MH_CreateHook(ll, (void*)&Hook_LoadLibraryExW, (void**)&g_origLoadLibraryExW) == MH_OK) MH_EnableHook(ll);
    usc::dstorage::Install(nullptr); // already loaded?

    LOGI("Startup complete");
}

// ------------------------------------------------------------------ exports
// Ultimate ASI Loader calls InitializeASI if exported (after all plugins are loaded);
// we also start from DllMain so nothing is missed if the loader is an older build.
extern "C" __declspec(dllexport) void InitializeASI() { Startup(); }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        Startup();
        break;
    case DLL_PROCESS_DETACH:
        usc::d3d12::SaveAll("detach");
        LogClose();
        break;
    }
    return TRUE;
}
