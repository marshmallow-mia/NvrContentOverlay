/*
 * NvrContentOverlay.dll -- serves a content pack's game data in place of the stock files,
 * without changing any game file.
 *
 * A content pack is a folder bin\win10\content\<id>\ that a mod's build makes: the manifests it
 * changes (the stock ones with its resources added), the extra packages those name, and the
 * script DLLs it replaces or adds. content.json lists every file with its sha256, and the sha256
 * of each stock manifest the pack was built from.
 *
 * With the arg "active": "<id>" the plugin checks the pack and the stock manifests, then
 *   - an open of <data dir>\manifests\<pkg> gets the pack's manifest,
 *   - an open of <data dir>\packages\<pkg>_<n> that the pack ships gets the pack's file,
 *   - the game's module loader (echovr.exe+0xEB0F0, which loads bin\win10\scripts\<hash>.dll:
 *     Echo runs script code from there, not from the package) gets the pack's copy of a script.
 * If anything doesn't check out (a stock manifest that isn't the one the pack was built from, a
 * missing or changed pack file) it hooks nothing and the game runs stock. Without "active" it
 * hooks only to log, with "logging": "verbose" (a probe of what the game opens, and when).
 */

#include <windows.h>
#include <bcrypt.h>
#include <MinHook.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nevr_plugin_interface.h"
#include "nlohmann/json.hpp"

namespace {

using json = nlohmann::json;

// echovr.exe 631547 (rad15_live), and its module loader:
//   void* LoadModule(const char* path, void* arg, int call_main, int reuse_loaded)
// LoadLibraryA(path), then RadPluginMain(arg) when call_main is set.
constexpr DWORD kLiveBuild = 0x6452DFF6;
constexpr uintptr_t kLoadModuleRva = 0xEB0F0;
constexpr uint8_t kLoadModulePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                           0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x04};

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using LoadModuleFn = void*(__fastcall*)(const char*, void*, int, int);

CreateFileWFn g_trueW = nullptr;
CreateFileAFn g_trueA = nullptr;
LoadModuleFn g_trueLoad = nullptr;
void* g_targetW = nullptr;
void* g_targetA = nullptr;
void* g_targetLoad = nullptr;

FILE* g_log = nullptr;
std::mutex g_logLock;
bool g_verbose = false;
std::wstring g_logDir;  // from the "log_dir" arg; default plugin_logs\NvrContentOverlay

// Filled before the hooks go in and only read afterwards.
bool g_active = false;
std::map<std::wstring, std::wstring> g_files;  // stock path (full, lower case) -> pack file
std::map<std::string, std::string> g_scripts;  // "<hash>.dll" (lower case) -> pack file (ANSI)
std::vector<std::wstring> g_firstPackages;  // <data dir>\packages\<pkg>_0 of each manifest

volatile LONG g_fileHits = 0;
volatile LONG g_scriptHits = 0;
volatile LONG g_logged = 0;
constexpr LONG kVerboseLines = 4000;

thread_local bool t_inHook = false;

ULONGLONG g_start = 0;  // process creation, in ms

ULONGLONG NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u +%llums] ", t.wHour, t.wMinute, t.wSecond,
            static_cast<unsigned long long>(NowMs() - g_start));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    return path;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr, nullptr);
    return out;
}

// A full path in one spelling: backslashes, no "." or "..", lower case.
std::wstring Canonical(const std::wstring& path) {
    std::wstring in = path;
    for (wchar_t& c : in)
        if (c == L'/') c = L'\\';
    // The game opens its data as \\?\c:\...: compare without that prefix.
    if (in.compare(0, 4, L"\\\\?\\") == 0 && in.compare(0, 8, L"\\\\?\\UNC\\") != 0) in.erase(0, 4);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(in.c_str(), MAX_PATH * 2, buf, nullptr);
    std::wstring out = (n == 0 || n >= MAX_PATH * 2) ? in : std::wstring(buf, n);
    if (!out.empty()) CharLowerBuffW(&out[0], static_cast<DWORD>(out.size()));
    return out;
}

/* <exe dir>\plugin_logs\NvrContentOverlay\NvrContentOverlay.log unless "log_dir" says otherwise
 * (relative to the exe's folder), where the other plugins log. */
