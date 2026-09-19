// UEShaderCache - D3D12 persistent PSO cache via ID3D12PipelineLibrary
//
// How it works
// ------------
//  * D3D12CreateDevice is hooked so we learn about every ID3D12Device the game creates.
//  * The device vtable entries CreateGraphicsPipelineState / CreateComputePipelineState /
//    (ID3D12Device2) CreatePipelineState are hooked (they live in d3d12.dll code, so one hook
//    covers every device instance).
//  * Every PSO description is hashed (shader bytecode + fixed-function state). The hash becomes
//    the name of the PSO inside an ID3D12PipelineLibrary.
//  * On a cache HIT the driver deserialises the pre-compiled PSO in microseconds instead of
//    compiling it (tens/hundreds of ms) -> no hitch.
//  * On a MISS we call the original Create* and Store the result in the library.
//  * The library is serialised to <CacheDir>\<exe>_d3d12_<vendor>_<device>.pipelinelib on a
//    timer, on ExitProcess/TerminateProcess and when the plugin detaches.
//
// The D3D12 runtime validates the stored description against the requested one, and it also
// validates driver/adapter version, so a stale or colliding entry simply results in a miss.

#include "d3d12_cache.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <unordered_map>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <chrono>

#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH ((HRESULT)0x887E0002L)
#endif
#ifndef D3D12_ERROR_ADAPTER_NOT_FOUND
#define D3D12_ERROR_ADAPTER_NOT_FOUND ((HRESULT)0x887E0001L)
#endif

namespace usc::d3d12 {

// ------------------------------------------------------------------ vtable indices
// ID3D12Device (IUnknown 0-2, ID3D12Object 3-6)
enum : size_t {
    VT_Device_CreateGraphicsPipelineState = 10,
    VT_Device_CreateComputePipelineState  = 11,
    VT_Device_CreateRootSignature         = 16,
    VT_Device_GetAdapterLuid              = 43,
    VT_Device1_CreatePipelineLibrary      = 44,
    VT_Device2_CreatePipelineState        = 47,
};

// ------------------------------------------------------------------ globals
typedef HRESULT (WINAPI* PFN_D3D12CreateDevice_t)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateGraphicsPSO)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateComputePSO)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePSO)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateRootSig)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
typedef void    (WINAPI* PFN_ExitProcess_t)(UINT);
typedef BOOL    (WINAPI* PFN_TerminateProcess_t)(HANDLE, UINT);

static PFN_D3D12CreateDevice_t g_origCreateDevice = nullptr;
static PFN_CreateGraphicsPSO   g_origCreateGraphics = nullptr;
static PFN_CreateComputePSO    g_origCreateCompute  = nullptr;
static PFN_CreatePSO           g_origCreateStream   = nullptr;
static PFN_CreateRootSig       g_origCreateRootSig  = nullptr;
static PFN_ExitProcess_t       g_origExitProcess    = nullptr;
static PFN_TerminateProcess_t  g_origTerminateProcess = nullptr;

static std::atomic<bool> g_vtableHooked{false};
static std::mutex        g_installMx;
static Stats             g_stats;
static std::mutex        g_statsMx;

// Root signature pointer -> hash of its serialized blob (for PSO hashing).
static std::mutex g_rsMx;
static std::unordered_map<void*, uint64_t> g_rootSigHashes;

// Re-entrancy guard: LoadGraphicsPipeline etc. must never recurse into our hooks.
static thread_local int t_reentry = 0;
struct ReentryGuard { ReentryGuard() { ++t_reentry; } ~ReentryGuard() { --t_reentry; } };

