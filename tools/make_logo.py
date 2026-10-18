"""Menu header logo (16 Oct 2026): the GX4000 console in pixel art (tools/gx4000_art.py)
on the left, an 'Arnold' wordmark on the right, on the black panel. Written as a C table of
horizontal runs for amiga_logo.c: pen, x, y, length, plus the RGB of the 8 pens (8-15) the
picture uses. The menu is hires PAL, whose pixels are twice as tall as wide, so the console is
drawn at 62x44 and each of its pixels is two hires pixels wide. Panel size 296 x 44.

python tools/make_logo.py  -> arnold/src/amiga/amiga_logo.c/.h and build/logo_preview.png
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gx4000_art

W, H = 296, 44
WHITE, RED, DGREY, LGREY = 2, 4, 5, 6   # menu pens


def render():
    # console: 62x44 art, quantised to 8 colours (pens 8-15), widened 2x
    art = gx4000_art.draw(62, 44)
    alpha = art.split()[3]
    rgb = art.convert('RGB')
    q = rgb.quantize(colors=8, method=Image.Quantize.MEDIANCUT)
    pal = (q.getpalette() + [0] * 24)[:24]
    qpx = q.load(); apx = alpha.load()
    pens = [[0] * W for _ in range(H)]
    for y in range(44):
        for x in range(62):
            if apx[x, y] > 128:
                for k in range(2):
                    pens[y][4 + 2 * x + k] = 8 + qpx[x, y]
    # wordmark
    # rendered at 4x and averaged down, then anti-aliased with the menu's two
    # greys (pens 5 and 6) where the letters meet the black panel
    S = 4
    word = Image.new('L', (W * S, H * S), 0)
    d = ImageDraw.Draw(word)
    f = ImageFont.truetype('C:/Windows/Fonts/ariblk.ttf', 34 * S)
    d.text((146 * S, -4 * S), 'Arnold', font=f, fill=255)
    word = word.resize((W, H), Image.BOX)
    # sharpened (John, 16 Oct): unsharp mask, then narrow grey bands so only
    # the true edge pixels go grey
    from PIL import ImageFilter
    word = word.filter(ImageFilter.UnsharpMask(radius=1, percent=120, threshold=0))
    wpx = word.load()
    for y in range(H):
        for x in range(W):
            v = wpx[x, y]
            if v > 160:
                pens[y][x] = WHITE
            elif v > 110:
                pens[y][x] = LGREY
            elif v > 70:
                pens[y][x] = DGREY
    # red bar under the word, like the Amstrad stripe
    for y in (39, 40):
        for x in range(148, 290):
            pens[y][x] = RED
    return pens, [tuple(pal[i:i + 3]) for i in range(0, 24, 3)]


def main():
    pens, pal = render()
    runs = []
    for y in range(H):
        x = 0
        while x < W:
            c = pens[y][x]
            if c == 0:
                x += 1
                continue
            x0 = x
            while x < W and pens[y][x] == c:
                x += 1
            runs.append((c, x0, y, x - x0))
    # preview with the menu's own colours for pens 2 and 4; hires pixels are 1:2, so 2x / 4x
    prev = Image.new('RGB', (W, H), (0, 0, 0))
    menu = {2: (255, 255, 255), 4: (221, 34, 34), 5: (119, 119, 119), 6: (204, 204, 204)}
    for c, x0, y, n in runs:
        col = menu.get(c) or pal[c - 8]
        for x in range(x0, x0 + n):
            prev.putpixel((x, y), col)
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    prev.resize((W * 2, H * 4), Image.NEAREST).save(os.path.join(ROOT, 'build', 'logo_preview.png'))
    lines = ['/* Arnold menu logo: horizontal runs (pen, x, y, length) on a black panel, and',
             ' * the RGB of pens 8-15 (the GX4000 picture). Made by tools/make_logo.py; do not edit. */',
             '#include "amiga_logo.h"',
             f'const int Arnold_LogoWidth = {W}, Arnold_LogoHeight = {H};',
             f'const int Arnold_LogoRuns = {len(runs)};',
             'const unsigned char Arnold_LogoPalette[8][3] = {',
             '\t' + ' '.join('{%d,%d,%d},' % p for p in pal),
             '};',
             'const unsigned short Arnold_Logo[][4] = {']
    for i in range(0, len(runs), 6):
        lines.append('\t' + ' '.join('{%d,%d,%d,%d},' % t for t in runs[i:i + 6]))
    lines.append('};')
    src = os.path.join(ROOT, 'arnold', 'src', 'amiga')
    open(os.path.join(src, 'amiga_logo.c'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    open(os.path.join(src, 'amiga_logo.h'), 'w', newline='\n').write(
        '#ifndef AMIGA_LOGO_H\n#define AMIGA_LOGO_H\nextern const int Arnold_LogoWidth, Arnold_LogoHeight, Arnold_LogoRuns;\n'
        'extern const unsigned char Arnold_LogoPalette[8][3];   /* pens 8-15 */\n'
        'extern const unsigned short Arnold_Logo[][4];   /* pen, x, y, length */\n#endif\n')
    print(len(runs), 'runs')


if __name__ == '__main__':
    main()
