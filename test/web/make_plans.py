#!/usr/bin/env python3
"""Draw synthetic estate-agent style floor plan sheets for testing automatic tracing.

    python test/web/make_plans.py out_dir     -> sheet_solid.png, sheet_double.png, sketch_single.png

Each sheet shows two floors side by side with the clutter real plans have: door swings, windows,
room labels with dimensions, stairs, fixtures, a compass, a scale bar and a border.
Geometry is known exactly (see GROUND / FIRST) so tests can check what the tracer returns.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, EXT, INT, DOOR = 1600, 1000, 14, 7, 45
GROUND_AT, FIRST_AT = (100, 150), (900, 150)
# footprints (sheet pixels, outer face of the walls)
GROUND = [(100, 150), (700, 150), (700, 550), (500, 550), (500, 750), (100, 750)]
FIRST = [(900, 150), (1500, 150), (1500, 550), (900, 550)]
# interior walls as centre-lines with door gaps: (x1, y1, x2, y2, [gap start offsets along the wall])
GROUND_WALLS = [(350, 150, 350, 450, [120]), (100, 450, 700, 450, [120, 450]), (300, 450, 300, 750, [120])]
FIRST_WALLS = [(1200, 150, 1200, 550, [80]), (1200, 350, 1500, 350, [150])]


def font(px):
    p = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    return ImageFont.truetype(p, px) if os.path.exists(p) else ImageFont.load_default()


def pieces(x1, y1, x2, y2, gaps):
    """Split a wall centre-line at its door gaps -> list of (x1, y1, x2, y2)."""
    horiz, out, a = y1 == y2, [], 0
    length = (x2 - x1) if horiz else (y2 - y1)
    for g in sorted(gaps) + [length]:
        b = g
        if b > a:
            out.append((x1 + a, y1, x1 + b, y1) if horiz else (x1, y1 + a, x1, y1 + b))
        a = g + DOOR
    return out


def expected_walls(walls):
    return [p for w in walls for p in pieces(*w)]


def draw_sheet(style):
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)

    def wall(x0, y0, x1, y1):  # axis-aligned filled wall box
        if style == "single":
            if x1 - x0 > y1 - y0:
                d.line([x0, (y0 + y1) // 2, x1, (y0 + y1) // 2], fill=0, width=2)
            else:
                d.line([(x0 + x1) // 2, y0, (x0 + x1) // 2, y1], fill=0, width=2)
            return
        d.rectangle([x0, y0, x1 - 1, y1 - 1], fill=0)
        if style == "double":
            d.rectangle([x0 + 2, y0 + 2, x1 - 3, y1 - 3], fill=255)

    def ext_edge(a, b, openings):  # exterior wall along polygon edge a->b (clockwise), drawn inside
        (ax, ay), (bx, by) = a, b
        horiz = ay == by
        lo, hi = (min(ax, bx), max(ax, bx)) if horiz else (min(ay, by), max(ay, by))
        cuts = sorted(openings)
        pos = lo
        for s, e, kind in cuts + [(hi, hi, None)]:
            if s > pos:
                seg(horiz, pos, s, a, b)
            if kind == "window":
                for off in (1, EXT // 2, EXT - 2):
                    line_along(horiz, s, e, a, b, off)
            pos = e

    def band(horiz, a, b):  # the EXT-thick strip just inside edge a->b
        (ax, ay), (bx, by) = a, b
        if horiz:
            return (ay, ay + EXT) if bx > ax else (ay - EXT, ay)   # top edge runs +x, bottom edge runs -x
        return (ax - EXT, ax) if by > ay else (ax, ax + EXT)       # right edge runs +y, left edge runs -y

    def seg(horiz, s, e, a, b):
        lo, hi = band(horiz, a, b)
        wall(s, lo, e, hi) if horiz else wall(lo, s, hi, e)

    def line_along(horiz, s, e, a, b, off):
        lo, _ = band(horiz, a, b)
        d.line([s, lo + off, e, lo + off] if horiz else [lo + off, s, lo + off, e], fill=0, width=1)

    def door(x, y, horiz, flip=1):  # leaf + swing arc for an opening starting at (x, y)
        if style == "single":
            return
        if horiz:
            d.line([x, y, x, y + flip * DOOR], fill=0, width=2)
            box = [x - DOOR, y - DOOR, x + DOOR, y + DOOR]
            d.arc(box, 0 if flip > 0 else 270, 90 if flip > 0 else 360, fill=0, width=1)
        else:
            d.line([x, y, x + flip * DOOR, y], fill=0, width=2)
            box = [x - DOOR, y - DOOR, x + DOOR, y + DOOR]
            d.arc(box, 0 if flip > 0 else 90, 90 if flip > 0 else 180, fill=0, width=1)

    def floor(poly, walls, openings, title):
        n = len(poly)
        if style == "single":  # appraiser-style sketch: the perimeter is one closed thin line
            d.line(poly + [poly[0]], fill=0, width=2)
        else:
            for i in range(n):
                ext_edge(poly[i], poly[(i + 1) % n], openings.get(i, []))
        for w in walls:
            for (x1, y1, x2, y2) in pieces(*w):
                if y1 == y2:
                    wall(x1, y1 - INT // 2, x2, y1 - INT // 2 + INT)
                else:
                    wall(x1 - INT // 2, y1, x1 - INT // 2 + INT, y2)
            x1, y1, x2, y2, gaps = w
            for g in gaps:
                door(x1 + g, y1, True) if y1 == y2 else door(x1, y1 + g, False)
        bx = min(p[0] for p in poly)
        by = max(p[1] for p in poly)
        d.text((bx, by + 18), title, fill=0, font=font(22))

    # ground floor: windows on the top and right walls, a front door and a wide patio door
    floor(GROUND, GROUND_WALLS, {
        0: [(180, 290, "window"), (450, 620, "window")],
        1: [(240, 360, "window")],
        2: [(530, 670, "window")],          # patio door: thin lines only
        4: [(180, 180 + DOOR, "door")],
    }, "GROUND FLOOR  Approx. 96.0 sq m")
    door(180 + DOOR, 750 - EXT, True, -1)
    floor(FIRST, FIRST_WALLS, {0: [(1000, 1120, "window"), (1300, 1420, "window")], 2: [(1000, 1150, "window")]},
          "FIRST FLOOR  Approx. 55.7 sq m")

    # clutter: labels with dimensions, stairs, fixtures, compass, scale bar, border, heading
    f = font(15)
    for (x, y, a, b) in [(150, 250, "KITCHEN", "5.00m x 6.00m"), (430, 280, "LIVING ROOM", "7.00m x 6.00m"), (130, 590, "STUDY", "4.00m x 6.00m"),
                         (340, 600, "HALL", ""), (960, 330, "BEDROOM 1", "6.00m x 8.00m"), (1270, 230, "BEDROOM 2", "6.00m x 4.00m"), (1270, 440, "BATHROOM", "")]:
        d.text((x, y), a, fill=0, font=f)
        d.text((x, y + 20), b, fill=0, font=f)
    for i in range(9):  # stairs
        d.line([330 + i * 14, 640, 330 + i * 14, 720], fill=0, width=1)
    d.rectangle([330, 640, 442, 720], outline=0, width=1)
    d.rectangle([1330, 390, 1470, 450], outline=0, width=1)   # bath
    d.ellipse([1345, 400, 1455, 440], outline=0, width=1)
    d.rectangle([560, 180, 660, 230], outline=0, width=1)     # sofa
    d.ellipse([1420, 60, 1480, 120], outline=0, width=2)      # compass
    d.polygon([(1450, 65), (1440, 95), (1460, 95)], fill=0)
    d.text((1444, 30), "N", fill=0, font=font(20))
    d.rectangle([100, 880, 220, 885], fill=0)                 # scale bar
    d.text((100, 892), "0            2m", fill=0, font=f)
    d.text((100, 50), "12 Example Road, Fort Collins", fill=0, font=font(30))
    d.text((100, 930), "Floor plan for illustrative purposes only. Not to scale.", fill=0, font=f)
    d.rectangle([20, 20, W - 21, H - 21], outline=0, width=1)
    return im


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    draw_sheet("solid").save(os.path.join(out, "sheet_solid.png"))
    draw_sheet("double").save(os.path.join(out, "sheet_double.png"))
    draw_sheet("single").save(os.path.join(out, "sketch_single.png"))
    # a low-quality phone-photo-ish variant: smaller, blurred, JPEG
    from PIL import ImageFilter
    draw_sheet("solid").resize((900, 562), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6)).save(os.path.join(out, "sheet_solid_small.jpg"), quality=55)


if __name__ == "__main__":
    main()
