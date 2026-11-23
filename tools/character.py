"""Show or switch the character Desktop Mate loads, without going through the game's UI.

    python tools/character.py              # list installed characters, * = selected
    python tools/character.py choutenchan  # select wss_choutenchan.cb (close the game first)

Handy for harvesting / testing a character you just added. The selection is the game's own
PlayerPrefs value (HKCU\\Software\\infiniteloop\\DesktopMate, "CharacterSlot/0_...").
"""
import argparse
import subprocess
import sys
import winreg

from common import game_arg, require_game

KEY = r"Software\infiniteloop\DesktopMate"
SLOT = "CharacterSlot/0_"


def read_slot():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
            i = 0
            while True:
                name, value, kind = winreg.EnumValue(k, i)
                if name.startswith(SLOT):
                    return name, bytes(value).rstrip(b"\0").decode("utf-8", "replace")
                i += 1
    except OSError:
        return None, None


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DesktopMate.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "DesktopMate.exe" in out


def main():
    ap = argparse.ArgumentParser()
    game_arg(ap)
    ap.add_argument("name", nargs="?", help="bundle name, e.g. wss_amechan or just amechan")
    args = ap.parse_args()
    bundles = sorted(p.stem for p in
                     (require_game(args.game) / "DesktopMate_Data" / "StreamingAssets" / "AssetBundle").glob("*.cb"))
    slot, current = read_slot()
    cur_bundle = current.rsplit("/", 1)[-1] if current else None

    if not args.name:
        for b in bundles:
            print(("* " if b == cur_bundle else "  ") + b)
        if cur_bundle and cur_bundle not in bundles:
            print(f"* {cur_bundle}  (built-in)")
        if not slot:
            print("(no selection saved yet - pick a character in the game once)")
        return

    want = next((b for b in bundles if b in (args.name, f"wss_{args.name}")), None)
    if not want:
        sys.exit(f"no bundle named {args.name!r}; installed: {', '.join(bundles)}")
    if not slot:
        sys.exit("no selection saved yet - pick any character in the game once, then retry")
    if game_running():
        sys.exit("close Desktop Mate first (it rewrites the selection when it exits)")
    prefix = current.rsplit("/", 1)[0] + "/" if "/" in current else ""
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, slot, 0, winreg.REG_BINARY, (prefix + want).encode() + b"\0")
    print(f"selected {want} (was {cur_bundle}) - start Desktop Mate")


if __name__ == "__main__":
    main()
