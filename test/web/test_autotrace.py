#!/usr/bin/env python3
"""Check automatic floor plan tracing in headless Chrome, through the real editor UI.

    python test/web/test_autotrace.py

Generates synthetic plan sheets (make_plans.py) in three drawing styles plus a small blurry JPEG,
loads each into the editor, lets it trace, and compares the saved geometry with the known truth.
Screenshots of every result land in sim/out/autotrace_*.png - look at them after changing the tracer.
"""
import json
import os
import subprocess
import sys
import tempfile
import time

from playwright.sync_api import sync_playwright

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_plans as mp  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = 8042
PX_PER_M = 50.0


def metres(poly, origin):
    return [((x - origin[0]) / PX_PER_M, (y - origin[1]) / PX_PER_M) for x, y in poly]


def check_outline(got, want, tol, what):
    assert len(got) == len(want), "%s: outline has %d corners, expected %d: %s" % (what, len(got), len(want), got)
    for w in want:
        d = min(abs(g[0] - w[0]) + abs(g[1] - w[1]) for g in got)
        assert d <= tol, "%s: no corner near %s (closest is %.2f m away): %s" % (what, w, d, got)


def check_walls(got, want, origin, tol_across, tol_end, what, extra=1):
    for (x1, y1, x2, y2) in want:
        a = ((x1 - origin[0]) / PX_PER_M, (y1 - origin[1]) / PX_PER_M, (x2 - origin[0]) / PX_PER_M, (y2 - origin[1]) / PX_PER_M)
        horiz = a[1] == a[3]
        ok = False
        for g in got:
            g = (min(g[0], g[2]), min(g[1], g[3]), max(g[0], g[2]), max(g[1], g[3]))
            if horiz and abs(g[1] - g[3]) < 0.01:
                ok |= abs(g[1] - a[1]) <= tol_across and abs(g[0] - a[0]) <= tol_end and abs(g[2] - a[2]) <= tol_end
            if not horiz and abs(g[0] - g[2]) < 0.01:
                ok |= abs(g[0] - a[0]) <= tol_across and abs(g[1] - a[1]) <= tol_end and abs(g[3] - a[3]) <= tol_end
        assert ok, "%s: wall %s not found in %s" % (what, [round(v, 2) for v in a], got)
    assert len(got) <= len(want) + extra, "%s: %d walls traced, expected about %d: %s" % (what, len(got), len(want), got)


