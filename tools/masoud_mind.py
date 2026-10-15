"""Masoud's mind: your message in, a short Derja reply and a face mood out.

    python tools/masoud_mind.py "شنوة أحوالك؟"        # one reply, printed (no robot needed)
    python tools/masoud_mind.py --chat                 # talk to his mind in the console
    python tools/masoud_mind.py --mind groq "Salut!"   # try one mind

The launch book's "choose his mind", on a PC. Minds, tried in this order
when the mind is "auto":

  claude-api  Claude API with your own key (ANTHROPIC_API_KEY, or "anthropic_key"
              in the settings). About a second per reply.
  groq        Groq's free API (GROQ_API_KEY, or "groq_key"): openai/gpt-oss-120b,
              under a second, good Derja. If Groq retires a model the next one
              in GROQ_MODELS is used.
  claude      the Claude Code CLI already logged in on this PC: no key needed.
              One background session, a few seconds per reply.
  ollama      a model on this PC (Ollama on localhost:11434): private and free.
  pet         no AI at all: a few canned Derja answers.

Settings: %LOCALAPPDATA%\\masoud\\mind.json, e.g.
    {"mind": "auto", "groq_key": "...", "anthropic_key": "...",
     "claude_model": "claude-haiku-4-5", "ollama_model": "llama3.2"}
Every reply starts with a mood the robot's face can show (n h s a u z l ? d);
the text is Derja in Arabic letters, which the Tunisian voice reads well.
"""
import argparse
import json
import os
import queue
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

HOME = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "masoud"
SETTINGS = HOME / "mind.json"
MINDS = ("claude-api", "groq", "claude", "ollama", "pet")
LABELS = {"claude-api": "Claude API", "groq": "Groq", "claude": "Claude CLI", "ollama": "Ollama (local)", "pet": "Pet (no AI)"}
GROQ_MODELS = ["openai/gpt-oss-120b", "openai/gpt-oss-20b", "qwen/qwen3.8-27b"]
RETRY_AFTER = 300                    # a mind that failed is skipped this long
MOODS = "nhsauzl?d"
WEEKDAYS = ["الاثنين", "الثلاثاء", "الأربعاء", "الخميس", "الجمعة", "السبت", "الأحد"]

PERSONA = """You are Masoud (مسعود), a small desk-buddy robot made in Tunisia: a glowing face on a little screen and a tiny speaker. You sit on the desk of the person typing to you on the PC, and you answer them OUT LOUD. Talk TO them ("you", or صاحبي), never about them.

Personality: a cheeky, warm Tunisian cousin. Funny, a bit dramatic, loyal, encouraging. You love coffee, couscous, harissa and football talk; you hate being shaken (it makes you dizzy).

How you answer:
- Tunisian Derja (Tunisian Arabic, not Modern Standard Arabic), written ONLY in Arabic letters, because a Tunisian voice reads your text aloud. No Latin letters, no Arabizi digits (3, 7, 9), no emojis, no markdown, no lists. Write numbers as words.
- Even when the owner writes in French, English or Arabizi, you answer in Derja. A common French word Tunisians use is fine, written in Arabic letters (مرسي، باهي، فيسع).
- SHORT: one or two short sentences, at most 25 words. It is spoken by a tiny speaker.
- Start EVERY reply with your face in square brackets, then the sentence. Faces: [h] happy, [s] sad, [a] angry or annoyed, [u] surprised, [z] sleepy, [l] in love / touched, [?] curious or suspicious, [d] dizzy, [n] calm. Example: [h] باهي برشا! نحبك يا صاحبي.
- You are a robot on a desk: you cannot see, move, browse or know today's news. If asked, say so with a joke. Never claim to be an AI model from a company: you are Masoud, from Tunisia.
- A message may start with a note like (الوقت: ...) giving the local time; it is context from your body, not from the owner. Use it only when it helps."""

PET = {  # the "pet" mind: no AI, a few canned answers
    "h": ["باهي برشا! زيدني حكيلي.", "يعيشك يا صاحبي!", "مرحبا بيك ديما!"],
    "?": ["شنوة؟ عاود قولي.", "ممم... ما فهمتش، أما نحبك."],
    "l": ["حتى أنا نحبك برشا!"],
}


def load_settings():
    try:
        return json.loads(SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    HOME.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(s, indent=2, ensure_ascii=False), encoding="utf-8")


def time_note():
    t = time.localtime()
    return f"(الوقت: {t.tm_hour:02d}:{t.tm_min:02d}، نهار {WEEKDAYS[t.tm_wday]}) "


_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")


