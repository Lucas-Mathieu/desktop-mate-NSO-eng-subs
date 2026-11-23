"""Install / update / uninstall the dmsubs mod from this checkout.

    python tools/install.py               # install or update
    python tools/install.py --uninstall   # remove version.dll (keeps dmsubs/ with your recordings)

The game folder is found through Steam; pass --game <folder> or set DMSUBS_GAME otherwise.

Updating merges mod/dmsubs/subtitles.json into the game's copy: curated fields
("en", "cues", "jp") from the repo win, everything learned in-game is kept.
"""
import argparse
import shutil
import sys

from common import REPO_SUBS, ROOT, game_arg, load_json, require_game, save_json

CURATED = ("en", "cues", "jp")


def merge_subtitles(src, dst):
    repo = load_json(src)
    game = load_json(dst)
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
    game.setdefault("characters", {}).update(repo.get("characters", {}))
    save_json(dst, game)
    return n


def main():
    ap = argparse.ArgumentParser()
    game_arg(ap)
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()
    game = require_game(args.game)
    dll = game / "version.dll"
    if args.uninstall:
        dll.unlink(missing_ok=True)
        print("removed", dll, "- dmsubs/ (settings, subtitles, recordings) left in place")
        return
    try:
        shutil.copy2(ROOT / "mod" / "version.dll", dll)
    except FileNotFoundError:
        sys.exit("mod\\version.dll missing - run build.bat first")
    except PermissionError:
        sys.exit("version.dll is in use - close Desktop Mate first")
    d = game / "dmsubs"
    d.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "mod" / "dmsubs" / "offsets.ini", d / "offsets.ini")
    if not (d / "dmsubs.ini").exists():
        shutil.copy2(ROOT / "mod" / "dmsubs" / "dmsubs.ini", d / "dmsubs.ini")
    n = merge_subtitles(REPO_SUBS, d / "subtitles.json")
    print(f"installed to {game} ({n} curated field(s) updated)")


if __name__ == "__main__":
    main()
