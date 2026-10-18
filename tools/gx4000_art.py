"""GX4000 console, top view, in pixel-art style (after John's photo and the Genesis example,
16 Oct 2026). draw(W, H) renders the design at any size with hard pixel edges.

python gx4000_art.py  -> gx4000_preview.png (icon size and menu size, 4x)
"""
from PIL import Image, ImageDraw

# palette: body off-white with two shades, outline, slot greys, red, badge
BG = (0, 0, 0, 0)
OUT = (70, 68, 62, 255)
BODY = (222, 219, 206, 255)
HI = (240, 238, 228, 255)
SH1 = (196, 192, 178, 255)
SH2 = (160, 156, 143, 255)
VENT = (110, 107, 98, 255)
SLOT = (90, 90, 92, 255)
SLOTL = (165, 166, 170, 255)
RED = (205, 32, 40, 255)
REDD = (140, 20, 28, 255)
PADG = (150, 152, 158, 255)
BADGE = (78, 84, 100, 255)
BLUE = (40, 70, 170, 255)
WHITE = (250, 250, 250, 255)

D_W, D_H = 200.0, 140.0   # design space


def draw(W, H):
    im = Image.new('RGBA', (W, H), BG)
    d = ImageDraw.Draw(im)
    sx, sy = W / D_W, H / D_H
    P = lambda x, y: (int(round(x * sx)), int(round(y * sy)))
    R = lambda x0, y0, x1, y1: [P(x0, y0), P(x1, y1)]

    # body: wide top, sides, curved front lip
    body = [(6, 18), (194, 18), (196, 26), (196, 112), (178, 116), (150, 120), (128, 132), (72, 132), (50, 120), (22, 116), (4, 112), (4, 26)]
    d.polygon([P(*p) for p in body], fill=BODY, outline=OUT)
    # front lip shading (below the main deck)
    d.polygon([P(*p) for p in [(8, 104), (192, 104), (192, 110), (176, 114), (148, 118), (126, 128), (74, 128), (52, 118), (24, 114), (8, 110)]], fill=SH1)
    d.line([P(8, 104), P(192, 104)], fill=SH2)
    # top vent slits (diagonal stripes along the back)
    n = max(6, min(18, int(80 * sx / 2.6)))          # keep slits at least ~2.5 px apart
    for i in range(n):
        x = 62 + i * (80.0 / n)
        d.line([P(x, 21), P(x + 3, 30)], fill=VENT, width=1)
    # three horizontal grooves across the deck
    for y in (44, 64, 86):
        d.line([P(6, y), P(194, y)], fill=SH2, width=max(1, int(sy * 1.2)))
        d.line([P(6, y + 2), P(194, y + 2)], fill=HI)
    # side rails (outer columns)
    for x0, x1 in ((6, 22), (178, 194)):
        d.rectangle(R(x0, 32, x1, 104), fill=SH1, outline=SH2)
    # the two pods with vent slits
    for cx in (50, 150):
        d.rounded_rectangle(R(cx - 18, 26, cx + 18, 112), radius=max(2, int(16 * sx)), fill=HI, outline=SH2)
        step = max(6.2, 2.6 / sy)                    # at least 2.6 px between slits
        y = 42.0
        while y < 104:
            if not (62 < y < 70 or 82 < y < 90):
                y0, y1 = P(0, y)[1], P(0, y)[1] + max(0, int(round(2.6 * sy)) - 1)
                d.rectangle([P(cx - 8, 0)[0], y0, P(cx + 8, 0)[0], y1], fill=VENT)
            y += step
    # cartridge slot in the middle
    d.rectangle(R(80, 76, 120, 88), fill=SLOTL, outline=OUT)
    d.rectangle(R(84, 79, 116, 85), fill=SLOT)
    # red power slider and grey pad-port semicircle, right
    d.rectangle(R(151, 70, 158, 90), fill=RED, outline=REDD)
    d.pieslice(R(134, 86, 166, 118), 180, 360, fill=PADG, outline=SH2)
    d.line([P(148, 96), P(148, 99)], fill=OUT); d.line([P(152, 96), P(152, 99)], fill=OUT)
    # Amstrad badge
    d.rectangle(R(84, 108, 116, 120), fill=BADGE, outline=OUT)
    d.polygon([P(104, 108), P(110, 108), P(103, 120), P(97, 120)], fill=RED)
    d.polygon([P(110, 108), P(114, 108), P(107, 120), P(103, 120)], fill=BLUE)
    d.line([P(87, 114), P(100, 114)], fill=WHITE)   # stands in for the 'AMSTRAD' lettering
    return im


if __name__ == '__main__':
    big = draw(100, 70)
    small = draw(62, 44)
    sheet = Image.new('RGBA', (100 * 4 + 62 * 8 + 30, 70 * 4 + 10), (60, 62, 110, 255))
    sheet.alpha_composite(big.resize((400, 280), Image.NEAREST), (5, 5))
    # menu version: each art pixel is 2 hires pixels wide; shown here at 4x vertical, 8x horizontal
    sheet.alpha_composite(small.resize((62 * 8, 44 * 4), Image.NEAREST), (420, 5))
    sheet.save('gx4000_preview.png')
    print('ok')
