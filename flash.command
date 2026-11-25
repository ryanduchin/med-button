#!/bin/bash
# Double-click (or run in Terminal) to build and upload the Med Button firmware.
set -e
cd "$(dirname "$0")"
if [ ! -x .venv/bin/pio ]; then
  echo "Installing PlatformIO (one time, a minute or two)..."
  python3 -m venv .venv
  .venv/bin/pip install -q platformio
fi
echo "Building and uploading. The first build downloads tools and takes a few minutes..."
.venv/bin/pio run -t upload
echo
echo "Done. The Atom restarts and reconnects on its own (white for a few seconds)."
echo "If the light stays dark, press the side reset button once."
echo "First install only: when it breathes purple, join the MedButton-Setup Wi-Fi from your phone."
