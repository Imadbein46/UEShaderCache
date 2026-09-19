# UEShaderCache.asi

**Persistent shader / PSO caching plugin for Unreal Engine 4 & 5 games, loaded via
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).**
Fixes the "first-time-you-see-it" shader-compilation stutter and reduces streaming hitches.

> Windows only · x64 + x86 · no runtime dependencies · MIT

---

## What it actually does (and what "DirectStorage" has to do with it)

Shader stutter in UE titles comes from the D3D12 driver compiling a **Pipeline State Object (PSO)**
the first time a material/shader combination is drawn. That takes 10–300 ms on the render thread → hitch.
DirectStorage is an *asset streaming* API and does not cache shaders; however UE5 games that ship
`dstorage.dll` can additionally hitch on IO, so the plugin tunes that too.

The plugin attacks the problem from three sides:

| Layer | Mechanism | Effect |
|---|---|---|
| **D3D12 driver** | Hooks `ID3D12Device::CreateGraphicsPipelineState / CreateComputePipelineState / ID3D12Device2::CreatePipelineState`. Every PSO is hashed (shader bytecode + fixed‑function state) and stored in an **`ID3D12PipelineLibrary`**, serialised to `ShaderCache\<game>_d3d12_<gpu>.pipelinelib`. | 2nd run onwards: PSOs are **deserialised in µs instead of compiled in ms**. Works even when the game ships no PSO cache. Survives driver cache purges. |
| **Unreal Engine** | Writes stutter CVars into `%LOCALAPPDATA%\<Project>\Saved\Config\Windows[NoEditor]\Engine.ini`: enables `r.ShaderPipelineCache.*` (UE's own bundled PSO cache + *user cache recording*), forces **fast precompile during loading** (`StartupMode=1`, large batch times), async PSO compile, UE5.1+ `r.PSOPrecache.*` with *skip‑draw instead of stall*, GC & async‑loading time‑limits, unlimited texture pool. | PSOs UE knows about are compiled under the loading screen, not in gameplay. New PSOs encountered are recorded and precompiled next launch. |
| **DirectStorage** (UE5, optional) | Hooks `DStorageGetFactory`, raises the staging buffer 32 MB → 256 MB, optional CPU‑only decompression. | Fewer IO stalls while streaming during PSO compile. |

DX11 titles get a driver‑cache hint (`__GL_SHADER_DISK_CACHE*` env vars for NVIDIA, cache relocated
next to the game) plus the Engine.ini CVars.

## Install

1. Install Ultimate ASI Loader into the game's **`Binaries\Win64\`** folder (the directory that contains
   `<Game>-Win64-Shipping.exe`). For UE games use the **`dsound.dll`**, **`winmm.dll`**, **`version.dll`** or
   **`dinput8.dll`** proxy name (`xinput1_3.dll` also works for most). Do **not** put it next to the tiny
   launcher exe in the root folder — that process exits immediately.
2. Copy `UEShaderCache.asi` (x64 for practically every UE4.x/5.x game; x86 only for 32‑bit UE4 builds)
   into the same folder, or into `scripts\` / `plugins\`.
3. Launch the game. `UEShaderCache.ini` and `UEShaderCache.log` appear next to the .asi;
   `ShaderCache\` appears next to the exe.
4. **The first session still compiles.** Play through / let loading screens finish. From the second
   launch the cache is used.

Steam/Epic overlay, ReShade and other ASI plugins are compatible. Anti‑cheat titles (EAC/BattlEye)
may block *any* DLL injection – that's a loader limitation, not something this plugin can bypass.

## Configuration (`UEShaderCache.ini`)

```ini
[General]
Enabled=1
LogLevel=1              ; 0 off, 1 info, 2 verbose
CacheDir=ShaderCache    ; relative to exe or absolute

[D3D12]
PipelineLibrary=1       ; persistent PSO library
SaveOnExit=1
SaveIntervalSeconds=60  ; periodic flush (0 = only on exit)
MaxCacheMB=512          ; rebuild if bigger

[D3D11]
HintDriverCache=1

[Unreal]
InjectCVars=1           ; master switch for Engine.ini edits
PSOCache=1              ; r.ShaderPipelineCache.*
PrecompileOnLoad=1      ; StartupMode=1 + big batch budgets
AsyncCompile=1          ; r.AsyncPipelineCompile, r.CreateShadersOnLoad, D3D12 PSO disk cache
GCTweaks=1              ; gc.* incremental / less frequent purges
StreamingTweaks=1       ; s.AsyncLoading* time limits, r.Streaming.PoolSize=0 (disable on <6 GB VRAM)
UE5PSOPrecache=1        ; r.PSOPrecache.* (UE 5.1+)
BackupIni=1             ; keeps Engine.ini.usc.bak

[DirectStorage]
Tune=1
StagingBufferMB=256
DisableGPUDecompression=0
```

The Engine.ini block is wrapped in `; >>> UEShaderCache BEGIN … ; <<< UEShaderCache END` markers,
updated in place, and removed cleanly by setting `InjectCVars=0` and deleting the block (or restoring
the `.usc.bak`).

## Verifying it works

`UEShaderCache.log` on exit:

```
[..] D3D12: loaded pipeline library ...\ShaderCache\Game-Win64-Shipping_d3d12_10DE_2684_....pipelinelib (48213 KB)
[..] D3D12: session stats - hits=6312 misses(compiled)=41 stored=41 skipped=0
```

`hits` = PSOs served from cache (no compile), `misses` = newly compiled this session.
On the first run hits≈0; on subsequent runs misses should approach 0 in areas you've already visited.

## Building

```bash
# Linux / WSL cross-compile (Debian/Ubuntu: apt install mingw-w64 cmake)
cmake -S . -B build-x64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-x64
cmake -S . -B build-x86 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86

# Windows / MSVC
cmake -S . -B build -A x64 && cmake --build build --config Release
```

Output: `build-*/UEShaderCache.asi`, statically linked (only kernel32/msvcrt/shell32/version imports).

## Source layout

```
src/dllmain.cpp        entry, InitializeASI export, LoadLibrary watcher, DX11 hints
src/common.*           ini config, logging, paths, Unreal project/version detection
src/d3d12_cache.*      D3D12CreateDevice + device vtable hooks, PSO hashing, ID3D12PipelineLibrary I/O
src/ue_cvars.*         Engine.ini CVar injection (UE4 / UE5 aware)
src/dstorage_tune.*    DStorageGetFactory hook (staging buffer / CPU decompression)
third_party/minhook    MinHook 1.3.4 (BSD-2)
```

## Design notes & limitations

* **PSO identity** is an FNV‑1a‑64 hash of all shader bytecode, root‑signature blob hash, and every
  fixed‑function field; the D3D12 runtime additionally validates the stored desc against the requested
  one, so a collision just results in a miss, never a wrong pipeline.
* Libraries are per‑adapter (`vendor_device_subsys_rev`) and the runtime rejects them on driver
  updates (`D3D12_ERROR_DRIVER_VERSION_MISMATCH`) → the plugin transparently rebuilds.
* Unknown newer PSO stream sub‑objects (future SDKs) are logged at `LogLevel=2` and passed through
  uncached rather than guessed.
* Save path uses a timed lock so a frozen render thread during `ExitProcess` can never deadlock exit.
* Vulkan RHI (`-vulkan`) is not covered – Vulkan drivers already persist pipeline caches via
  `VkPipelineCache` and UE handles that itself.
* Cannot help with hitches that are *not* PSO/IO related (e.g. blueprint spawning, physics).

## Credits

ThirteenAG – Ultimate ASI Loader · Tsuda Kageyu – MinHook
