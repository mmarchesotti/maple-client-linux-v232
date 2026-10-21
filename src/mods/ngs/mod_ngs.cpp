// mod_ngs — let the client run under Wine on a local Swordie server.
//
// Nexon's anti-cheat (NGS/BlackCipher) cannot run under Wine, and some
// Themida-protected client code misbehaves there. This mod applies the fixes:
//
//   patch_throw     the patch/NGS status check fails under Wine and the client
//                   throws a fatal CPatchException. We flip the JZ guarding the
//                   throw to a JMP so it never runs (C++ EH state stays intact).
//   [net] redirect  the login host is a DNS name that resolves to Nexon's servers.
//                   Any connect() to the Swordie ports (8483, 8484, 8585-8604)
//                   is diverted to [net] server (default 127.0.0.1).
//   stack_walk_fix  on every map load, protected code walks the main thread's
//                   stack down from StackBase, zeroing it. Under Wine that wipes
//                   live frames. We move the walk to unused stack.
//
// Every C++ throw is also logged to maplemods.log with a short backtrace.
#include "../../sdk/mod_sdk.h"
#include <cstring>
#include <cstdio>

static const ModContext* C;

// ---- C++ throw logging (kernel32!RaiseException) -----------------------------
// The client's CRT is statically linked: its throw wrapper calls
// KERNEL32!RaiseException directly. A C++ throw shows up as
// RaiseException(0xE06D7363, flags, 4, args) where
//   args[0]=0x19930520 (EH magic)  args[1]=exception object
//   args[2]=_ThrowInfo*            args[3]=module image base
typedef void (WINAPI *RaiseException_t)(DWORD, DWORD, DWORD, const ULONG_PTR*);
static RaiseException_t orig_raise = nullptr;

static const DWORD EH_MAGIC      = 0x19930520;
static const DWORD EH_EXCEPTION  = 0xE06D7363;   // MSVC C++ exception code

static int g_log_budget = 400;

// Stack backtrace, resolved dynamically (kernel32 forwards it from ntdll on x64).
// Frames inside MapleStory.exe (base 0x140000000, no relocations) are Ghidra
// addresses verbatim.
typedef USHORT (WINAPI *CaptureStack_t)(ULONG, ULONG, PVOID*, PULONG);
static CaptureStack_t p_capture = nullptr;
static int g_stack_budget = 48;

// ---- WinSock connect() redirect ----------------------------------------------
// We avoid including winsock2.h (conflicts with the sdk's windows.h); sockaddr
// is decoded by hand.
typedef int (WINAPI *connect_t)(uintptr_t, const void*, int);
static connect_t orig_connect = nullptr;
static bool g_net_log      = false;   // [net] log
static bool g_net_redirect = false;   // [net] redirect
static uint8_t g_server[4] = {127,0,0,1};   // [net] server
static int g_net_budget = 200;

// ---- patch_throw ---------------------------------------------------------------
// The patch/version check lives in FUN_14170df60 (Themida-virtualised body). Its
// throw block is intact .text:
//   14170eec1  CMP  dword[RSP+0x8c], 4      ; status == 4 (OK) ?
//   14170eec9  JZ   0x14170eef3  (74 28)    ; OK -> skip the throw
//   14170eeee  CALL 0x143bf54f0             ; else throw CPatchException
// Flipping the JZ to an unconditional JMP (74 -> EB) skips the throw without ever
// calling RaiseException, so the C++ exception machinery is never corrupted.
static const uint64_t CPATCH_THROW_JZ = 0x14170eec9ULL;
static const uint8_t  OP_JZ  = 0x74;   // original
static const uint8_t  OP_JMP = 0xEB;   // patched (unconditional)

