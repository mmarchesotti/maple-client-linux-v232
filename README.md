# maplestory-v232-linux

Run the **MapleStory v232 (GMS 232.2) client** under **Wine on Linux** against
your own local [Swordie](https://forum.ragezone.com/threads/xmas-release-v232-swordie-source.1257894/)
server.

Tested with Wine 11.12 on Arch Linux + Hyprland. The client logs in, picks a
world, channel and character, enters the field, and changes maps through
portals and warps.

This repo has **no game files**. You need your own v232 client set up with the
Swordie AuthHook (`Swordie.dll`), and a running Swordie server.

## Quick start

You need `wine` and `winetricks` installed, a v232 client with the Swordie
AuthHook (`Swordie.dll`) and its `BlackCipher/` folder, and a Swordie server.

1. Copy everything inside [`dist/`](dist/) into your client folder (the one with
   `MapleStory.exe`).
2. If the server is **not** on this machine, open `maplemods.ini` and set its IP:
   ```ini
   [net]
   server=192.168.1.50
   ```
3. Run:
   ```sh
   ./play.sh
   ```

The first run takes a few minutes. `play.sh` sets up a Wine prefix in
`~/.wine-maple` with the DirectX libraries the client needs, then renames your
AuthHook to `Swordie_orig.dll` and puts the mod loader in its place. Later runs
start the game right away.

If something goes wrong, check `maplemods.log` and `wine.log` in the client
folder.

**What's in `dist/`**

| File | What it is |
|---|---|
| `play.sh` | setup + launcher |
| `maplemods_loader.dll` | mod loader; `play.sh` installs it as `Swordie.dll` and it chain-loads your AuthHook |
| `mods/mod_ngs.dll` | the Wine fixes |
| `mods/mod_window.dll` | reports the game's window size (for the Hyprland fix) |
| `maplemods.ini` | settings (server IP, fixes on/off) |
| `maple_winsync.py` | keeps the window at the game's resolution on Hyprland; `play.sh` starts it |

**Uninstall:** delete the files above plus `Swordie.dll`, then rename
`Swordie_orig.dll` back to `Swordie.dll`.

## What gets fixed

| Stage | Symptom | Cause | Fix |
|---|---|---|---|
| Startup | null-pointer crash in the GR2D DirectX 9 engine | Wine's builtin `d3dx9_43` | `winetricks d3dx9_43 d3dcompiler_43 d3dcompiler_47` |
| Startup | fatal `CPatchException`, then an uncaught `ZException` | Nexon's anti-cheat (BlackCipher) can't run under Wine, so the patch/NGS status check fails | `mod_ngs` `patch_throw`: flips `JZ` to `JMP` at `0x14170eec9` so the throw never runs (C++ EH state stays intact) |
| Login | client connects to Nexon's servers | the login host is a DNS name | `mod_ngs` `[net] redirect`: sends the Swordie ports (8483/8484/8585–8604) to `[net] server` (default `127.0.0.1`) |
| Play / every map change | window shrinks and darkens, then the client crashes; map loads take 3–4 s longer | Themida-protected code walks the main thread's stack down from `StackBase`, zeroing it. Under Wine that wipes live stack frames, and the stack is fully committed, so the walk covers ~1 MB | `mod_ngs` `stack_walk_fix`: a hardware watchpoint catches the walk and moves it to one page above `StackLimit`, so it only zeroes that unused page (~15 ms). `StackBase` is restored afterwards |
| Play (rare) | a worker thread dies, e.g. after opening the inventory (`wine.log`: `Exception frame is not in stack limits`) | the same walk on another thread | `stack_walk_fix` arms the watchpoint on every thread, not just the main one |
| In-game browser crash | keyboard stops working (arrow keys still move) | Wine's crash dialog for `MapleBrowser_WZ2.exe` takes the keyboard focus | `play.sh` turns the dialog off (`HKCU\Software\Wine\WineDbg` `ShowCrashDialog=0`); crashes are still logged to `wine.log` |
| Resolution change | squashed picture, offset mouse | Hyprland keeps the floating Xwayland window at the old size | `mod_window` writes the requested size to `maplewindow.txt`; `maple_winsync.py` resizes the window via `hyprctl` |

All options are in `maplemods.ini`. On **Windows**, `play.sh` doesn't apply:
rename `Swordie.dll` to `Swordie_orig.dll` and `maplemods_loader.dll` to
`Swordie.dll` yourself, and set `[ngs] enabled=0` (the anti-cheat works there).

## Known issues

- **Server on another machine:** tested only with the server on `127.0.0.1`
  (local, or through an SSH tunnel). With `server=<LAN IP>`, Swordie must also
  advertise that IP for its channels. If it doesn't work, forward the ports to
  this machine's loopback instead and keep `server=127.0.0.1`:
  ```sh
  # run on the server machine
  A=(); for p in 8483 8484 3000 $(seq 8585 8604); do A+=(-R 127.0.0.1:$p:127.0.0.1:$p); done
  ssh -f -N -o ExitOnForwardFailure=yes "${A[@]}" you@linux-box
  ```
- **Window size on other compositors:** the resize sync uses `hyprctl`, so it
  only works on Hyprland. Elsewhere, resize the window by hand if the picture
  looks squashed after login.
- **In-game browser:** the client's CEF browser (`MapleBrowser_WZ2.exe`) crashes
  under Wine, so the event and guide pop-ups stay blank. The game keeps running;
  close the pop-ups.

## Building from source (optional)

Only needed if you want to change the mods. You need a MinGW-w64 x86_64
toolchain: `x86_64-w64-mingw32-g++` on Linux (e.g. the `mingw-w64-gcc`
package), or [w64devkit](https://github.com/skeeto/w64devkit) on Windows.

```sh
bash src/build.sh                 # -> src/build/{maplemods_loader.dll,maplemods.ini,mods/*.dll}
```

Set `CROSS` to use a specific toolchain prefix, e.g.
`CROSS=/opt/w64devkit/bin/x86_64-w64-mingw32- bash src/build.sh`. Copy the
output over `dist/` (or straight into your client folder) and run `play.sh`. The
DLLs in `dist/` were built with GCC 16.2 (w64devkit, and the Arch `mingw-w64-gcc`
package for `mod_ngs.dll`).

### How it loads

`nmcogame64.dll` (already modified by the AuthHook) imports `Swordie.dll` by
ordinal 1, only to force it to load. Our loader (installed as `Swordie.dll`)
takes that slot: it exports
ordinal 1 (a no-op), chain-loads the real AuthHook as `Swordie_orig.dll`, then
loads every `mods\*.dll` and calls its `ModInit(const ModContext*)`.

```
src/
  loader/dllmain.cpp   -> maplemods_loader.dll (links MinHook)
  sdk/mod_sdk.h        plugin API: logging, ini, hooking, byte patching
  mods/ngs/            the Wine fixes
  mods/window/         window-size reporting
  minhook/             vendored MinHook (BSD-2-Clause, see its LICENSE.txt)
  maplemods.ini        default settings (copied to dist/)
dist/                  what users copy into the client folder
```

The client image has no relocations and always loads at `0x140000000`, so the
addresses in the source are both Ghidra addresses and live addresses.

To add a mod, create `src/mods/<name>/mod_<name>.cpp` that exports
`extern "C" MOD_EXPORT void ModInit(const ModContext* ctx)`, add a `[<name>]`
section to `maplemods.ini`, and rebuild. `build.sh` picks it up automatically.

## Disclaimer

For personal use with your own local private server. The fixes bypass the
client's anti-cheat checks, so never point this client at official servers.
This project is not affiliated with or endorsed by Nexon.
