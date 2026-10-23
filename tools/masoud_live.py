"""Masoud lives: he talks, and he answers your messages with his AI mind.

    python tools/masoud_live.py                # run now; chat in this console too (Ctrl+C to stop)
    python tools/masoud_live.py --install      # start with Windows, in the background, from now on
    python tools/masoud_live.py --stop         # stop the background one (e.g. to upload a sketch)
    python tools/masoud_live.py --uninstall    # no more autostart

Chat with him at http://127.0.0.1:8766 (the "Chat with Masoud" shortcut on the
Desktop): you type, his mind (masoud_mind.py) answers in your language (English, French or Arabic), his face
takes the mood of the answer and he says it out loud. In "Say it" mode, or
with a message that starts with "!", he says your words exactly. In a console
you can type to him too; when the background one is already running, the
console chats through it.

On his own he greets you, says a line on every mood change, and when
it has been quiet for a while he says something: a question, a fun fact, a joke,
a remark on the time of day, in the language he last spoke. His AI mind writes
these lines in batches (knowing the time and what you talked about) and they are
voiced ahead of time, so they come out at once; the fixed lines below fill in
while it is offline or on Pet.

In the background (--wait, which --install uses) it waits for Masoud to be
plugged in and reconnects if he is unplugged. It holds his USB port while it
runs, so stop it before uploading a sketch. Log: %LOCALAPPDATA%\\masoud\\live.log
Lines are rendered once with his voice and cached in
%LOCALAPPDATA%\\masoud\\voice. Needs: pip install edge-tts pyserial, ffmpeg on PATH.
"""
import argparse
import ctypes
import hashlib
import json
import os
import queue
import random
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import serial
from serial.tools import list_ports

sys.path.insert(0, str(Path(__file__).resolve().parent))
import masoud_mind as mm  # noqa: E402
import masoud_say as ms  # noqa: E402

HOME = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "masoud"
CACHE, PIDFILE, LOG = HOME / "voice", HOME / "live.pid", HOME / "live.log"
STARTUP = Path(os.environ.get("APPDATA", "")) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup" / "Masoud.lnk"
CHAT_PORT = 8766
CHAT_URL = f"http://127.0.0.1:{CHAT_PORT}/"
PAGE = Path(__file__).with_name("masoud_chat.html")
BRAND = Path(__file__).resolve().parent.parent / "brand"

