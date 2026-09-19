// UEShaderCache - common utilities (logging, config, paths, UE detection)
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

namespace usc {

// ---------------------------------------------------------------- Config
struct Config {
    // [General]
    bool  enabled            = true;
    int   logLevel           = 1;      // 0 = off, 1 = info, 2 = verbose
    std::wstring cacheDir;             // resolved absolute path
    // [D3D12]
    bool  d3d12PipelineLibrary = true; // persist PSOs via ID3D12PipelineLibrary
    bool  d3d12SaveOnExit      = true;
    int   d3d12SaveIntervalSec = 60;   // periodic flush; 0 = disabled
    int   d3d12MaxCacheMB      = 512;  // discard library if larger
    // [D3D11]
    bool  d3d11HintDriverCache = true; // force driver shader cache on (NV/AMD)
    // [Unreal]
    bool  ueInjectCVars        = true; // write Engine.ini overrides
    bool  uePSOCache           = true; // r.ShaderPipelineCache.Enabled etc.
    bool  uePrecompileOnLoad   = true; // r.ShaderPipelineCache.StartupMode / PrecompileBatchTime
    bool  ueAsyncCompile       = true; // r.AsyncPipelineCompile / r.CreateShadersOnLoad
    bool  ueDisableGCDuringPSO = true; // gc.TimeBetweenPurgingPendingKillObjects hint
    bool  ueStreamingTweaks    = true; // s.AsyncLoading*, r.Streaming.PoolSize
    bool  ueDisablePSOPrecacheStall = true; // UE5.1+: r.PSOPrecache.ProxyCreationWhenPSOReady etc.
    bool  ueBackupIni          = true;
    // [DirectStorage]
    bool  dstorageTune         = true; // bump staging buffer via IDStorageFactory::SetStagingBufferSize
    int   dstorageStagingMB    = 256;
    bool  dstorageDisableGPUDecompression = false;
};

Config& GetConfig();
void    LoadConfig(HMODULE self);

// ---------------------------------------------------------------- Logging
void LogOpen(HMODULE self);
void Log(int level, const char* fmt, ...);
void LogClose();
#define LOGI(...) ::usc::Log(1, __VA_ARGS__)
#define LOGV(...) ::usc::Log(2, __VA_ARGS__)

// ---------------------------------------------------------------- Paths
std::wstring GetModuleDir(HMODULE m);
std::wstring GetExeDir();
std::wstring GetExeName();          // without extension
bool  EnsureDir(const std::wstring& dir);
bool  FileExists(const std::wstring& p);
std::string  Narrow(const std::wstring& s);
std::wstring Widen(const std::string& s);
bool  ReadFileAll(const std::wstring& p, std::vector<uint8_t>& out);
bool  WriteFileAtomic(const std::wstring& p, const void* data, size_t size);

// ---------------------------------------------------------------- Unreal detection
struct UEInfo {
    bool  isUnreal      = false;
    int   majorVersion  = 0;      // 4 or 5, 0 = unknown
    int   minorVersion  = 0;
    std::wstring projectName;     // e.g. "MyGame" for MyGame\Binaries\Win64\MyGame-Win64-Shipping.exe
    std::wstring projectRoot;     // ...\MyGame
    std::wstring savedConfigDir;  // %LOCALAPPDATA%\MyGame\Saved\Config\Windows (or WindowsNoEditor for UE4)
};
const UEInfo& DetectUnreal();

// ---------------------------------------------------------------- Hashing
uint64_t FNV1a64(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL);

} // namespace usc