// Walk _ThrowInfo (RVAs relative to `base`) -> CatchableTypeArray ->
// CatchableType -> TypeDescriptor.name. Returns false on anything that isn't a
// well-formed throw inside a loaded module.
static bool throw_type_name(uintptr_t base, const void* ti, char* out, size_t outcap) {
    if (!base || (base & 0xffff)) return false;            // module bases are 64K-aligned
    if (*(const uint16_t*)base != 0x5A4D) return false;    // 'MZ'
    uint32_t ntoff   = *(const uint32_t*)(base + 0x3c);
    uint32_t imgsize = *(const uint32_t*)(base + ntoff + 0x50);   // SizeOfImage (PE32+)
    auto inimg = [&](const void* p, size_t need) {
        uintptr_t a = (uintptr_t)p;
        return a >= base && (a + need) <= (base + imgsize);
    };
    if (!inimg(ti, 16)) return false;
    uint32_t ctaRVA = *(const uint32_t*)((const uint8_t*)ti + 12);
    const uint8_t* cta = (const uint8_t*)base + ctaRVA; if (!inimg(cta, 8)) return false;
    int n = *(const int*)cta; if (n < 1 || n > 64) return false;
    uint32_t ctRVA = *(const uint32_t*)(cta + 4);
    const uint8_t* ct = (const uint8_t*)base + ctRVA; if (!inimg(ct, 8)) return false;
    uint32_t tdRVA = *(const uint32_t*)(ct + 4);
    const uint8_t* td = (const uint8_t*)base + tdRVA; if (!inimg(td, 24)) return false;
    const char* name = (const char*)(td + 16);             // TypeDescriptor.name (x64)
    size_t i = 0;
    for (; i < outcap - 1; i++) {
        if (!inimg(name + i, 1)) return false;
        char c = name[i]; out[i] = c; if (c == '\0') break;
    }
    out[i] = '\0';
    return true;
}

static void WINAPI detour_raise(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
    if (code == EH_EXCEPTION && nargs >= 4 && args && (DWORD)args[0] == EH_MAGIC) {
        uintptr_t base = (uintptr_t)args[3];
        const void* ti = (const void*)args[2];
        char name[256];
        if (throw_type_name(base, ti, name, sizeof(name)) && g_log_budget > 0) {
            g_log_budget--;
            C->logf("mod_ngs: C++ throw %s", name);
            // frame[0] is the shared throw wrapper; frame[1+] is the game code
            // that issued the throw.
            if (p_capture && g_stack_budget > 0) {
                g_stack_budget--;
                PVOID fr[16];
                USHORT fn = p_capture(0, 16, fr, nullptr);
                for (USHORT i = 0; i < fn; i++)
                    C->logf("mod_ngs:     frame[%u] = %p", (unsigned)i, fr[i]);
            }
        }
    }
    orig_raise(code, flags, nargs, args);     // always raise normally
}

