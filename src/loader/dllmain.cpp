// Swordie.dll proxy + plugin loader.
//
// Takes the nmcogame64 ordinal-1 slot, chain-loads the real AuthHook
// (Swordie_orig.dll), then loads every mods\*.dll and calls its ModInit,
// handing each a ModContext with shared services.

#include "../sdk/mod_sdk.h"
#include "../minhook/include/MinHook.h"
#include <cstdio>
#include <cstdarg>

static HMODULE g_self = nullptr;
static wchar_t g_dir[MAX_PATH]   = L"";   // folder containing Swordie.dll
static wchar_t g_log[MAX_PATH]   = L"";
static wchar_t g_ini[MAX_PATH]   = L"";

// ---- shared services exposed to mods --------------------------------------
static void svc_logf(const char* fmt, ...) {
    FILE* f = _wfopen(g_log, L"a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void* svc_abs(uint64_t ghidra) {
    return (uint8_t*)GetModuleHandleW(nullptr) + (ghidra - 0x140000000ULL);
}
static int svc_ini_int(const char* section, const char* key, int def) {
    wchar_t ws[128], wk[128];
    MultiByteToWideChar(CP_UTF8, 0, section, -1, ws, 128);
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wk, 128);
    return (int)GetPrivateProfileIntW(ws, wk, def, g_ini);
}
static int svc_create_hook(void* target, void* detour, void** orig) {
    return (int)MH_CreateHook(target, detour, orig);
}
static int svc_enable_hook(void* target) {
    return (int)MH_EnableHook(target ? target : MH_ALL_HOOKS);
}
static bool svc_patch(void* addr, const void* bytes, size_t len) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, bytes, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

static ModContext g_ctx = {
    MOD_ABI_VERSION, nullptr, svc_abs, svc_logf, svc_ini_int,
    svc_create_hook, svc_enable_hook, svc_patch
};

// ---- loader ----------------------------------------------------------------
static void load_mods() {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\mods\\*.dll", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { svc_logf("no mods\\*.dll found"); return; }
    do {
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\mods\\%ls", g_dir, fd.cFileName);
        HMODULE m = LoadLibraryW(path);
        if (!m) { svc_logf("mod load FAILED: %ls (err %lu)", fd.cFileName, GetLastError()); continue; }
        auto init = (void(*)(const ModContext*))GetProcAddress(m, "ModInit");
        if (!init) { svc_logf("mod has no ModInit: %ls", fd.cFileName); continue; }
        svc_logf("mod loaded: %ls -> ModInit", fd.cFileName);
        init(&g_ctx);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI init_thread(LPVOID) {
    // chain-load the real AuthHook so local login redirection still works
    wchar_t orig[MAX_PATH];
    _snwprintf(orig, MAX_PATH, L"%ls\\Swordie_orig.dll", g_dir);
    HMODULE real = LoadLibraryW(orig);
    svc_logf("chain-load Swordie_orig.dll -> %p %ls", (void*)real,
             real ? L"(ok)" : L"(MISSING - login hook will not run!)");

    g_ctx.image_base = (uint8_t*)GetModuleHandleW(nullptr);
    if (MH_Initialize() != MH_OK) { svc_logf("MinHook init FAILED"); }
    else svc_logf("MinHook initialized; image_base=%p", (void*)g_ctx.image_base);

    load_mods();

    // enable anything mods created but did not enable themselves
    MH_EnableHook(MH_ALL_HOOKS);
    svc_logf("loader: init complete");
    return 0;
}

extern "C" __declspec(dllexport) void APIDummy_ctor() {}  // ordinal 1

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameW(g_self, g_dir, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
        _snwprintf(g_log, MAX_PATH, L"%ls\\maplemods.log", g_dir);
        _snwprintf(g_ini, MAX_PATH, L"%ls\\maplemods.ini", g_dir);
        if (FILE* f = _wfopen(g_log, L"w")) fclose(f);   // truncate
        svc_logf("maplemods loader attached, pid=%lu", GetCurrentProcessId());
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
