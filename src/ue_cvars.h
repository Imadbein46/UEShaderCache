// UEShaderCache - Unreal Engine console variable injection
#pragma once
#include "common.h"

namespace usc::ue {
// Writes the PSO-cache / precompile / streaming CVars into the user's Engine.ini.
// Returns true if the file was written or already up to date.
bool InjectCVars();
}
