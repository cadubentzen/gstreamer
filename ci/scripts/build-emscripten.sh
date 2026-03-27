#!/bin/bash
set -eux
builddir="$1"
source /opt/emsdk/emsdk_env.sh
# Regenerate entry points (emcc wrapper) in case image build ran as different user
python3 /opt/emsdk/upstream/emscripten/bootstrap.py
npm install gl
# Disable Rust plugins for now — wasm32-unknown-emscripten cross-compilation
# needs a specific nightly + emscripten SDK integration that isn't set up yet
meson setup "$builddir" \
  --cross-file cross-files/wasm32-emscripten.txt \
  -Ddebug=false \
  -Drs=disabled
meson compile -C "$builddir"
xvfb-run python3 "$builddir/subprojects/gst-devtools/validate/tools/gst-validate-launcher" \
  check.gstreamer check.gst-plugins-base check.gst-editing-services \
  --meson-build-dir "$builddir" \
  --meson-no-rebuild \
  --timeout-factor "${TIMEOUT_FACTOR:-2}"
