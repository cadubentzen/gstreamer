#!/bin/bash
# Assemble GitLab Pages deployment from WASM build artifacts.
# Usage: deploy-pages.sh <builddir>
set -eu

builddir="${1:?Usage: deploy-pages.sh <builddir>}"
public="public"
src_wasm="subprojects/gst-devtools/validate/tools/wasm"
src_ges="subprojects/gst-editing-services/tools"
src_testsuites="subprojects/gst-integration-testsuites/wasm"
src_dots="subprojects/gst-devtools/dots-viewer/static"

rm -rf "$public"
mkdir -p "$public/validate" "$public/ges" "$public/dots-viewer"

# Landing page, service worker and favicon
cp "$src_wasm/pages/index.html" "$public/"
cp "$src_wasm/pages/coi-serviceworker.js" "$public/"
cp "$src_wasm/pages/favicon.svg" "$public/"

# WASM binaries from builddir
cp "$builddir"/ges-validate-wasm-1.0.js "$public/"
cp "$builddir"/ges-validate-wasm-1.0.wasm "$public/"
cp "$builddir"/ges-validate-wasm-1.0.worker.js "$public/" 2>/dev/null || true
cp "$builddir"/gst-validate-wasm-1.0.js "$public/" 2>/dev/null || true
cp "$builddir"/gst-validate-wasm-1.0.wasm "$public/" 2>/dev/null || true
cp "$builddir"/gst-validate-wasm-1.0.worker.js "$public/" 2>/dev/null || true

# GES HTML page
cp "$src_ges/ges-launch.html" "$public/"

# gst-validate pipeline runner page
cp "$src_wasm/pages/gst-launch.html" "$public/"

# Validate demo HTML pages
cp "$src_wasm/demos/"*.html "$public/validate/"

# Validate test files
cp -r "$src_testsuites"/* "$public/"

# Dots viewer bundle
cp -r "$src_dots/dist" "$public/dots-viewer/"

echo "Pages assembled in $public/ ($(find "$public" -type f | wc -l) files)"
