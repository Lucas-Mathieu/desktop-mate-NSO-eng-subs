"""Transcribe recorded voice lines and fill dmsubs/subtitles.json.

The mod saves every voice line that has no subtitle yet to
    <game>/dmsubs/clips/<character>/<clip>.wav
This tool transcribes each new WAV with faster-whisper (Japanese), adds a
machine translation ("en_auto") and the timed Japanese segments, and writes
them into <game>/dmsubs/subtitles.json. The game picks the change up live.

Curated fields ("en", "cues") are never touched, and always win over "en_auto".

    .venv\\Scripts\\python tools\\learn.py            # process new clips once
    .venv\\Scripts\\python tools\\learn.py --watch    # keep running while you play
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

DEFAULT_GAME = Path(r"D:\SteamLibrary\steamapps\common\Desktop Mate")
PRE_ROLL = 0.15  # the mod starts each WAV this many seconds before the clip

# Vocabulary hint for whisper (NEEDY STREAMER OVERLOAD terms it otherwise mishears,
# e.g. ピ -> "ピュア", 超てんちゃん -> "頂点ちゃん").
PROMPT = "ピ、超てんちゃん、アメちゃん、インターネットエンジェル、オタク、配信、エゴサ、デスクトップ。"

# Whisper likes to invent these on silence / very short clips.
HALLUCINATIONS = ("ご視聴ありがとうございました", "チャンネル登録", "お疲れ様でした。", "字幕", "Thank you for watching")


def add_cuda_dll_dirs():
    if os.name != "nt":
        return
    import site
    for sp in site.getsitepackages():
        nv = Path(sp) / "nvidia"
        if nv.is_dir():
            for b in nv.glob("*/bin"):
                os.add_dll_directory(str(b))
                os.environ["PATH"] = str(b) + os.pathsep + os.environ["PATH"]


def load_db(path):
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8-sig"))
    return {"lines": {}}


def save_db(path, db):
    out = dict(db, lines=dict(sorted(db["lines"].items())))  # sorted copy; never rebind db["lines"]
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for attempt in range(20):  # the game may be reading it right now
        try:
            os.replace(tmp, path)
            break
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.1)


def clean(text):
    t = text.strip()
    return "" if any(h in t for h in HALLUCINATIONS) else t


def load_wav16k(path):
    """Mono 16-bit WAV (the mod writes 48 kHz) -> float32 @ 16 kHz."""
    import wave
    import numpy as np
    with wave.open(str(path)) as w:
        rate, n = w.getframerate(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float32) / 32768.0
    if rate == 16000:
        return x
    if rate % 16000:
        raise ValueError(f"unsupported sample rate {rate}")
    k = rate // 16000
    t = np.arange(-48, 49)
    h = np.sinc(t / k) * np.hamming(len(t))  # low-pass at the new Nyquist
    h /= h.sum()
    return np.convolve(x, h, mode="same")[::k].astype(np.float32)


def process(model, wav, entry):
    audio = load_wav16k(wav)
    speech, info = [], None
    for prompt in (PROMPT, None):  # the vocabulary prompt occasionally makes whisper return nothing
        segs, info = model.transcribe(audio, language="ja", beam_size=5, condition_on_previous_text=False,
                                      vad_filter=False, temperature=0.0, initial_prompt=prompt)
        speech = [s for s in segs if s.no_speech_prob < 0.6 and clean(s.text)]
        if speech:
            break
    if not speech:
        entry.update({"jp": "", "nospeech": True})
        return entry
    entry["jp"] = "".join(clean(s.text) for s in speech)
    entry["segments"] = [[round(max(0.0, s.start - PRE_ROLL), 2), round(max(0.0, s.end - PRE_ROLL), 2), clean(s.text)]
                         for s in speech]
    tr, _ = model.transcribe(audio, language="ja", task="translate", beam_size=5,
                             condition_on_previous_text=False, temperature=0.0)
    entry["en_auto"] = " ".join(clean(s.text) for s in tr if clean(s.text)).strip()
    entry["len"] = round(info.duration - PRE_ROLL - 0.5, 2)
    entry.pop("nospeech", None)
    return entry


def run_once(model, dmsubs, retranscribe=False):
    db_path = dmsubs / "subtitles.json"
    db = load_db(db_path)
    lines = db.setdefault("lines", {})
    todo = []
    for wav in sorted((dmsubs / "clips").glob("*/*.wav")):
        key = f"{wav.parent.name}/{wav.stem}"
        e = lines.get(key, {})
        if retranscribe or ("jp" not in e):
            todo.append((key, wav))
    for i, (key, wav) in enumerate(todo, 1):
        e = lines.get(key, {})
        t = time.time()
        process(model, wav, e)
        lines[key] = e
        print(f"[{i}/{len(todo)}] {key} ({time.time() - t:.1f}s)  {e.get('jp', '')}  ->  {e.get('en_auto', '')}",
              flush=True)
        save_db(db_path, db)  # save as we go so the game shows lines immediately
    return len(todo)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--model", default="large-v3")
    ap.add_argument("--watch", action="store_true", help="keep checking for new clips")
    ap.add_argument("--retranscribe", action="store_true", help="redo clips that already have a transcript")
    args = ap.parse_args()
    dmsubs = args.game / "dmsubs"
    if not dmsubs.is_dir():
        sys.exit(f"{dmsubs} not found - is the mod installed?")

    add_cuda_dll_dirs()
    from faster_whisper import WhisperModel
    try:
        model = WhisperModel(args.model, device="cuda", compute_type="float16")
    except Exception as ex:  # no usable GPU
        print("CUDA unavailable, using CPU (slow):", ex)
        model = WhisperModel(args.model, device="cpu", compute_type="int8")

    n = run_once(model, dmsubs, args.retranscribe)
    print(f"{n} clip(s) processed")
    while args.watch:
        time.sleep(3)
        run_once(model, dmsubs)


if __name__ == "__main__":
    main()
