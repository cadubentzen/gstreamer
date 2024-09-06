#!/usr/bin/env python3
"""Simple HTTP server with COOP/COEP headers for WASM threading support.

Usage: serve.py [port] [--builddir DIR] [--media-root DIR] [--src-dir DIR]

Serves:
  /            -> builddir (for .js/.wasm/.data files)
  /media/      -> media-root (test media files)
  /validate/   -> src-dir (HTML page and static assets)
"""

import argparse
import http.server
import os
import sys


MEDIA_ROOT = ""
SRC_DIR = ""
BUILD_DIR = "."


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler that adds Cross-Origin isolation headers."""

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        if path.startswith("/media/") and MEDIA_ROOT:
            rel = path[len("/media/"):]
            return os.path.join(MEDIA_ROOT, rel)
        if path.startswith("/validate/") and SRC_DIR:
            rel = path[len("/validate/"):]
            return os.path.join(SRC_DIR, rel)
        # Default: serve from builddir
        rel = path.lstrip("/")
        return os.path.join(BUILD_DIR, rel)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?", type=int, default=8080)
    parser.add_argument("--builddir", default=".")
    parser.add_argument("--media-root", default="")
    parser.add_argument("--src-dir", default="")
    args = parser.parse_args()

    BUILD_DIR = os.path.abspath(args.builddir)
    MEDIA_ROOT = os.path.abspath(args.media_root) if args.media_root else ""
    SRC_DIR = os.path.abspath(args.src_dir) if args.src_dir else ""

    with http.server.HTTPServer(("", args.port), COOPCOEPHandler) as httpd:
        print(f"Serving on http://localhost:{args.port}")
        print(f"  Build: {BUILD_DIR}")
        if MEDIA_ROOT:
            print(f"  Media: {MEDIA_ROOT}")
        if SRC_DIR:
            print(f"  Source: {SRC_DIR}")
        httpd.serve_forever()