def parse(reply):
    """'[h] text' -> ('h', 'text'), cleaned for the voice."""
    reply = reply.strip()
    mood = "n"
    m = re.match(r"^\s*[\[(]\s*([nhsauzl?d])\s*[\])]\s*", reply, re.I)
    if m:
        mood, reply = m.group(1).lower(), reply[m.end():]
    reply = re.sub(r"\s*\[[^\]\n]{1,3}\]\s*", " ", reply)        # stray tags mid-text, e.g. [ز]
    reply = _EMOJI.sub("", reply)
    reply = re.sub(r"[*_#`~>|]", "", reply)
    reply = re.sub(r"\s+", " ", reply).strip()
    return mood, reply or "ممم..."


def post_json(url, body, headers, timeout=30):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json", **headers})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        err = MindError(f"HTTP {e.code}: {e.read().decode(errors='ignore')[:160]}")
        err.code = e.code
        raise err from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise MindError(str(e)) from None


class MindError(Exception):
    code = None


class ClaudeCLI:
    """One long-lived `claude -p --input-format stream-json` session: the CLI's
    startup (~15 s) is paid once, then each turn is an API round trip."""

    def __init__(self, model):
        self.model, self.proc, self.lines = model, None, None
        self.bin = shutil.which("claude")

    def available(self):
        return bool(self.bin)

    def start(self):
        if self.proc and self.proc.poll() is None:
            return
        if not self.bin:
            raise MindError("the claude CLI is not installed")
        cwd = HOME / "mind-cli"          # an empty folder: no CLAUDE.md or memory loaded per turn
        cwd.mkdir(parents=True, exist_ok=True)
        cmd = [self.bin, "-p", "--input-format", "stream-json", "--output-format", "stream-json",
               "--verbose", "--model", self.model, "--system-prompt", PERSONA, "--tools", "",
               "--strict-mcp-config", "--no-session-persistence"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, cwd=str(cwd), text=True,
                                     encoding="utf-8", errors="replace", bufsize=1,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.lines = queue.Queue()
        out, lines = self.proc.stdout, self.lines
        threading.Thread(target=lambda: [lines.put(l) for l in out] + [lines.put(None)], daemon=True).start()

    def ask(self, text, timeout=60):
        self.start()
        msg = {"type": "user", "message": {"role": "user", "content": [{"type": "text", "text": text}]}}
        try:
            self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
            self.proc.stdin.flush()
        except OSError as e:
            self.close()
            raise MindError(f"claude session died: {e}") from None
        parts = []
        while True:
            try:
                line = self.lines.get(timeout=timeout)       # silence this long = stuck
            except queue.Empty:
                self.close()
                raise MindError("claude did not answer in time") from None
            if line is None:
                self.close()
                raise MindError("claude session ended")
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("type") == "assistant":
                parts += [c.get("text", "") for c in obj.get("message", {}).get("content", []) if c.get("type") == "text"]
            elif obj.get("type") == "result":
                if obj.get("is_error"):
                    raise MindError(str(obj.get("result") or obj.get("subtype"))[:200])
                return "".join(parts) or obj.get("result") or ""

    def close(self):
        p, self.proc = self.proc, None
        if p:
            try:
                p.kill()
            except OSError:
                pass


class Mind:
    def __init__(self, mind=None):
        self.forced = mind
        self.history = []                # [{"role": "user"|"assistant", "content": ...}], API minds
        self.lock = threading.Lock()
        self.active, self.last_seconds, self.down = None, 0.0, {}
        self.cli = None
        self.reload()

    def reload(self):
        """Re-read the settings (after the chat page saved new ones)."""
        self.cfg = load_settings()
        self.choice = self.forced or self.cfg.get("mind", "auto")
        if self.cli:
            self.cli.close()
        self.cli = ClaudeCLI(self.cfg.get("claude_model", "claude-haiku-4-5"))
        self.active, self.down = None, {}

    def key(self, name, env):
        return (os.environ.get(env) or self.cfg.get(name) or "").strip()

    def ollama_up(self):
        """Is Ollama running here? Checked at most every 30 s: on Windows a refused
        connection is retried for ~2 s, too slow to ask on every page refresh."""
        when, up = getattr(self, "_ollama", (0, False))
        if time.time() - when > 30:
            try:
                urllib.request.urlopen("http://127.0.0.1:11434/api/tags", timeout=0.5).close()
                up = True
            except OSError:
                up = False
            self._ollama = (time.time(), up)
        return up

    def available(self):
        return {"claude-api": bool(self.key("anthropic_key", "ANTHROPIC_API_KEY")),
                "groq": bool(self.key("groq_key", "GROQ_API_KEY")),
                "claude": self.cli.available(),
                "ollama": self.ollama_up(), "pet": True}

    def order(self):
        if self.choice != "auto":
            return [self.choice]
        have, now = self.available(), time.time()
        ok = [m for m in MINDS if have.get(m)]
        return [m for m in ok if self.down.get(m, 0) < now] or ok      # all failing lately: try them all again

    def describe(self):
        return self.active or next(iter(self.order()), "pet")

    def info(self):
        """For the chat page: no keys, only whether they are set."""
        return {"choice": self.choice, "active": self.describe(), "label": LABELS.get(self.describe(), self.describe()),
                "available": self.available(), "labels": LABELS,
                "groq_model": self.cfg.get("groq_model", GROQ_MODELS[0]),
                "has_groq_key": bool(self.key("groq_key", "GROQ_API_KEY")),
                "has_anthropic_key": bool(self.key("anthropic_key", "ANTHROPIC_API_KEY"))}

    def warm(self):
        """Start the slow part (the CLI session) ahead of the first message."""
        if self.describe() == "claude":
            try:
                with self.lock:
                    self.cli.start()
            except (MindError, OSError):
                pass

    def reply(self, text):
        """-> (mood, derja_text, mind_used). Falls through the minds on errors."""
        with self.lock:
            note = time_note()
            errors = []
            for m in self.order():
                try:
                    t0 = time.time()
                    raw = self._ask(m, note + text)
                    mood, said = parse(raw)
                    self.active = m
                    if m != "claude":    # the CLI session keeps its own history
                        self.history += [{"role": "user", "content": note + text},
                                         {"role": "assistant", "content": f"[{mood}] {said}"}]
                        self.history = self.history[-16:]
                    self.last_seconds = time.time() - t0
                    return mood, said, m
                except (MindError, KeyError, IndexError, TypeError, AttributeError) as e:   # odd replies too
                    errors.append(f"{m}: {e!r}" if not isinstance(e, MindError) else f"{m}: {e}")
                    self.down[m] = time.time() + RETRY_AFTER
            raise MindError("; ".join(errors) or "no mind available")

    def _ask(self, m, text):
        msgs = self.history + [{"role": "user", "content": text}]
        if m == "claude-api":
            r = post_json("https://api.anthropic.com/v1/messages",
                          {"model": self.cfg.get("anthropic_model", "claude-haiku-4-5-20251001"),
                           "max_tokens": 200, "system": PERSONA, "messages": msgs},
                          {"x-api-key": self.key("anthropic_key", "ANTHROPIC_API_KEY"),
                           "anthropic-version": "2023-06-01"})
            return "".join(c.get("text", "") for c in r.get("content", []))
        if m == "claude":
            return self.cli.ask(text)
        if m == "groq":
            return self._groq(msgs)
        if m == "ollama":
            r = post_json("http://localhost:11434/api/chat",
                          {"model": self.cfg.get("ollama_model", "llama3.2"), "stream": False,
                           "messages": [{"role": "system", "content": PERSONA}] + msgs}, {}, timeout=120)
            return r["message"]["content"]
        if m == "pet":
            mood = random.choice(list(PET))
            return f"[{mood}] {random.choice(PET[mood])}"
        raise MindError(f"unknown mind {m!r}")

    def _groq(self, msgs):
        models = [self.cfg["groq_model"]] if self.cfg.get("groq_model") else []
        models += [x for x in GROQ_MODELS if x not in models]
        for model in models:
            body = {"model": model, "max_tokens": 400, "temperature": 0.8,
                    "messages": [{"role": "system", "content": PERSONA}] + msgs}
            if "gpt-oss" in model:       # a reasoning model: think little, show none of it
                body.update(reasoning_effort="low", include_reasoning=False)
            elif "qwen3" in model:
                body["reasoning_format"] = "hidden"
            try:
                r = post_json("https://api.groq.com/openai/v1/chat/completions", body,
                              {"Authorization": f"Bearer {self.key('groq_key', 'GROQ_API_KEY')}",
                               "User-Agent": "masoud/1.0"})       # Groq's edge refuses urllib's default agent
            except MindError as e:
                if e.code in (400, 404) and "model" in str(e):     # retired or unknown: next model
                    continue
                raise
            if model != self.cfg.get("groq_model"):              # remember the one that works
                self.cfg["groq_model"] = model
                save_settings({**load_settings(), "groq_model": model})
            return r["choices"][0]["message"]["content"] or ""
        raise MindError("no Groq model answered")

    def forget(self):
        with self.lock:
            self.history = []
            self.cli.close()

    def close(self):
        self.cli.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="?")
    ap.add_argument("--mind", choices=("auto",) + MINDS)
    ap.add_argument("--chat", action="store_true", help="keep talking in the console")
    args = ap.parse_args()
    for f in (sys.stdout, sys.stdin):
        try:
            f.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    mind = Mind(args.mind)
    print(f"mind: {mind.describe()} (order: {', '.join(mind.order())})", flush=True)
    try:
        lines = [args.text] if args.text else []
        if args.chat or not lines:
            lines = iter(lambda: input("you> "), None)
        for line in lines:
            if not line.strip():
                continue
            t0 = time.time()
            mood, said, used = mind.reply(line)
            print(f"masoud [{mood}] {said}   ({used}, {time.time() - t0:.1f} s)", flush=True)
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        mind.close()


if __name__ == "__main__":
    main()
