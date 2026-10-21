#!/usr/bin/env python3
"""Keep the MapleStory window's Hyprland size equal to the size the game asks for.

When the client switches resolution (e.g. 1024x768 at login -> 1366x768 in the
field), Hyprland keeps the floating Xwayland window at its old size and Wine
accepts it, which squashes the picture and offsets the mouse. mod_window
([window] report_size=1) writes the size the game requested to maplewindow.txt
in the client folder; this resizes the window to match whenever they differ.

  python3 maple_winsync.py [--once]     (exits when MapleStory.exe exits)
Env: GAME = client folder (default: this script's folder). play.sh starts it.
"""
import json, os, subprocess, sys, time

SIZE_FILE = os.path.join(os.environ.get("GAME", os.path.dirname(os.path.abspath(__file__))),
                         "maplewindow.txt")


def wanted_size():
    try:
        w, h = open(SIZE_FILE).read().split()[:2]
        return int(w), int(h)
    except (OSError, ValueError):
        return None


def hypr(*args):
    return subprocess.run(["hyprctl", *args], capture_output=True, text=True, timeout=3).stdout


def game_client():
    try:
        for c in json.loads(hypr("clients", "-j")):
            if "maplestory" in c.get("class", "").lower() and c.get("size", [0, 0])[0] >= 200:
                return c
    except (json.JSONDecodeError, subprocess.SubprocessError):
        pass
    return None


def monitor_size():
    try:
        for m in json.loads(hypr("monitors", "-j")):
            if m.get("focused"):
                return m["width"], m["height"]
    except (json.JSONDecodeError, subprocess.SubprocessError):
        pass
    return None


def sync_once():
    want = wanted_size()
    c = game_client()
    if not want or not c or tuple(c["size"]) == want:
        return False
    addr = "address:" + c["address"]
    hypr("dispatch", "resizewindowpixel", f"exact {want[0]} {want[1]},{addr}")
    mon = monitor_size()
    x, y = c["at"]
    if mon and (x + want[0] > mon[0] or y + want[1] > mon[1]):
        hypr("dispatch", "movewindowpixel", f"exact 0 0,{addr}")
    print(f"[winsync {time.strftime('%T')}] resized {c['size'][0]}x{c['size'][1]} -> {want[0]}x{want[1]}", flush=True)
    return True


def client_alive():
    return subprocess.run(["pgrep", "-f", "MapleStory.exe"], capture_output=True).returncode == 0


def main():
    if "--once" in sys.argv:
        sync_once()
        return
    seen = False
    while True:
        alive = client_alive()
        seen |= alive
        if seen and not alive:
            return
        sync_once()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