void OpenLog() {
    std::wstring exe = ExeDir();
    std::wstring dir = g_logDir.empty() ? exe + L"\\plugin_logs\\NvrContentOverlay"
                                        : (g_logDir.size() > 1 && g_logDir[1] == L':' ? g_logDir : exe + L"\\" + g_logDir);
    for (size_t at = dir.find(L'\\', 3); at != std::wstring::npos; at = dir.find(L'\\', at + 1))
        CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    g_log = _wfopen((dir + L"\\NvrContentOverlay.log").c_str(), L"w");
}

DWORD ExeTimestamp() {
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

// The file's sha256 as lower-case hex, or "" when it can't be read.
std::string Sha256(const std::wstring& path) {
    HANDLE f = g_trueW ? g_trueW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_SEQUENTIAL_SCAN, nullptr)
                       : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<UCHAR> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while ((ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) && got > 0)
            BCryptHashData(hash, buf.data(), got, 0);
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char hex[] = "0123456789abcdef";
            for (UCHAR b : digest) {
                out += hex[b >> 4];
                out += hex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

bool IsHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool IsSafeId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return s != "." && s != "..";
}

// A path inside the pack: forward slashes, plain names, nothing absolute or climbing out.
bool IsPackPath(const std::string& p) {
    if (p.empty() || p.size() > 200 || p.front() == '/' || p.back() == '/') return false;
    size_t start = 0;
    while (true) {
        size_t end = p.find('/', start);
        std::string part = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-'))
                return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
}

std::string BaseName(const std::string& p) {
    size_t at = p.find_last_of('/');
    return at == std::string::npos ? p : p.substr(at + 1);
}

std::wstring PackFile(const std::wstring& dir, const std::string& rel) {
    std::wstring out = dir + L"\\" + Widen(rel);
    for (wchar_t& c : out)
        if (c == L'/') c = L'\\';
    return out;
}

/* Reads and checks bin\win10\content\<id>\content.json, the pack's files and the stock manifests.
 * Fills g_files and g_scripts; false (with the reason logged) leaves both empty. */
bool LoadPack(const std::string& id) {
    if (!IsSafeId(id)) {
        Log("pack '%s': not a valid id", id.c_str());
        return false;
    }
    const std::wstring exe = ExeDir();
    const std::wstring dir = exe + L"\\content\\" + Widen(id);
    const std::wstring root = exe + L"\\..\\..";

    json pack;
    {
        FILE* f = _wfopen((dir + L"\\content.json").c_str(), L"rb");
        if (!f) {
            Log("pack '%s': no content.json in %s", id.c_str(), Narrow(dir).c_str());
            return false;
        }
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1u << 20)) text.append(buf, n);
        fclose(f);
        pack = json::parse(text, nullptr, false);
    }
    auto str = [&](const json& o, const char* key) -> std::string {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (!pack.is_object() || pack.value("format", 0) != 1 || str(pack, "id") != id) {
        Log("pack '%s': content.json isn't a format 1 pack with this id", id.c_str());
        return false;
    }

    // Every file the pack has, checked against its sha256.
    std::map<std::string, std::string> files;
    auto fit = pack.find("files");
    if (fit == pack.end() || !fit->is_object()) {
        Log("pack '%s': content.json lists no files", id.c_str());
        return false;
    }
    for (auto& [rel, sha] : fit->items()) {
        std::string want = sha.is_string() ? sha.get<std::string>() : std::string();
        for (char& c : want) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (!IsPackPath(rel) || !IsHex(want, 64)) {
            Log("pack '%s': bad file entry '%s'", id.c_str(), rel.c_str());
            return false;
        }
        std::string got = Sha256(PackFile(dir, rel));
        if (got != want) {
            Log("pack '%s': %s is %s (sha256 %s, want %s)", id.c_str(), rel.c_str(),
                got.empty() ? "missing" : "changed", got.c_str(), want.c_str());
            return false;
        }
        files[rel] = want;
    }

    std::string dataRel = str(pack, "data_dir");
    if (!IsPackPath(dataRel)) {
        Log("pack '%s': bad data_dir '%s'", id.c_str(), dataRel.c_str());
        return false;
    }
    const std::wstring data = PackFile(root, dataRel);

    std::map<std::wstring, std::wstring> mapped;
    auto mit = pack.find("manifests");
    if (mit == pack.end() || !mit->is_array()) {
        Log("pack '%s': content.json has no manifests", id.c_str());
        return false;
    }
    for (const json& m : *mit) {
        std::string pkg = m.is_object() ? str(m, "package") : std::string();
        std::string stock = m.is_object() ? str(m, "stock_sha256") : std::string();
        std::string file = m.is_object() ? str(m, "file") : std::string();
        int count = m.is_object() ? m.value("stock_packages", 0) : 0;
        if (!IsHex(pkg, 16) || !IsHex(stock, 64) || count < 1 || !files.count(file)) {
            Log("pack '%s': bad manifest entry for '%s'", id.c_str(), pkg.c_str());
            return false;
        }
        const std::wstring stockPath = data + L"\\manifests\\" + Widen(pkg);
        std::string got = Sha256(stockPath);
        if (got != stock) {
            Log("pack '%s': the game's manifest %s is %s (sha256 %s), the pack was built from %s: "
                "not serving it (restore the stock game files)",
                id.c_str(), pkg.c_str(), got.empty() ? "missing" : "not stock", got.c_str(), stock.c_str());
            return false;
        }
        mapped[Canonical(stockPath)] = PackFile(dir, file);
        g_firstPackages.push_back(data + L"\\packages\\" + Widen(pkg) + L"_0");
        auto pit = m.find("packages");
        if (pit != m.end() && !pit->is_array()) {
            Log("pack '%s': packages of %s isn't a list", id.c_str(), pkg.c_str());
            return false;
        }
        if (pit != m.end()) {
            for (const json& p : *pit) {
                std::string rel = p.is_string() ? p.get<std::string>() : std::string();
                std::string name = BaseName(rel);
                bool ours = files.count(rel) && name.size() > pkg.size() + 1 && name.compare(0, pkg.size(), pkg) == 0 &&
                            name[pkg.size()] == '_';
                int n = ours ? atoi(name.c_str() + pkg.size() + 1) : -1;
                if (!ours || n < count || std::to_string(n) != name.substr(pkg.size() + 1)) {
                    Log("pack '%s': '%s' isn't one of %s's added packages (from _%d on)", id.c_str(), rel.c_str(),
                        pkg.c_str(), count);
                    return false;
                }
                mapped[Canonical(data + L"\\packages\\" + Widen(name))] = PackFile(dir, rel);
            }
        }
    }

    std::map<std::string, std::string> scripts;
    auto sit = pack.find("scripts");
    if (sit != pack.end() && !sit->is_array()) {
        Log("pack '%s': scripts isn't a list", id.c_str());
        return false;
    }
    if (sit != pack.end() && !sit->empty()) {
        // The loader takes an ANSI path: the pack's folder must have one.
        std::wstring wdir = dir;
        BOOL lossy = FALSE;
        int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wdir.c_str(), -1, nullptr, 0, nullptr, &lossy);
        if (lossy) {
            wchar_t shortPath[MAX_PATH];
            DWORD sn = GetShortPathNameW(dir.c_str(), shortPath, MAX_PATH);
            if (sn > 0 && sn < MAX_PATH) wdir = shortPath;
            lossy = FALSE;
            n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wdir.c_str(), -1, nullptr, 0, nullptr, &lossy);
        }
        if (lossy || n <= 0) {
            Log("pack '%s': its folder has no ANSI path for the game's script loader", id.c_str());
            return false;
        }
        std::string adir(n - 1, '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wdir.c_str(), -1, &adir[0], n, nullptr, nullptr);
        for (const json& s : *sit) {
            std::string rel = s.is_string() ? s.get<std::string>() : std::string();
            std::string name = BaseName(rel);
            bool ok = files.count(rel) && rel == "scripts/" + name && name.size() == 20 &&
                      IsHex(name.substr(0, 16), 16) && name.compare(16, 4, ".dll") == 0;
            if (!ok) {
                Log("pack '%s': '%s' isn't a script (scripts/<16 hex>.dll)", id.c_str(), rel.c_str());
                return false;
            }
            scripts[name] = adir + "\\scripts\\" + name;
        }
    }

    g_files.swap(mapped);
    g_scripts.swap(scripts);
    Log("pack '%s' %s: %zu files checked, %zu stock files served from the pack, %zu scripts", id.c_str(),
        str(pack, "version").c_str(), files.size(), g_files.size(), g_scripts.size());
    return true;
}

