// mod_window — report the game's requested window size.
//
// [window] report_size=1: record the client-area size the game asks for on its
// main window (class MapleStoryClass) via SetWindowPos/MoveWindow, in
// maplewindow.txt next to MapleStory.exe ("W H"). Under Wayland compositors the
// floating Xwayland window can keep its old size when the client switches
// resolution (1024x768 login -> 1366x768 field), and Wine then accepts the
// compositor's size, so the requested size is only visible here. The Linux-side
// maple_winsync.py (started by play.sh) reads the file and resizes the window to match.
#include "../../sdk/mod_sdk.h"
#include <cstdio>
#include <cstring>

static const ModContext* C;

typedef BOOL (WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *MoveWindow_t)(HWND, int, int, int, int, BOOL);
static SetWindowPos_t orig_setwindowpos = nullptr;
static MoveWindow_t   orig_movewindow   = nullptr;
static char g_size_file[MAX_PATH];
static int  g_last_w = 0, g_last_h = 0;
static int  g_log_budget = 20;

static bool is_game_window(HWND h) {
    char cls[64];
    return h && GetClassNameA(h, cls, sizeof(cls)) && strcmp(cls, "MapleStoryClass") == 0;
}

// Requested outer size -> client size, using the window's own frame metrics.
static void report(HWND h, int w, int ht, const char* via) {
    if (!is_game_window(h) || w < 200 || ht < 200) return;
    RECT frame = {0, 0, 0, 0};
    AdjustWindowRectEx(&frame, (DWORD)GetWindowLongPtrA(h, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrA(h, GWL_EXSTYLE));
    int cw = w - (frame.right - frame.left), ch = ht - (frame.bottom - frame.top);
    if (cw == g_last_w && ch == g_last_h) return;
    g_last_w = cw; g_last_h = ch;
    if (FILE* f = fopen(g_size_file, "w")) { fprintf(f, "%d %d\n", cw, ch); fclose(f); }
    if (g_log_budget > 0) {
        g_log_budget--;
        C->logf("mod_window: game requested %dx%d via %s (client %dx%d)", w, ht, via, cw, ch);
    }
}

static BOOL WINAPI detour_setwindowpos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(flags & SWP_NOSIZE)) report(h, cx, cy, "SetWindowPos");
    return orig_setwindowpos(h, after, x, y, cx, cy, flags);
}

static BOOL WINAPI detour_movewindow(HWND h, int x, int y, int w, int ht, BOOL repaint) {
    report(h, w, ht, "MoveWindow");
    return orig_movewindow(h, x, y, w, ht, repaint);
}

static void hook(const char* name, void* detour, void** orig) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) u32 = LoadLibraryW(L"user32.dll");
    void* target = u32 ? (void*)GetProcAddress(u32, name) : nullptr;
    if (target && C->create_hook(target, detour, orig) == 0 && C->enable_hook(target) == 0)
        C->logf("mod_window: hooked user32!%s", name);
    else
        C->logf("mod_window: FAILED to hook user32!%s", name);
}

extern "C" MOD_EXPORT void ModInit(const ModContext* ctx) {
    C = ctx;
    if (ctx->abi_version != MOD_ABI_VERSION) return;

    if (C->ini_int("window", "report_size", 0)) {
        GetModuleFileNameA(nullptr, g_size_file, sizeof(g_size_file));
        char* slash = strrchr(g_size_file, '\\');
        if (slash) strcpy(slash + 1, "maplewindow.txt");
        hook("SetWindowPos", (void*)detour_setwindowpos, (void**)&orig_setwindowpos);
        hook("MoveWindow", (void*)detour_movewindow, (void**)&orig_movewindow);
    }
}