// ------------------------------------------------------------------ per-adapter library
struct Library {
    ID3D12Device1*          device  = nullptr;   // AddRef'd, keeps device alive for Serialize()
    ID3D12PipelineLibrary*  lib     = nullptr;
    std::vector<uint8_t>    blob;                // backing memory for CreatePipelineLibrary
    std::wstring            path;
    std::shared_timed_mutex rw;                  // shared: Load/Store, exclusive: Serialize
    std::atomic<bool>       dirty{false};
    std::atomic<long>       storedSinceSave{0};
    bool                    unsupported = false;
};
static std::mutex g_libsMx;
static std::map<uint64_t, std::shared_ptr<Library>> g_libs;   // key = adapter LUID

static uint64_t LuidKey(const LUID& l) { return ((uint64_t)(uint32_t)l.HighPart << 32) | (uint32_t)l.LowPart; }

static std::wstring AdapterCachePath(ID3D12Device* dev, const LUID& luid) {
    UINT vendor = 0, devId = 0, sub = 0, rev = 0;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        auto pCreate = (HRESULT (WINAPI*)(REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory1");
        IDXGIFactory4* f = nullptr;
        if (pCreate && SUCCEEDED(pCreate(__uuidof(IDXGIFactory4), (void**)&f)) && f) {
            IDXGIAdapter1* a = nullptr;
            if (SUCCEEDED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a)) && a) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d))) {
                    vendor = d.VendorId; devId = d.DeviceId; sub = d.SubSysId; rev = d.Revision;
                    LOGI("D3D12: adapter '%s' vendor=%04X device=%04X", Narrow(d.Description).c_str(), vendor, devId);
                }
                a->Release();
            }
            f->Release();
        }
    }
    (void)dev;
    wchar_t name[256];
    swprintf(name, 256, L"\\%s_d3d12_%04X_%04X_%08X_%02X.pipelinelib", GetExeName().c_str(), vendor, devId, sub, rev);
    return GetConfig().cacheDir + name;
}

static std::shared_ptr<Library> GetLibrary(ID3D12Device* dev) {
    LUID luid = dev->GetAdapterLuid();
    uint64_t key = LuidKey(luid);
    {
        std::lock_guard<std::mutex> lk(g_libsMx);
        auto it = g_libs.find(key);
        if (it != g_libs.end()) return it->second->unsupported ? nullptr : it->second;
    }

    auto L = std::make_shared<Library>();
    // Check feature support first
    D3D12_FEATURE_DATA_SHADER_CACHE sc{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &sc, sizeof(sc))) ||
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
        LOGI("D3D12: driver does not support pipeline libraries (flags=0x%X); PSO caching disabled for this adapter", sc.SupportFlags);
        L->unsupported = true;
    } else if (FAILED(dev->QueryInterface(__uuidof(ID3D12Device1), (void**)&L->device)) || !L->device) {
        LOGI("D3D12: ID3D12Device1 unavailable; PSO caching disabled");
        L->unsupported = true;
    } else {
        EnsureDir(GetConfig().cacheDir);
        L->path = AdapterCachePath(dev, luid);

        HRESULT hr = E_FAIL;
        if (ReadFileAll(L->path, L->blob) && !L->blob.empty()) {
            size_t maxBytes = (size_t)GetConfig().d3d12MaxCacheMB << 20;
            if (L->blob.size() > maxBytes) {
                LOGI("D3D12: cache file %zu MB exceeds MaxCacheMB=%d, rebuilding", L->blob.size() >> 20, GetConfig().d3d12MaxCacheMB);
                L->blob.clear();
            } else {
                hr = L->device->CreatePipelineLibrary(L->blob.data(), L->blob.size(), __uuidof(ID3D12PipelineLibrary), (void**)&L->lib);
                if (FAILED(hr)) {
                    const char* why = hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "driver version changed" :
                                      hr == D3D12_ERROR_ADAPTER_NOT_FOUND ? "adapter changed" :
                                      hr == E_INVALIDARG ? "blob corrupt" : "unknown";
                    LOGI("D3D12: existing pipeline library rejected (0x%08X: %s) - rebuilding", (unsigned)hr, why);
                    L->lib = nullptr;
                    L->blob.clear();
                } else {
                    LOGI("D3D12: loaded pipeline library %s (%zu KB)", Narrow(L->path).c_str(), L->blob.size() >> 10);
                }
            }
        }
        if (!L->lib) {
            hr = L->device->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), (void**)&L->lib);
            if (FAILED(hr) || !L->lib) {
                LOGI("D3D12: CreatePipelineLibrary failed 0x%08X; PSO caching disabled", (unsigned)hr);
                L->unsupported = true;
                L->device->Release(); L->device = nullptr;
            } else {
                LOGI("D3D12: created new empty pipeline library -> %s", Narrow(L->path).c_str());
                L->dirty = true; // so an (empty) file gets written and path is validated early
            }
        }
    }

    std::lock_guard<std::mutex> lk(g_libsMx);
    auto it = g_libs.find(key);
    if (it != g_libs.end()) { // raced
        if (L->lib) L->lib->Release();
        if (L->device) L->device->Release();
        return it->second->unsupported ? nullptr : it->second;
    }
    g_libs[key] = L;
    return L->unsupported ? nullptr : L;
}

