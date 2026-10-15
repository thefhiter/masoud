"""Masoud always talks: run it once, or install it to start with Windows.

    python tools/masoud_live.py                # talk now (Ctrl+C to stop)
    python tools/masoud_live.py --install      # start with Windows, in the background, from now on
    python tools/masoud_live.py --stop         # stop the background one (e.g. to upload a sketch)
    python tools/masoud_live.py --uninstall    # no more autostart

While it runs he greets you, says a Derja line on every mood change (his
own or one you send), and when it has been quiet for a while he says
something on his own: water, coffee, "why so quiet?", good morning or good
night by the clock. In the console you can also type a sentence (Arabic
letters) to make him say it, or a mood key (h s a u z l ? d n).

In the background (--wait, which --install uses) it waits for Masoud to be
plugged in and reconnects if he is unplugged. It holds his USB port while it
runs, so stop it before uploading a sketch. Log: %LOCALAPPDATA%\\masoud\\live.log
Lines are rendered once with the Tunisian voice and cached in
%LOCALAPPDATA%\\masoud\\voice. Needs: pip install edge-tts pyserial, ffmpeg on PATH.
"""
import argparse
import ctypes
import hashlib
import os
import queue
import random
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

sys.path.insert(0, str(Path(__file__).resolve().parent))
import masoud_say as ms  # noqa: E402

HOME = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "masoud"
CACHE, PIDFILE, LOG = HOME / "voice", HOME / "live.pid", HOME / "live.log"
STARTUP = Path(os.environ.get("APPDATA", "")) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup" / "Masoud.lnk"

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
# What he says by himself when it has been quiet.
CHATTER = [
    "ما تنساش تشرب الماء!",
    "شنوة أحوالك اليوم؟",
    "أنا هنا، كان تحتاجني قولي.",
    "تعرف؟ أنا روبو صغير، أما قلبي كبير.",
    "يزي من الخدمة، ارتاح شوية.",
    "شبيك ساكت؟",
    "نحب نسمع شوية موسيقى!",
    "تحب قهوة وإلا تاي؟",
    "باهي برشا، نكملو؟",
    "حكيلي حاجة!",
    "كل نهار، مسعود!",
]
CLOCK = [  # (from hour, to hour, line)
    (5, 12, "صباح الخير! نهارك مسعود."),
    (12, 18, "العشية زينة، نكملو الخدمة؟"),
    (18, 23, "مساء الخير! كيفاش كان نهارك؟"),
    (23, 29, "الوقت فات... برا ارقد، تصبح على خير."),
]
ARDUINO_VIDS = {0x2341, 0x2A03, 0x1A86, 0x0403, 0x10C4}   # Arduino, clones' CH340 / FTDI / CP210x


def say_print(*a):
    print(*a, flush=True)


def voice_clip(text, voice, pitch):
    """8 kHz u8 audio for text, from the cache or freshly rendered."""
    key = hashlib.sha1(f"v3|{voice}|{pitch}|{text}".encode()).hexdigest()[:16]   # v3: louder processing
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
    say_print(f"{time.strftime('%H:%M:%S')}  says: {label}")
    try:
        ms.stream(port, data)
    except SystemExit as e:              # stream() exits on protocol trouble; keep living
        say_print(f"  (could not speak: {e})")


def clock_line():
    h = time.localtime().tm_hour
    for a, b, line in CLOCK:
        if a <= h < b or a <= h + 24 < b:
            return line
    return CHATTER[0]


def pick_chatter(recent):
    if random.random() < 0.25 and clock_line() not in recent:
        return clock_line()
    choices = [c for c in CHATTER if c not in recent] or CHATTER
    return random.choice(choices)


# ---- finding Masoud ---------------------------------------------------------

def candidate_ports(preferred):
    if preferred:
        return [preferred]
    ports = list_ports.comports()
    usb = [p.device for p in ports if p.vid in ARDUINO_VIDS]
    other = [p.device for p in ports if p.vid not in ARDUINO_VIDS and "bluetooth" not in (p.description or "").lower()]
    return usb + other


def connect(preferred):
    for name in candidate_ports(preferred):
        try:
            return ms.find_masoud(name)
        except (SystemExit, serial.SerialException, OSError):
            continue
    return None


# ---- one process at a time, autostart -----------------------------------------

def pid_alive(pid):
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)   # query limited information
    if not h:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return code.value == 259                                     # STILL_ACTIVE


def running_pid():
    try:
        pid = int(PIDFILE.read_text())
        return pid if pid != os.getpid() and pid_alive(pid) else None
    except (OSError, ValueError):
        return None


def stop():
    pid = running_pid()
    if not pid:
        say_print("Masoud's talker is not running.")
        return
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    say_print(f"Stopped (pid {pid}). His USB port is free now.")