// Whether the game has opened its data already: it keeps its packages open from when it
// reads the manifests, so a stock package that can't be opened without sharing is in use.
// Serving the pack then would hand the game the pack's scripts with the stock data.

bool DataAlreadyOpen() {
    for (const std::wstring& path : g_firstPackages) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            continue;
        }
        if (GetLastError() == ERROR_SHARING_VIOLATION) return true;
    }
    return false;
}

// The pack's file for an open of a stock file, or nullptr. Reads only.
const std::wstring* PackFor(const std::wstring& name, DWORD access, DWORD disposition) {
    if (!g_active || g_files.empty()) return nullptr;
    if (disposition != OPEN_EXISTING ||
        (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)))
        return nullptr;
    auto it = g_files.find(Canonical(name));
    return it == g_files.end() ? nullptr : &it->second;
}

HANDLE OpenW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
             HANDLE tmpl, const char* via) {
    if (t_inHook || !name) return g_trueW(name, access, share, sa, disposition, flags, tmpl);
    t_inHook = true;
    HANDLE h;
    if (const std::wstring* pack = PackFor(name, access, disposition)) {
        h = g_trueW(pack->c_str(), access, share, sa, disposition, flags, tmpl);
        DWORD err = GetLastError();
        LONG n = InterlockedIncrement(&g_fileHits);
        if (h == INVALID_HANDLE_VALUE || n <= 50 || g_verbose)
            Log("%s %ls -> %ls%s", via, name, pack->c_str(), h == INVALID_HANDLE_VALUE ? " FAILED" : "");
        SetLastError(err);
    } else {
        h = g_trueW(name, access, share, sa, disposition, flags, tmpl);
        if (g_verbose && InterlockedIncrement(&g_logged) <= kVerboseLines) {
            DWORD err = GetLastError();
            Log("%s %ls (access %08lx, disposition %lu)%s", via, name, access, disposition,
                h == INVALID_HANDLE_VALUE ? " not opened" : "");
            SetLastError(err);
        }
    }
    t_inHook = false;
    return h;
}

HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE tmpl) {
    return OpenW(name, access, share, sa, disposition, flags, tmpl, "open");
}

HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disposition, DWORD flags, HANDLE tmpl) {
    if (t_inHook || !name) return g_trueA(name, access, share, sa, disposition, flags, tmpl);
    wchar_t wide[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2))
        return g_trueA(name, access, share, sa, disposition, flags, tmpl);
    return OpenW(wide, access, share, sa, disposition, flags, tmpl, "openA");
}

// The pack's copy of a script the game loads from ...\scripts\<hash>.dll, or nullptr.
const std::string* ScriptFor(const char* path) {
    if (!g_active || g_scripts.empty()) return nullptr;
    size_t len = strlen(path);
    if (len < 28) return nullptr;  // "scripts\" + 16 hex + ".dll"
    const char* name = path + len - 20;
    char sep = name[-1];
    if ((sep != '\\' && sep != '/') || _strnicmp(name - 8, "scripts", 7) != 0) return nullptr;
    if (len > 28 && name[-9] != '\\' && name[-9] != '/') return nullptr;
    std::string key(name, 20);
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = g_scripts.find(key);
    return it == g_scripts.end() ? nullptr : &it->second;
}

void* __fastcall LoadModuleHook(const char* path, void* arg, int callMain, int reuse) {
    if (path) {
        if (const std::string* pack = ScriptFor(path)) {
            void* h = g_trueLoad(pack->c_str(), arg, callMain, reuse);
            LONG n = InterlockedIncrement(&g_scriptHits);
            if (!h || n <= 50 || g_verbose) Log("script %s -> %s%s", path, pack->c_str(), h ? "" : " FAILED");
            return h;
        }
        if (g_verbose && InterlockedIncrement(&g_logged) <= kVerboseLines) Log("module %s", path);
    }
    return g_trueLoad(path, arg, callMain, reuse);
}

bool Hook(void* target, void* detour, void** original, void** slot, const char* what) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        Log("ERROR: hooking %s: %s", what, MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    *slot = target;
    return true;
}

bool HookKernel32(const char* function, void* detour, void** original, void** slot) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* t = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, function)) : nullptr;
    if (!t) {
        Log("ERROR: kernel32!%s not found", function);
        return false;
    }
    return Hook(t, detour, original, slot, function);
}

void Unhook() {
    for (void** target : {&g_targetW, &g_targetA, &g_targetLoad}) {
        if (*target) {
            MH_DisableHook(*target);
            MH_RemoveHook(*target);
            *target = nullptr;
        }
    }
}