// ws2_32!connect detour: log the destination; divert Swordie ports to loopback.
// sockaddr layout (Windows):
//   AF_INET  (2):  u16 family; u16 port(BE); u8 addr[4]; ...
//   AF_INET6 (23): u16 family; u16 port(BE); u32 flowinfo; u8 addr[16]; ...
static int WINAPI detour_connect(uintptr_t s, const void* name, int namelen) {
    if (name && namelen >= 4) {
        const uint8_t* sa = (const uint8_t*)name;
        unsigned fam = (unsigned)(*(const uint16_t*)sa);
        if (fam == 2 /*AF_INET*/ && namelen >= 8) {
            unsigned port = ((unsigned)sa[2] << 8) | sa[3];   // BE -> host
            const uint8_t* ip = sa + 4;
            if (g_net_log && g_net_budget > 0) {
                g_net_budget--;
                C->logf("mod_ngs: connect -> %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
            }
            // The login host resolves to rotating IPs, so key off the Swordie port
            // set (login 8484, API 8483, channels 8585-8604), not a fixed address.
            bool is_sword_port = (port==8484 || port==8483 || (port>=8585 && port<=8604));
            // A loopback server accepts any 127.* destination as-is.
            bool at_server = g_server[0]==127 ? ip[0]==127 : memcmp(ip, g_server, 4)==0;
            if (g_net_redirect && is_sword_port && !at_server) {
                uint8_t buf[32];
                int n = namelen > (int)sizeof(buf) ? (int)sizeof(buf) : namelen;
                memcpy(buf, sa, n);
                memcpy(buf + 4, g_server, 4);
                C->logf("mod_ngs: connect REDIRECT %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
                        ip[0],ip[1],ip[2],ip[3],port, g_server[0],g_server[1],g_server[2],g_server[3],port);
                return orig_connect(s, buf, n);
            }
        } else if (fam == 23 /*AF_INET6*/ && namelen >= 8) {
            unsigned port = ((unsigned)sa[2] << 8) | sa[3];
            if (g_net_log && g_net_budget > 0) {
                g_net_budget--;
                const uint8_t* a = sa + 8;
                C->logf("mod_ngs: connect6 -> [%x:%x:...:%x%x]:%u",
                        (a[0]<<8)|a[1], (a[2]<<8)|a[3], a[14], a[15], port);
            }
        }
    }
    return orig_connect(s, name, namelen);
}

// ---- Map-load stack-walk fix ([ngs] stack_walk_fix=1) ------------------------
// On every map load, Themida-virtualised code reached from FUN_14397fb70 runs
//   TEB+8 -= 4; *(DWORD*)[TEB+8] = 0;      until TEB+8 reaches TEB StackLimit
// i.e. it walks the main thread's stack down from StackBase, zeroing it. Under
// Wine the main stack is [0x12000,0x110000) and the walk wipes the live frames
// (AV at 0x143a2e600 on Play; ret to 0 / AVs on portals). Fix:
//  1. int3 on FUN_14397fb70's `sub rsp,0x3b8` (0x14397fb7a). The VEH emulates the
//     sub and arms DR0 (write watch on this thread's TEB+8) in the context it
//     returns with. Arming from another thread races with exception-context
//     restores and is silently lost.
//  2. On the first write that starts a walk inside the real stack, move the cursor
//     to 64 KB below the live rsp so the walk only zeroes unused stack, and drop
//     DR0 so it runs at full speed. StackLimit is never touched: stack probes read
//     it, and moving it crashes on portals.
//  3. walk_watcher restores StackBase once the cursor reaches StackLimit (3 s stall
//     fallback). The next map load re-arms DR0 through the int3.
static const uintptr_t FE_ENTRY_SUB = 0x14397fb7aULL;
static const uint8_t   FE_ENTRY_BYTES[7] = {0x48,0x81,0xec,0xb8,0x03,0x00,0x00};   // sub rsp,0x3b8
static const uintptr_t WALK_MARGIN   = 0x10000;
static const uintptr_t DR7_L0_MASK   = 0xF0003;
static const uintptr_t DR7_L0_WRITE8 = 1 | (1u << 16) | (3u << 18);
static bool g_walk_fix = false;
static PVOID g_veh = nullptr;
static volatile uintptr_t g_walk_teb = 0;
static volatile uintptr_t g_real_stackbase = 0, g_real_limit = 0;
static volatile bool g_walk_active = false;
static volatile long g_walk_count = 0;
static uintptr_t g_self_lo = 0, g_self_size = 0;   // this DLL's image range

static bool on_field_entry_int3(EXCEPTION_POINTERS* ep) {
    CONTEXT* cx = ep->ContextRecord;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (at == FE_ENTRY_SUB + 1) at--;                 // tolerate post-int3 address
    if (at != FE_ENTRY_SUB) return false;
    cx->Rsp -= 0x3b8;                                 // the following movs/call ignore flags
    cx->Rip = FE_ENTRY_SUB + sizeof(FE_ENTRY_BYTES);
    uintptr_t teb = (uintptr_t)NtCurrentTeb();
    if (!g_walk_active) {
        g_walk_teb       = teb;
        g_real_stackbase = *(const uintptr_t*)(teb + 8);
        g_real_limit     = *(const uintptr_t*)(teb + 0x10);
    }
    cx->Dr0 = teb + 8;
    cx->Dr7 = (cx->Dr7 & ~DR7_L0_MASK) | DR7_L0_WRITE8;
    cx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return true;
}

static bool on_stackbase_write(EXCEPTION_POINTERS* ep) {
    CONTEXT* cx = ep->ContextRecord;
    if (!(cx->Dr6 & 1)) return false;
    cx->Dr6 = 0;
    // Our own StackBase write below re-triggers DR0 from inside this handler.
    if ((uintptr_t)cx->Rip - g_self_lo < g_self_size) return true;
    PNT_TIB t = (PNT_TIB)NtCurrentTeb();
    if ((uintptr_t)t != g_walk_teb || g_walk_active) return true;
    uintptr_t v = (uintptr_t)t->StackBase;
    uintptr_t safe = ((uintptr_t)cx->Rsp - WALK_MARGIN) & ~(uintptr_t)3;
    if (v >= g_real_limit && v < g_real_stackbase && v > safe && safe > g_real_limit) {
        g_walk_active = true;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);      // keep the flag store before the write
        t->StackBase = (PVOID)safe;
        cx->Dr7 &= ~DR7_L0_MASK;
        long k = InterlockedIncrement(&g_walk_count);
        C->logf("mod_ngs: stack walk #%ld redirected %llx -> %llx (rsp %llx, limit %llx)", k,
                (unsigned long long)v, (unsigned long long)safe, (unsigned long long)cx->Rsp,
                (unsigned long long)g_real_limit);
    }
    return true;
}

