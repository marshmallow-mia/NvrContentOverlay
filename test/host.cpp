// A stand-in for the game: loads NvrContentOverlay.dll the way nEVR does, inits it with the
// given args, then opens files and prints what it got. test/run.ps1 lays out a fake install
// around it (this exe in bin\win10, the plugin in bin\win10\plugins, or in NCO_TEST_DLL's path
// for nEVR's root layout) and checks the output.
//
//   host.exe <args json> [--hold <path>] (--read <path> | --write <path> | --shutdown)...
//
// --hold opens <path> before the init and keeps it open (the game holding its packages);
// --read prints the first line of what opening <path> for reading gets; --write tries to
// open it for writing; --shutdown calls NvrPluginShutdown.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../nevr_plugin_interface.h"

static std::wstring Wide(const char* s) {
    wchar_t buf[1024];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 1024);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: host.exe <args json> [--hold p] [--read p | --write p | --shutdown]...\n");
        return 2;
    }
    wchar_t dll[MAX_PATH];
    if (!GetEnvironmentVariableW(L"NCO_TEST_DLL", dll, MAX_PATH)) {
        GetModuleFileNameW(nullptr, dll, MAX_PATH);
        wcscpy(wcsrchr(dll, L'\\') + 1, L"plugins\\NvrContentOverlay.dll");
    }
    HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        printf("load failed %lu\n", GetLastError());
        return 1;
    }
    auto info = reinterpret_cast<NvrPluginGetInfo_fn>(GetProcAddress(m, "NvrPluginGetInfo"));
    auto api = reinterpret_cast<NvrPluginGetApiVersion_fn>(GetProcAddress(m, "NvrPluginGetApiVersion"));
    auto init = reinterpret_cast<NvrPluginInitEx_fn>(GetProcAddress(m, "NvrPluginInitEx"));
    auto shutdown = reinterpret_cast<NvrPluginShutdown_fn>(GetProcAddress(m, "NvrPluginShutdown"));
    if (!info || !api || !init || !shutdown) {
        printf("exports missing\n");
        return 1;
    }
    NvrPluginInfo i = info();
    printf("info %s %u.%u.%u api %u\n", i.name, i.version_major, i.version_minor, i.version_patch, api());

    HANDLE held = INVALID_HANDLE_VALUE;
    int a = 2;
    if (a + 1 < argc && strcmp(argv[a], "--hold") == 0) {
        held = CreateFileW(Wide(argv[a + 1]).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                           nullptr);
        printf("hold %s\n", held == INVALID_HANDLE_VALUE ? "failed" : "ok");
        a += 2;
    }

    NvrGameContext ctx = {};
    ctx.base_addr = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    ctx.flags = NEVR_HOST_IS_CLIENT;
    ctx.ctx_size = sizeof(ctx);
    printf("init %d\n", init(&ctx, argv[1]));

    for (; a < argc; ++a) {
        if (strcmp(argv[a], "--shutdown") == 0) {
            shutdown();
            printf("shutdown\n");
        } else if (a + 1 < argc && (strcmp(argv[a], "--read") == 0 || strcmp(argv[a], "--write") == 0)) {
            bool write = argv[a][2] == 'w';
            const char* path = argv[++a];
            HANDLE h = CreateFileW(Wide(path).c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                printf("%s %s: error %lu\n", write ? "write" : "read", path, GetLastError());
                continue;
            }
            char buf[64] = {};
            DWORD got = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
            CloseHandle(h);
            if (char* nl = strpbrk(buf, "\r\n")) *nl = 0;
            printf("%s %s: %s\n", write ? "write" : "read", path, buf);
        }
    }
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    return 0;
}
