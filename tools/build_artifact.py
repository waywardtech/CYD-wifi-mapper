#!/usr/bin/env python3
"""Build dist/artifact.html: the whole app in one web page (setup app + emulated CYD).

    pip install ziglang          # provides the C++ -> WebAssembly compiler
    pio test -e native           # once, to fetch ArduinoJson
    python tools/build_artifact.py

lib/core (the firmware's UI and logic) is compiled to WebAssembly with wasm/wasm_main.cpp and
embedded, with web/emulator.html, into a copy of web/index.html.
"""
import base64
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    aj = sorted(glob.glob(os.path.join(ROOT, ".pio", "libdeps", "*", "ArduinoJson", "src")))
    if not aj:
        sys.exit("ArduinoJson not found - run 'pio test -e native' once first")
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    wasm = os.path.join(ROOT, "dist", "wm.wasm")
    cmd = [sys.executable, "-m", "ziglang", "c++", "-target", "wasm32-wasi", "-Oz", "-s", "-std=c++17",
           "-fno-exceptions", "-fno-rtti", "-mexec-model=reactor", "-DARDUINOJSON_ENABLE_STD_STRING=1",
           "-I" + os.path.join(ROOT, "lib", "core"), "-I" + aj[0],
           os.path.join(ROOT, "wasm", "wasm_main.cpp"), os.path.join(ROOT, "lib", "core", "wm_core.cpp"),
           os.path.join(ROOT, "lib", "core", "wm_app.cpp"), "-o", wasm]
    subprocess.check_call(cmd)
    with open(wasm, "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    with open(os.path.join(ROOT, "sim", "sim_main.cpp")) as f:   # reuse the simulator's demo house
        m = re.search(r'static const char\* HOUSE =\s*((?:\s*"(?:[^"\\]|\\.)*")+);', f.read())
    demo = json.loads("".join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', m.group(1))))
    with open(os.path.join(ROOT, "web", "emulator.html")) as f:
        emu = f.read().replace("__WASM_B64__", b64).replace("__DEMO_JSON__", json.dumps(demo))
    with open(os.path.join(ROOT, "web", "index.html")) as f:
        page = f.read()
    assert "</body>" in page
    out = os.path.join(ROOT, "dist", "artifact.html")
    with open(out, "w") as f:
        f.write(page.replace("</body>", emu + "</body>"))
    print("wrote %s (%d KB, wasm %d KB)" % (out, os.path.getsize(out) // 1024, os.path.getsize(wasm) // 1024))


if __name__ == "__main__":
    main()
