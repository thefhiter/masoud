"""Masoud talks: keep this running and he speaks for himself.

    python tools/masoud_live.py                 # port found automatically
    python tools/masoud_live.py --port COM14

He greets you, then says a Derja line every time his mood changes, whether
he picked the mood himself or you sent one. Type a sentence (Arabic letters)
and press Enter to make him say it. Type a mood key alone (h s a u z l ? d n)
to set his face, and he will say that mood's line. Ctrl+C to stop.

Lines are rendered once with the Tunisian voice and cached in
%LOCALAPPDATA%\\masoud\\voice, so after the first run he answers instantly.
Needs: pip install edge-tts pyserial, and ffmpeg on PATH.
"""
import argparse
import hashlib
import os
import queue
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import masoud_say as ms  # noqa: E402

GREETING = "أسلامة! أنا مسعود. كل نهار، مسعود!"
# What he says for each mood, keyed by the name the robot prints on Serial.
LINES = {
    "Mrigel!": "باهي برشا! أنا فرحان.",
    "Mghboun": "آه... أنا مغبون شوية.",
    "Harissa!": "هريسة! ما تقلقنيش!",
    "Ya mimti!": "يا ميمتي! شنوة هذا؟",
    "Na3sen": "نعسان... خليني نرقد شوية.",
    "Mghrom": "نحبك برشا!",
    "Chkoun?!": "شكون؟ شكون غادي؟",
    "Dawkha! Bchwaya 3liya!": "دوخة! بالشوية عليا!",
}
CACHE = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "masoud" / "voice"


def voice_clip(text, voice, pitch):
    """8 kHz u8 audio for text, from the cache or freshly rendered."""
    key = hashlib.sha1(f"{voice}|{pitch}|{text}".encode()).hexdigest()[:16]
    path = CACHE / f"{key}.u8"
    if path.exists():
        return path.read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        mp3 = Path(tmp) / "say.mp3"
        ms.tts(text, voice, mp3)
        data = ms.to_u8(mp3, pitch)
    CACHE.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def speak(port, data, label):
    print(f"  says: {label}")
    try:
        ms.stream(port, data)
    except SystemExit as e:              # stream() exits on protocol trouble; keep living
        print(f"  (could not speak: {e})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port (found automatically otherwise)")
    ap.add_argument("--voice", default=ms.VOICE)
    ap.add_argument("--pitch", type=float, default=1.12)
    ap.add_argument("--quiet-gap", type=float, default=4.0, help="seconds of silence before he comments on a new mood")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdin.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    print("preparing his voice lines...")
    clips = {name: voice_clip(text, args.voice, args.pitch) for name, text in LINES.items()}
    hello = voice_clip(GREETING, args.voice, args.pitch)

    port = ms.find_masoud(args.port)
    print(f"Masoud found on {port.port}. Type a sentence and press Enter to make him say it.")
    speak(port, hello, GREETING)

    typed = queue.Queue()
    threading.Thread(target=lambda: [typed.put(l.strip()) for l in sys.stdin], daemon=True).start()

    buf, last_spoke = b"", time.time()
    try:
        while True:
            buf += port.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "ignore").strip()
                if line.startswith("[") and line.endswith("]"):
                    mood = line[1:-1]
                    text = LINES.get(mood)
                    if text and time.time() - last_spoke > args.quiet_gap:
                        speak(port, clips[mood], f"[{mood}] {text}")
                        last_spoke, buf = time.time(), b""
            try:
                msg = typed.get_nowait()
            except queue.Empty:
                continue
            if not msg:
                continue
            if len(msg) == 1:                # a mood key: set his face, he will comment
                port.write(msg.encode())
                last_spoke = 0
                continue
            speak(port, voice_clip(msg, args.voice, args.pitch), msg)
            last_spoke, buf = time.time(), b""
    except KeyboardInterrupt:
        print("\nBislema!")
    finally:
        port.close()


if __name__ == "__main__":
    main()
