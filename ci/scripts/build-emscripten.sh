#!/bin/bash
set -eux
builddir="$1"
source /opt/emsdk/emsdk_env.sh
# Regenerate entry points (emcc wrapper) in case image build ran as different user
python3 /opt/emsdk/upstream/emscripten/bootstrap.py
npm install gl
export RUSTUP_HOME="${RUSTUP_HOME:-/usr/local/rustup}"
export CARGO_HOME="${CARGO_HOME:-/usr/local/cargo}"
export PATH="$CARGO_HOME/bin:$PATH"
rustup target add wasm32-unknown-emscripten
rustup toolchain install nightly --component rust-src
rustup component add rust-src --toolchain nightly-x86_64-unknown-linux-gnu
rustup target add wasm32-unknown-emscripten --toolchain nightly

meson setup "$builddir" \
  --cross-file cross-files/wasm32-emscripten.txt \
  -Ddebug=false
meson compile -C "$builddir"
