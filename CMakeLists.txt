cmake_minimum_required(VERSION 3.16)
project(UEShaderCache VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "This plugin targets Windows. Use -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake (or i686) to cross compile.")
endif()

# ---- MinHook (vendored, static) -------------------------------------------------------
set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook)
add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
    ${MINHOOK_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)

# ---- Plugin ---------------------------------------------------------------------------
add_library(UEShaderCache SHARED
    src/dllmain.cpp
    src/common.cpp
    src/d3d12_cache.cpp
    src/ue_cvars.cpp
    src/dstorage_tune.cpp
    src/version.rc)

target_compile_definitions(UEShaderCache PRIVATE
    UNICODE _UNICODE
    _WIN32_WINNT=0x0A00
    USC_VERSION="${PROJECT_VERSION}")
target_link_libraries(UEShaderCache PRIVATE minhook version dxgi ole32)

set_target_properties(UEShaderCache PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "UEShaderCache")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(UEShaderCache PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -fno-exceptions)
    # Fully static: no libgcc/libstdc++/winpthread DLL dependencies
    target_link_options(UEShaderCache PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s)
    # Stable export name on both x86 (no @-decoration) and x64
    target_link_options(UEShaderCache PRIVATE -Wl,--kill-at)
elseif(MSVC)
    target_compile_options(UEShaderCache PRIVATE /W4 /EHs-c- /GR-)
    set_property(TARGET UEShaderCache PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS UEShaderCache RUNTIME DESTINATION . LIBRARY DESTINATION .)
