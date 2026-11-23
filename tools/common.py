"""Shared helpers for the dmsubs tools."""
import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO_SUBS = ROOT / "mod" / "dmsubs" / "subtitles.json"
STEAM_APP_ID = "3301060"


def _steam_libraries():
    roots = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            roots.append(Path(winreg.QueryValueEx(k, "SteamPath")[0]))
    except OSError:
        pass
    roots.append(Path(r"C:\Program Files (x86)\Steam"))
    libs = []
    for steam in roots:
        vdf = steam / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace")):
                libs.append(Path(p.replace("\\\\", "\\")))
        libs.append(steam)
    return libs


def find_game():
    """Desktop Mate folder: $DMSUBS_GAME, else looked up in the Steam libraries."""
    env = os.environ.get("DMSUBS_GAME")
    if env:
        return Path(env)
    for lib in _steam_libraries():
        game = lib / "steamapps" / "common" / "Desktop Mate"
        if (game / "DesktopMate.exe").exists():
            return game
    return None


def game_arg(ap):
    """Adds --game (defaults to the auto-detected folder)."""
    ap.add_argument("--game", type=Path, default=find_game(),
                    help="Desktop Mate folder (default: found via Steam, or $DMSUBS_GAME)")


def require_game(game):
    if not game or not (game / "DesktopMate.exe").exists():
        raise SystemExit(f"DesktopMate.exe not found in {game} - pass --game <folder> or set DMSUBS_GAME")
    return game


def load_json(path):
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8-sig"))
    return {"lines": {}}


def save_json(path, db):
    """Writes atomically; retries while the game has the file open."""
    import time
    out = dict(db, lines=dict(sorted(db.get("lines", {}).items())))
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.1)


def repo_url():
    """https URL of this checkout's 'origin' remote (your fork), or None."""
    try:
        url = subprocess.run(["git", "remote", "get-url", "origin"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    m = re.match(r"(?:git@github\.com:|https://github\.com/)(.+?)(?:\.git)?$", url)
    return f"https://github.com/{m.group(1)}" if m else url