static void SaveLibrary(Library& L, const char* reason) {
    if (!L.lib || !L.dirty.exchange(false)) return;
    // Timed: during ExitProcess another thread may be frozen mid-Store; never deadlock the exit path.
    std::unique_lock<std::shared_timed_mutex> lk(L.rw, std::defer_lock);
    if (!lk.try_lock_for(std::chrono::seconds(5))) { LOGI("D3D12: save skipped, library busy [%s]", reason); L.dirty = true; return; }
    SIZE_T size = L.lib->GetSerializedSize();
    if (!size) return;
    std::vector<uint8_t> out(size);
    HRESULT hr = L.lib->Serialize(out.data(), size);
    lk.unlock();
    if (FAILED(hr)) { LOGI("D3D12: Serialize failed 0x%08X", (unsigned)hr); L.dirty = true; return; }
    if (!WriteFileAtomic(L.path, out.data(), out.size())) {
        LOGI("D3D12: failed to write %s (err %lu)", Narrow(L.path).c_str(), GetLastError());
        L.dirty = true;
        return;
    }
    long n = L.storedSinceSave.exchange(0);
    LOGI("D3D12: saved pipeline library (%zu KB, +%ld PSOs) [%s]", out.size() >> 10, n, reason);
}

void SaveAll(const char* reason) {
    std::vector<std::shared_ptr<Library>> libs;
    {
        std::lock_guard<std::mutex> lk(g_libsMx);
        for (auto& kv : g_libs) libs.push_back(kv.second);
    }
    for (auto& L : libs) SaveLibrary(*L, reason);
}

Stats GetStats() { std::lock_guard<std::mutex> lk(g_statsMx); return g_stats; }
static void Bump(long Stats::*m) { std::lock_guard<std::mutex> lk(g_statsMx); (g_stats.*m)++; }

