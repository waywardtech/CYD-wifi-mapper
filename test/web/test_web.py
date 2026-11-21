#!/usr/bin/env python3
"""End-to-end check of the setup web app in headless Chrome against tools/mock_device.py.

    pip install playwright && python test/web/test_web.py        (uses the installed Chrome)

Walks the real flow: create a scan set, add floors (rectangle + traced over an image), set the
scale, save, and checks the JSON the CYD would receive.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = 8041


def png(path, w=400, h=300):
    """A plain grey PNG to stand in for a floor plan image."""
    raw = b"".join(b"\x00" + b"\x60" * (w * 3) for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    sd = tempfile.mkdtemp(prefix="wm_sd_")
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "mock_device.py"), "--port", str(PORT), "--root", sd])
    time.sleep(1.0)
    shots = os.path.join(ROOT, "sim", "out")
    os.makedirs(shots, exist_ok=True)
    try:
        with sync_playwright() as pw:
            b = pw.chromium.launch(channel="chrome", headless=True)
            pg = b.new_page(viewport={"width": 900, "height": 800})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))

            def answer(text):  # the app's own in-page dialog
                pg.fill("#dlg input", text)
                pg.click("#dlg .ok")
            pg.goto("http://127.0.0.1:%d/" % PORT)
            pg.wait_for_selector("text=Scan sets")

            # pure helpers
            ev = pg.evaluate
            assert ev("WM.inferLevel('Basement')") == -1 and ev("WM.inferLevel('Ground floor')") == 0
            assert ev("WM.inferLevel('First floor')") == 1 and ev("WM.inferLevel('Level 3')") == 3
            assert abs(ev("WM.parseLength('13ft 6in')") - 4.1148) < 1e-6 and ev("WM.parseLength('420cm')") == 4.2
            assert ev("WM.parseLength('4.2')") == 4.2 and ev("WM.parseLength('banana')") is None
            assert ev("WM.slugify('  Whole House! (v2) ')") == "whole-house-v2"

            # create a scan set, add a rectangle basement
            pg.fill("input[aria-label='New scan set name']", "Whole House")
            pg.click("text=Create scan set")
            pg.wait_for_selector("text=No floors yet")
            pg.fill("input[aria-label='New floor name']", "Basement")
            pg.click("text=Add floor")
            pg.wait_for_selector("canvas")
            pg.click("text=simple rectangle")
            answer("12m x 6m")
            pg.click("text=Save floor to CYD")
            pg.wait_for_selector("text=4 corners, 0 walls")

            # add a ground floor traced over an image: outline, scale, one wall
            pg.fill("input[aria-label='New floor name']", "Ground floor")
            pg.click("text=Add floor")
            pg.wait_for_selector("canvas")
            img = os.path.join(sd, "plan.png")
            png(img)
            pg.set_input_files("input[type=file]", img)
            time.sleep(0.3)
            def tap(ix, iy):
                box = pg.locator("canvas").bounding_box()  # re-read: the hint line above can re-wrap
                k = min(box["width"] / 400, box["height"] / 300)  # image is fitted from the top-left corner
                pg.mouse.click(box["x"] + ix * k, box["y"] + iy * k)
            for p in [(50, 50), (350, 53), (352, 250), (48, 250), (50, 50)]:  # slightly off-square: snap fixes it
                tap(*p)
            pg.click("button:text-is('Scale')")
            tap(50, 50); tap(350, 50)
            answer("30ft")
            pg.wait_for_selector("text=Scale set")
            pg.click("button:text-is('Walls')")
            tap(200, 50); tap(200, 150)
            pg.screenshot(path=os.path.join(shots, "web_editor.png"))
            pg.click("text=Save floor to CYD")
            pg.wait_for_selector("text=4 corners, 1 walls")
            pg.screenshot(path=os.path.join(shots, "web_project.png"))
            assert not errors, errors

            # standalone mode (how it runs as a Claude artifact or from disk): no device API at all,
            # no native dialogs, export + re-import round trip
            pg.on("dialog", lambda d: errors.append("native dialog used: " + d.message) or d.dismiss())
            pg.goto("file://" + os.path.join(ROOT, "web", "index.html"))
            pg.wait_for_selector("text=Standalone mode")
            pg.fill("input[aria-label='New scan set name']", "Office test")
            pg.click("text=Create scan set")
            pg.fill("input[aria-label='New floor name']", "Office")
            pg.click("text=Add floor")
            pg.click("text=simple rectangle")
            answer("13ft x 10ft")
            pg.click("text=Save floor to CYD")
            pg.wait_for_selector("text=Put it on the CYD")
            exported = json.loads(pg.input_value("textarea"))
            assert exported["floors"][0]["outline"] == [[0, 0], [3.96, 0], [3.96, 3.05], [0, 3.05]], exported
            with pg.expect_download() as dl:
                pg.click("text=Download project.json")
            saved = os.path.join(sd, "export.json")
            dl.value.save_as(saved)
            pg.click("text=Delete this scan set")
            pg.click("#dlg .ok")
            pg.wait_for_selector("text=None yet")
            pg.set_input_files("input[aria-label='Import project.json']", saved)
            pg.wait_for_selector("text=4 corners, 0 walls")
            pg.screenshot(path=os.path.join(shots, "web_standalone.png"), full_page=True)
            assert not errors, errors
            b.close()

        with open(os.path.join(sd, "wifimap", "whole-house", "project.json")) as f:
            p = json.load(f)
        fl = {f["name"]: f for f in p["floors"]}
        assert fl["Basement"]["level"] == -1 and fl["Basement"]["outline"] == [[0, 0], [12, 0], [12, 6], [0, 6]], fl["Basement"]
        g = fl["Ground floor"]
        assert g["level"] == 0 and g["id"] != fl["Basement"]["id"]
        assert g["outline"] == [[0, 0], [9.14, 0], [9.14, 6.1], [0, 6.1]], g["outline"]  # 30 ft wide, squared up
        assert g["walls"] == [[4.57, 0, 4.57, 3.05]], g["walls"]
        print("web app OK:", json.dumps(p)[:160], "...")
    finally:
        srv.terminate()


if __name__ == "__main__":
    main()