// Whether the game holds a stock manifest open right now (it can't be opened without sharing).
void LogManifestState() {
    std::wstring data = ExeDir() + L"\\..\\..\\_data\\5932408047\\rad15\\win10\\manifests\\48037dc70b0ecab2";
    HANDLE h = CreateFileW(data.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        Log("probe: the main manifest isn't held open at init");
    } else {
        Log("probe: the main manifest can't be opened alone at init (error %lu%s)", err,
            err == ERROR_SHARING_VIOLATION ? ": the game holds it open" : "");
    }
}

struct Args {
    std::string active;
};

/* nEVR's args: a flat object of strings, e.g. {"active":"echocombat","logging":"verbose"}. */
Args ParseArgs(const char* argsJson) {
    Args out;
    if (!argsJson || !*argsJson) return out;
    json args = json::parse(argsJson, nullptr, false);
    if (!args.is_object()) return out;
    auto get = [&](const char* key) -> std::string {
        auto it = args.find(key);
        if (it == args.end()) return "";
        if (it->is_string()) return it->get<std::string>();
        return it->dump();
    };
    out.active = get("active");
    g_verbose = get("logging") == "verbose";
    g_logDir = Widen(get("log_dir"));
    return out;
}

bool g_installed = false;

int Install(const char* argsJson) {
    if (g_installed) return 0;  // already through the other entry point
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        g_start = ((static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime) / 10000;
    Args args = ParseArgs(argsJson);
    OpenLog();
    Log("NvrContentOverlay 1.0.0, %s", args.active.empty() ? "no active pack" : args.active.c_str());

    const bool live = ExeTimestamp() == kLiveBuild;
    uint8_t* load = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + kLoadModuleRva;
    const bool loaderOk = live && memcmp(load, kLoadModulePrologue, sizeof(kLoadModulePrologue)) == 0;
    if (g_verbose) LogManifestState();

    if (!args.active.empty()) {
        if (!LoadPack(args.active)) {
            Log("the game runs without the pack");
            return 1;
        }
        if (DataAlreadyOpen()) {
            Log("loaded too late: the game has opened its data already. List this plugin with "
                "`early: true` in config.yaml (nEVR with the early load pass). The game runs without the pack");
            g_files.clear();
            g_scripts.clear();
            return 1;
        }
        if (!g_scripts.empty() && !loaderOk) {
            Log("the pack has scripts, but this isn't the build its script loader is known for (%s): the game "
                "runs without the pack",
                live ? "loader changed" : "not echovr.exe 631547");
            return 1;
        }
    } else if (!g_verbose) {
        Log("nothing to do");
        return 0;
    }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ERROR: MH_Initialize: %s", MH_StatusToString(s));
        return 1;
    }
    g_active = !args.active.empty();
    bool ok = true;
    if (!g_files.empty() || g_verbose)
        ok = HookKernel32("CreateFileW", reinterpret_cast<void*>(CreateFileWHook), reinterpret_cast<void**>(&g_trueW),
                          &g_targetW) &&
             HookKernel32("CreateFileA", reinterpret_cast<void*>(CreateFileAHook), reinterpret_cast<void**>(&g_trueA),
                          &g_targetA);
    if (ok && loaderOk && (!g_scripts.empty() || g_verbose))
        ok = Hook(load, reinterpret_cast<void*>(LoadModuleHook), reinterpret_cast<void**>(&g_trueLoad), &g_targetLoad,
                  "the module loader");
    if (!ok) {
        g_active = false;
        Unhook();
        Log("the game runs without the pack");
        return 1;
    }
    g_installed = true;
    Log(g_active ? "serving the pack" : "logging only (verbose, no pack)");
    return 0;
}

}  // namespace

extern "C" {

NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "content_overlay";
    info.description = "Serves a content pack's game data in place of the stock files";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }

uint32_t NvrPluginGetCapabilities(void) {
    return NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY | NEVR_PLUGIN_CAP_HOOKS_ENGINE;
}

int NvrPluginInitEx(const NvrGameContext*, const char* args) { return Install(args); }

int NvrPluginInit(const NvrGameContext*) { return Install(nullptr); }

void NvrPluginShutdown(void) {
    g_active = false;
    Unhook();
    Log("shut down (%ld file opens and %ld scripts served from the pack)", g_fileHits, g_scriptHits);
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
