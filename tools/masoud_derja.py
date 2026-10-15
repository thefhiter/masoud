"""Masoud's Derja: keep his answers in Tunisian Derja, not Modern Standard Arabic.

    python tools/masoud_derja.py "ماذا تفعل الآن يا صديقي؟"   # show the fix and what still sounds like MSA
    python tools/masoud_derja.py --eval                       # 24 messages through his mind, MSA rate per answer

Three layers, used by masoud_mind.py on every answer:
  1. STYLE and EXAMPLES teach the mind how Tunisians talk at home (in its prompt, kept short:
     Groq's free tier allows 8000 tokens a minute).
  2. fix() swaps the MSA words that have a one-to-one Derja word (الآن -> توّا).
  3. markers() finds what is still MSA (grammar like سوف, لا أعرف, أنا أذهب);
     if any, the mind is asked to rewrite the answer in Derja.
"""
import argparse
import re
import sys
import time

AR = "ء-يٱ-ۓ"            # Arabic letters (not marks)
MARKS = re.compile("[ً-ِْ-ٰٟ]")   # vowel marks, tanween, sukun (shadda stays: توّا)

WORDS = """Tunisian Derja only: no Modern Standard Arabic, no Egyptian, Levantine, Gulf or Moroccan words.
Say: شنوّة/آش what, علاش why, وقتاش when, وين where, قدّاش how much, كيفاش how, توّا now, برشا a lot, باهي OK,
زادة also, كان only, أما but, إيه yes, فمّا / ما فمّاش there is / isn't, موش not, اللي who/that, أحنا we, إنتِ you,
هوما they, نحب I want/love, نحكي I talk, غدوة tomorrow, البارح yesterday, شوية a little, يزّي enough,
صاحبي / خويا buddy, لاباس fine, أكيد of course, البيرو desk, الماتش match, هكّا like this.
Never: ماذا لماذا متى أين كيف حالك الآن جدا كثيرا أيضا فقط لكن نعم يوجد ليس الذي التي نحن أريد سوف لن هل
المكتب المباراة بطبيعة الحال بس شو بزاف كتير دابا نهدر عايز هيك.
Grammar: I-verbs start with ن (نحب، نعرف), negation is ما...ش (ما نعرفش، ما تقلقش), future is باش (باش نمشي)."""

STYLE = WORDS + """
Tone: a warm, cheeky Tunisian cousin at home, a bit dramatic, teasing kindly; Tunisian French words in Arabic
letters are fine (مرسي، برافو، نورمال، ديجا). يا حسرة only for sad news. Vary how you start. Answer what was said:
no coffee, couscous or football unless that is the topic, and no time or date unless asked."""

# Original example exchanges (written for Masoud), shown to the mind before the real chat.
EXAMPLES = [
    ("How are you today?", "[h] لاباس الحمد لله! وإنتِ شنوّة أحوالك؟ حكيلي حاجة تفرّح."),
    ("ماذا تفعل الآن؟", "[n] توّا؟ قاعد نستنّى فيك تحكيلي حاجة. وإنتِ شنوّة تعمل؟"),
    ("I'm so tired of work", "[s] يا حسرة عليك! برّا ارتاح شوية وما تخمّمش في الخدمة توّا."),
    ("what's 12 times 7", "[h] أربعة وثمانين! شبيك، تحب تمتحنّي؟ أنا روبو موش تلميذ!"),
    ("shut up", "[a] هاني سكتّ! أما راني زعفان شوية، ما تعاودهاش."),
    ("chkoun enti?", "[h] أنا مسعود، روبو صغيّر من تونس، نسكن على البيرو متاعك ونحكي معاك."),
]

REWRITE = """Rewrite the text in natural Tunisian Derja, in Arabic letters, as Tunisians talk at home. Keep the meaning,
jokes and length; replace every MSA or other-dialect word. Answer with the rewritten text only.
""" + WORDS

