#pragma once
// ---------------------------------------------------------------------------
// maplemods plugin SDK
//
// Each feature is its own DLL placed in the `mods` folder next to Swordie.dll.
// The loader (Swordie.dll) loads every *.dll there and calls the exported
// ModInit(const ModContext*). All shared services (logging, config, hooking,
// byte patching, the client image base) come through ModContext so a mod links
// nothing but this header.
//
// A mod, minimally:
//
//   #include "mod_sdk.h"
//   static const ModContext* C;
//   extern "C" MOD_EXPORT void ModInit(const ModContext* ctx) {
//       C = ctx;
//       if (!C->ini_int("foo", "enabled", 0)) return;    // per-mod toggle
//       C->logf("mod_foo: active");
//       // ... C->create_hook(...)/C->enable_hook(...) or C->patch(...)
//   }
// ---------------------------------------------------------------------------
#include <windows.h>
#include <cstdint>

#define MOD_ABI_VERSION 1
#define MOD_EXPORT __declspec(dllexport)

struct ModContext {
    uint32_t abi_version;        // == MOD_ABI_VERSION; a mod should check this

    uint8_t* image_base;         // live base of MapleStory.exe (0x140000000)
    // Convert a Ghidra address (0x140000000-based) to a live pointer.
    void*    (*abs)(uint64_t ghidra_addr);

    void     (*logf)(const char* fmt, ...);   // append to maplemods.log
    // Read maplemods.ini: int value from [section] key, else def.
    int      (*ini_int)(const char* section, const char* key, int def);

    // Function hooking (one shared MinHook instance in the loader).
    // create_hook: MH_STATUS (0 = ok); orig receives a callable to the original.
    int      (*create_hook)(void* target, void* detour, void** orig);
    int      (*enable_hook)(void* target);    // target == nullptr enables all

    // Raw byte patch (handles VirtualProtect). Returns true on success.
    bool     (*patch)(void* addr, const void* bytes, size_t len);
};

// A mod exports this. Called once, on a worker thread, after the client is up.
extern "C" MOD_EXPORT void ModInit(const ModContext* ctx);