// ------------------------------------------------------------------ hashing helpers
struct Hasher {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) { if (p && n) h = FNV1a64(p, n, h); }
    template <class T> void pod(const T& v) { bytes(&v, sizeof(T)); }
    void u32(uint32_t v) { pod(v); }
    void bytecode(const D3D12_SHADER_BYTECODE& bc) {
        u32((uint32_t)bc.BytecodeLength);
        if (bc.pShaderBytecode && bc.BytecodeLength) bytes(bc.pShaderBytecode, bc.BytecodeLength);
    }
    void rootSig(ID3D12RootSignature* rs) {
        uint64_t v = 0;
        if (rs) {
            std::lock_guard<std::mutex> lk(g_rsMx);
            auto it = g_rootSigHashes.find(rs);
            v = it != g_rootSigHashes.end() ? it->second : 0xDEAD0000ull;   // unknown -> still deterministic-ish; runtime validates
        }
        pod(v);
    }
    void inputLayout(const D3D12_INPUT_LAYOUT_DESC& il) {
        u32(il.NumElements);
        for (UINT i = 0; i < il.NumElements && il.pInputElementDescs; i++) {
            const auto& e = il.pInputElementDescs[i];
            if (e.SemanticName) bytes(e.SemanticName, strlen(e.SemanticName));
            u32(e.SemanticIndex); u32(e.Format); u32(e.InputSlot); u32(e.AlignedByteOffset);
            u32(e.InputSlotClass); u32(e.InstanceDataStepRate);
        }
    }
    void streamOutput(const D3D12_STREAM_OUTPUT_DESC& so) {
        u32(so.NumEntries); u32(so.NumStrides); u32(so.RasterizedStream);
        for (UINT i = 0; i < so.NumEntries && so.pSODeclaration; i++) {
            const auto& d = so.pSODeclaration[i];
            u32(d.Stream); if (d.SemanticName) bytes(d.SemanticName, strlen(d.SemanticName));
            u32(d.SemanticIndex); pod(d.StartComponent); pod(d.ComponentCount); pod(d.OutputSlot);
        }
        for (UINT i = 0; i < so.NumStrides && so.pBufferStrides; i++) u32(so.pBufferStrides[i]);
    }
    void viewInstancing(const D3D12_VIEW_INSTANCING_DESC& vi) {
        u32(vi.ViewInstanceCount); u32(vi.Flags);
        for (UINT i = 0; i < vi.ViewInstanceCount && vi.pViewInstanceLocations; i++) pod(vi.pViewInstanceLocations[i]);
    }
};

static uint64_t HashGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    Hasher H; H.u32('G');
    H.rootSig(d.pRootSignature);
    H.bytecode(d.VS); H.bytecode(d.PS); H.bytecode(d.DS); H.bytecode(d.HS); H.bytecode(d.GS);
    H.streamOutput(d.StreamOutput);
    H.pod(d.BlendState); H.u32(d.SampleMask); H.pod(d.RasterizerState); H.pod(d.DepthStencilState);
    H.inputLayout(d.InputLayout);
    H.u32(d.IBStripCutValue); H.u32(d.PrimitiveTopologyType);
    H.u32(d.NumRenderTargets); H.bytes(d.RTVFormats, sizeof(d.RTVFormats)); H.u32(d.DSVFormat);
    H.pod(d.SampleDesc); H.u32(d.NodeMask); H.u32(d.Flags & ~D3D12_PIPELINE_STATE_FLAG_DEBUG);
    return H.h;
}

static uint64_t HashCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d) {
    Hasher H; H.u32('C');
    H.rootSig(d.pRootSignature);
    H.bytecode(d.CS); H.u32(d.NodeMask); H.u32(d.Flags & ~D3D12_PIPELINE_STATE_FLAG_DEBUG);
    return H.h;
}

// Sub-object types newer than the MinGW / older SDK headers.
enum : int {
    SUBOBJ_DEPTH_STENCIL2 = 0x17, SUBOBJ_RASTERIZER1 = 0x1b, SUBOBJ_RASTERIZER2 = 0x1c,
};
// Sizes of the newer structs (bytes), see d3d12.h in the Agility SDK.
enum : size_t { SZ_DEPTH_STENCIL_DESC2 = 60, SZ_RASTERIZER_DESC1 = 44, SZ_RASTERIZER_DESC2 = 40 };