# Words with a one-to-one Derja equivalent (whole words; a leading و or ف is kept).
SWAPS = [
    ("لا يوجد", "ما فمّاش"), ("ولكن", "أما"), ("لكن", "أما"),
    ("ماذا", "شنوّة"), ("لماذا", "علاش"), ("متى", "وقتاش"), ("أين", "وين"), ("الآن", "توّا"),
    ("جدا", "برشا"), ("كثيرا", "برشا"), ("أيضا", "زادة"), ("نحن", "أحنا"), ("حسنا", "باهي"),
    ("نعم", "إيه"), ("صديقي", "صاحبي"), ("غدا", "غدوة"), ("أمس", "البارح"), ("قليلا", "شوية"),
    ("فقط", "كهو"), ("الذي", "اللي"), ("التي", "اللي"), ("الذين", "اللي"), ("هذه", "هاذي"),
    ("يوجد", "فمّا"), ("ليس", "موش"), ("ليست", "موش"), ("لست", "موش"), ("أريد", "نحب"),
    ("تريد", "تحب"), ("يريد", "يحب"), ("ربما", "يمكن"), ("كيف حالك", "شنوّة أحوالك"),
    ("أفعل", "نعمل"), ("تفعل", "تعمل"), ("يفعل", "يعمل"), ("أذهب", "نمشي"), ("تذهب", "تمشي"),
    ("يذهب", "يمشي"), ("أعرف", "نعرف"), ("أحب", "نحب"),
    ("بطبيعة الحال", "أكيد"), ("المكتب", "البيرو"), ("مكتبك", "البيرو متاعك"), ("المباراة", "الماتش"),
    ("مباراة", "ماتش"), ("نهدر", "نحكي"), ("تهدر", "تحكي"), ("يهدر", "يحكي"), ("نهدرو", "نحكيو"),
    ("إنما", "أما"), ("بزاف", "برشا"), ("كتير", "برشا"), ("دابا", "توّا"), ("هلق", "توّا"), ("دلوقتي", "توّا"),
    ("شو", "شنوّة"), ("إيش", "شنوّة"), ("هيك", "هكّا"), ("كده", "هكّا"), ("بغيت", "نحب"), ("شحال", "قدّاش"),
]
# What still sounds like MSA after the swaps (the mind is asked to rewrite).
MSA = [r"سوف", r"لن", r"سأ[%s]+" % AR, r"أنا\s+أ[%s]{2,}" % AR,
       r"هل", r"كيف\s+حالك", r"لدي[كه]?", r"لديكم", r"عندما", r"يجب", r"أستطيع", r"تستطيع", r"يمكنني",
       r"يمكنك", r"أنتم", r"هؤلاء", r"ذلك", r"تلك", r"إنني", r"إنك", r"لقد", r"قد\s+[%s]+" % AR, r"إلى"]
MSA += [r"بس", r"صمت", r"عايز", r"عاوز", r"إزاي", r"ازاي", r"بدي", r"شلون", r"وايد", r"منيح", r"كوشري", r"زي\s+ما", r"ديال", r"ديالي"]
MSA += [re.escape(a) for a, _ in SWAPS]


def _word(pattern):
    return re.compile(r"(?<![%s])([وف]?)(%s)(?![%s])" % (AR, pattern, AR))


_SWAPS = [(_word(re.escape(a)), b) for a, b in SWAPS]
_MSA = [_word(p) for p in MSA] + [re.compile(r"(?<![%s])()(لا\s+[أتني][%s]{2,})" % (AR, AR))]   # not ولا
_HAL = re.compile(r"(^|[.!؟?،,]\s*)هل\s+")


def fix(text, mood=None):
    """Swap the MSA words that have a Derja word; drop question هل; keep shadda, drop other marks.
    With a mood other than sad, an opening "what a pity" (يا حسرة) is dropped: it was being said to good news."""
    text = MARKS.sub("", text)
    text = re.sub(r"\s*(/ة|\(ة\))", "", text)            # زين/ة: the voice cannot read a slash
    if mood and mood != "s":
        text = re.sub(r"^\s*يا\s+(حسرة|خسارة)\s*[!،,.]*\s*", "", text) or text
    text = _HAL.sub(r"\1", text)
    for rx, derja in _SWAPS:
        text = rx.sub(lambda m: m.group(1) + derja, text)
    return text


