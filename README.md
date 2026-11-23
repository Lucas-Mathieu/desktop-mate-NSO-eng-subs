# dmsubs — English subtitles for Desktop Mate

Shows an English subtitle above the character, in sync with what she says.
Currently translated: **Ame-chan** (`wss_amechan.cb`) and **Chouten-chan** (`wss_choutenchan.cb`).

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

## Install

```
build.bat                      (needs Visual Studio C++ tools; produces mod\version.dll)
python tools\install.py        (close Desktop Mate first)
```

Uninstall: `python tools\install.py --uninstall` (or delete `version.dll` from the game folder).

## After a game update

If the game updates `UnityPlayer.dll`, the mod disables itself (see `dmsubs\dmsubs.log`) instead of
crashing. Regenerate the addresses and reinstall:

```
python tools\gen_offsets.py
python tools\install.py
```

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
| `record_new_lines` | 1 | save new, untranslated lines as WAV for `tools\learn.py` |
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

### New characters / lines

1. With the character on screen, press **Ctrl+Alt+Shift+H** (harvest): every loaded clip that has no
   subtitle yet is played once and recorded to `dmsubs\clips\<character>\<clip>.wav`
   (lines are also recorded whenever they play naturally).
2. `.venv\Scripts\python tools\learn.py` — transcribes them with faster-whisper large-v3 (GPU) and adds
   `jp` + `en_auto` + timed `segments`. `--watch` keeps it running while you play.
3. Write proper `en` / `cues` for the new entries.

Python setup for `learn.py`: `python -m venv .venv && .venv\Scripts\pip install faster-whisper nvidia-cublas-cu12 "nvidia-cudnn-cu12>=9,<10" numpy`

## Layout

```
src/            the DLL: dmsubs.cpp (hooks, harvest), overlay.cpp (subtitle window),
                subtitles.cpp (JSON db), unity.cpp (head bone / camera projection),
                capture.cpp (WASAPI process-loopback recorder)
tools/          gen_offsets.py, learn.py, install.py
mod/            what gets installed: version.dll + dmsubs/{offsets.ini,dmsubs.ini,subtitles.json}
third_party/    MinHook (BSD-2)
```