MOOD_KEYS = "nhsauzl?d"
# The names the robot prints on Serial, in MOOD_KEYS order (the firmware's own names).
MOOD_NAMES = ["Labes", "Mrigel!", "Mghboun", "Harissa!", "Ya mimti!", "Na3sen", "Mghrom", "Chkoun?!", "Dawkha! Bchwaya 3liya!"]
GREETING = {
    "en": "Hello! I'm Masoud, your desk buddy. Every day is a lucky day!",
    "fr": "Salut ! Moi c'est Masoud, ton copain de bureau. Chaque jour est un jour de chance !",
    "ar": "مرحبا! أنا مسعود، صديقك على المكتب. كل يوم هو يوم سعيد!",
}
OFFLINE = {
    "en": "My brain is offline right now... try again in a moment.",
    "fr": "Mon cerveau est en panne... réessaie dans un instant.",
    "ar": "عقلي لا يعمل الآن... حاول بعد قليل.",
}
# What he says for each mood, keyed by the name the robot prints on Serial.
LINES = {
    "Mrigel!": {"en": "Yay! I'm so happy!", "fr": "Youpi ! Je suis trop content !", "ar": "رائع! أنا سعيد جدا!"},
    "Mghboun": {"en": "Oh... I feel a little sad.", "fr": "Oh... je suis un peu triste.", "ar": "آه... أشعر ببعض الحزن."},
    "Harissa!": {"en": "Grr! Don't make me angry!", "fr": "Grr ! Ne m'énerve pas !", "ar": "لا تغضبني!"},
    "Ya mimti!": {"en": "Whoa! What was that?", "fr": "Oh là là ! C'était quoi, ça ?", "ar": "يا إلهي! ما هذا؟"},
    "Na3sen": {"en": "So sleepy... let me nap a little.", "fr": "J'ai sommeil... laisse-moi faire une petite sieste.",
               "ar": "أنا نعسان... دعني أغفو قليلا."},
    "Mghrom": {"en": "I love you so much!", "fr": "Je t'adore !", "ar": "أحبك كثيرا!"},
    "Chkoun?!": {"en": "Hmm, who's there?", "fr": "Hmm, qui est là ?", "ar": "همم، من هناك؟"},
    "Dawkha! Bchwaya 3liya!": {"en": "Whoa, I'm dizzy! Easy on me!", "fr": "Oh, j'ai la tête qui tourne ! Doucement !",
                               "ar": "أشعر بالدوار! على مهلك!"},
}
# What he says by himself when it has been quiet.
CHATTER = {
    "en": ["Don't forget to drink some water!", "How's your day going?", "I'm right here if you need me.",
           "Did you know? I'm a small robot with a big heart.", "Time for a little break, maybe?", "Why so quiet?",
           "I'd love some music right now!", "Tell me something fun!", "Every day is a lucky day!"],
    "fr": ["N'oublie pas de boire de l'eau !", "Comment se passe ta journée ?", "Je suis là si tu as besoin de moi.",
           "Tu sais quoi ? Je suis un petit robot avec un grand cœur.", "Et si on faisait une petite pause ?",
           "Pourquoi tu ne dis rien ?", "J'écouterais bien un peu de musique !", "Raconte-moi un truc marrant !",
           "Chaque jour est un jour de chance !"],
    "ar": ["لا تنس أن تشرب الماء!", "كيف يمضي يومك؟", "أنا هنا إن احتجت إلي.", "هل تعلم؟ أنا روبوت صغير بقلب كبير.",
           "ما رأيك في استراحة قصيرة؟", "لماذا أنت صامت؟", "أود سماع بعض الموسيقى الآن!", "أخبرني شيئا ممتعا!",
           "كل يوم هو يوم سعيد!"],
}
CLOCK = {  # (from hour, to hour, line)
    "en": [(5, 12, "Good morning! Have a lucky day."), (12, 18, "Good afternoon! Shall we keep going?"),
           (18, 23, "Good evening! How was your day?"), (23, 29, "It's late... time to sleep. Good night!")],
    "fr": [(5, 12, "Bonjour ! Passe une belle journée."), (12, 18, "Bon après-midi ! On continue ?"),
           (18, 23, "Bonsoir ! Ta journée s'est bien passée ?"), (23, 29, "Il est tard... au lit. Bonne nuit !")],
    "ar": [(5, 12, "صباح الخير! أتمنى لك يوما سعيدا."), (12, 18, "مساء الخير! هل نكمل العمل؟"),
           (18, 23, "مساء الخير! كيف كان يومك؟"), (23, 29, "لقد تأخر الوقت... حان وقت النوم. تصبح على خير!")],
}
ARDUINO_VIDS = {0x2341, 0x2A03, 0x1A86, 0x0403, 0x10C4}   # Arduino, clones' CH340 / FTDI / CP210x


def say_print(*a):
    print(*a, flush=True)


def speakable(text):
    """Text as the voice should read it: "Masoud" in Latin letters comes out as "Mazou" in French
    and "a sued" in English (checked with Whisper), "Massoud" is said right in both."""
    return re.sub(r"\bMasoud\b", "Massoud", text, flags=re.I)


def voice_clip(text, voice, pitch, cache=True):
    """8 kHz u8 audio for text, from the cache or freshly rendered (and kept only with cache:
    his AI-written lines are said once, so they would just pile up)."""
    text = speakable(text)
    key = hashlib.sha1(f"v3|{voice}|{pitch}|{text}".encode()).hexdigest()[:16]   # v3: louder processing
    path = CACHE / f"{key}.u8"
    if path.exists():
        return path.read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        mp3 = Path(tmp) / "say.mp3"
        ms.tts(text, voice, mp3)
        data = ms.to_u8(mp3, pitch)
    if cache:
        CACHE.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return data


def clock_line(lang):
    h = time.localtime().tm_hour
    for a, b, line in CLOCK[lang]:
        if a <= h < b or a <= h + 24 < b:
            return line
    return CHATTER[lang][0]


def pick_chatter(recent, lang):
    if random.random() < 0.25 and clock_line(lang) not in recent:
        return clock_line(lang)
    choices = [c for c in CHATTER[lang] if c not in recent] or CHATTER[lang]
    return random.choice(choices)


