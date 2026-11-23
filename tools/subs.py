"""Work with the subtitle database while translating.

    python tools/subs.py status              # per character: translated / machine-only / missing
    python tools/subs.py todo [character]    # lines that still need a proper English translation
    python tools/subs.py pull                # copy lines learned in-game into the repo copy

There are two copies of subtitles.json:
  <game>/dmsubs/subtitles.json   what the mod reads (live); learn.py writes new lines here
  mod/dmsubs/subtitles.json      the curated copy you commit and ship (install.py / package.py)

Typical loop: harvest in game -> learn.py -> `subs.py pull` -> write "en"/"cues" in the repo copy
-> `install.py` (merges them back into the game) -> check in game -> commit.
"""
import argparse
import json
import sys
from collections import defaultdict

from common import REPO_SUBS, game_arg, load_json, require_game, save_json

LEARNED = ("jp", "en_auto", "segments", "len", "nospeech")


def split(key):
    return key.split("/", 1) if "/" in key else ("(any)", key)


def translated(e):
    return bool(e.get("en") or e.get("cues"))


def cmd_status(game_db, repo_db):
    rows = defaultdict(lambda: [0, 0, 0, 0, 0])  # translated, machine-only, silent (no speech), game-only
    for key, e in repo_db["lines"].items():
        r = rows[split(key)[0]]
        r[0 if translated(e) else 2 if e.get("nospeech") else 1] += 1
    for key, e in game_db["lines"].items():
        if key not in repo_db["lines"]:
            rows[split(key)[0]][2 if e.get("nospeech") else 3] += 1
    names = repo_db.get("characters", {})
    print(f"{'character':<28}{'translated':>11}{'machine':>9}{'no speech':>11}{'not pulled':>12}")
    for ch in sorted(rows):
        t, m, s, g, _ = rows[ch]
        label = f"{ch} ({names[ch]})" if ch in names else ch
        print(f"{label:<28}{t:>11}{m:>9}{s:>11}{g:>12}")
    if any(r[3] for r in rows.values()):
        print("\n'not pulled' lines exist only in the game copy - run `python tools/subs.py pull`")


def cmd_todo(repo_db, character):
    todo = {k: e for k, e in repo_db["lines"].items()
            if not translated(e) and not e.get("nospeech") and (not character or split(k)[0] == character)}
    for k, e in todo.items():
        print(json.dumps({k: {f: e[f] for f in ("jp", "en_auto", "segments") if f in e}},
                         ensure_ascii=False, indent=2)[2:-2] + ",")
    print(f"\n{len(todo)} line(s) without \"en\"/\"cues\" in {REPO_SUBS}", file=sys.stderr)


def cmd_pull(game_db, repo_db):
    added = filled = 0
    lines = repo_db.setdefault("lines", {})
    for key, e in game_db["lines"].items():
        if key not in lines:
            if e.get("nospeech"):
                continue  # sound effects / silence: nothing to translate
            lines[key] = dict(e)
            added += 1
            continue
        cur = lines[key]
        for f in LEARNED + ("en", "cues"):
            if f in e and f not in cur:
                cur[f] = e[f]
                filled += 1
        if any(f in e and f in cur and e[f] != cur[f] for f in ("en", "cues")):
            print(f"note: {key} has a different en/cues in the game copy (repo copy kept)")
    save_json(REPO_SUBS, repo_db)
    print(f"{added} new line(s), {filled} field(s) filled -> {REPO_SUBS}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    game_arg(ap)
    ap.add_argument("command", choices=("status", "todo", "pull"))
    ap.add_argument("character", nargs="?", help="todo: only this character id, e.g. Amechan01")
    args = ap.parse_args()
    repo_db = load_json(REPO_SUBS)
    repo_db.setdefault("lines", {})
    if args.command == "todo":
        return cmd_todo(repo_db, args.character)
    game_db = load_json(require_game(args.game) / "dmsubs" / "subtitles.json")
    game_db.setdefault("lines", {})
    (cmd_status if args.command == "status" else cmd_pull)(game_db, repo_db)


if __name__ == "__main__":
    main()
