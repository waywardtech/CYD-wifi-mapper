#!/usr/bin/env python3
"""Regenerate the pictures in docs/images/ used by README.md.

    pio test -e native && sim/run.sh && python tools/build_artifact.py
    python tools/readme_images.py

Every picture comes from the project's own code: device screens from the desktop simulator
(sim/), the hero from the WebAssembly build of the device app (dist/artifact.html), and the
tracing figures from the setup page (web/index.html) run on generated plan sheets. Needs
Pillow and Playwright with Chrome.
"""
import json
import os
import sys
import tempfile

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "images")
SIM = os.path.join(ROOT, "sim", "out")
sys.path.insert(0, os.path.join(ROOT, "test", "web"))
import make_plans as mp  # noqa: E402

# simulator frame -> README name
SCREENS = {
    "01_home": "screen-home",
    "03_place_2d": "screen-place-2d",
    "05_place_iso_height": "screen-place-height",
    "07_bind_list": "screen-bind",
    "09_survey_2d": "screen-survey-2d",
    "08_survey_iso": "screen-survey-iso",
    "11_view_field_iso_rotated": "screen-view-iso",
    "12_view_field_2d_best": "screen-view-2d",
    "13_view_field_2d_router": "screen-view-router",
    "14_view_dots_iso_router": "screen-view-dots",
    "15_view_mesh_split_iso": "screen-mesh-iso",
    "16_view_mesh_split_2d": "screen-mesh-2d",
    "17_sample_detail": "screen-detail",
    "18_view_basement_iso": "screen-basement",
}


def screens():
    for src, dst in SCREENS.items():
        im = Image.open(os.path.join(SIM, src + ".png")).convert("RGB")
        im.resize((640, 480), Image.LANCZOS).save(os.path.join(OUT, dst + ".png"), optimize=True)


def tap_fn(pg, sel, w, h):
    def tap(x, y):
        box = pg.locator(sel).bounding_box()
        k = min(box["width"] / w, box["height"] / h)
        pg.mouse.click(box["x"] + x * k, box["y"] + y * k)
        pg.wait_for_timeout(300)
    return tap


def hero(b):
    """The emulated CYD in its yellow bezel, showing the demo survey."""
    with open(os.path.join(SIM, "demo_project.json")) as f:
        project = f.read()
    with open(os.path.join(SIM, "demo_samples.jsonl")) as f:
        samples = f.read()
    pg = b.new_page(viewport={"width": 900, "height": 1000}, device_scale_factor=2)
    pg.add_init_script("""(() => { if (!localStorage.getItem('wm:/wifimap/demo-house/project.json')) {
        localStorage.setItem('wm:/wifimap/demo-house/project.json', %s);
        localStorage.setItem('wm:/wifimap/demo-house/samples.jsonl', %s); } })()""" % (json.dumps(project), json.dumps(samples)))
    pg.goto("file://" + os.path.join(ROOT, "dist", "artifact.html"))
    pg.wait_for_selector("text=Running.")
    pg.add_style_tag(content="html, body, #emu { background: transparent !important; }")
    tap = tap_fn(pg, "#bezel canvas", 320, 240)

    def drag(x0, y0, x1, y1):
        box = pg.locator("#bezel canvas").bounding_box()
        fx, fy = box["width"] / 320, box["height"] / 240
        pg.mouse.move(box["x"] + x0 * fx, box["y"] + y0 * fy)
        pg.mouse.down()
        pg.mouse.move(box["x"] + x1 * fx, box["y"] + y1 * fy, steps=10)
        pg.mouse.up()
        pg.wait_for_timeout(400)

    tap(100, 35)                     # open the demo house: it has samples, so it opens in VIEW, ISO
    drag(150, 120, 185, 112)
    shots = []
    for name, taps in (("router", 1), ("mesh", 4)):   # Best AP -> Router 1 -> Mesh 1 -> Mesh 2 -> Link dBm -> Link node
        for _ in range(taps):
            tap(120, 225)
        path = os.path.join(OUT, "_bezel-%s.png" % name)
        pg.locator("#bezel").screenshot(path=path, omit_background=True)
        shots.append(path)
    pg.close()
    # side by side, with a transparent gutter, for the top of the README
    ims = [Image.open(p).convert("RGBA") for p in shots]
    w = 640
    ims = [im.resize((w, round(im.height * w / im.width)), Image.LANCZOS) for im in ims]
    gap = 40
    sheet = Image.new("RGBA", (2 * w + gap, max(im.height for im in ims)), (0, 0, 0, 0))
    sheet.paste(ims[0], (0, 0), ims[0])
    sheet.paste(ims[1], (w + gap, 0), ims[1])
    sheet.save(os.path.join(OUT, "hero.png"), optimize=True)
    for p in shots:
        os.remove(p)