def install():
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    script = Path(__file__).resolve()
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{STARTUP}');"
          f"$s.TargetPath='{pythonw}';$s.Arguments='\"{script}\" --wait';"
          f"$s.WorkingDirectory='{script.parent}';$s.Description='Masoud talks';$s.Save()")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)
    say_print(f"Installed: Masoud will talk whenever he is plugged in, from every Windows start.\n  {STARTUP}")
    if not running_pid():
        subprocess.Popen([str(pythonw), str(script), "--wait"], cwd=str(script.parent),
                         creationflags=0x00000008 | 0x00000200)   # detached, own process group
        say_print("Started in the background now.")


def uninstall():
    stop()
    try:
        STARTUP.unlink()
        say_print("Autostart removed.")
    except FileNotFoundError:
        say_print("No autostart was installed.")


# ---- the talking loop ----------------------------------------------------------

def live(args):
    HOME.mkdir(parents=True, exist_ok=True)
    if running_pid():
        say_print("Masoud's talker is already running (use --stop first).")
        return
    PIDFILE.write_text(str(os.getpid()))
    say_print("preparing his voice lines...")
    moods = {name: voice_clip(text, args.voice, args.pitch) for name, text in LINES.items()}
    hello = voice_clip(GREETING, args.voice, args.pitch)

    typed = queue.Queue()
    if sys.stdin and sys.stdin.isatty():
        threading.Thread(target=lambda: [typed.put(l.strip()) for l in sys.stdin], daemon=True).start()

    recent = []
    while True:
        port = connect(args.port)
        if not port:
            if not args.wait:
                say_print("Masoud not found. Is he plugged in, and the Serial Monitor closed?")
                return
            time.sleep(5)
            continue
        say_print(f"Masoud found on {port.port}." + (" Type a sentence to make him say it." if sys.stdin and sys.stdin.isatty() else ""))
        speak(port, hello, GREETING)
        buf, last_spoke = b"", time.time()
        next_chat = time.time() + args.every * random.uniform(0.7, 1.3)
        try:
            while True:
                buf += port.read(256)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "ignore").strip()
                    if line.startswith("[") and line.endswith("]"):
                        mood = line[1:-1]
                        if mood in moods and time.time() - last_spoke > args.quiet_gap:
                            speak(port, moods[mood], f"[{mood}] {LINES[mood]}")
                            last_spoke, buf = time.time(), b""
                            next_chat = last_spoke + args.every * random.uniform(0.7, 1.3)
                if args.every > 0 and time.time() > next_chat:
                    text = pick_chatter(recent)
                    recent = (recent + [text])[-4:]
                    speak(port, voice_clip(text, args.voice, args.pitch), text)
                    last_spoke, buf = time.time(), b""
                    next_chat = last_spoke + args.every * random.uniform(0.7, 1.3)
                try:
                    msg = typed.get_nowait()
                except queue.Empty:
                    continue
                if not msg:
                    continue
                if len(msg) == 1:            # a mood key: set his face, he will comment
                    port.write(msg.encode())
                    last_spoke = 0
                    continue
                speak(port, voice_clip(msg, args.voice, args.pitch), msg)
                last_spoke, buf = time.time(), b""
        except (serial.SerialException, OSError):
            say_print("Masoud was unplugged." + (" Waiting for him..." if args.wait else ""))
            try:
                port.close()
            except Exception:
                pass
            if not args.wait:
                return
            time.sleep(3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port (found automatically otherwise)")
    ap.add_argument("--voice", default=ms.VOICE)
    ap.add_argument("--pitch", type=float, default=1.12)
    ap.add_argument("--every", type=float, default=30, help="seconds of quiet before he talks on his own (0 = never)")
    ap.add_argument("--quiet-gap", type=float, default=2.0, help="seconds after speaking before he comments on a mood")
    ap.add_argument("--wait", action="store_true", help="wait for Masoud and reconnect after unplugging")
    ap.add_argument("--install", action="store_true", help="start with Windows, in the background")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--stop", action="store_true", help="stop the background talker")
    args = ap.parse_args()

    if sys.stdout is None or not hasattr(sys.stdout, "write"):   # pythonw: no console, log to a file
        HOME.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = open(LOG, "a", encoding="utf-8", buffering=1)
    else:
        try:
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stdin.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    if args.stop:
        stop()
    elif args.install:
        install()
    elif args.uninstall:
        uninstall()
    else:
        try:
            live(args)
        except KeyboardInterrupt:
            say_print("\nBislema!")
        finally:
            try:
                if PIDFILE.exists() and PIDFILE.read_text().strip() == str(os.getpid()):
                    PIDFILE.unlink()
            except OSError:
                pass


if __name__ == "__main__":
    main()
