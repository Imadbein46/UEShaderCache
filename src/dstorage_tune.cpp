// UEShaderCache - DirectStorage tuning
//
// DirectStorage itself has nothing to do with shaders, but UE5 titles that ship dstorage.dll
// (e.g. via the IoStore/DirectStorage plugin) can hitch on asset streaming when the default
// 32 MB staging buffer fills up, and some GPUs hitch during GDeflate GPU decompression while
// also compiling PSOs. We hook DStorageGetFactory and:
//   * call DStorageSetConfiguration() *before* the factory is created (GPU decompression toggle)
//   * call IDStorageFactory::SetStagingBufferSize() on the returned factory
//
// Only the tiny ABI subset we touch is declared here (dstorage.h is not part of MinGW).

#include "dstorage_tune.h"
#include <MinHook.h>
#include <unknwn.h>
#include <atomic>

namespace usc::dstorage {

// --- minimal DirectStorage ABI ------------------------------------------------------------
struct DSTORAGE_CONFIGURATION {
    UINT32 NumSubmitThreads;
    INT32  NumBuiltInCpuDecompressionThreads;
    BOOL   ForceMappingLayer;
    BOOL   DisableBypassIO;
    BOOL   DisableTelemetry;
    BOOL   DisableGpuDecompressionMetacommand;
    BOOL   DisableGpuDecompression;
};
// {6924EA0C-C3CD-4826-B10A-F64F4ED927C1}
static const GUID IID_IDStorageFactory = { 0x6924ea0c, 0xc3cd, 0x4826, { 0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1 } };

struct IDStorageFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(const void* desc, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenFile(const WCHAR* path, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(UINT32 capacity, const CHAR* name, REFIID riid, void** ppv) = 0;
    virtual void    STDMETHODCALLTYPE SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(UINT32 size) = 0;
};

typedef HRESULT (WINAPI* PFN_DStorageGetFactory)(REFIID, void**);
typedef HRESULT (WINAPI* PFN_DStorageSetConfiguration)(const DSTORAGE_CONFIGURATION*);

static PFN_DStorageGetFactory       g_origGetFactory = nullptr;
static PFN_DStorageSetConfiguration g_setConfig      = nullptr;
static std::atomic<bool>            g_installed{false};
static std::atomic<bool>            g_configured{false};
static std::atomic<bool>            g_stagingSet{false};

static HRESULT WINAPI Hook_DStorageGetFactory(REFIID riid, void** ppv) {
    const Config& c = GetConfig();
    if (!g_configured.exchange(true) && g_setConfig && c.dstorageDisableGPUDecompression) {
        DSTORAGE_CONFIGURATION cfg{};
        cfg.DisableGpuDecompression = TRUE;
        HRESULT hr = g_setConfig(&cfg);
        LOGI("DStorage: DStorageSetConfiguration(DisableGpuDecompression=1) -> 0x%08X", (unsigned)hr);
    }
    HRESULT hr = g_origGetFactory(riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && !g_stagingSet.exchange(true)) {
        IDStorageFactory* f = nullptr;
        if (SUCCEEDED(((IUnknown*)*ppv)->QueryInterface(IID_IDStorageFactory, (void**)&f)) && f) {
            UINT32 bytes = (UINT32)c.dstorageStagingMB << 20;
            HRESULT shr = f->SetStagingBufferSize(bytes);
            LOGI("DStorage: SetStagingBufferSize(%d MB) -> 0x%08X", c.dstorageStagingMB, (unsigned)shr);
            f->Release();
        }
    }
    return hr;
}

bool Install(HMODULE dstorageDll) {
    if (g_installed.load()) return true;
    if (!GetConfig().dstorageTune) return false;
    if (!dstorageDll) dstorageDll = GetModuleHandleW(L"dstorage.dll");
    if (!dstorageDll) return false;

    void* target = (void*)GetProcAddress(dstorageDll, "DStorageGetFactory");
    g_setConfig = (PFN_DStorageSetConfiguration)GetProcAddress(dstorageDll, "DStorageSetConfiguration");
    if (!target) return false;
    if (MH_CreateHook(target, (void*)&Hook_DStorageGetFactory, (void**)&g_origGetFactory) != MH_OK) return false;
    MH_EnableHook(target);
    g_installed = true;
    LOGI("DStorage: dstorage.dll detected - DStorageGetFactory hooked (staging=%d MB, cpuDecomp=%d)",
         GetConfig().dstorageStagingMB, GetConfig().dstorageDisableGPUDecompression ? 1 : 0);
    return true;
}

} // namespace usc::dstorage
