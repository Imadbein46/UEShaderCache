// UEShaderCache - common utilities implementation
#include "common.h"
#include <shlobj.h>
#include <cstdarg>
#include <algorithm>
#include <mutex>

namespace usc {

// ---------------------------------------------------------------- Config
static Config g_cfg;
Config& GetConfig() { return g_cfg; }

static bool IniBool(const wchar_t* sec, const wchar_t* key, bool def, const std::wstring& ini) {
    return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini.c_str()) != 0;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& ini) {
    return (int)GetPrivateProfileIntW(sec, key, def, ini.c_str());
}
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def, const std::wstring& ini) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(sec, key, def, buf, (DWORD)std::size(buf), ini.c_str());
    return buf;
}

void LoadConfig(HMODULE self) {
    std::wstring dir = GetModuleDir(self);
    std::wstring ini = dir + L"\\UEShaderCache.ini";
    Config& c = g_cfg;

    c.enabled  = IniBool(L"General", L"Enabled", true, ini);
    c.logLevel = IniInt (L"General", L"LogLevel", 1, ini);
    std::wstring cd = IniStr(L"General", L"CacheDir", L"", ini);
    if (cd.empty()) {
        // default: <exe dir>\ShaderCache
        cd = GetExeDir() + L"\\ShaderCache";
    } else if (cd.size() > 1 && cd[1] != L':' && cd[0] != L'\\') {
        cd = GetExeDir() + L"\\" + cd; // relative to exe
    }
    c.cacheDir = cd;

    c.d3d12PipelineLibrary = IniBool(L"D3D12", L"PipelineLibrary", true, ini);
    c.d3d12SaveOnExit      = IniBool(L"D3D12", L"SaveOnExit", true, ini);
    c.d3d12SaveIntervalSec = IniInt (L"D3D12", L"SaveIntervalSeconds", 60, ini);
    c.d3d12MaxCacheMB      = IniInt (L"D3D12", L"MaxCacheMB", 512, ini);

    c.d3d11HintDriverCache = IniBool(L"D3D11", L"HintDriverCache", true, ini);

    c.ueInjectCVars        = IniBool(L"Unreal", L"InjectCVars", true, ini);
    c.uePSOCache           = IniBool(L"Unreal", L"PSOCache", true, ini);
    c.uePrecompileOnLoad   = IniBool(L"Unreal", L"PrecompileOnLoad", true, ini);
    c.ueAsyncCompile       = IniBool(L"Unreal", L"AsyncCompile", true, ini);
    c.ueDisableGCDuringPSO = IniBool(L"Unreal", L"GCTweaks", true, ini);
    c.ueStreamingTweaks    = IniBool(L"Unreal", L"StreamingTweaks", true, ini);
    c.ueDisablePSOPrecacheStall = IniBool(L"Unreal", L"UE5PSOPrecache", true, ini);
    c.ueBackupIni          = IniBool(L"Unreal", L"BackupIni", true, ini);

    c.dstorageTune         = IniBool(L"DirectStorage", L"Tune", true, ini);
    c.dstorageStagingMB    = IniInt (L"DirectStorage", L"StagingBufferMB", 256, ini);
    c.dstorageDisableGPUDecompression = IniBool(L"DirectStorage", L"DisableGPUDecompression", false, ini);

    // Write defaults back if the ini doesn't exist yet so users can discover options.
    if (!FileExists(ini)) {
        const char* text =
            "; UEShaderCache.ini - Unreal Engine shader / PSO caching plugin (Ultimate ASI Loader)\r\n"
            "[General]\r\n"
            "Enabled=1\r\n"
            "; 0 = off, 1 = info, 2 = verbose\r\n"
            "LogLevel=1\r\n"
            "; Where to keep the persistent pipeline cache (relative to the game exe, or absolute)\r\n"
            "CacheDir=ShaderCache\r\n"
            "\r\n"
            "[D3D12]\r\n"
            "; Persist compiled Pipeline State Objects between runs via ID3D12PipelineLibrary\r\n"
            "PipelineLibrary=1\r\n"
            "SaveOnExit=1\r\n"
            "; Periodic flush to disk (seconds, 0 = only on exit)\r\n"
            "SaveIntervalSeconds=60\r\n"
            "; Delete and rebuild the cache if it grows past this size\r\n"
            "MaxCacheMB=512\r\n"
            "\r\n"
            "[D3D11]\r\n"
            "; Ask the driver to keep its shader disk cache enabled (helps DX11 UE4 titles)\r\n"
            "HintDriverCache=1\r\n"
            "\r\n"
            "[Unreal]\r\n"
            "; Write stutter-related CVars into the game's Engine.ini (%LOCALAPPDATA%\\<Game>\\Saved\\Config\\...)\r\n"
            "InjectCVars=1\r\n"
            "; Enable UE's own PSO cache (r.ShaderPipelineCache.*)\r\n"
            "PSOCache=1\r\n"
            "; Precompile every cached PSO during loading screens instead of on first use\r\n"
            "PrecompileOnLoad=1\r\n"
            "; Compile PSOs on worker threads / create shaders at load\r\n"
            "AsyncCompile=1\r\n"
            "; Reduce garbage-collection hitching\r\n"
            "GCTweaks=1\r\n"
            "; Async loading time limits + unlimited texture pool (disable on GPUs with <6 GB VRAM)\r\n"
            "StreamingTweaks=1\r\n"
            "; UE5.1+ PSO precaching tweaks (skip draw instead of stalling when a PSO is not ready)\r\n"
            "UE5PSOPrecache=1\r\n"
            "; Keep a .bak of the original Engine.ini\r\n"
            "BackupIni=1\r\n"
            "\r\n"
            "[DirectStorage]\r\n"
            "; If the game uses DirectStorage (dstorage.dll), enlarge the staging buffer to cut IO hitching\r\n"
            "Tune=1\r\n"
            "StagingBufferMB=256\r\n"
            "; Force CPU decompression (can help GPUs that hitch during GDeflate)\r\n"
            "DisableGPUDecompression=0\r\n";
        WriteFileAtomic(ini, text, strlen(text));
    }
}

