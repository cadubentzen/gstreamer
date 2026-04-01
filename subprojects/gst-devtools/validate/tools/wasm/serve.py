#!/usr/bin/env python3
"""Simple HTTP server with COOP/COEP headers for WASM threading support.

Usage: serve.py [port] [--builddir DIR] [--media-root DIR] [--src-dir DIR] [--testsuites-dir DIR]

Serves:
  /            -> builddir (for .js/.wasm/.data files)
  /media/      -> media-root (test media files)
  /validate/   -> src-dir (HTML page and static assets)
  /ges/        -> gst-editing-services/tools (auto-discovered)
  /testsuites/ -> testsuites-dir (validate test files)
  /dots-viewer/ -> dots-viewer/static (auto-discovered)
"""

import argparse
import http.server
import os
import sys
from urllib.parse import urlparse


MEDIA_ROOT = ""
SRC_DIR = ""
BUILD_DIR = "."
TESTSUITES_DIR = ""
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# gst-devtools/validate/tools/wasm -> gst-devtools
_DEVTOOLS_ROOT = os.path.abspath(os.path.join(_SCRIPT_DIR, "..", "..", ".."))
# gst-devtools -> subprojects
_SUBPROJECTS_ROOT = os.path.abspath(os.path.join(_DEVTOOLS_ROOT, ".."))
DOTS_VIEWER_DIR = os.path.join(_DEVTOOLS_ROOT, "dots-viewer", "static")
GES_TOOLS_DIR = os.path.join(_SUBPROJECTS_ROOT, "gst-editing-services", "tools")


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler that adds Cross-Origin isolation headers."""

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        path = urlparse(path).path
        if path.startswith("/media/") and MEDIA_ROOT:
            rel = path[len("/media/"):]
            return os.path.join(MEDIA_ROOT, rel)
        if path.startswith("/validate/") and SRC_DIR:
            rel = path[len("/validate/"):]
            return os.path.join(SRC_DIR, rel)
        if path.startswith("/testsuites/") and TESTSUITES_DIR:
            rel = path[len("/testsuites/"):]
            return os.path.join(TESTSUITES_DIR, rel)
        if path.startswith("/ges/"):
            rel = path[len("/ges/"):]
            return os.path.join(GES_TOOLS_DIR, rel)
        if path.startswith("/dots-viewer/"):
            rel = path[len("/dots-viewer/"):]
            return os.path.join(DOTS_VIEWER_DIR, rel)
        # Default: serve from builddir
        rel = path.lstrip("/")
        return os.path.join(BUILD_DIR, rel)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?", type=int, default=8080)
    parser.add_argument("--builddir", default=".")
    parser.add_argument("--media-root", default="")
    parser.add_argument("--src-dir", default="")
    parser.add_argument("--testsuites-dir", default="")
    args = parser.parse_args()

    BUILD_DIR = os.path.abspath(args.builddir)
    MEDIA_ROOT = os.path.abspath(args.media_root) if args.media_root else ""
    SRC_DIR = os.path.abspath(args.src_dir) if args.src_dir else ""
    TESTSUITES_DIR = os.path.abspath(args.testsuites_dir) if args.testsuites_dir else ""

    with http.server.HTTPServer(("", args.port), COOPCOEPHandler) as httpd:
        print(f"Serving on http://localhost:{args.port}")
        print(f"  Build: {BUILD_DIR}")
        if MEDIA_ROOT:
            print(f"  Media: {MEDIA_ROOT}")
        if SRC_DIR:
            print(f"  Source: {SRC_DIR}")
        if TESTSUITES_DIR:
            print(f"  Testsuites: {TESTSUITES_DIR}")
        httpd.serve_forever()
