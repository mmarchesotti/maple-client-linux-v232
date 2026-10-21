#!/usr/bin/env bash
# Build the loader (maplemods_loader.dll, runs as Swordie.dll) and every mod (mods/*.dll).
# Windows: Git Bash + w64devkit. Linux: auto-uses the mingw cross-compiler.
set -euo pipefail
cd "$(dirname "$0")"

# Override with e.g. CROSS=/opt/w64devkit/bin/x86_64-w64-mingw32-
CROSS="${CROSS:-x86_64-w64-mingw32-}"
if command -v "${CROSS}g++" >/dev/null 2>&1; then
    CXX="${CROSS}g++" ; CC="${CROSS}gcc"
else
    CXX=g++ ; CC=gcc                                              # w64devkit shell
fi

CFLAGS="-O2 -Iminhook/include -DUNICODE -D_UNICODE"
CXXFLAGS="$CFLAGS -std=c++17 -fno-exceptions -fno-rtti"
LD="-static -static-libgcc -static-libstdc++"

rm -rf build && mkdir -p build/obj build/mods

# ---- loader: maplemods_loader.dll (links MinHook) ----
$CC  $CFLAGS -c minhook/src/hook.c       -o build/obj/hook.o
$CC  $CFLAGS -c minhook/src/buffer.c     -o build/obj/buffer.o
$CC  $CFLAGS -c minhook/src/trampoline.c -o build/obj/trampoline.o
$CC  $CFLAGS -c minhook/src/hde/hde64.c  -o build/obj/hde64.o
$CXX $CXXFLAGS -c loader/dllmain.cpp     -o build/obj/dllmain.o
$CXX -shared $LD -o build/maplemods_loader.dll build/obj/*.o exports.def -lkernel32 -luser32 -lws2_32
echo "built build/maplemods_loader.dll (installed as Swordie.dll by play.sh)"

# ---- feature mods: one DLL each ----
for dir in mods/*/; do
    name=$(basename "$dir")
    src=$(ls "$dir"*.cpp)
    out="build/mods/mod_${name}.dll"
    $CXX $CXXFLAGS -shared $LD -o "$out" "$src" -lkernel32 -luser32
    echo "built $out"
done

cp maplemods.ini build/maplemods.ini
echo "---"
echo "copy build/* over dist/ (or straight into the client folder), then run play.sh."