// ---------------------------------------------------------------- Logging
static FILE*      g_log = nullptr;
static std::mutex g_logMx;

void LogOpen(HMODULE self) {
    if (g_cfg.logLevel <= 0) return;
    std::wstring p = GetModuleDir(self) + L"\\UEShaderCache.log";
    g_log = _wfopen(p.c_str(), L"w");
}

void Log(int level, const char* fmt, ...) {
    if (level > g_cfg.logLevel) return;
    std::lock_guard<std::mutex> lk(g_logMx);
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    OutputDebugStringA(line);
}

void LogClose() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

// ---------------------------------------------------------------- Paths
std::wstring GetModuleDir(HMODULE m) {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, buf, (DWORD)std::size(buf));
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}
std::wstring GetExeDir() { return GetModuleDir(nullptr); }

std::wstring GetExeName() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring s(buf);
    size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s = s.substr(p + 1);
    p = s.find_last_of(L'.');
    if (p != std::wstring::npos) s = s.substr(0, p);
    return s;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    size_t p = dir.find_last_of(L"\\/");
    if (p != std::wstring::npos && p > 2) EnsureDir(dir.substr(0, p));
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}

bool ReadFileAll(const std::wstring& p, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (1ll << 31)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0; bool ok = true;
    size_t off = 0;
    while (off < out.size()) {
        if (!ReadFile(h, out.data() + off, (DWORD)std::min<size_t>(out.size() - off, 1 << 26), &rd, nullptr) || rd == 0) { ok = false; break; }
        off += rd;
    }
    CloseHandle(h);
    return ok;
}