def main():
    sd = tempfile.mkdtemp(prefix="wm_sd_")
    plans = tempfile.mkdtemp(prefix="wm_plans_")
    mp.main.__globals__["sys"].argv = ["make_plans", plans]
    mp.main()
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "mock_device.py"), "--port", str(PORT), "--root", sd])
    time.sleep(1.0)
    shots = os.path.join(ROOT, "sim", "out")
    os.makedirs(shots, exist_ok=True)
    try:
        with sync_playwright() as pw:
            b = pw.chromium.launch(channel="chrome", headless=True)
            pg = b.new_page(viewport={"width": 1100, "height": 900})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.on("dialog", lambda d: errors.append("native dialog used: " + d.message) or d.dismiss())

            def answer(text):  # the app's own in-page dialog
                pg.fill("#dlg input", text)
                pg.click("#dlg .ok")

            def floor(project, name, sheet, size, pick, scale_pts, shot):
                """Add one floor from a sheet image; returns the saved floor dict."""
                pg.goto("http://127.0.0.1:%d/" % PORT)
                pg.wait_for_selector("text=Scan sets")
                if pg.locator("b", has_text=project).count():
                    pg.locator(".card", has_text=project).locator("button").click()
                else:
                    pg.fill("input[aria-label='New scan set name']", project)
                    pg.click("text=Create scan set")
                pg.wait_for_selector("text=Floors")
                pg.fill("input[aria-label='New floor name']", name)
                pg.click("text=Add floor")
                pg.wait_for_selector("canvas")
                pg.set_input_files("input[type=file]", os.path.join(plans, sheet))

                def tap(ix, iy):
                    box = pg.locator("canvas").bounding_box()
                    k = min(box["width"] / size[0], box["height"] / size[1])
                    pg.mouse.click(box["x"] + ix * k, box["y"] + iy * k)

                if pick:
                    try:
                        pg.wait_for_selector("text=This sheet shows 2 plans", timeout=10000)
                    except Exception:
                        pg.screenshot(path=os.path.join(shots, "autotrace_FAILED_%s.png" % shot))
                        raise AssertionError("%s: expected 2 plans, message was: %s %s" % (shot, pg.locator("#msg").inner_text(), errors))
                    tap(*pick)
                pg.wait_for_selector("text=Traced automatically")
                if scale_pts:
                    tap(*scale_pts[0]); tap(*scale_pts[1])
                    answer("12m")
                    try:
                        pg.wait_for_selector("text=Scale set", timeout=8000)
                    except Exception:
                        pg.screenshot(path=os.path.join(shots, "autotrace_FAILED_%s.png" % shot))
                        raise AssertionError("%s: scale step failed, message was: %s %s" % (shot, pg.locator("#msg").inner_text(), errors))
                else:
                    assert "Scale reused" in pg.locator("#msg").inner_text()
                pg.screenshot(path=os.path.join(shots, "autotrace_%s.png" % shot))
                pg.click("text=Save floor to CYD")
                pg.wait_for_selector("text=Placed objects")
                slug = project.lower().replace(" ", "-")
                with open(os.path.join(sd, "wifimap", slug, "project.json")) as f:
                    return [x for x in json.load(f)["floors"] if x["name"] == name][0]

            g_out, f_out = metres(mp.GROUND, mp.GROUND_AT), metres(mp.FIRST, mp.FIRST_AT)
            full = (mp.W, mp.H)

            # 1. solid walls, two floors on one sheet; second floor reuses the sheet's scale
            g = floor("Solid", "Ground floor", "sheet_solid.png", full, (300, 300), [(100, 150), (700, 150)], "solid_ground")
            check_outline(g["outline"], g_out, 0.1, "solid ground")
            check_walls(g["walls"], mp.expected_walls(mp.GROUND_WALLS), mp.GROUND_AT, 0.1, 0.35, "solid ground")
            f1 = floor("Solid", "First floor", "sheet_solid.png", full, (1000, 300), None, "solid_first")
            check_outline(f1["outline"], f_out, 0.1, "solid first")
            check_walls(f1["walls"], mp.expected_walls(mp.FIRST_WALLS), mp.FIRST_AT, 0.1, 0.35, "solid first")

            # 2. walls drawn as double lines
            g = floor("Double", "Ground floor", "sheet_double.png", full, (300, 300), [(100, 150), (700, 150)], "double_ground")
            check_outline(g["outline"], g_out, 0.1, "double-line ground")
            check_walls(g["walls"], mp.expected_walls(mp.GROUND_WALLS), mp.GROUND_AT, 0.12, 0.4, "double-line ground")

            # 3. the same sheet as a small, blurry JPEG (e.g. saved from a listing page)
            k = 900 / mp.W
            g = floor("Small", "Ground floor", "sheet_solid_small.jpg", (900, 562), (300 * k, 300 * k), [(100 * k, 150 * k), (700 * k, 150 * k)], "small_ground")
            check_outline(g["outline"], g_out, 0.15, "small jpeg ground")
            check_walls(g["walls"], mp.expected_walls(mp.GROUND_WALLS), mp.GROUND_AT, 0.15, 0.45, "small jpeg ground")

            # 4. single-line sketch (thin perimeter line, thin interior lines): outline must be right,
            #    interior walls are best effort
            g = floor("Sketch", "Ground floor", "sketch_single.png", full, (300, 300), [(100, 150), (700, 150)], "single_ground")
            check_outline(g["outline"], g_out, 0.1, "single-line ground")
            assert len(g["walls"]) >= 5, g["walls"]

            assert not errors, errors
            b.close()
        print("auto-trace OK: solid (2 floors), double-line, small JPEG, single-line sketch")
    finally:
        srv.terminate()


if __name__ == "__main__":
    main()
