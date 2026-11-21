"""Rasterise simulator draw-command dumps (sim/out/*.txt) to PNG, plus a contact sheet."""
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, S = 320, 240, 3  # render at 3x so the output is readable


def col(c):
    c = int(c)
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def font(px):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",):
        if os.path.exists(p):
            return ImageFont.truetype(p, px)
    return ImageFont.load_default()


F1, F2 = font(8 * S), font(11 * S)


def render(path):
    final = Image.new("RGB", (W * S, H * S))
    im, band = final, None
    d = ImageDraw.Draw(im)

    def flush():
        if band:
            final.paste(im.crop((0, band[0] * S, W * S, (band[1] + 1) * S)), (0, band[0] * S))

    for ln in open(path):
        p = ln.rstrip("\n").split(" ")
        k = p[0]
        if k == "B":  # start of a band: draw into a scratch layer, keep only the band's rows
            flush()
            band = (int(p[1]), int(p[2]))
            im = Image.new("RGB", (W * S, H * S))
            d = ImageDraw.Draw(im)
        elif k == "R":
            x, y, w, h = map(int, p[1:5])
            if w > 0 and h > 0:
                d.rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], fill=col(p[5]))
        elif k == "r":
            x, y, w, h = map(int, p[1:5])
            d.rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], outline=col(p[5]), width=S)
        elif k == "L":
            a, b, c, e = map(int, p[1:5])
            d.line([a * S + 1, b * S + 1, c * S + 1, e * S + 1], fill=col(p[5]), width=S)
        elif k == "T":
            q = [int(v) * S for v in p[1:7]]
            d.polygon(q, fill=col(p[7]))
        elif k == "C":
            x, y, r = map(int, p[1:4])
            d.ellipse([(x - r) * S, (y - r) * S, (x + r) * S, (y + r) * S], fill=col(p[4]))
        elif k == "S":
            x, y, size = map(int, p[1:4])
            d.text((x * S, y * S + (0 if size == 1 else 1 * S)), " ".join(p[5:]), fill=col(p[4]), font=F1 if size == 1 else F2)
    flush()
    return final


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "sim/out"
    ims = []
    for t in sorted(glob.glob(os.path.join(out, "*.txt"))):
        im = render(t)
        im.save(t[:-4] + ".png")
        ims.append((os.path.basename(t)[:-4], im))
    cols = 3
    rows = (len(ims) + cols - 1) // cols
    tw, th = W * 2, H * 2
    sheet = Image.new("RGB", (cols * (tw + 12) + 12, rows * (th + 34) + 12), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    f = font(16)
    for i, (name, im) in enumerate(ims):
        x, y = 12 + (i % cols) * (tw + 12), 12 + (i // cols) * (th + 34)
        sheet.paste(im.resize((tw, th), Image.LANCZOS), (x, y))
        d.text((x, y + th + 4), name, fill=(220, 220, 220), font=f)
    sheet.save(os.path.join(out, "contact_sheet.png"))
    print("rendered", len(ims), "screens")


main()
