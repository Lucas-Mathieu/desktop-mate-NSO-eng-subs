"""Install / update / uninstall the dmsubs mod.

    python tools/install.py               # install or update
    python tools/install.py --uninstall   # remove version.dll (keeps dmsubs/ with your recordings)

Updating merges mod/dmsubs/subtitles.json into the game's copy: curated fields
("en", "cues", "jp") from the repo win, everything learned in-game is kept.
"""
import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME = Path(r"D:\SteamLibrary\steamapps\common\Desktop Mate")
CURATED = ("en", "cues", "jp")


def merge_subtitles(src, dst):
    repo = json.loads(src.read_text(encoding="utf-8-sig"))
    game = json.loads(dst.read_text(encoding="utf-8-sig")) if dst.exists() else {"lines": {}}
    lines = game.setdefault("lines", {})
    n = 0
    for key, e in repo.get("lines", {}).items():
        cur = lines.setdefault(key, {})
        for f in CURATED:
            if f in e and cur.get(f) != e[f]:
                cur[f] = e[f]
                n += 1
        for f, v in e.items():
            cur.setdefault(f, v)
    game["lines"] = dict(sorted(lines.items()))
    tmp = dst.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(game, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(20):  # the game may be reading it right now
        try:
            os.replace(tmp, dst)
            break
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.1)
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()
    game = args.game
    if not (game / "DesktopMate.exe").exists():
        sys.exit(f"DesktopMate.exe not found in {game} (use --game)")
    dll = game / "version.dll"
    if args.uninstall:
        dll.unlink(missing_ok=True)
        print("removed", dll, "- dmsubs/ (settings, subtitles, recordings) left in place")
        return
    try:
        shutil.copy2(ROOT / "mod" / "version.dll", dll)
    except PermissionError:
        sys.exit("version.dll is in use - close Desktop Mate first")
    d = game / "dmsubs"
    d.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "mod" / "dmsubs" / "offsets.ini", d / "offsets.ini")
    if not (d / "dmsubs.ini").exists():
        shutil.copy2(ROOT / "mod" / "dmsubs" / "dmsubs.ini", d / "dmsubs.ini")
    n = merge_subtitles(ROOT / "mod" / "dmsubs" / "subtitles.json", d / "subtitles.json")
    print(f"installed to {game} ({n} curated field(s) updated)")


if __name__ == "__main__":
    main()