class Muse:
    """His own lines, written by his AI mind in batches (one line per mood + quiet-room
    chatter) and voiced ahead of time, so a mood comment comes out at once."""
    PER_KEY = 3                   # lines kept per mood / for chatter
    GAP, BACKOFF = 40, 180        # seconds between batches, and after a failed one

    def __init__(self, hub, args):
        self.hub, self.args = hub, args
        self.ready = []           # [(lang, key, mood, text, clip)]
        self.lately = []          # what he said on his own lately: the mind must not repeat it
        self.lock = threading.Lock()
        self.next_batch = 0.0
        threading.Thread(target=self.worker, daemon=True).start()

    def take(self, key, lang):
        """-> (mood, text, clip) of a ready line for this mood key (or "c"), else None."""
        with self.lock:
            self.ready = [r for r in self.ready if r[0] == lang]    # he changed language: old ones go
            for i, r in enumerate(self.ready):
                if r[1] == key:
                    return self.ready.pop(i)[2:]
        return None

    def said(self, text):
        self.lately = (self.lately + [text])[-16:]

    def low(self, lang):
        with self.lock:
            mine = [r[1] for r in self.ready if r[0] == lang]
        return mine.count("c") < 2 or not set(MOOD_KEYS[1:]) <= set(mine)   # every speaking mood has a line

    def worker(self):
        while True:
            time.sleep(2)
            hub, lang = self.hub, self.hub.lang
            if not hub.port or hub.mind.choice == "pet" or time.time() < self.next_batch or not self.low(lang):
                continue
            self.next_batch = time.time() + self.GAP
            try:
                lines, used = hub.mind.muse(lang, self.lately)
            except mm.MindError as e:
                say_print(f"  (his own lines stay the fixed ones for now: {e})")
                self.next_batch = time.time() + self.BACKOFF
                continue
            say_print(f"  {mm.LABELS.get(used, used)} wrote {len(lines)} new lines for him ({lang})")
            for key, mood, text in lines:
                hub.quiet()
                if hub.lang != lang:
                    break
                try:
                    clip = voice_clip(text, self.args.voice, self.args.pitch, cache=False)
                except Exception:
                    break                 # no voice now (internet?): try again with the next batch
                with self.lock:
                    self.ready.append((lang, key, mood, text, clip))
                    same = [r for r in self.ready if r[0] == lang and r[1] == key]
                    if len(same) > self.PER_KEY:
                        self.ready.remove(same[0])


# ---- what the chat page sees ------------------------------------------------------

class Hub:
    """Shared between the serial loop, the chat server and the console."""

    def __init__(self, mind, args):
        self.mind, self.args = mind, args
        self.lock = threading.Lock()
        self.events, self.next_id = [], 1
        self.port = None          # COM name while Masoud is connected
        self.speaking = self.thinking = False
        self.mood = "n"
        self.last_lang = "en"      # the language of his last answer: his own lines follow it
        self.todo = queue.Queue()  # jobs for the serial loop: ("face", keys) / ("speak", mood, text, clip)
        self.voice = queue.Queue()  # answers waiting for their voice, rendered in order
        threading.Thread(target=self.voice_worker, daemon=True).start()

    def quiet(self):
        """Wait while he is speaking. Rendering a voice at the same time starves the stream
        (measured: 202 dropouts in one clip while the other languages were being rendered)."""
        while self.speaking:
            time.sleep(0.05)

    def voice_worker(self):
        """Render answers' voices off the chat thread, so the text shows at once."""
        while True:
            mood, text = self.voice.get()
            self.quiet()
            try:
                clip = voice_clip(text, self.args.voice, self.args.pitch)
            except Exception as e:                    # no internet for the voice, ffmpeg missing...
                say_print(f"  (could not make his voice: {e})")
                self.add("system", "He could not find his voice (internet?).", "system")
                continue
            if self.port:
                self.todo.put(("speak", mood, text, clip))

    @property
    def lang(self):
        """The language of his own lines: the setting, or (auto) the one he last answered in."""
        forced = self.mind.language()
        return forced if forced in mm.LANGUAGES else self.last_lang

    def add(self, who, text, kind, **extra):
        with self.lock:
            ev = {"id": self.next_id, "t": time.time(), "who": who, "text": text, "kind": kind, **extra}
            self.next_id += 1
            self.events = (self.events + [ev])[-300:]
        return ev

    def state(self, since):
        with self.lock:
            return {"events": [e for e in self.events if e["id"] > since], "port": self.port,
                    "speaking": self.speaking, "thinking": self.thinking, "mood": self.mood,
                    "mind": mm.LABELS.get(self.mind.describe(), self.mind.describe()),
                    "language": self.mind.language()}

    def message(self, text, mode="chat", cid=None):
        """Your message -> his answer (or your words, in "say" mode), queued to be spoken."""
        text = (text or "").strip()[:500]
        if not text:
            return None
        if text.startswith("!"):                     # "!words": he says them exactly
            mode, text = "say", text[1:].strip()
            if not text:
                return None
        self.add("you", text, mode, cid=cid)
        mood, used, seconds, extra = None, None, 0.0, {}
        if mode == "say":
            said, kind = text, "said"
            self.last_lang = mm.detect_lang(text)
        else:
            self.thinking = True
            self.todo.put(("face", "8"))              # eyes up: he is thinking
            try:
                mood, said, used = self.mind.reply(text)
                seconds, kind = self.mind.last_seconds, "reply"
            except mm.MindError as e:                 # every mind failed: he says so, never stays silent
                say_print(f"  (his mind failed: {e})")
                mood, said, kind, extra["error"] = "s", OFFLINE[self.lang], "error", str(e)
            finally:
                self.thinking = False
            self.mood = mood
            if kind == "reply":
                self.last_lang = mm.detect_lang(said)
            failed = [mm.LABELS.get(m, m) for m in self.mind.failed if m != used]
            if failed:                                # the chosen mind failed and a backup answered
                extra["failed"] = failed
                say_print(f"  ({', '.join(failed)} failed; {mm.LABELS.get(used, 'no mind')} answered)")
        plugged = bool(self.port)
        ev = self.add("masoud", said, kind, mood=mood, mind=mm.LABELS.get(used),
                      seconds=round(seconds, 1), cid=cid, voiceless=not plugged, **extra)
        if plugged:
            self.voice.put((mood, said))
        return ev

    def poke(self, key):
        if key and key in MOOD_KEYS:
            self.mood = key
            self.todo.put(("face", key))


