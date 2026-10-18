"""Compatibility sweep report: reads build/sweep (tools/headless_sweep.sh) and writes
build/sweep/sheet.png (two frames per cart: 12 s and 30 s) and build/sweep/report.md.

Flags per cart:
  blank   the 30 s frame has fewer than 3 colours or under 2% non-background pixels
  frozen  the 12 s and 30 s frames are identical (may just be a title screen)
  silent  the sound never rises above the floor
"""
import math
import os
import wave
from collections import Counter

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SW = os.path.join(ROOT, "build", "sweep")


def load(p):
    return Image.open(p).convert("RGB") if os.path.exists(p) else None


def picture_stats(img):
    px = img.getdata()
    c = Counter(px)
    bg, nbg = c.most_common(1)[0]
    return len(c), 1 - nbg / len(px)


def sound_stats(p):
    if not os.path.exists(p):
        return None
    w = wave.open(p)
    d = w.readframes(w.getnframes())
    left = d[0::2]
    if not left:
        return 0.0, 0
    m = sum(left) / len(left)
    rms = math.sqrt(sum((x - m) ** 2 for x in left) / len(left))
    return rms, len(set(left))


def main():
    rows = []
    for line in open(os.path.join(SW, "results.txt"), encoding="utf-8", errors="replace"):
        parts = line.rstrip("\n").split("|")
        if len(parts) < 4:
            continue
        n, name, r1, r2 = parts[:4]
        a = load(os.path.join(SW, f"{n}-12s.ppm"))
        b = load(os.path.join(SW, f"{n}-30s.ppm"))
        s = sound_stats(os.path.join(SW, f"{n}.wav"))
        flags = []
        if b is None or "checksum" not in r2:
            flags.append("no frame (crashed or timed out)")
        else:
            ncol, cover = picture_stats(b)
            if ncol < 3 or cover < 0.02:
                flags.append("blank")
            if a is not None and list(a.getdata()) == list(b.getdata()):
                flags.append("frozen")
        if s is None:
            flags.append("no sound file")
        elif s[0] < 1.0:
            flags.append("silent")
        rows.append((n, name, a, b, s, flags))

    # contact sheet: 4 carts a row, two half-size frames each
    tw, th = 192, 136
    cols = 4
    cell_w, cell_h = 2 * tw + 12, th + 30
    nrows = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell_w, nrows * cell_h), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 13)
    except OSError:
        font = ImageFont.load_default()
    for i, (n, name, a, b, s, flags) in enumerate(rows):
        x, y = (i % cols) * cell_w, (i // cols) * cell_h
        for j, img in enumerate((a, b)):
            if img is not None:
                sheet.paste(img.resize((tw, th)), (x + 4 + j * (tw + 4), y + 24))
        colour = (255, 120, 120) if any(f in ("blank", "silent") or f.startswith("no ") for f in flags) else \
                 (255, 210, 120) if flags else (170, 230, 170)
        draw.text((x + 4, y + 4), name[:46] + ("  [" + ", ".join(flags) + "]" if flags else ""), fill=colour, font=font)
    sheet.save(os.path.join(SW, "sheet.png"))

    ok = [r for r in rows if not r[5]]
    out = [f"# Compatibility sweep: {len(rows)} carts (GX4000 / Plus Top 80)", "",
           "Each cart ran 30 s of emulated time on the PC with the generic play script",
           "(fire, space, then joystick moves). Same core as the Amiga build.", "",
           f"No flags: {len(ok)}. Flagged: {len(rows) - len(ok)}.", "",
           "| # | Title | Colours | Picture cover | Sound level | Flags |",
           "|---|---|---|---|---|---|"]
    for n, name, a, b, s, flags in rows:
        ncol, cover = picture_stats(b) if b is not None else (0, 0)
        snd = f"{s[0]:.1f}" if s else "-"
        out.append(f"| {n} | {name[6:] if name[:3] == n else name} | {ncol} | {cover:.0%} | {snd} | {', '.join(flags)} |")
    open(os.path.join(SW, "report.md"), "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("\n".join(out[:8]))
    for n, name, a, b, s, flags in rows:
        if flags:
            print(n, name, flags)


if __name__ == "__main__":
    main()
