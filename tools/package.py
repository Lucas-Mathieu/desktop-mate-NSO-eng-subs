"""Build the release zip: dist/dmsubs-<version>.zip

    python tools/package.py v1.0.0

Contents (extract into the Desktop Mate folder):
    version.dll
    dmsubs/offsets.ini, dmsubs/dmsubs.ini (recording off), dmsubs/subtitles.json
    dmsubs/LICENSE-MinHook.txt, dmsubs/README.txt
"""
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOD = ROOT / "mod"

PLAYER_README = """dmsubs - English subtitles for Desktop Mate (Ame-chan & Chouten-chan)
https://github.com/Lucas-Mathieu/desktop-mate-NSO-eng-subs

Install: extract the zip into the Desktop Mate folder so version.dll sits next to DesktopMate.exe
(Steam: right-click Desktop Mate > Manage > Browse local files).
Uninstall: delete version.dll and the dmsubs folder.
Settings: dmsubs\\dmsubs.ini. Problems: see dmsubs\\dmsubs.log.
"""


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "dev"
    dll = MOD / "version.dll"
    if not dll.exists():
        sys.exit("mod\\version.dll missing - run build.bat first")
    ini = (MOD / "dmsubs" / "dmsubs.ini").read_text(encoding="utf-8")
    ini = re.sub(r"(?m)^record_new_lines=.*$", "record_new_lines=0", ini)
    ini = re.sub(r"(?m)^show_untranslated_clip_names=.*$", "show_untranslated_clip_names=0", ini)
    out = ROOT / "dist" / f"dmsubs-{version}.zip"
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, "version.dll")
        z.write(MOD / "dmsubs" / "offsets.ini", "dmsubs/offsets.ini")
        z.write(MOD / "dmsubs" / "subtitles.json", "dmsubs/subtitles.json")
        z.writestr("dmsubs/dmsubs.ini", ini)
        z.write(ROOT / "third_party" / "minhook" / "LICENSE.txt", "dmsubs/LICENSE-MinHook.txt")
        z.writestr("dmsubs/README.txt", PLAYER_README)
    print(out)


if __name__ == "__main__":
    main()