static DWORD WINAPI walk_watcher(void*) {
    uintptr_t prev = 0;
    ULONGLONG since = 0;
    int warn_budget = 5;
    for (;;) {
        PNT_TIB t = (PNT_TIB)g_walk_teb;
        if (!t) { Sleep(50); continue; }
        uintptr_t cur = (uintptr_t)t->StackBase;
        if (g_walk_active) {
            ULONGLONG now = GetTickCount64();
            if (cur != prev) { prev = cur; since = now; }
            bool done = cur <= g_real_limit + 4, stalled = now - since >= 3000;
            if (done || stalled) {
                if (done) Sleep(1);                    // let the final store land
                t->StackBase = (PVOID)g_real_stackbase;
                g_walk_active = false;
                prev = 0;
                C->logf("mod_ngs: stack walk #%ld %s; StackBase restored to %llx", (long)g_walk_count,
                        done ? "finished" : "STALLED 3s", (unsigned long long)g_real_stackbase);
            }
            Sleep(1);
        } else {
            // A walk we did not catch leaves StackBase below its real value.
            if (cur != g_real_stackbase && warn_budget > 0) {
                warn_budget--;
                C->logf("mod_ngs: WARNING main StackBase %llx != %llx outside a redirected walk",
                        (unsigned long long)cur, (unsigned long long)g_real_stackbase);
            }
            Sleep(20);
        }
    }
    return 0;
}

static bool arm_field_entry_int3() {
    volatile uint8_t* p = (uint8_t*)C->abs(FE_ENTRY_SUB);
    for (int i = 0; i < 600; i++) {                   // up to ~60 s for Themida decrypt
        if (memcmp((const void*)p, FE_ENTRY_BYTES, sizeof(FE_ENTRY_BYTES)) == 0) {
            uint8_t cc = 0xCC;
            bool ok = C->patch((void*)p, &cc, 1);
            C->logf("mod_ngs: stack_walk_fix %s (int3 @%llx)", ok ? "armed" : "PATCH FAILED",
                    (unsigned long long)FE_ENTRY_SUB);
            return ok;
        }
        Sleep(100);
    }
    C->logf("mod_ngs: stack_walk_fix NOT armed (bytes @%llx never matched)", (unsigned long long)FE_ENTRY_SUB);
    return false;
}

