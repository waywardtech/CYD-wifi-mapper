#!/usr/bin/env python3
"""Drive dist/artifact.html (setup app + WebAssembly device emulator) in headless Chrome.

    python tools/build_artifact.py && python test/web/test_emulator.py

Opens the page from disk, which is how it runs as a standalone page or artifact: no device API,
no native dialogs. Walks PLACE -> BIND -> SURVEY -> VIEW on the emulated screen with pointer
events and checks what the firmware code wrote to storage. Screenshots land in sim/out/emu_*.png.
"""
import json
import os

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGE = os.path.join(ROOT, "dist", "artifact.html")
PROJECT = "wm:/wifimap/demo-house/project.json"
SAMPLES = "wm:/wifimap/demo-house/samples.jsonl"


def main():
    shots = os.path.join(ROOT, "sim", "out")
    os.makedirs(shots, exist_ok=True)
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="chrome", headless=True)
        pg = b.new_page(viewport={"width": 900, "height": 1000})
        errors = []
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.on("dialog", lambda d: errors.append("native dialog used: " + d.message) or d.dismiss())
        pg.goto("file://" + PAGE)
        pg.wait_for_selector("text=Running.")

        def tap(x, y):  # device pixels (320x240)
            box = pg.locator("#bezel canvas").bounding_box()
            pg.mouse.click(box["x"] + (x + 0.5) * box["width"] / 320, box["y"] + (y + 0.5) * box["height"] / 240)
            pg.wait_for_timeout(350)

        def drag(x0, y0, x1, y1):
            box = pg.locator("#bezel canvas").bounding_box()
            fx, fy = box["width"] / 320, box["height"] / 240
            pg.mouse.move(box["x"] + x0 * fx, box["y"] + y0 * fy)
            pg.mouse.down()
            pg.mouse.move(box["x"] + x1 * fx, box["y"] + y1 * fy, steps=8)
            pg.mouse.up()
            pg.wait_for_timeout(350)

        def stored(key):
            return pg.evaluate("k => store.get(k)", key)

        pg.screenshot(path=os.path.join(shots, "emu_home.png"))
        tap(100, 35)                       # open demo-house
        assert pg.evaluate("WM_EMU.wm_screen()") == 1, "map screen expected"
        # demo has objects but no samples -> starts in SURVEY. Go round to PLACE.
        tap(30, 225); tap(30, 225)
        tap(300, 10)                       # ISO -> 2D
        tap(90, 225)                       # ADD
        tap(200, 120)                      # a spot on the plan
        tap(100, 24 + 30 + 10)             # "Mesh node"
        objs = json.loads(stored(PROJECT))["objects"]
        assert objs[-1]["name"] == "Mesh node 3" and objs[-1]["bssids"] == [], objs[-1]
        tap(148, 225)                      # BIND (new object is selected)
        pg.screenshot(path=os.path.join(shots, "emu_bind.png"))
        tap(100, 24 + 10)                  # strongest entry = the simulated radio of that node
        objs = json.loads(stored(PROJECT))["objects"]
        assert objs[-1]["bssids"] and objs[-1]["bssids"][0].startswith("02:51:"), objs[-1]

        tap(30, 225)                       # -> SURVEY
        n = 0
        for y in range(45, 200, 38):
            for x in range(80, 250, 42):
                tap(x, y)
                tap(120, 225)              # SCAN HERE
                pg.wait_for_timeout(250)
                n += 1
        lines = [l for l in stored(SAMPLES).split("\n") if l.strip()]
        assert len(lines) == n, (len(lines), n)
        first = json.loads(lines[0])
        assert first["a"] and first["a"][0][1] < 0 and "c" in first, first
        pg.screenshot(path=os.path.join(shots, "emu_survey.png"))

        tap(30, 225)                       # -> VIEW
        pg.screenshot(path=os.path.join(shots, "emu_view_2d.png"))
        tap(300, 10)                       # -> ISO
        drag(120, 120, 190, 100)
        pg.screenshot(path=os.path.join(shots, "emu_view_iso.png"))

        # HOTSPOT SETUP on the device opens the SETUP tab, which sees the same scan set and samples
        tap(10, 10)                        # home
        tap(60, 225)                       # HOTSPOT SETUP
        pg.wait_for_selector("text=Standalone mode")
        pg.locator(".card", has_text="demo-house").locator("button").click()
        pg.wait_for_selector("text=Mesh node 3")
        pg.screenshot(path=os.path.join(shots, "emu_setup.png"))
        pg.click("#tabs button:text-is('DEVICE')")
        assert pg.evaluate("WM_EMU.wm_screen()") == 2, "should still be on the hotspot screen"
        tap(160, 225)                      # DONE
        assert pg.evaluate("WM_EMU.wm_screen()") == 0
        assert not errors, errors
        b.close()
    print("emulator OK: %d simulated samples, bind + view + setup hand-off" % n)


if __name__ == "__main__":
    main()