bool WriteFileAtomic(const std::wstring& p, const void* data, size_t size) {
    std::wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint8_t* b = (const uint8_t*)data;
    size_t off = 0; bool ok = true;
    while (off < size) {
        DWORD wr = 0;
        if (!WriteFile(h, b + off, (DWORD)std::min<size_t>(size - off, 1 << 26), &wr, nullptr)) { ok = false; break; }
        off += wr;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// ---------------------------------------------------------------- Unreal detection
static bool DirExists(const std::wstring& d) {
    DWORD a = GetFileAttributesW(d.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ParentDir(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
static std::wstring LeafName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

// Read version from the exe's VERSIONINFO (UE fills "FileVersion" with e.g. 4.27.2.0 / 5.3.2.0)
static void ReadUEVersionFromExe(UEInfo& info) {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return;
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return;
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) && ffi) {
        int maj = HIWORD(ffi->dwFileVersionMS), min = LOWORD(ffi->dwFileVersionMS);
        if (maj == 4 || maj == 5) { info.majorVersion = maj; info.minorVersion = min; }
    }
}

const UEInfo& DetectUnreal() {
    static UEInfo info;
    static bool done = false;
    if (done) return info;
    done = true;

    std::wstring exeDir = GetExeDir();
    std::wstring exeName = GetExeName();

    // Layout A (shipping): <Root>\<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe
    // Layout B (launcher): <Root>\<Project>.exe  (tiny bootstrap exe next to Engine\ and <Project>\)
    std::wstring binDir = LeafName(exeDir);          // Win64 / WinGDK / Win32
    std::wstring maybeBinaries = ParentDir(exeDir);  // ...\Binaries
    if (LeafName(maybeBinaries) == L"Binaries") {
        info.projectRoot = ParentDir(maybeBinaries);
        info.projectName = LeafName(info.projectRoot);
        std::wstring root = ParentDir(info.projectRoot);
        if (DirExists(root + L"\\Engine") || DirExists(info.projectRoot + L"\\Content\\Paks")) {
            info.isUnreal = true;
        }
    }
    if (!info.isUnreal && DirExists(exeDir + L"\\Engine") ) {
        // bootstrap exe layout
        info.isUnreal = true;
        info.projectName = exeName;
        info.projectRoot = exeDir + L"\\" + exeName;
        if (!DirExists(info.projectRoot)) {
            // Find first dir that contains Binaries\Win64
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((exeDir + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L"Engine") != 0 && fd.cFileName[0] != L'.') {
                        std::wstring cand = exeDir + L"\\" + fd.cFileName;
                        if (DirExists(cand + L"\\Binaries") || DirExists(cand + L"\\Content")) {
                            info.projectRoot = cand; info.projectName = fd.cFileName; break;
                        }
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
    // Strip "-Win64-Shipping" style suffix from project name derived from exe.
    if (info.isUnreal) {
        size_t d = info.projectName.find(L"-Win");
        if (d != std::wstring::npos) info.projectName = info.projectName.substr(0, d);
    }

    ReadUEVersionFromExe(info);
    // Heuristics when version info is missing
    if (info.isUnreal && info.majorVersion == 0) {
        std::wstring root = ParentDir(info.projectRoot);
        if (DirExists(root + L"\\Engine\\Binaries\\ThirdParty\\ShaderConductor") ||
            FileExists(info.projectRoot + L"\\Content\\Paks\\global.utoc") ||
            FileExists(info.projectRoot + L"\\Content\\Paks\\global.ucas"))
            info.majorVersion = 5;
        else
            info.majorVersion = 4;
    }

    if (info.isUnreal) {
        wchar_t local[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
            std::wstring base = std::wstring(local) + L"\\" + info.projectName + L"\\Saved\\Config\\";
            // UE5 uses "Windows", UE4 used "WindowsNoEditor". Prefer whichever exists; else by version.
            if (DirExists(base + L"Windows"))              info.savedConfigDir = base + L"Windows";
            else if (DirExists(base + L"WindowsNoEditor")) info.savedConfigDir = base + L"WindowsNoEditor";
            else info.savedConfigDir = base + (info.majorVersion >= 5 ? L"Windows" : L"WindowsNoEditor");
        }
    }
    return info;
}

// ---------------------------------------------------------------- Hashing
uint64_t FNV1a64(const void* data, size_t len, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

} // namespace usc