def traces(b):
    """Before / after pictures of automatic tracing."""
    plans = tempfile.mkdtemp(prefix="wm_plans_")
    mp.main.__globals__["sys"].argv = ["make_plans", plans]
    mp.main()
    for name in ("sheet_solid", "sheet_double", "sketch_single"):
        im = Image.open(os.path.join(plans, name + ".png")).convert("RGB")
        im.resize((800, 500), Image.LANCZOS).save(os.path.join(OUT, "plan-" + name.replace("_", "-") + ".png"), optimize=True)

    pg = b.new_page(viewport={"width": 1100, "height": 900}, device_scale_factor=2)
    pg.goto("file://" + os.path.join(ROOT, "web", "index.html"))
    pg.wait_for_selector("text=Scan sets")
    pg.fill("input[aria-label='New scan set name']", "Readme")
    pg.click("text=Create scan set")
    ground, first = (60, 115, 740, 795), (870, 115, 1530, 595)   # sheet pixels around each plan
    for sheet, pick, out, box in (("sheet_solid", (300, 300), "trace-solid-ground", ground),
                                  ("sheet_solid", (1000, 300), "trace-solid-first", first),
                                  ("sheet_double", (300, 300), "trace-double-ground", ground),
                                  ("sketch_single", (300, 300), "trace-sketch-ground", ground)):
        pg.fill("input[aria-label='New floor name']", out)
        pg.click("text=Add floor")
        pg.wait_for_selector("canvas")
        pg.set_input_files("input[type=file]", os.path.join(plans, sheet + ".png"))
        pg.wait_for_selector("text=This sheet shows 2 plans")
        tap_fn(pg, "canvas", mp.W, mp.H)(*pick)
        pg.wait_for_selector("text=Traced automatically")
        pg.locator("canvas").screenshot(path=os.path.join(OUT, out + ".png"))
        im = Image.open(os.path.join(OUT, out + ".png")).convert("RGB")
        k = min(im.width / mp.W, im.height / mp.H)       # the sheet is fitted from the top-left corner
        im = im.crop(tuple(round(v * k) for v in box))
        im.resize((520, round(im.height * 520 / im.width)), Image.LANCZOS).save(os.path.join(OUT, out + ".png"), optimize=True)
        pg.click("text=‹ Back")
        if pg.locator("#dlg").count():
            pg.click("#dlg .ok")
        pg.wait_for_selector("text=Add floor")

    # the editor as a whole, for the setup section
    pg.fill("input[aria-label='New floor name']", "Ground floor")
    pg.click("text=Add floor")
    pg.set_input_files("input[type=file]", os.path.join(plans, "sheet_solid.png"))
    pg.wait_for_selector("text=This sheet shows 2 plans")
    tap_fn(pg, "canvas", mp.W, mp.H)(300, 300)
    pg.wait_for_selector("text=Traced automatically")
    path = os.path.join(OUT, "setup-editor.png")
    pg.screenshot(path=path, clip={"x": 0, "y": 0, "width": 1100, "height": 880})
    im = Image.open(path).convert("RGB")
    im.resize((im.width // 2, im.height // 2), Image.LANCZOS).save(path, optimize=True)
    pg.close()


def main():
    os.makedirs(OUT, exist_ok=True)
    screens()
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="chrome", headless=True)
        hero(b)
        traces(b)
        b.close()
    for f in sorted(os.listdir(OUT)):
        print("%-28s %4d KB" % (f, os.path.getsize(os.path.join(OUT, f)) // 1024))


if __name__ == "__main__":
    main()