class ChatServer(BaseHTTPRequestHandler):
    hub = None
    HOSTS = {f"127.0.0.1:{CHAT_PORT}", f"localhost:{CHAT_PORT}"}

    def log_message(self, *a):        # quiet
        pass

    def allowed(self):
        """Only this PC's own page: blocks other websites (and DNS rebinding) from using him."""
        origin = self.headers.get("Origin")
        return self.headers.get("Host") in self.HOSTS and (origin is None or origin.split("//")[-1] in self.HOSTS)

    def send(self, code, body, kind="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.allowed():
            return self.send(403, {"error": "forbidden"})
        url = urlparse(self.path)
        if url.path in ("/", "/index.html"):
            return self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        if url.path.startswith("/brand/") and url.path.endswith(".svg"):
            f = BRAND / Path(url.path).name
            if f.is_file():
                return self.send(200, f.read_bytes(), "image/svg+xml")
        if url.path == "/api/state":
            since = int((parse_qs(url.query).get("since") or ["0"])[0] or 0)
            return self.send(200, self.hub.state(since))
        if url.path == "/api/mind":
            return self.send(200, self.hub.mind.info())
        self.send(404, {"error": "not found"})

    def do_POST(self):
        if not self.allowed() or not self.headers.get("Content-Type", "").startswith("application/json"):
            return self.send(403, {"error": "forbidden"})
        try:
            body = json.loads(self.rfile.read(min(int(self.headers.get("Content-Length") or 0), 20000)) or b"{}")
        except ValueError:
            return self.send(400, {"error": "bad json"})
        path = urlparse(self.path).path
        if path == "/api/chat":
            ev = self.hub.message(body.get("text"), body.get("mode", "chat"), body.get("cid"))
            return self.send(200, ev or {"error": "empty"})
        if path == "/api/mood":
            self.hub.poke(str(body.get("key", ""))[:1])
            return self.send(200, {"ok": True})
        if path == "/api/forget":
            self.hub.mind.forget()
            self.hub.add("system", "He forgot the conversation.", "system")
            return self.send(200, {"ok": True})
        if path == "/api/mind":
            s = mm.load_settings()
            if body.get("mind") in ("auto",) + mm.MINDS:
                s["mind"] = body["mind"]
            if body.get("language") in ("auto",) + tuple(mm.LANGUAGES):
                s["language"] = body["language"]
            for k in ("groq_key", "anthropic_key"):
                if isinstance(body.get(k), str) and body[k].strip():
                    s[k] = body[k].strip()
                if body.get("clear_" + k):
                    s.pop(k, None)
            mm.save_settings(s)
            self.hub.mind.reload()
            return self.send(200, self.hub.mind.info())
        self.send(404, {"error": "not found"})


def serve_chat(hub):
    ChatServer.hub = hub
    try:
        server = ThreadingHTTPServer(("127.0.0.1", CHAT_PORT), ChatServer)
    except OSError as e:
        say_print(f"chat page unavailable (port {CHAT_PORT} busy?): {e}")
        return None
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    say_print(f"Chat with him at {CHAT_URL}")
    return server


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


def desktop_shortcut():
    buf = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf)   # CSIDL_DESKTOPDIRECTORY (follows OneDrive)
    return Path(buf.value or Path.home() / "Desktop") / "Chat with Masoud.url"


