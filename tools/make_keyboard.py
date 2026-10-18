"""Draw the CPC Plus keyboard with the Amiga key names on the keys and write it
as an uncompressed 2-plane IFF ILBM (640 x 170) for the Arnold menu's
Keyboard window. Usage: python -I tools/make_keyboard.py [out.iff]
Needs Pillow for the preview PNG only; the IFF is written by hand."""
import struct, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 640, 170
# (CPC legend, Amiga key, width in half-key units)  -- 6128 Plus layout, main block
ROWS = [
    [("ESC", "Esc", 2), ("1", "1", 2), ("2", "2", 2), ("3", "3", 2), ("4", "4", 2), ("5", "5", 2), ("6", "6", 2),
     ("7", "7", 2), ("8", "8", 2), ("9", "9", 2), ("0", "0", 2), ("-", "-", 2), ("^", "=", 2), ("CLR", "Del", 2), ("DEL", "Bksp", 2)],
    [("TAB", "Tab", 3), ("Q", "Q", 2), ("W", "W", 2), ("E", "E", 2), ("R", "R", 2), ("T", "T", 2), ("Y", "Y", 2),
     ("U", "U", 2), ("I", "I", 2), ("O", "O", 2), ("P", "P", 2), ("@", "[", 2), ("[", "]", 2), ("RETURN", "Return", 3)],
    [("CAPS", "Caps", 4), ("A", "A", 2), ("S", "S", 2), ("D", "D", 2), ("F", "F", 2), ("G", "G", 2), ("H", "H", 2),
     ("J", "J", 2), ("K", "K", 2), ("L", "L", 2), (":", ";", 2), (";", "'", 2), ("]", "#", 2), ("", "", 2)],
    [("SHIFT", "Shift", 5), ("Z", "Z", 2), ("X", "X", 2), ("C", "C", 2), ("V", "V", 2), ("B", "B", 2), ("N", "N", 2),
     ("M", "M", 2), (",", ",", 2), (".", ".", 2), ("/", "/", 2), ("\\", "\\", 2), ("SHIFT", "Shift", 5)],
    [("CTRL", "Ctrl", 4), ("COPY", "Alt", 4), ("SPACE", "Space", 16), ("ENTER", "KP Enter", 4)],
]
PAD = [  # function pad, right: (CPC, Amiga)
    [("f7", "KP7/F7"), ("f8", "KP8/F8"), ("f9", "KP9/F9")],
    [("f4", "KP4/F4"), ("f5", "KP5/F5"), ("f6", "KP6/F6")],
    [("f1", "KP1/F1"), ("f2", "KP2/F2"), ("f3", "KP3/F3")],
    [("f0", "KP0/F10"), ("UP", "Up"), ("f.", "KP .")],
    [("LEFT", "Left"), ("DOWN", "Down"), ("RIGHT", "Right")],
]

def draw():
    img = Image.new("P", (W, H), 0)
    img.putpalette([0, 0, 0, 255, 255, 255, 170, 170, 170, 255, 220, 80] + [0] * (256 * 3 - 12))
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype("consola.ttf", 11); fs = ImageFont.truetype("consola.ttf", 9)
    except OSError:
        f = fs = ImageFont.load_default()
    d.text((8, 2), "Amstrad CPC Plus keyboard: CPC legend above, the Amiga key below.  Help = menu.  Joystick: port 2.", fill=1, font=f)
    unit, kh, gap = 13, 24, 2
    y = 22
    for row in ROWS:
        x = 8
        for cpc, ami, w in row:
            kw = unit * w - gap
            if cpc:
                d.rectangle([x, y, x + kw, y + kh], fill=2, outline=1)
                d.text((x + 3, y + 1), cpc, fill=0, font=fs)
                d.text((x + 3, y + 12), ami, fill=3 if ami != cpc else 0, font=fs)
            x += unit * w
        y += kh + gap + 2
    # function pad
    px0, py = 460, 22
    for prow in PAD:
        x = px0
        for cpc, ami in prow:
            d.rectangle([x, py, x + 54, py + kh], fill=2, outline=1)
            d.text((x + 3, py + 1), cpc, fill=0, font=fs)
            d.text((x + 3, py + 12), ami, fill=3, font=fs)
            x += 58
        py += kh + gap + 2
    return img

def ilbm(img, path):
    w, h = img.size
    px = img.load()
    rowbytes = ((w + 15) // 16) * 2
    body = bytearray()
    for y in range(h):
        for p in range(2):
            row = bytearray(rowbytes)
            for x in range(w):
                if (px[x, y] >> p) & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            body += row
    pal = img.getpalette()[:12]
    bmhd = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, 2, 0, 0, 0, 0, 1, 1, w, h)
    camg = struct.pack(">I", 0x29000 | 0x8000)   # PAL hires
    chunks = b"BMHD" + struct.pack(">I", len(bmhd)) + bmhd
    chunks += b"CMAP" + struct.pack(">I", 12) + bytes(pal)
    chunks += b"CAMG" + struct.pack(">I", 4) + camg
    chunks += b"BODY" + struct.pack(">I", len(body)) + bytes(body) + (b"\0" if len(body) & 1 else b"")
    with open(path, "wb") as fh:
        fh.write(b"FORM" + struct.pack(">I", 4 + len(chunks)) + b"ILBM" + chunks)

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "run/Keyboard.iff"
    im = draw()
    ilbm(im, out)
    im.convert("RGB").save(out.rsplit(".", 1)[0] + ".png")
    print("wrote", out)
