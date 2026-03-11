#!/usr/bin/env python3
#
# Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
#
# Orchestrator script for running GstValidate WASM tests in a browser.
# Launched as a subprocess by gst-validate-launcher via GstWasmTest.

import argparse
import http.server
import os
import shutil
import signal
import socketserver
import sys
import threading
import time


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler that adds COOP/COEP headers for SharedArrayBuffer."""

    def __init__(self, *args, builddir=None, media_root=None,
                 src_dir=None, **kwargs):
        self.builddir = builddir
        self.media_root = media_root
        self.src_dir = src_dir
        super().__init__(*args, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def translate_path(self, path):
        if path.startswith("/media/"):
            return os.path.join(self.media_root, path[len("/media/"):])
        elif path.startswith("/validate/"):
            return os.path.join(self.src_dir, path[len("/validate/"):])
        else:
            return os.path.join(self.builddir, path.lstrip("/"))

    def log_message(self, format, *args):
        pass  # Suppress HTTP logs


class ThreadedHTTPServer(socketserver.ThreadingMixIn,
                         http.server.HTTPServer):
    allow_reuse_address = True


def make_handler(builddir, media_root, src_dir):
    """Create a handler class with the correct paths bound."""
    def handler(*args, **kwargs):
        return COOPCOEPHandler(
            *args, builddir=builddir, media_root=media_root,
            src_dir=src_dir, **kwargs)
    return handler


def generate_test_html(ws_server=None, uuid=None):
    """Generate HTML that configures the WASM module with env vars."""
    env_setup = ""
    if ws_server or uuid:
        env_lines = []
        if ws_server:
            env_lines.append(
                f'ENV["GST_VALIDATE_SERVER"] = allocateUTF8("{ws_server}");')
        if uuid:
            env_lines.append(
                f'ENV["GST_VALIDATE_UUID"] = allocateUTF8("{uuid}");')
        env_setup = """
            preRun: [function(Module) {
                %s
            }],""" % "\n                ".join(env_lines)

    return """<!doctype html>
<html>
  <head>
    <title>GstValidate WASM Runner</title>
  </head>
  <body>
    <canvas id="canvas" width="640px" height="480px"></canvas>
    <pre id="output"></pre>
    <script>
      var Module = {
        canvas: document.getElementById('canvas'),
        locateFile: function(path) {
          return '/' + path;
        },%(env_setup)s
        print: function(text) {
          console.log(text);
          document.getElementById('output').textContent += text + '\\n';
        },
        printErr: function(text) {
          console.error(text);
          document.getElementById('output').textContent += text + '\\n';
        },
      };
    </script>
    <script src="/gst-validate-wasm-1.0.js"></script>
  </body>
</html>
""" % {"env_setup": env_setup}


def main():
    parser = argparse.ArgumentParser(
        description="Run GstValidate WASM test in a browser")
    parser.add_argument("--test-file", required=True,
                        help="Path to .validatetest file")
    parser.add_argument("--builddir", required=True,
                        help="WASM build directory")
    parser.add_argument("--media-root", required=True,
                        help="Media files root directory")
    parser.add_argument("--src-dir", required=True,
                        help="WASM source directory")
    parser.add_argument("--ws-server", default=None,
                        help="WebSocket server URI for IPC")
    parser.add_argument("--uuid", default=None,
                        help="Test UUID for IPC")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Test timeout in seconds")
    args = parser.parse_args()

    # Copy test file to builddir as the preloaded data
    # (already done at build time via --preload-file)

    # Start HTTP server
    handler = make_handler(args.builddir, args.media_root, args.src_dir)
    httpd = ThreadedHTTPServer(("localhost", 0), handler)
    port = httpd.server_address[1]

    http_thread = threading.Thread(target=httpd.serve_forever,
                                   kwargs={"poll_interval": 0.05})
    http_thread.daemon = True
    http_thread.start()

    # Generate test HTML with env vars
    html_content = generate_test_html(args.ws_server, args.uuid)
    html_path = os.path.join(args.builddir, "_gst_validate_wasm_test.html")
    with open(html_path, "w") as f:
        f.write(html_content)

    ret = 1
    try:
        from playwright.sync_api import sync_playwright

        with sync_playwright() as p:
            browser = p.chromium.launch(
                headless=True,
                args=[
                    "--no-sandbox",
                    "--disable-gpu",
                    "--enable-features=SharedArrayBuffer",
                ])
            context = browser.new_context()
            page = context.new_page()

            # Collect console messages
            page.on("console", lambda msg: print(
                f"[browser] {msg.type}: {msg.text}", file=sys.stderr))

            url = f"http://localhost:{port}/_gst_validate_wasm_test.html"
            page.goto(url)

            # Wait for test completion
            try:
                page.wait_for_function(
                    "window._gstValidateResult !== undefined",
                    timeout=args.timeout * 1000)
                ret = page.evaluate("window._gstValidateResult")
            except Exception as e:
                print(f"Test timed out or failed: {e}", file=sys.stderr)
                ret = 1

            browser.close()
    except ImportError:
        print("ERROR: playwright not installed. "
              "Install with: pip install playwright && "
              "playwright install chromium", file=sys.stderr)
        ret = 1

    # Cleanup
    httpd.shutdown()
    try:
        os.unlink(html_path)
    except OSError:
        pass

    sys.exit(ret)


if __name__ == "__main__":
    main()
