# NvrContentOverlay.dll -- a nEVR plugin that serves a content pack's game data (manifests,
# added packages, script DLLs) in place of the stock files, without changing any game file.
# See content_overlay.cpp and README.md.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
#         -DNEVR_SDK_INCLUDE=<nevr-runtime>/sdk/include
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(NvrContentOverlay VERSION 1.2.0 LANGUAGES C CXX)

# nEVR's plugin SDK headers (nevr/plugin_interface.h, nevr/plugin_logger.h), from nevr-runtime's sdk/include.
# They are not copied into this repo. The plugin reports the NEVR_PLUGIN_API_VERSION these headers define, and logs
# through the host's log when the host's context carries one (ctx_size covers ctx->log: plugin API 6 or newer);
# otherwise its lines go to stderr.
set(NEVR_SDK_INCLUDE "" CACHE PATH "nevr-runtime's sdk/include directory (required)")
if(NOT NEVR_SDK_INCLUDE)
    message(FATAL_ERROR "NEVR_SDK_INCLUDE is not set. Configure with -DNEVR_SDK_INCLUDE=<nevr-runtime>/sdk/include")
endif()
if(NOT EXISTS "${NEVR_SDK_INCLUDE}/nevr/plugin_logger.h")
    message(FATAL_ERROR "NEVR_SDK_INCLUDE=${NEVR_SDK_INCLUDE} has no nevr/plugin_logger.h. Point it at nevr-runtime's sdk/include")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinHook, pinned as in NvrAssetPatches: the v1.3.4 release tarball, checked by hash.
include(FetchContent)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
    URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(content_overlay SHARED content_overlay.cpp NvrContentOverlay.def)
target_include_directories(content_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party
                           ${NEVR_SDK_INCLUDE})
target_link_libraries(content_overlay PRIVATE minhook bcrypt)
target_compile_options(content_overlay PRIVATE -Wall -Wextra)
set_target_properties(content_overlay PROPERTIES OUTPUT_NAME "NvrContentOverlay" PREFIX "")
target_link_options(content_overlay PRIVATE -Wl,--exclude-all-symbols)