def install():
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    script = Path(__file__).resolve()
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{STARTUP}');"
          f"$s.TargetPath='{pythonw}';$s.Arguments='\"{script}\" --wait';"
          f"$s.WorkingDirectory='{script.parent}';$s.Description='Masoud talks';$s.Save()")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)
    say_print(f"Installed: Masoud will talk whenever he is plugged in, from every Windows start.\n  {STARTUP}")
    link = desktop_shortcut()
    link.write_text(f"[InternetShortcut]\nURL={CHAT_URL}\n", encoding="ascii")
    say_print(f"Chat shortcut: {link}")
    if not running_pid():
        subprocess.Popen([str(pythonw), str(script), "--wait"], cwd=str(script.parent),
                         creationflags=0x00000008 | 0x00000200)   # detached, own process group
        say_print("Started in the background now.")


def uninstall():
    stop()
    for f, what in ((STARTUP, "Autostart"), (desktop_shortcut(), "Chat shortcut")):
        try:
            f.unlink()
            say_print(f"{what} removed.")
        except FileNotFoundError:
            pass


# ---- the console ----------------------------------------------------------------------

def console(hub):
    """Type to him: a sentence = chat, !sentence = he says it, one letter = a mood."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if len(line) == 1:
            hub.poke(line)
            continue
        ev = hub.message(line)
        if ev and (ev.get("voiceless") or ev["kind"] == "error"):   # otherwise speak() prints it
            say_print(f"masoud> {ev['text']}")


def remote_console():
    """The background one holds his port: chat through its server instead."""
    say_print(f"Masoud is already running in the background; talking to him through it ({CHAT_URL}).\n"
              "Type a message (or !words for him to say them, Ctrl+C to quit).")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        path, body = ("/api/mood", {"key": line}) if len(line) == 1 else ("/api/chat", {"text": line})
        req = urllib.request.Request(CHAT_URL.rstrip("/") + path, json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                ev = json.loads(r.read().decode())
        except (urllib.error.URLError, OSError, ValueError) as e:
            say_print(f"  (no answer from the background talker: {e})")
            continue
        if ev.get("text"):
            mood = MOOD_NAMES[MOOD_KEYS.index(ev["mood"])] if ev.get("mood") in list(MOOD_KEYS) else ""
            say_print(f"masoud> {'[' + mood + '] ' if mood else ''}{ev['text']}")


# ---- the talking loop ----------------------------------------------------------

def live(args):
    HOME.mkdir(parents=True, exist_ok=True)
    if running_pid():
        if sys.stdin and sys.stdin.isatty():
            return remote_console()
        say_print("Masoud's talker is already running (use --stop first).")
        return
    PIDFILE.write_text(str(os.getpid()))
    k32 = ctypes.windll.kernel32                   # his voice must not wait behind other programs
    k32.SetPriorityClass(k32.GetCurrentProcess(), 0x8000)   # ABOVE_NORMAL_PRIORITY_CLASS
    mind = mm.Mind()
    hub = Hub(mind, args)
    say_print(f"his mind: {mm.LABELS.get(mind.describe())} (in order: {', '.join(mind.order())})")
    threading.Thread(target=mind.warm, daemon=True).start()
    serve_chat(hub)
    if args.open:
        webbrowser.open(CHAT_URL)
    say_print("preparing his voice lines...")
    voice_clip(GREETING[hub.lang], args.voice, args.pitch)

    def warm_lines():                    # render (or load) every line in every language, once, in the background
        for lang in mm.LANGUAGES:
            for text in [GREETING[lang], OFFLINE[lang]] + [v[lang] for v in LINES.values()] + CHATTER[lang] + [c[2] for c in CLOCK[lang]]:
                hub.quiet()
                try:
                    voice_clip(text, args.voice, args.pitch)
                except Exception:
                    return               # offline: they render when first needed
    threading.Thread(target=warm_lines, daemon=True).start()
    interactive = bool(sys.stdin and sys.stdin.isatty())
    if interactive:
        threading.Thread(target=console, args=(hub,), daemon=True).start()

    def speak(port, data, label):
        say_print(f"{time.strftime('%H:%M:%S')}  says: {label}")
        hub.speaking = True
        try:
            ms.stream(port, data)
        except SystemExit as e:              # stream() exits on protocol trouble; keep living
            say_print(f"  (could not speak: {e})")
        finally:
            hub.speaking = False

    recent = []
    muse = Muse(hub, args)
    while True:
        port = connect(args.port)
        if not port:
            if not args.wait:
                say_print("Masoud not found. Is he plugged in, and the Serial Monitor closed?")
                return
            time.sleep(5)
            continue
        hub.port = port.port
        say_print(f"Masoud found on {port.port}." + (" Type to him here, or use the chat page." if interactive else ""))
        while not hub.todo.empty():          # nothing stale from while he was away
            hub.todo.get_nowait()
        hello = GREETING[hub.lang]
        hub.add("masoud", hello, "auto", mood="h")
        speak(port, voice_clip(hello, args.voice, args.pitch), hello)
        buf, last_spoke, quiet_moods_until = b"", time.time(), 0.0
        next_chat = time.time() + args.every * random.uniform(0.7, 1.3)
        try:
            while True:
                buf += port.read(256)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "ignore").strip()
                    if line.startswith("[") and line.endswith("]") and line[1:-1] in MOOD_NAMES:
                        mood = line[1:-1]
                        hub.mood = MOOD_KEYS[MOOD_NAMES.index(mood)]
                        now = time.time()
                        if mood in LINES and now - last_spoke > args.quiet_gap and now > quiet_moods_until:
                            got = muse.take(hub.mood, hub.lang)
                            if got:
                                _, text, clip = got
                            else:
                                text = LINES[mood][hub.lang]
                                clip = voice_clip(text, args.voice, args.pitch)
                            hub.add("masoud", text, "auto", mood=hub.mood)
                            speak(port, clip, f"[{mood}] {text}" + (" (ai)" if got else ""))
                            muse.said(text)
                            last_spoke, buf = time.time(), b""
                            next_chat = last_spoke + args.every * random.uniform(0.7, 1.3)
                if hub.thinking:                 # an answer is on its way: no chatter now
                    next_chat = max(next_chat, time.time() + 5)
                if args.every > 0 and time.time() > next_chat:
                    got = muse.take("c", hub.lang)
                    if got:
                        _, text, clip = got
                    else:
                        text = pick_chatter(recent, hub.lang)
                        recent = (recent + [text])[-4:]
                        clip = voice_clip(text, args.voice, args.pitch)
                    hub.add("masoud", text, "auto")
                    speak(port, clip, text + (" (ai)" if got else ""))
                    muse.said(text)
                    last_spoke, buf = time.time(), b""
                    next_chat = last_spoke + args.every * random.uniform(0.7, 1.3)
                try:
                    job = hub.todo.get_nowait()
                except queue.Empty:
                    continue
                if job[0] == "face":             # a mood from the page or console: he comments on it
                    port.write(job[1].encode())
                    if job[1] in MOOD_KEYS:
                        last_spoke = 0
                elif job[0] == "speak":
                    _, mood, text, clip = job
                    if mood:                     # eyes back to the middle, the answer's mood on his face
                        port.write(("5" + mood).encode())
                        quiet_moods_until = time.time() + 2   # and no comment on that mood: he is answering
                        time.sleep(0.15)
                    speak(port, clip, text)
                    last_spoke, buf = time.time(), b""
                    next_chat = last_spoke + args.every * random.uniform(0.7, 1.3)
        except (serial.SerialException, OSError):
            say_print("Masoud was unplugged." + (" Waiting for him..." if args.wait else ""))
            hub.port = None
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
    ap.add_argument("--every", type=float, default=18, help="seconds of quiet before he talks on his own (0 = never)")
    ap.add_argument("--quiet-gap", type=float, default=2.0, help="seconds after speaking before he comments on a mood")
    ap.add_argument("--wait", action="store_true", help="wait for Masoud and reconnect after unplugging")
    ap.add_argument("--open", action="store_true", help="open the chat page in the browser")
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