static DWORD WINAPI walk_fix_thread(void*) {
    if (arm_field_entry_int3()) walk_watcher(nullptr);
    return 0;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x80000003u && on_field_entry_int3(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code == 0x80000004u && on_stackbase_write(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;                // never swallow anything else
}

// [net] server=a.b.c.d from maplemods.ini (next to MapleStory.exe). The SDK only
// reads ints, so read the string directly.
static void read_server_ip() {
    char ini[MAX_PATH], val[64];
    GetModuleFileNameA(nullptr, ini, sizeof(ini));
    char* slash = strrchr(ini, '\\');
    if (!slash) return;
    strcpy(slash + 1, "maplemods.ini");
    GetPrivateProfileStringA("net", "server", "127.0.0.1", val, sizeof(val), ini);
    unsigned a, b, c, d; char extra;
    if (sscanf(val, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        g_server[0] = (uint8_t)a; g_server[1] = (uint8_t)b; g_server[2] = (uint8_t)c; g_server[3] = (uint8_t)d;
    } else {
        C->logf("mod_ngs: [net] server=\"%s\" is not an IPv4 address; using 127.0.0.1", val);
    }
}

// Patch the CPatchException throw block to skip the throw (JZ -> JMP).
// Waits for Themida to decrypt the byte before patching.
static void patch_cpatch_throw() {
    volatile uint8_t* p = (uint8_t*)C->abs(CPATCH_THROW_JZ);
    uint8_t cur = 0;
    for (int i = 0; i < 250; i++) {                 // up to ~25s (throw fires ~13s in)
        cur = *p;
        if (cur == OP_JZ || cur == OP_JMP) break;
        Sleep(100);
    }
    if (cur == OP_JMP) { C->logf("mod_ngs: CPatch throw already neutralised (EB) @%llx", CPATCH_THROW_JZ); return; }
    if (cur != OP_JZ)  { C->logf("mod_ngs: patch_throw ABORT - byte 0x%02X @%llx (expected 74); not patching", cur, CPATCH_THROW_JZ); return; }
    uint8_t jmp = OP_JMP;
    if (C->patch((void*)p, &jmp, 1))
        C->logf("mod_ngs: CPatch throw neutralised at source (JZ->JMP @%llx); EH left intact", CPATCH_THROW_JZ);
    else
        C->logf("mod_ngs: patch_throw FAILED @%llx (err %lu)", CPATCH_THROW_JZ, GetLastError());
}

extern "C" MOD_EXPORT void ModInit(const ModContext* ctx) {
    C = ctx;
    if (ctx->abi_version != MOD_ABI_VERSION) return;
    if (!C->ini_int("ngs", "enabled", 0)) { C->logf("mod_ngs: disabled"); return; }

    bool do_patch  = C->ini_int("ngs", "patch_throw", 1) != 0;
    g_walk_fix     = C->ini_int("ngs", "stack_walk_fix", 0) != 0;
    g_net_log      = C->ini_int("net", "log", 0) != 0;
    g_net_redirect = C->ini_int("net", "redirect", 0) != 0;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) k32 = LoadLibraryW(L"kernel32.dll");
    auto target = k32 ? (void*)GetProcAddress(k32, "RaiseException") : nullptr;
    if (!target) { C->logf("mod_ngs: ABORT - RaiseException not found"); return; }

    p_capture = (CaptureStack_t)GetProcAddress(k32, "RtlCaptureStackBackTrace");
    if (!p_capture) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) p_capture = (CaptureStack_t)GetProcAddress(nt, "RtlCaptureStackBackTrace");
    }

    int st = C->create_hook(target, (void*)detour_raise, (void**)&orig_raise);
    if (st != 0) { C->logf("mod_ngs: create_hook failed (%d) on RaiseException @%p", st, target); return; }
    C->enable_hook(target);
    C->logf("mod_ngs: active - hooked kernel32!RaiseException @%p; backtrace=%s patch_throw=%s",
            target, p_capture ? "on" : "UNAVAILABLE", do_patch ? "on" : "off");

    if (do_patch) patch_cpatch_throw();

    if (g_walk_fix) {
        g_veh = AddVectoredExceptionHandler(1, veh);
        C->logf("mod_ngs: VEH %s (stack_walk_fix)", g_veh ? "armed" : "FAILED to install");
    }
    if (g_walk_fix && g_veh) {
        HMODULE me = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&veh, &me);
        g_self_lo   = (uintptr_t)me;
        g_self_size = ((PIMAGE_NT_HEADERS)(g_self_lo + ((PIMAGE_DOS_HEADER)me)->e_lfanew))->OptionalHeader.SizeOfImage;
        HANDLE t = CreateThread(nullptr, 0, walk_fix_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }

    if (g_net_redirect) read_server_ip();
    if (g_net_log || g_net_redirect) {
        HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
        if (!ws) ws = LoadLibraryW(L"ws2_32.dll");
        void* ct = ws ? (void*)GetProcAddress(ws, "connect") : nullptr;
        if (ct && C->create_hook(ct, (void*)detour_connect, (void**)&orig_connect) == 0) {
            C->enable_hook(ct);
            C->logf("mod_ngs: hooked ws2_32!connect @%p; net_log=%s redirect=%s",
                    ct, g_net_log ? "on" : "off", g_net_redirect ? "on" : "off");
            if (g_net_redirect)
                C->logf("mod_ngs: Swordie ports -> %u.%u.%u.%u", g_server[0], g_server[1], g_server[2], g_server[3]);
        } else {
            C->logf("mod_ngs: connect hook NOT installed (ws2_32=%p connect=%p)", ws, ct);
        }
    }
}
