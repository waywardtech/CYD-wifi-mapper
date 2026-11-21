#!/usr/bin/env bash
# Build and run the desktop simulator, then render screenshots into sim/out/.
# Needs g++, Python 3 with Pillow, and ArduinoJson (fetched by `pio test -e native` or `pio run`).
set -euo pipefail
cd "$(dirname "$0")/.."
AJ=$(ls -d .pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1 || true)
if [ -z "$AJ" ]; then echo "ArduinoJson not found - run 'pio test -e native' once first"; exit 1; fi
mkdir -p sim/out && rm -f sim/out/*.txt sim/out/*.png
g++ -std=gnu++17 -O1 -Wall -Wextra -Ilib/core -I"$AJ" sim/sim_main.cpp lib/core/wm_core.cpp lib/core/wm_app.cpp -o sim/out/sim
./sim/out/sim
python3 sim/render.py sim/out
