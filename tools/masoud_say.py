"""Make Masoud talk: text -> Tunisian voice -> his speakers.

    python tools/masoud_say.py "أسلامة! أنا مسعود"              # speak, port found automatically
    python tools/masoud_say.py "مريڨل!" --mood h                # set his face first (h = happy)
    python tools/masoud_say.py --file song.mp3                  # any audio file
    python tools/masoud_say.py "أسلامة" --preview hello.wav     # no robot: write what he'd sound like

The voice is Microsoft's Tunisian Arabic neural voice (edge-tts, ar-TN-HediNeural),
pitched up a little so it sounds like a small robot, band-limited for the tiny
speakers, and sent as 8-bit 8 kHz mono. Write the text in Arabic letters; the
voice reads Latin-letter Derja badly.

Streaming uses the same credit flow control as the arduino-sound-player project:
send 0x01, wait for OK:START, then send exactly CHUNK bytes per 0x06 credit.
Needs: pip install edge-tts pyserial, and ffmpeg on PATH.
"""
import argparse
import asyncio
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

import serial
from serial.tools import list_ports

RATE, CHUNK, BAUD = 8000, 32, 115200
START, CREDIT, WHO = b"\x01", b"\x06", b"\x05"
VOICE = "ar-TN-HediNeural"


def tts(text, voice, out):
    import edge_tts
    asyncio.run(edge_tts.Communicate(text, voice).save(str(out)))


def to_u8(path, pitch):
    """Decode anything ffmpeg reads into 8 kHz unsigned 8-bit mono for his speakers."""
    filters = []
    if pitch != 1.0:   # higher voice, same speed
        filters += [f"asetrate=24000*{pitch}", "aresample=24000", f"atempo={1 / pitch:.4f}"]
    filters += ["highpass=f=300", "lowpass=f=3400",   # what 28 mm speakers can actually play
                "dynaudnorm=f=150:g=15", "volume=0.9"]
    cmd = ["ffmpeg", "-loglevel", "error", "-i", str(path), "-af", ",".join(filters),
           "-ar", str(RATE), "-ac", "1", "-f", "u8", "-"]
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    fade = min(len(data), 160)                         # 20 ms fades: no click at either end
    out = bytearray(data)
    for i in range(fade):
        k = i / fade
        out[i] = int(128 + (out[i] - 128) * k)
        out[-1 - i] = int(128 + (out[-1 - i] - 128) * k)
    return bytes(out)


def write_wav(data, path):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)        # 8-bit WAV is unsigned, same as the robot
        w.setframerate(RATE)
        w.writeframes(data)


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, BAUD, 0.05
    s.dtr = False                # don't pulse reset: Masoud keeps his face and mood
    s.rts = False
    s.open()
    return s


def find_masoud(preferred=None):
    ports = [preferred] if preferred else [p.device for p in list_ports.comports()]
    for name in ports:
        try:
            s = open_port(name)
        except (serial.SerialException, OSError):
            continue
        s.reset_input_buffer()
        s.write(WHO)
        deadline = time.time() + 1.2
        buf = b""
        while time.time() < deadline:
            buf += s.read(64)
            if b"MASOUD" in buf:
                return s
        if preferred:            # a fresh reset: wait out the bootloader and the splash
            time.sleep(4)
            s.write(WHO)
            if b"MASOUD" in s.read(256):
                return s
        s.close()
    sys.exit("Masoud not found. Is the Uno plugged in, and the Serial Monitor closed?")


def read_line(s, want, timeout):
    deadline, buf = time.time() + timeout, b""
    while time.time() < deadline:
        buf += s.read(64)
        if want in buf:
            return buf
    return buf


def stream(s, data):
    s.reset_input_buffer()
    s.write(START)
    got = read_line(s, b"OK:START", 2.0)
    if b"OK:START" not in got:
        sys.exit("Masoud did not start the stream (busy streaming already?)")
    credits = got.split(b"OK:START", 1)[1].count(CREDIT)
    pos, t0 = 0, time.time()
    while pos < len(data):
        credits += s.read(max(1, s.in_waiting)).count(CREDIT)
        while credits and pos < len(data):
            s.write(data[pos:pos + CHUNK])
            pos += CHUNK
            credits -= 1
    tail = read_line(s, b"OK:IDLE", 3.0)
    under = tail.split(b"OK:IDLE:", 1)[1].split(b"\r")[0].decode(errors="ignore") if b"OK:IDLE:" in tail else "?"
    print(f"spoke {len(data) / RATE:.1f} s in {time.time() - t0:.1f} s, underruns: {under}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="?", help="what Masoud says (Arabic letters)")
    ap.add_argument("--file", help="play an audio file instead of speaking text")
    ap.add_argument("--mood", help="face to show first: n h s a u z l ? d")
    ap.add_argument("--voice", default=VOICE, help="edge-tts voice, e.g. ar-TN-ReemNeural")
    ap.add_argument("--pitch", type=float, default=1.12, help="1.0 = natural voice")
    ap.add_argument("--port", help="COM port (found automatically otherwise)")
    ap.add_argument("--preview", help="write a WAV of what he'd say instead of sending it")
    args = ap.parse_args()
    if not args.text and not args.file:
        ap.error("give some text or --file")

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(args.file) if args.file else Path(tmp) / "say.mp3"
        if not args.file:
            tts(args.text, args.voice, src)
        data = to_u8(src, 1.0 if args.file else args.pitch)

    if args.preview:
        write_wav(data, args.preview)
        print(f"wrote {args.preview}: {len(data) / RATE:.1f} s at {RATE} Hz, 8-bit")
        return
    s = find_masoud(args.port)
    try:
        if args.mood:
            s.write(args.mood[:1].encode())
            time.sleep(0.15)
        stream(s, data)
    finally:
        s.close()


if __name__ == "__main__":
    main()
