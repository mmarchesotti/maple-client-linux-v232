#!/usr/bin/env bash
# Run MapleStory v232 under Wine. Lives in the client folder (next to
# MapleStory.exe). On the first run it sets up the Wine prefix and installs the
# mod loader; after that it just launches the game.
#
#   ./play.sh
#
# Env: WINEPREFIX (default ~/.wine-maple)
set -u
cd "$(dirname "$0")" || exit 1
export GAME="$PWD"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-maple}"

die() { echo "error: $*" >&2; exit 1; }

[ -f MapleStory.exe ] || die "MapleStory.exe not found. Copy these files into your client folder and run play.sh from there."
command -v wine >/dev/null || die "wine is not installed."
[ -d BlackCipher ] || echo "warning: BlackCipher/ is missing; the client will fail with CSecurityNGSFailed. Restore it from your client copy." >&2

# 1. Wine prefix with native DirectX 9 (Wine's builtin d3dx9_43 crashes the client).
if ! grep -qs '"d3dx9_43"="native' "$WINEPREFIX/user.reg"; then
    command -v winetricks >/dev/null || die "winetricks is not installed (needed once to set up $WINEPREFIX)."
    echo "Setting up Wine prefix $WINEPREFIX (first run only, takes a few minutes)..."
    winetricks -q d3dx9_43 d3dcompiler_43 d3dcompiler_47 || die "winetricks failed."
fi
# The in-game browser crashes under Wine, and Wine's crash dialog takes the keyboard
# focus from the game. Keep the crash in wine.log only.
if ! grep -qs '"ShowCrashDialog"=dword:00000000' "$WINEPREFIX/user.reg"; then
    wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f > /dev/null 2>&1
fi

# 2. Mod loader. The client loads Swordie.dll; our loader takes that name and
#    chain-loads the original AuthHook from Swordie_orig.dll.
is_loader() { grep -qa 'maplemods loader' "$1" 2>/dev/null; }
if [ -f maplemods_loader.dll ]; then
    if [ -f Swordie.dll ] && ! is_loader Swordie.dll; then
        [ -f Swordie_orig.dll ] && die "both Swordie.dll and Swordie_orig.dll are the AuthHook? Remove one and retry."
        echo "Keeping your AuthHook as Swordie_orig.dll."
        mv Swordie.dll Swordie_orig.dll
    fi
    cp -f maplemods_loader.dll Swordie.dll
fi
is_loader Swordie.dll || die "Swordie.dll is not the mod loader and maplemods_loader.dll is missing."
[ -f Swordie_orig.dll ] || echo "warning: Swordie_orig.dll (the Swordie AuthHook) is missing; login will not work." >&2

# 3. Launch.
if pgrep -f MapleStory.exe >/dev/null; then
    echo "MapleStory is already running."
else
    rm -f maplewindow.txt
    setsid wine MapleStory.exe WebStart whatever > wine.log 2>&1 < /dev/null &
    echo "Client started. Logs: $GAME/maplemods.log (mods), $GAME/wine.log (Wine)."
fi

# Hyprland keeps the window at its old size when the game changes resolution;
# this resizes it to match. Other desktops don't need it.
if command -v hyprctl >/dev/null && command -v python3 >/dev/null; then
    python3 maple_winsync.py > /dev/null 2>&1 &
fi
