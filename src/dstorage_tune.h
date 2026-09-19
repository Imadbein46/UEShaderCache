// UEShaderCache - DirectStorage tuning
#pragma once
#include "common.h"

namespace usc::dstorage {
// Hook DStorageGetFactory in the given (or already-loaded) dstorage.dll. Returns true when hooked.
bool Install(HMODULE dstorageDll = nullptr);
}
