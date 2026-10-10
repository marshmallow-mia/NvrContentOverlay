# MinGW-w64 cross toolchain (Linux or macOS host) for the Windows x64 DLL.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Everything static: the DLL must not need libgcc_s_seh-1.dll, libstdc++-6.dll or
# libwinpthread-1.dll beside it (hosts load plugins with a restricted search path).
# -s strips symbols (the static runtime's DWARF would otherwise bloat the DLL),
# section GC drops what the static runtime brings but nothing calls, and
# --no-insert-timestamp keeps the output byte-identical between builds.
set(CMAKE_C_FLAGS_INIT "-static-libgcc -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-static-libgcc -static-libstdc++ -ffunction-sections -fdata-sections")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -s -Wl,--gc-sections -Wl,--no-insert-timestamp")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -s -Wl,--gc-sections -Wl,--no-insert-timestamp")
