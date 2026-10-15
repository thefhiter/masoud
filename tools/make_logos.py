"""Build Masoud's logo files in brand/ as plain SVG paths (no font needed).

usage: python make_logos.py path/to/Rubik[wght].ttf
Rubik (Google Fonts, OFL) at weight 800 for the wordmark.
"""
import sys
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

NIGHT, YELLOW, CYAN, WHITE = "#0A0E17", "#FFC61A", "#00D7FF", "#F4F5F1"
OUT = Path(__file__).resolve().parent.parent / "brand"

# The mark, on a 120 x 120 grid: night head, open eye, winking eye, lucky spark.
HEAD = '<rect x="6" y="14" width="108" height="92" rx="30" fill="{head}"/>'
EYES = ('<rect x="30" y="40" width="24" height="34" rx="9" fill="{eye}"/>'
        '<path d="M66 70V58a14 14 0 0 1 28 0v12h-8V58a6 6 0 0 0-12 0v12z" fill="{eye}"/>')
SPARK = '<path d="M104 2l3.2 8.8L116 14l-8.8 3.2L104 26l-3.2-8.8L92 14l8.8-3.2z" fill="{spark}"/>'


def mark(head=NIGHT, eye=CYAN, spark=YELLOW, x=0, y=0, size=120):
    s = size / 120
    body = HEAD.format(head=head) + EYES.format(eye=eye) + (SPARK.format(spark=spark) if spark else "")
    return f'<g transform="translate({x:.2f} {y:.2f}) scale({s:.4f})">{body}</g>'


def svg(w, h, body, bg=None):
    fill = f'<rect width="{w}" height="{h}" fill="{bg}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {h:.0f}" '
            f'width="{w:.0f}" height="{h:.0f}">{fill}{body}</svg>\n')


def glyph_runs(font, text, size):
    """SVG paths for text set at `size` px; returns (paths, advance widths)."""
    upm = font["head"].unitsPerEm
    cmap, gs, hmtx = font.getBestCmap(), font.getGlyphSet(), font["hmtx"]
    scale = size / upm
    out = []
    for ch in text:
        name = cmap[ord(ch)]
        pen = SVGPathPen(gs)
        gs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, 0, 0)))
        out.append((pen.getCommands(), hmtx[name][0] * scale))
    return out, font["OS/2"].sxHeight * scale, font["hhea"].ascent * scale


def wordmark(font, color, head=NIGHT, eye=CYAN, size=100):
    """'masoud' where the 'o' is Masoud's head."""
    runs, xh, asc = glyph_runs(font, "masoud", size)
    parts, x = [], 0.0
    base = asc + 6
    for i, (path, adv) in enumerate(runs):
        if i == 3:                                     # the 'o' becomes the head
            hs = xh * 1.18
            parts.append(mark(head=head, eye=eye, x=x + 2, y=base - hs * 0.98, size=hs, spark=None))
            x += hs + 4
            continue
        parts.append(f'<path transform="translate({x:.2f} {base:.2f})" d="{path}" fill="{color}"/>')
        x += adv
    return x, base + size * 0.12, "".join(parts)


def main():
    font = TTFont(sys.argv[1])
    instantiateVariableFont(font, {"wght": 800}, inplace=True)
    OUT.mkdir(exist_ok=True)
    files = {
        "masoud-mark.svg": svg(120, 120, mark()),
        "masoud-app-icon.svg": svg(120, 120, '<rect width="120" height="120" rx="28" fill="#FFC61A"/>'
                                   + mark(spark=None, x=14, y=12, size=92)),
        "masoud-app-icon-night.svg": svg(120, 120, '<rect width="120" height="120" rx="28" fill="#0A0E17"/>'
                                         + mark(head="#1A2233", spark=YELLOW, x=12, y=12, size=96)),
    }
    # on dark grounds the head turns white with night eyes, so it never disappears
    for name, color, head, eye, bg in [("masoud-wordmark.svg", NIGHT, NIGHT, CYAN, None),
                                       ("masoud-wordmark-white.svg", WHITE, WHITE, NIGHT, None),
                                       ("masoud-wordmark-on-yellow.svg", NIGHT, NIGHT, CYAN, YELLOW)]:
        w, h, body = wordmark(font, color, head, eye)
        pad = 24 if bg else 0
        files[name] = svg(w + 2 * pad, h + 2 * pad, f'<g transform="translate({pad} {pad})">{body}</g>', bg)
    for name, text in files.items():
        (OUT / name).write_text(text, encoding="utf-8")
        print(name)


if __name__ == "__main__":
    main()
