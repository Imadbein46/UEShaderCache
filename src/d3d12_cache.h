// UEShaderCache - D3D12 persistent PSO cache via ID3D12PipelineLibrary
#pragma once
#include "common.h"

namespace usc::d3d12 {

// Installs hooks on D3D12CreateDevice (+ device vtable once the first device is created).
// Safe to call once at startup; returns false if d3d12.dll is unavailable.
bool Install();

// Flush every dirty pipeline library to disk. Called by the saver thread, on ExitProcess and on detach.
void SaveAll(const char* reason);

// Statistics for logging
struct Stats {
    long created = 0;     // PSOs compiled by the driver (cache miss)
    long loaded  = 0;     // PSOs served from the persistent library (cache hit)
    long stored  = 0;     // PSOs newly stored into the library
    long skipped = 0;     // PSOs we could not hash (unknown stream subobject etc.)
};
Stats GetStats();

} // namespace usc::d3d12
