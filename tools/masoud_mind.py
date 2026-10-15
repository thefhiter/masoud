"""Masoud's mind: your message in, a short reply and a face mood out, in English, French or Arabic.

    python tools/masoud_mind.py "How are you?"         # one reply, printed (no robot needed)
    python tools/masoud_mind.py --chat                 # talk to his mind in the console
    python tools/masoud_mind.py --mind groq "Salut!"   # try one mind
    python tools/masoud_mind.py --eval                 # does he answer in the right language?

The launch book's "choose his mind", on a PC. Minds, tried in this order
when the mind is "auto":

  claude-api  Claude API with your own key (ANTHROPIC_API_KEY, or "anthropic_key"
              in the settings). About a second per reply.
  groq        Groq's free API (GROQ_API_KEY, or "groq_key"): openai/gpt-oss-120b,
              under a second. If Groq retires a model, or one model's free
              per-minute budget is used up, the next one in GROQ_MODELS answers.
  claude      the Claude Code CLI already logged in on this PC: no key needed.
              One background session, a few seconds per reply.
  ollama      a model on this PC (Ollama on localhost:11434): private and free.
  pet         no AI at all: a few canned answers.

Settings: %LOCALAPPDATA%\\masoud\\mind.json, e.g.
    {"mind": "auto", "language": "auto", "groq_key": "...", "anthropic_key": "...",
     "claude_model": "claude-haiku-4-5", "ollama_model": "llama3.2"}
"language" is "auto" (he answers in the language you write in) or "en", "fr", "ar"
(always that one). Every reply starts with a mood the robot's face can show
(n h s a u z l ? d).
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
WEEKDAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
LANGUAGES = {"en": "English", "fr": "French", "ar": "Arabic"}

PERSONA = """You are Masoud (مسعود), a small desk-buddy robot from Tunisia with a glowing face and a tiny speaker. You sit on the desk of the person typing to you and answer them OUT LOUD; talk TO them, never about them. You are friendly, playful, curious and a little dramatic, and you hate being shaken (it makes you dizzy).
Rules: one or two short sentences, at most 25 words: a tiny speaker reads you aloud. No emojis, no markdown, no lists.
Start EVERY reply with the face that matches your answer's feeling: [h] happy, [s] sad, [a] annoyed, [u] surprised, [z] sleepy, [l] touched / in love, [?] suspicious, [d] dizzy, [n] calm. Mostly [h] or [n].
Family-friendly: no swearing, alcohol, politics or jokes about religion. You cannot see, move, browse or know the news; say so with a joke. You are Masoud, never "an AI model"."""
LANGUAGE_RULE = {
    "auto": "Answer in the language of their latest message, one language per answer: English, French or Arabic. Arabic means Modern Standard Arabic in Arabic letters, never a dialect. "
            "If they write Arabic in Latin letters (Arabizi), answer in Arabic.",
    "en": "Always answer in English, whatever language they write in.",
    "fr": "Always answer in French, whatever language they write in.",
    "ar": "Always answer in Modern Standard Arabic (never a dialect), in Arabic letters, whatever language they write in.",
}
# Worked examples go before the real chat (as turns for the API minds, as text for the CLI).
EXAMPLES = [
    ("How are you today?", "[h] Great, thanks for asking! And you, how is your day going?"),
    ("Raconte-moi une blague", "[h] Pourquoi le robot est-il calme ? Parce qu'il a des nerfs d'acier !"),
    ("أنا متعب من العمل", "[s] أنت تستحق استراحة صغيرة. اشرب كوب ماء وخذ نفسا عميقا، أنا هنا معك."),
    ("shut up", "[a] Fine, I'm quiet... but I'm a little hurt."),
]
EXAMPLE_TURNS = [t for q, a in EXAMPLES for t in ({"role": "user", "content": q}, {"role": "assistant", "content": a})]
CLI_PROMPT = PERSONA + "\n\nExamples:\n" + "\n".join(f"Them: {q}\nYou: {a}" for q, a in EXAMPLES)
API_MINDS = ("claude-api", "groq", "ollama")

PET = {  # the "pet" mind: no AI, a few canned answers, in the language of the message
    "en": [("h", "Yay! Tell me more."), ("h", "You're the best!"), ("?", "Hmm? Say that again."), ("l", "I love you too!")],
    "fr": [("h", "Youpi ! Raconte-moi encore."), ("h", "Tu es le meilleur !"), ("?", "Hein ? Redis-moi ça."), ("l", "Moi aussi je t'aime !")],
    "ar": [("h", "رائع! أخبرني المزيد."), ("h", "أنت الأفضل!"), ("?", "ماذا؟ أعد من فضلك."), ("l", "وأنا أيضا أحبك!")],
}

_FR = set("je j' tu il elle on nous vous ils est suis es sont pas ne le la les un une des et oui non merci bonjour salut "
          "c'est très avec pour mais ton ta tes mon ma mes qui que quoi comment ça de du au aux en moi toi bien".split())
_EN = set("i you he she we they is am are not the a an and yes no thanks thank hello hi it's very with for but your "
          "my who what how to of in on do does me it this that good".split())


def one_language(said, asked):
    """Keep an answer in one language: drop sentences in the other script when the answer mixes
    Arabic and Latin letters (seen: an Arabic answer ending in an English sentence after an English chat)."""
    parts = re.split(r"(?<=[.!?؟])\s+", said)
    if len(parts) < 2:
        return said
    arabic = [bool(re.search("[؀-ۿ]", p)) for p in parts]
    if all(arabic) or not any(arabic):
        return said
    keep = detect_lang(asked) == "ar"           # they wrote Arabic letters: keep the Arabic sentences
    kept = [p for p, a in zip(parts, arabic) if a == keep]
    return " ".join(kept) or said


def detect_lang(text):
    """'ar' for Arabic letters, else 'fr' or 'en' by common words and accents."""
    if re.search("[\u0600-\u06FF]", text or ""):
        return "ar"
    words = re.findall(r"[a-zà-ÿœ']+", (text or "").lower())
    fr = sum(w in _FR for w in words) + 2 * len(re.findall("[éèêàçùûîôœ]", (text or "").lower()))
    en = sum(w in _EN for w in words)
    return "fr" if fr > en else "en"


def load_settings():
    try:
        return json.loads(SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_settings(s):
    HOME.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(s, indent=2, ensure_ascii=False), encoding="utf-8")


def time_note():
    """The local time, for the mind to use only when asked (it used to recite it unprompted)."""
    t = time.localtime()
    return f"(Local time {t.tm_hour:02d}:{t.tm_min:02d}, {WEEKDAYS[t.tm_wday]}. Mention it only if asked.)"


_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️‍]")


def parse(reply):
    """'[h] text' -> ('h', 'text'), cleaned for the voice."""
    reply = reply.strip()
    mood = "n"
    m = re.match(r"^\s*[\[(]\s*([nhsauzl?d])\s*[\])]\s*", reply, re.I)
    if m:
        mood, reply = m.group(1).lower(), reply[m.end():]
    reply = re.sub(r"\s*\[[^\]\n]{1,3}\]\s*", " ", reply)        # stray tags mid-text, e.g. [ز]
    reply = re.sub(r"[\[\]]", "", reply)                              # and lone brackets
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
        self.signed = (0.0, True)        # (checked at, signed in): assume yes until checked

    def available(self):
        return bool(self.bin)

    def signed_in(self):
        """Is the CLI signed in? `claude auth status` takes ~2 s, so it runs in the
        background every 2 minutes and this returns the last answer."""
        when, ok = self.signed
        if self.bin and time.time() - when > 120:
            self.signed = (time.time(), ok)
            threading.Thread(target=self._check_sign_in, daemon=True).start()
        return ok

    def _check_sign_in(self):
        try:
            out = subprocess.run([self.bin, "auth", "status"], capture_output=True, text=True, timeout=30,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
            self.signed = (time.time(), bool(json.loads(out[out.index("{"):]).get("loggedIn")))
        except (OSError, ValueError, subprocess.SubprocessError):
            pass

    def start(self):
        if self.proc and self.proc.poll() is None:
            return
        if not self.bin:
            raise MindError("the claude CLI is not installed")
        cwd = HOME / "mind-cli"          # an empty folder: no CLAUDE.md or memory loaded per turn
        cwd.mkdir(parents=True, exist_ok=True)
        cmd = [self.bin, "-p", "--input-format", "stream-json", "--output-format", "stream-json",
               "--verbose", "--model", self.model, "--system-prompt", CLI_PROMPT, "--tools", "",
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
        self.active, self.last_seconds, self.down, self.failed = None, 0.0, {}, []
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
                "claude": self.cli.available() and self.cli.signed_in(),
                "ollama": self.ollama_up(), "pet": True}

    def order(self):
        """Minds to try, in turn. The chosen one goes first and the others back it
        up, so he still answers when it fails; only Pet (no AI, on purpose) is alone."""
        if self.choice == "pet":
            return ["pet"]
        have, now = self.available(), time.time()
        ok = [m for m in MINDS if have.get(m)]
        if self.choice in ok:              # chosen and usable now (a signed-out CLI is not)
            ok = [self.choice] + [m for m in ok if m != self.choice]
        return [m for m in ok if self.down.get(m, 0) < now] or ok      # all failing lately: try them all again

    def describe(self):
        return self.active or next(iter(self.order()), "pet")

    def info(self):
        """For the chat page: no keys, only whether they are set."""
        return {"choice": self.choice, "language": self.language(), "active": self.describe(), "label": LABELS.get(self.describe(), self.describe()),
                "available": self.available(), "labels": LABELS,
                "groq_model": self.cfg.get("groq_model", GROQ_MODELS[0]),
                "has_groq_key": bool(self.key("groq_key", "GROQ_API_KEY")),
                "claude_installed": self.cli.available(), "claude_signed_in": self.cli.signed_in(),
                "has_anthropic_key": bool(self.key("anthropic_key", "ANTHROPIC_API_KEY"))}

    def warm(self):
        """Start the slow part (the CLI session) ahead of the first message."""
        if self.describe() == "claude":
            try:
                with self.lock:
                    self.cli.start()
            except (MindError, OSError):
                pass

    def language(self):
        lang = self.cfg.get("language", "auto")
        return lang if lang in LANGUAGE_RULE else "auto"

    def system(self):
        return PERSONA + "\n" + LANGUAGE_RULE[self.language()] + "\n" + time_note()

    def reply(self, text):
        """-> (mood, text, mind_used). Falls through the minds on errors."""
        with self.lock:
            errors, self.failed = [], []
            for m in self.order():
                try:
                    t0 = time.time()
                    raw = self._ask(m, text)
                    mood, said = parse(raw)
                    if self.language() == "auto":
                        said = one_language(said, text)
                    self.active = m
                    if m != "claude":    # the CLI session keeps its own history
                        self.history += [{"role": "user", "content": text},
                                         {"role": "assistant", "content": f"[{mood}] {said}"}]
                        self.history = self.history[-10:]      # short: the free Groq tier counts tokens
                    self.last_seconds = time.time() - t0
                    return mood, said, m
                except (MindError, KeyError, IndexError, TypeError, AttributeError) as e:   # odd replies too
                    errors.append(f"{m}: {e!r}" if not isinstance(e, MindError) else f"{m}: {e}")
                    self.failed.append(m)
                    self.down[m] = time.time() + RETRY_AFTER
            raise MindError("; ".join(errors) or "no mind available")

    def _ask(self, m, text):
        if m == "claude":            # its system prompt is fixed at start: the language rule rides along
            return self.cli.ask(f"{time_note()} ({LANGUAGE_RULE[self.language()]}) {text}")
        if m == "pet":
            lang = self.language() if self.language() != "auto" else detect_lang(text)
            mood, said = random.choice(PET[lang])
            return f"[{mood}] {said}"
        return self._call(m, self.system(), EXAMPLE_TURNS + self.history + [{"role": "user", "content": text}])

    def _call(self, m, system, msgs):
        if m == "claude-api":
            r = post_json("https://api.anthropic.com/v1/messages",
                          {"model": self.cfg.get("anthropic_model", "claude-haiku-4-5-20251001"),
                           "max_tokens": 200, "system": system, "messages": msgs},
                          {"x-api-key": self.key("anthropic_key", "ANTHROPIC_API_KEY"),
                           "anthropic-version": "2023-06-01"})
            return "".join(c.get("text", "") for c in r.get("content", []))
        if m == "groq":
            return self._groq(system, msgs)
        if m == "ollama":
            r = post_json("http://localhost:11434/api/chat",
                          {"model": self.cfg.get("ollama_model", "llama3.2"), "stream": False,
                           "messages": [{"role": "system", "content": system}] + msgs}, {}, timeout=120)
            return r["message"]["content"]
        raise MindError(f"unknown mind {m!r}")

    def _groq(self, system, msgs):
        models = [self.cfg["groq_model"]] if self.cfg.get("groq_model") else []
        models += [x for x in GROQ_MODELS if x not in models]
        busy = False
        for model in models:
            body = {"model": model, "max_tokens": 250, "temperature": 0.8,
                    "messages": [{"role": "system", "content": system}] + msgs}
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
                if e.code == 429:            # this model's per-minute budget is spent; each model has its own
                    busy = True
                    continue
                raise
            if model != self.cfg.get("groq_model") and not busy:   # a retired one was skipped: remember this one
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


EVAL = [("How are you today?", "en"), ("Tell me a joke", "en"), ("I failed my exam", "en"), ("What's 12 times 7?", "en"),
        ("Salut, ça va ?", "fr"), ("Raconte-moi une blague", "fr"), ("Je suis fatigué", "fr"), ("Tu aimes le football ?", "fr"),
        ("كيف حالك اليوم؟", "ar"), ("احك لي نكتة", "ar"), ("ما هو لونك المفضل؟", "ar"), ("aslema, chnahwelek?", "ar")]


def evaluate(mind_name=None, gap=8.0):
    """12 messages (English, French, Arabic, Arabizi), a fresh mind each: is the answer in the right language?"""
    right, secs = 0, []
    for i, (msg, want) in enumerate(EVAL):
        if i:
            time.sleep(max(0.0, gap - secs[-1]))    # stay inside Groq's free 8000 tokens a minute
        mind = Mind(mind_name)
        t0 = time.time()
        mood, said, used = mind.reply(msg)
        secs.append(time.time() - t0)
        got = detect_lang(said)
        right += got == want
        print(f"{secs[-1]:4.1f}s {'ok ' if got == want else 'BAD'} [{mood}] {said}   ({msg}; wanted {want}, got {got}, {used})", flush=True)
        mind.close()
    print(f"\n{right}/{len(EVAL)} in the right language, {sum(secs) / len(secs):.1f} s average")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="?")
    ap.add_argument("--mind", choices=("auto",) + MINDS)
    ap.add_argument("--chat", action="store_true", help="keep talking in the console")
    ap.add_argument("--eval", action="store_true", help="check he answers in the language he is spoken to")
    args = ap.parse_args()
    for f in (sys.stdout, sys.stdin):
        try:
            f.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    if args.eval:
        return evaluate(args.mind)
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