// Returns 0 on failure (unknown subobject).
static uint64_t HashStream(const D3D12_PIPELINE_STATE_STREAM_DESC& sd, bool& isCompute) {
    Hasher H; H.u32('S');
    isCompute = false;
    const uint8_t* p   = (const uint8_t*)sd.pPipelineStateSubobjectStream;
    const uint8_t* end = p + sd.SizeInBytes;
    const size_t ptrAlign = sizeof(void*);
    auto alignUp = [](size_t v, size_t a) { return (v + a - 1) & ~(a - 1); };

    while (p + sizeof(UINT) <= end) {
        UINT type = *(const UINT*)p;
        size_t dataSize = 0, dataAlign = 4;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: dataSize = sizeof(void*); dataAlign = ptrAlign; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            dataSize = sizeof(D3D12_SHADER_BYTECODE); dataAlign = ptrAlign; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: dataSize = sizeof(D3D12_STREAM_OUTPUT_DESC); dataAlign = ptrAlign; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: dataSize = sizeof(D3D12_BLEND_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: dataSize = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: dataSize = sizeof(D3D12_RASTERIZER_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: dataSize = sizeof(D3D12_DEPTH_STENCIL_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: dataSize = sizeof(D3D12_INPUT_LAYOUT_DESC); dataAlign = ptrAlign; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: dataSize = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: dataSize = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: dataSize = sizeof(D3D12_RT_FORMAT_ARRAY); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: dataSize = sizeof(DXGI_FORMAT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: dataSize = sizeof(DXGI_SAMPLE_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: dataSize = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: dataSize = sizeof(D3D12_CACHED_PIPELINE_STATE); dataAlign = ptrAlign; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: dataSize = sizeof(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: dataSize = sizeof(D3D12_DEPTH_STENCIL_DESC1); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: dataSize = sizeof(D3D12_VIEW_INSTANCING_DESC); dataAlign = ptrAlign; break;
        case SUBOBJ_DEPTH_STENCIL2: dataSize = SZ_DEPTH_STENCIL_DESC2; break;
        case SUBOBJ_RASTERIZER1:    dataSize = SZ_RASTERIZER_DESC1; break;
        case SUBOBJ_RASTERIZER2:    dataSize = SZ_RASTERIZER_DESC2; break;
        default:
            LOGV("D3D12: unknown PSO stream subobject type %u - not caching", type);
            return 0;
        }
        const uint8_t* data = p + alignUp(sizeof(UINT), dataAlign);
        size_t total = alignUp((data - p) + dataSize, ptrAlign);
        if (p + total > end) return 0;

        H.u32(type);
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: H.rootSig(*(ID3D12RootSignature* const*)data); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: isCompute = true; [[fallthrough]];
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            H.bytecode(*(const D3D12_SHADER_BYTECODE*)data); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: H.streamOutput(*(const D3D12_STREAM_OUTPUT_DESC*)data); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:  H.inputLayout(*(const D3D12_INPUT_LAYOUT_DESC*)data); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: H.viewInstancing(*(const D3D12_VIEW_INSTANCING_DESC*)data); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: break; // driver blob, not part of identity
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: H.u32(*(const UINT*)data & ~D3D12_PIPELINE_STATE_FLAG_DEBUG); break;
        default: H.bytes(data, dataSize); break;
        }
        p += total;
    }
    return H.h;
}

static void MakeName(wchar_t out[32], uint64_t h) { swprintf(out, 32, L"usc_%016llx", (unsigned long long)h); }

// ------------------------------------------------------------------ hooks
static HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T blobLen, REFIID riid, void** ppv) {
    HRESULT hr = g_origCreateRootSig(self, nodeMask, blob, blobLen, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && blob && blobLen) {
        uint64_t h = FNV1a64(blob, blobLen);
        std::lock_guard<std::mutex> lk(g_rsMx);
        g_rootSigHashes[*ppv] = h;
        if (g_rootSigHashes.size() > 65536) g_rootSigHashes.clear(); // pathological; keep bounded
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPipelineState(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    if (t_reentry || !desc || !ppv) return g_origCreateGraphics(self, desc, riid, ppv);
    ReentryGuard g;
    auto L = GetLibrary(self);
    if (!L) return g_origCreateGraphics(self, desc, riid, ppv);

    wchar_t name[32]; MakeName(name, HashGraphics(*desc));
    {
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT hr = L->lib->LoadGraphicsPipeline(name, desc, riid, ppv);
        if (SUCCEEDED(hr)) { Bump(&Stats::loaded); return hr; }
    }
    HRESULT hr = g_origCreateGraphics(self, desc, riid, ppv);
    if (SUCCEEDED(hr) && *ppv) {
        Bump(&Stats::created);
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT shr = L->lib->StorePipeline(name, (ID3D12PipelineState*)*ppv);
        if (SUCCEEDED(shr)) { Bump(&Stats::stored); L->dirty = true; L->storedSinceSave++; }
        else LOGV("D3D12: StorePipeline(graphics) failed 0x%08X", (unsigned)shr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateComputePipelineState(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    if (t_reentry || !desc || !ppv) return g_origCreateCompute(self, desc, riid, ppv);
    ReentryGuard g;
    auto L = GetLibrary(self);
    if (!L) return g_origCreateCompute(self, desc, riid, ppv);

    wchar_t name[32]; MakeName(name, HashCompute(*desc));
    {
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT hr = L->lib->LoadComputePipeline(name, desc, riid, ppv);
        if (SUCCEEDED(hr)) { Bump(&Stats::loaded); return hr; }
    }
    HRESULT hr = g_origCreateCompute(self, desc, riid, ppv);
    if (SUCCEEDED(hr) && *ppv) {
        Bump(&Stats::created);
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT shr = L->lib->StorePipeline(name, (ID3D12PipelineState*)*ppv);
        if (SUCCEEDED(shr)) { Bump(&Stats::stored); L->dirty = true; L->storedSinceSave++; }
        else LOGV("D3D12: StorePipeline(compute) failed 0x%08X", (unsigned)shr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** ppv) {
    if (t_reentry || !desc || !ppv || !desc->pPipelineStateSubobjectStream) return g_origCreateStream(self, desc, riid, ppv);
    ReentryGuard g;
    auto L = GetLibrary(self);
    if (!L) return g_origCreateStream(self, desc, riid, ppv);

    bool isCompute = false;
    uint64_t h = HashStream(*desc, isCompute);
    if (!h) { Bump(&Stats::skipped); return g_origCreateStream(self, desc, riid, ppv); }

    // Stream PSOs need ID3D12PipelineLibrary1::LoadPipeline
    ID3D12PipelineLibrary1* lib1 = nullptr;
    if (FAILED(L->lib->QueryInterface(__uuidof(ID3D12PipelineLibrary1), (void**)&lib1)) || !lib1) {
        Bump(&Stats::skipped);
        return g_origCreateStream(self, desc, riid, ppv);
    }
    wchar_t name[32]; MakeName(name, h);
    {
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT hr = lib1->LoadPipeline(name, desc, riid, ppv);
        if (SUCCEEDED(hr)) { Bump(&Stats::loaded); lib1->Release(); return hr; }
    }
    HRESULT hr = g_origCreateStream(self, desc, riid, ppv);
    if (SUCCEEDED(hr) && *ppv) {
        Bump(&Stats::created);
        std::shared_lock<std::shared_timed_mutex> lk(L->rw);
        HRESULT shr = lib1->StorePipeline(name, (ID3D12PipelineState*)*ppv);
        if (SUCCEEDED(shr)) { Bump(&Stats::stored); L->dirty = true; L->storedSinceSave++; }
        else LOGV("D3D12: StorePipeline(stream) failed 0x%08X", (unsigned)shr);
    }
    lib1->Release();
    return hr;
}

static void HookDeviceVTable(ID3D12Device* dev) {
    if (g_vtableHooked.load()) return;
    std::lock_guard<std::mutex> lk(g_installMx);
    if (g_vtableHooked.load()) return;

    void** vt = *(void***)dev;
    MH_STATUS s1 = MH_CreateHook(vt[VT_Device_CreateGraphicsPipelineState], (void*)&Hook_CreateGraphicsPipelineState, (void**)&g_origCreateGraphics);
    MH_STATUS s2 = MH_CreateHook(vt[VT_Device_CreateComputePipelineState],  (void*)&Hook_CreateComputePipelineState,  (void**)&g_origCreateCompute);
    MH_STATUS s3 = MH_CreateHook(vt[VT_Device_CreateRootSignature],         (void*)&Hook_CreateRootSignature,         (void**)&g_origCreateRootSig);
    MH_STATUS s4 = MH_ERROR_NOT_CREATED;
    ID3D12Device2* dev2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device2), (void**)&dev2)) && dev2) {
        void** vt2 = *(void***)dev2;
        s4 = MH_CreateHook(vt2[VT_Device2_CreatePipelineState], (void*)&Hook_CreatePipelineState, (void**)&g_origCreateStream);
        dev2->Release();
    }
    MH_EnableHook(MH_ALL_HOOKS);
    LOGI("D3D12: device vtable hooked (graphics=%d compute=%d rootsig=%d stream=%d)", s1, s2, s3, s4);
    g_vtableHooked = true;
}

static HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDevice) {
    HRESULT hr = g_origCreateDevice(adapter, fl, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(((IUnknown*)*ppDevice)->QueryInterface(__uuidof(ID3D12Device), (void**)&dev)) && dev) {
            LOGI("D3D12: device created (feature level 0x%X)", fl);
            HookDeviceVTable(dev);
            dev->Release();
        }
    }
    return hr;
}

// ------------------------------------------------------------------ process exit
static void WINAPI Hook_ExitProcess(UINT code) {
    SaveAll("ExitProcess");
    Stats s = GetStats();
    LOGI("D3D12: session stats - hits=%ld misses(compiled)=%ld stored=%ld skipped=%ld", s.loaded, s.created, s.stored, s.skipped);
    g_origExitProcess(code);
}
static BOOL WINAPI Hook_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) SaveAll("TerminateProcess");
    return g_origTerminateProcess(h, code);
}

static DWORD WINAPI SaverThread(LPVOID) {
    int interval = GetConfig().d3d12SaveIntervalSec;
    for (;;) {
        Sleep((DWORD)interval * 1000);
        SaveAll("periodic");
    }
    return 0;
}

// ------------------------------------------------------------------ install
bool Install() {
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) { LOGI("D3D12: d3d12.dll not available"); return false; }
    void* target = (void*)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!target) return false;

    if (MH_CreateHook(target, (void*)&Hook_D3D12CreateDevice, (void**)&g_origCreateDevice) != MH_OK) {
        LOGI("D3D12: failed to hook D3D12CreateDevice");
        return false;
    }

    if (GetConfig().d3d12SaveOnExit) {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        void* ep = kb ? (void*)GetProcAddress(kb, "ExitProcess") : nullptr;
        if (!ep && k32) ep = (void*)GetProcAddress(k32, "ExitProcess");
        void* tp = kb ? (void*)GetProcAddress(kb, "TerminateProcess") : nullptr;
        if (!tp && k32) tp = (void*)GetProcAddress(k32, "TerminateProcess");
        if (ep) MH_CreateHook(ep, (void*)&Hook_ExitProcess, (void**)&g_origExitProcess);
        if (tp) MH_CreateHook(tp, (void*)&Hook_TerminateProcess, (void**)&g_origTerminateProcess);
    }
    MH_EnableHook(MH_ALL_HOOKS);

    if (GetConfig().d3d12SaveIntervalSec > 0) {
        HANDLE t = CreateThread(nullptr, 0, SaverThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    LOGI("D3D12: D3D12CreateDevice hooked; PSO cache dir = %s", Narrow(GetConfig().cacheDir).c_str());
    return true;
}

} // namespace usc::d3d12
