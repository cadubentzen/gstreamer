#!/usr/bin/env python3
#
# Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
#
# Orchestrator script for running GstValidate WASM tests in a browser.
# Launched as a subprocess by gst-validate-launcher via GstWasmTest.

import argparse
import http.server
import json
import os
import socketserver
import sys
import threading
import uuid


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


def generate_test_html(test_content, port, ws_server=None, uuid=None):
    """Generate HTML that configures the WASM module and injects test file."""
    # Build preRun functions list
    pre_run_lines = []

    # Inject the test file content into the WASM VFS via FS.writeFile
    test_content_json = json.dumps(test_content)
    pre_run_lines.append(
        f'FS.writeFile("/test.validatetest", {test_content_json});')

    # Set environment variables
    if ws_server:
        pre_run_lines.append(
            f'ENV["GST_VALIDATE_SERVER"] = allocateUTF8("{ws_server}");')
    if uuid:
        pre_run_lines.append(
            f'ENV["GST_VALIDATE_UUID"] = allocateUTF8("{uuid}");')

    pre_run_js = "\n                ".join(pre_run_lines)

    return f"""<!doctype html>
<html>
  <head>
    <title>GstValidate WASM Runner</title>
  </head>
  <body>
    <canvas id="canvas" width="640px" height="480px"></canvas>
    <pre id="output"></pre>
    <script>
      var Module = {{
        canvas: document.getElementById('canvas'),
        locateFile: function(path) {{
          return '/' + path;
        }},
        preRun: [function() {{
                {pre_run_js}
        }}],
        print: function(text) {{
          console.log(text);
          document.getElementById('output').textContent += text + '\\n';
        }},
        printErr: function(text) {{
          console.error(text);
          document.getElementById('output').textContent += text + '\\n';
        }},
      }};
    </script>
    <script src="/gst-validate-wasm-1.0.js"></script>
  </body>
</html>
"""


def build_test_content(test_file, media_url):
    """Read the test file and prepend a set-globals block with media URL."""
    with open(test_file, "r") as f:
        original = f.read()

    set_globals = f'set-globals, GST_WASM_MEDIA_URL="{media_url}"\n'
    return set_globals + original


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

    # Start HTTP server
    handler = make_handler(args.builddir, args.media_root, args.src_dir)
    httpd = ThreadedHTTPServer(("localhost", 0), handler)
    port = httpd.server_address[1]

    http_thread = threading.Thread(target=httpd.serve_forever,
                                   kwargs={"poll_interval": 0.05})
    http_thread.daemon = True
    http_thread.start()

    # Build test content with set-globals for media URL
    media_url = f"http://localhost:{port}/media"
    test_content = build_test_content(args.test_file, media_url)

    # Generate test HTML with injected test file
    html_content = generate_test_html(
        test_content, port, args.ws_server, args.uuid)
    test_id = uuid.uuid4().hex[:8]
    html_path = os.path.join(args.builddir, f"_gst_validate_wasm_test_{test_id}.html")
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
                    "--enable-features=SharedArrayBuffer",
                ])
            context = browser.new_context()
            page = context.new_page()

            # Collect console messages
            page.on("console", lambda msg: print(
                f"[browser] {msg.type}: {msg.text}", file=sys.stderr))

            url = f"http://localhost:{port}/_gst_validate_wasm_test_{test_id}.html"
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
