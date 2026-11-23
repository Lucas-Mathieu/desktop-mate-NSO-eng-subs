# dmsubs — English subtitles for Desktop Mate

Shows an English subtitle above the character, in sync with what she says.
Currently translated: **Ame-chan** (`wss_amechan.cb`) and **Chouten-chan** (`wss_choutenchan.cb`).

Want subtitles for another character? Nothing in the mod is specific to these two - see
[Adding other characters (fork guide)](#adding-other-characters-fork-guide).

## Installing

1. Download `dmsubs-vX.Y.Z.zip` from the [latest release](https://github.com/Lucas-Mathieu/desktop-mate-NSO-eng-subs/releases/latest).
2. Open the game folder: in Steam, right-click **Desktop Mate** → **Manage** → **Browse local files**
   (usually `C:\Program Files (x86)\Steam\steamapps\common\Desktop Mate`).
3. Extract the zip **into that folder**, so that `version.dll` sits next to `DesktopMate.exe`:
   ```
   Desktop Mate\
   ├── DesktopMate.exe
   ├── version.dll        <- from the zip
   └── dmsubs\            <- from the zip
   ```
4. Start Desktop Mate as usual. Subtitles appear above Ame-chan / Chouten-chan whenever they talk.

**Uninstall:** delete `version.dll` (and the `dmsubs` folder) from the game folder.

**Game updates:** nothing to do. The first time you start an updated game, the mod downloads
Unity's public debug info for the new build (~25 MB, needs internet once) and adapts itself.

**Not working?** Check `dmsubs\dmsubs.log`.

## How it works

- `version.dll` is a proxy DLL that Desktop Mate (Unity) loads at startup. It forwards the real
  `version.dll` functions and installs hooks in `UnityPlayer.dll`.
- It hooks Unity's native `AudioSource::Play` / `PlayOneShot` / `Stop`, so it knows **exactly which
  voice clip starts and when**. The clip name (e.g. `38_F2A`) plus the character
  (`Amechan01`, `ChouTenchan01`) is looked up in `dmsubs\subtitles.json`.
- The subtitle is drawn in a click-through window owned by the game window, positioned above the
  character's `Head` bone (projected with the game camera) every frame.
- Function addresses come from **Unity's public symbol server** for the exact `UnityPlayer.dll` build,
  so the game's encrypted bundles and obfuscated scripts are never touched.

## Building from source

Needs Windows, [Visual Studio or the Build Tools](https://visualstudio.microsoft.com/downloads/) with
the **Desktop development with C++** workload, and Python 3.

```
build.bat                      (produces mod\version.dll)
python tools\install.py        (copies it into the game; close Desktop Mate first)
python tools\package.py v1.2.0 (makes the release zip in dist\)
```

The tools find the game through your Steam libraries; if that fails pass `--game <folder>` or set the
`DMSUBS_GAME` environment variable. `python tools\install.py --uninstall` removes `version.dll` again.

## Game updates

`dmsubs\offsets.ini` caches where the hooked functions live in the installed `UnityPlayer.dll`.
When the game updates, the mod notices the new build, downloads Unity's public PDB for it from
`symbolserver.unity3d.com`, looks the functions up with Windows' dbghelp, rewrites `offsets.ini` and
deletes the PDB (`src/symbols.cpp`). `tools\gen_offsets.py` does the same from the command line,
for development.

## Settings — `<game>\dmsubs\dmsubs.ini`

| key | default | |
|---|---|---|
| `font`, `font_size`, `max_width` | Segoe UI, 26, 760 | text look / wrap width (px) |
| `background_alpha` | 150 | dark box behind the text, 0 = outline only |
| `show_japanese` | 0 | also show the original line, smaller, under the English |
| `show_untranslated_clip_names` | 0 | show `[clip]` for lines with no English (for translating) |
| `follow_character` | 1 | 0 = fixed at the bottom of the screen |
| `follow_smoothing`, `follow_dead_zone_px` | 0.35, 18 | easing time (s) and ignored head sway (px) |
| `head_offset_world`, `gap_px` | 0, 12 | nudge the position |
| `record_new_lines` | 0 (release), 1 (source) | save new, untranslated lines as WAV for `tools\learn.py` |
| `harvest_skip` | `Voi_,SE` | clip-name parts ignored by harvest |

## Adding / fixing translations

`<game>\dmsubs\subtitles.json` (reloaded live while the game runs):

```json
"Amechan01/38_F2A": {
  "jp": "会えない間ずっと寂しかったよ。また明日もピと一緒だね",
  "en": "I was so lonely while we were apart. We get to be together again tomorrow, P-chan.",
  "cues": [[0.0, "I was so lonely while we were apart."], [2.9, "We get to be together again tomorrow, P-chan."]]
}
```

`cues` (seconds from the start of the line) win over `en`; `en` wins over `en_auto` (machine translation).
`mod\dmsubs\subtitles.json` is the curated copy shipped by `install.py` (it merges, never deletes what
you learned in-game).

## Adding other characters (fork guide)

The DLL is generic: it subtitles **any** voice clip it finds in `subtitles.json`, keyed by
`<character id>/<clip name>`. Adding a character is mostly translation work, no C++ needed.

**0. Fork and set up**

1. Fork this repo on GitHub and clone your fork.
2. `build.bat`, then `python tools\install.py` (close the game first).
3. Optional, for automatic transcription (NVIDIA GPU recommended, CPU works but is slow):
   ```
   python -m venv .venv
   .venv\Scripts\pip install faster-whisper nvidia-cublas-cu12 "nvidia-cudnn-cu12>=9,<10" numpy
   ```
4. Your local game copy now has `record_new_lines=1` in `<game>\dmsubs\dmsubs.ini` (the release zip
   ships with it off).

**1. Select the character**

Pick it in the game, or with the game closed:
```
python tools\character.py              (lists installed character bundles, * = selected)
python tools\character.py aierutan     (selects aierutan.cb)
```

**2. Find its character id**

Start the game and let her talk once. `<game>\dmsubs\dmsubs.log` shows every clip she plays:
```
play Amechan01/38_F2A len=6.944 (no subtitle)
     ^^^^^^^^^ ^^^^^^
     character clip
```
The id comes from the model's `Eff_Cha_<id>` / `Eff_<id>` object, or the model's own name otherwise
(`Unity_CharacterId` in `src/unity.cpp` if you need to change that rule).

**3. Record her lines**

Press **Ctrl+Alt+Shift+H** with her on screen: every loaded clip without a subtitle is played once and
recorded to `<game>\dmsubs\clips\<id>\<clip>.wav` (press again to stop; she's chatty, it takes a
few minutes). Lines are also recorded whenever they play naturally, so some only show up later
(alarms, special events). Clip names containing `harvest_skip` parts (sound effects) are skipped.

**4. Transcribe**

Add her name and catchphrases to `tools\vocabulary.txt` (whisper mishears rare words), then:
```
.venv\Scripts\python tools\learn.py            (--watch keeps it running while you play)
```
Each line gets `jp`, timed `segments` and a rough machine translation `en_auto`, written to the game's
`subtitles.json`. The game shows `en_auto` right away, so you can already check timing in game.

**5. Translate**

```
python tools\subs.py pull      (copies the learned lines into mod\dmsubs\subtitles.json)
python tools\subs.py todo      (prints the lines that still need English)
python tools\subs.py status    (progress per character)
```
In `mod\dmsubs\subtitles.json`, write an `en` for each line (`en_auto` is often wrong - listen to the
WAV), and `cues` for long lines that should change mid-sentence, using the `segments` times as a
guide. Give the character a display name under `"characters"` (used in the release README):
```json
"characters": { "Amechan01": "Ame-chan", "NewChara01": "New-chan" },
```
Then `python tools\install.py` merges your translations into the game (it reloads them live).
You can also edit the game's copy directly while playing and copy the result back.

**6. Release**

Update the character list at the top of this README, commit, then `python tools\package.py vX.Y.Z` and
attach `dist\dmsubs-vX.Y.Z.zip` to a GitHub release on your fork. The zip's README points to your
fork automatically (taken from `git remote`). Never commit or ship `<game>\dmsubs\clips`: those are the
game's voice recordings.

PRs adding characters to this repo are welcome too.

## Layout

```
src/            the DLL: dmsubs.cpp (hooks, harvest), overlay.cpp (subtitle window),
                subtitles.cpp (JSON db), unity.cpp (head bone / camera projection),
                capture.cpp (WASAPI process-loopback recorder), symbols.cpp (self-update on game updates)
tools/          install.py, package.py, character.py (switch character), learn.py + vocabulary.txt
                (transcribe), subs.py (translation helper), gen_offsets.py (dev), common.py
mod/            what gets installed: version.dll + dmsubs/{offsets.ini,dmsubs.ini,subtitles.json}
third_party/    MinHook (BSD-2)
```
