// UEShaderCache - inject stutter-related console variables into the game's user Engine.ini
//
// Unreal reads %LOCALAPPDATA%\<Project>\Saved\Config\Windows[NoEditor]\Engine.ini very early
// during FConfigCacheIni init, long before the RHI is created - so we must write the file before
// the game gets that far. Ultimate ASI Loader loads .asi plugins from DllMain of a proxy DLL
// (dinput8/version/winmm...) which happens before the game's entry point runs -> we're early enough.
//
// We write into a dedicated [SystemSettings] + [ConsoleVariables] block wrapped in marker comments so
// repeated runs update in place and users can remove it cleanly.

#include "ue_cvars.h"
#include <string>
#include <vector>
#include <sstream>

namespace usc::ue {

static const char* kBegin = "; >>> UEShaderCache BEGIN (auto-generated, edit UEShaderCache.ini instead)";
static const char* kEnd   = "; <<< UEShaderCache END";

struct KV { std::string key, value; };

static std::vector<KV> BuildCVars(const UEInfo& ue) {
    const Config& c = GetConfig();
    std::vector<KV> v;
    const bool ue5 = ue.majorVersion >= 5;
    const int  minor = ue.minorVersion;

    if (c.uePSOCache) {
        // Unreal's bundled PSO cache (the .stablepc / .upipelinecache files shipped in Content/PipelineCaches)
        v.push_back({"r.ShaderPipelineCache.Enabled", "1"});
        // Record PSOs the game encounters into the user cache so *next* run precompiles them
        v.push_back({"r.ShaderPipelineCache.SaveUserCache", "1"});
        v.push_back({"r.ShaderPipelineCache.LogPSO", "0"});
        v.push_back({"r.ShaderPipelineCache.SaveBoundPSOLog", "0"});
        v.push_back({"r.ShaderPipelineCache.AutoSaveTime", "30"});
        v.push_back({"r.ShaderPipelineCache.SaveAfterPSOsLogged", "0"}); // rely on AutoSaveTime
        // Mask off nothing: precompile all recorded PSOs (0 = ignore game mask)
        v.push_back({"r.ShaderPipelineCache.GameFileMaskEnabled", "0"});
        v.push_back({"r.ShaderPipelineCache.PreCompileMask", "-1"});
        // Reload cache when a new PSO cache file appears (e.g. after DLC)
        v.push_back({"r.ShaderPipelineCache.ReportPSO", "0"});
        v.push_back({"r.ShaderPipelineCache.PrintNewPSODescriptors", "0"});
    }
    if (c.uePrecompileOnLoad) {
        // 0 = paused, 1 = fast (all at once, best done under a loading screen), 2 = background/trickle
        v.push_back({"r.ShaderPipelineCache.StartupMode", "1"});
        // Compile as many as possible per frame while in "fast" mode (ms budget) - stalls are fine on a loading screen
        v.push_back({"r.ShaderPipelineCache.BatchTime", "16.0"});
        v.push_back({"r.ShaderPipelineCache.BatchSize", "50"});
        v.push_back({"r.ShaderPipelineCache.PrecompileBatchTime", "16.0"});
        v.push_back({"r.ShaderPipelineCache.PrecompileBatchSize", "50"});
        v.push_back({"r.ShaderPipelineCache.BackgroundBatchTime", "2.0"});
        v.push_back({"r.ShaderPipelineCache.BackgroundBatchSize", "5"});
        // Let the loading screen wait for precompile to finish rather than hitching in gameplay
        v.push_back({"r.ShaderPipelineCache.ClearOnPrecompileComplete", "0"});
    }
    if (c.ueAsyncCompile) {
        // Compile PSOs on the RHI worker threads instead of the render thread
        v.push_back({"r.AsyncPipelineCompile", "1"});
        v.push_back({"r.CreateShadersOnLoad", "1"});
        // Precompile shaders for materials at load instead of when first drawn
        v.push_back({"r.ShaderPipelineCache.LazyLoadShadersWhenPSOCacheIsPresent", "0"});
        v.push_back({"r.D3D12.PSO.DiskCache", "1"});
        v.push_back({"r.D3D12.PSO.DriverOptimizedDiskCache", "1"});
        v.push_back({"D3D12.PSO.DiskCache", "1"});
        v.push_back({"D3D12.PSO.DriverOptimizedDiskCache", "1"});
    }
    if (ue5 && c.ueDisablePSOPrecacheStall) {
        // UE5.1+ PSO precaching system - compile in the background and skip a draw instead of blocking.
        v.push_back({"r.PSOPrecaching", "1"});
        v.push_back({"r.PSOPrecache.Components", "1"});
        v.push_back({"r.PSOPrecache.Resources", "1"});
        v.push_back({"r.PSOPrecache.GlobalShaders", "1"});
        v.push_back({"r.PSOPrecache.ProxyCreationWhenPSOReady", "1"});
        v.push_back({"r.PSOPrecache.ProxyCreationDelayStrategy", "0"}); // 0 = skip, 1 = use default material
        v.push_back({"r.PSOPrecache.ComputeShadersDuringLoading", "1"});
        v.push_back({"r.PSOPrecache.NaniteShaders", "1"});
        if (minor >= 3) {
            v.push_back({"r.PSOPrecache.MinimalPSOs", "0"});
            v.push_back({"r.PSOPrecache.LoadPSOsFromDisk", "1"});
        }
        v.push_back({"r.SkipDrawOnPSOPrecaching", "1"});
        // 5.x shader library loading
        v.push_back({"r.ShaderLibrary.PrintExtendedStats", "0"});
        v.push_back({"r.ShaderCodeLibrary.SeparateLoadingCache", "1"});
    }
    if (c.ueDisableGCDuringPSO) {
        // Big GC sweeps cause frame spikes; spread them out and make them incremental
        v.push_back({"gc.TimeBetweenPurgingPendingKillObjects", "900"});
        v.push_back({"gc.NumRetriesBeforeForcingGC", "5"});
        v.push_back({"gc.MinDesiredObjectsPerSubTask", "20"});
        v.push_back({"gc.CreateGCClusters", "1"});
        v.push_back({"gc.ActorClusteringEnabled", "1"});
        if (ue5) {
            v.push_back({"gc.IncrementalBeginDestroyEnabled", "1"});
            v.push_back({"gc.AllowIncrementalReachability", "1"});
            v.push_back({"gc.AllowIncrementalGather", "1"});
        }
    }
    if (c.ueStreamingTweaks) {
        // Streaming: spend a bit more CPU per frame on IO instead of hitching later
        v.push_back({"s.AsyncLoadingThreadEnabled", "1"});
        v.push_back({"s.AsyncLoadingTimeLimit", "4.0"});
        v.push_back({"s.LevelStreamingActorsUpdateTimeLimit", "4.0"});
        v.push_back({"s.UnregisterComponentsTimeLimit", "4.0"});
        v.push_back({"s.PriorityAsyncLoadingExtraTime", "8.0"});
        v.push_back({"s.LevelStreamingComponentsRegistrationGranularity", "5"});
        v.push_back({"s.LevelStreamingComponentsUnregistrationGranularity", "5"});
        v.push_back({"r.Streaming.PoolSize", "0"});         // 0 = unlimited texture pool (no evictions -> no re-uploads)
        v.push_back({"r.Streaming.UseAllMips", "0"});
        v.push_back({"r.Streaming.FramesForFullUpdate", "1"});
        v.push_back({"r.Streaming.LimitPoolSizeToVRAM", "1"});
    }
    return v;
}

static bool IsMarkerBegin(const std::string& l) { return l.find(">>> UEShaderCache BEGIN") != std::string::npos; }
static bool IsMarkerEnd  (const std::string& l) { return l.find("<<< UEShaderCache END")   != std::string::npos; }

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool InjectCVars() {
    const Config& c = GetConfig();
    const UEInfo& ue = DetectUnreal();
    if (!c.ueInjectCVars) return false;
    if (!ue.isUnreal) { LOGI("UE: game not recognised as Unreal Engine - CVar injection skipped"); return false; }
    if (ue.savedConfigDir.empty()) return false;

    std::vector<KV> cvars = BuildCVars(ue);
    if (cvars.empty()) return false;

    EnsureDir(ue.savedConfigDir);
    std::wstring iniPath = ue.savedConfigDir + L"\\Engine.ini";

    std::vector<uint8_t> raw;
    std::string original;
    if (ReadFileAll(iniPath, raw)) original.assign(raw.begin(), raw.end());

    // Split lines, strip previous UEShaderCache block and any duplicate keys we are about to set
    // (only within [SystemSettings] / [ConsoleVariables] sections - UE reads both at startup).
    std::vector<std::string> lines;
    {
        std::istringstream ss(original);
        std::string l;
        while (std::getline(ss, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
    }
    std::vector<std::string> kept;
    bool inBlock = false, inTargetSection = false;
    for (auto& l : lines) {
        if (IsMarkerBegin(l)) { inBlock = true; continue; }
        if (IsMarkerEnd(l))   { inBlock = false; continue; }
        if (inBlock) continue;
        std::string t = Trim(l);
        if (!t.empty() && t[0] == '[') {
            inTargetSection = (t == "[SystemSettings]" || t == "[ConsoleVariables]");
            kept.push_back(l);
            continue;
        }
        if (inTargetSection) {
            size_t eq = t.find('=');
            if (eq != std::string::npos) {
                std::string key = Trim(t.substr(0, eq));
                bool dup = false;
                for (auto& kv : cvars) if (_stricmp(kv.key.c_str(), key.c_str()) == 0) { dup = true; break; }
                if (dup) continue;
            }
        }
        kept.push_back(l);
    }
    // Drop trailing blank lines / now-empty sections are harmless for UE.
    while (!kept.empty() && Trim(kept.back()).empty()) kept.pop_back();

    std::string out;
    for (auto& l : kept) { out += l; out += "\r\n"; }
    if (!out.empty()) out += "\r\n";
    out += kBegin; out += "\r\n";
    out += "; Detected Unreal Engine "; out += std::to_string(ue.majorVersion); out += "."; out += std::to_string(ue.minorVersion);
    out += " - project '"; out += Narrow(ue.projectName); out += "'\r\n";
    out += "[SystemSettings]\r\n";
    for (auto& kv : cvars) { out += kv.key; out += "="; out += kv.value; out += "\r\n"; }
    out += kEnd; out += "\r\n";

    if (out == original) { LOGI("UE: Engine.ini already up to date (%s)", Narrow(iniPath).c_str()); return true; }

    if (c.ueBackupIni && !original.empty() && !FileExists(iniPath + L".usc.bak")) {
        WriteFileAtomic(iniPath + L".usc.bak", original.data(), original.size());
    }
    // UE sometimes marks the ini read-only; clear that.
    SetFileAttributesW(iniPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!WriteFileAtomic(iniPath, out.data(), out.size())) {
        LOGI("UE: failed to write %s (err %lu)", Narrow(iniPath).c_str(), GetLastError());
        return false;
    }
    LOGI("UE: wrote %zu CVars to %s (UE %d.%d, project '%s')", cvars.size(), Narrow(iniPath).c_str(),
         ue.majorVersion, ue.minorVersion, Narrow(ue.projectName).c_str());
    return true;
}

} // namespace usc::ue