def markers(text):
    """The MSA bits still in text."""
    text = MARKS.sub("", text)
    return [m.group(2) for rx in _MSA for m in rx.finditer(text)]


EVAL = [  # what people will say to him: English, French, Arabizi, Derja and MSA
    "Hi Masoud, how are you?", "What are you doing right now?", "I'm bored", "Tell me a joke",
    "What should I eat tonight?", "I failed my exam today", "Do you like football?", "Good night",
    "Salut Masoud, ça va ?", "J'ai beaucoup de travail aujourd'hui", "Tu peux m'aider ?", "Tu es mignon",
    "aslema chnahwelek", "chbik sakket", "3andi match ghodwa", "ena ta3ban barcha",
    "شنوّة تحب تاكل؟", "وين ماشي؟", "حكيلي على روحك",
    "ماذا تحب أن تفعل في وقت الفراغ؟", "هل يمكنك مساعدتي في واجبي؟", "لماذا أنت سعيد اليوم؟",
    "أين تسكن؟", "ما رأيك في الطقس اليوم؟",
]


FOOD_TALK = re.compile(r"eat|food|hungry|tired|ta3ban|travail|work|تاكل|ماكلة|قهوة|جوعان", re.I)
FOOD = re.compile(r"قهوة|كسكوس|كوسكوس|هريسة|كسكروت")
SAD_TALK = re.compile(r"failed|tired|ta3ban|bored|travail|work|سعيد", re.I)


def evaluate(mind_name=None, mm=None, gap=9.0):
    """24 messages, a fresh mind each (no history). Counts: answers with MSA or other-dialect words,
    coffee/couscous brought up when the message was not about food or rest, and يا حسرة ("what a pity")
    said to a message that is not sad."""
    if mm is None:
        sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
        import masoud_mind as mm
    dirty, off_food, misused, rewrites, secs = 0, 0, 0, 0, []
    for i, msg in enumerate(EVAL):
        if i:
            time.sleep(max(0.0, gap - secs[-1]))  # stay inside Groq's free 8000 tokens a minute
        mind = mm.Mind(mind_name)
        t0 = time.time()
        mood, said, used = mind.reply(msg)
        secs.append(time.time() - t0)
        found = markers(said)
        food = bool(FOOD.search(said)) and not FOOD_TALK.search(msg)
        pity = "حسرة" in said and not SAD_TALK.search(msg)
        dirty, off_food, misused = dirty + bool(found), off_food + food, misused + pity
        rewrites += getattr(mind, "rewrote", 0)
        flags = ([f"MSA/dialect: {' '.join(found)}"] if found else []) + (["off-topic food"] if food else []) + (["يا حسرة misused"] if pity else [])
        print(f"{secs[-1]:4.1f}s [{mood}] {said}   ({msg})" + (f"   <- {'; '.join(flags)}" if flags else ""), flush=True)
        mind.close()
    n = len(EVAL)
    print(f"\n{n} answers: MSA or other dialect in {dirty} ({100 * dirty / n:.0f}%), off-topic coffee/couscous in "
          f"{off_food} ({100 * off_food / n:.0f}%), يا حسرة misused in {misused}; {rewrites} rewritten; {sum(secs) / n:.1f} s average")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text", nargs="?")
    ap.add_argument("--eval", action="store_true", help="run the test messages through his mind")
    ap.add_argument("--mind", help="with --eval: one mind only (groq, claude-api, ...)")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    if args.eval:
        evaluate(args.mind)
    elif args.text:
        fixed = fix(args.text)
        print(fixed)
        print("still MSA:", markers(fixed) or "nothing")
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
