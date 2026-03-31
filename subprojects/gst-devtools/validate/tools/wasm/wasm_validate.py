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
import sys
import threading
import uuid


class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler that adds COOP/COEP headers and Range request support."""

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

    def do_GET(self):
        """Handle GET with Range request support for seeking."""
        if self.path.startswith("/test/"):
            return self.handle_test_route()

        range_header = self.headers.get("Range")
        if not range_header:
            return super().do_GET()

        path = self.translate_path(self.path)
        try:
            f = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return

        try:
            file_size = os.fstat(f.fileno()).st_size
            # Parse "bytes=start-end" or "bytes=start-"
            range_spec = range_header.replace("bytes=", "")
            parts = range_spec.split("-")
            start = int(parts[0]) if parts[0] else 0
            end = int(parts[1]) if parts[1] else file_size - 1
            end = min(end, file_size - 1)
            length = end - start + 1

            self.send_response(206)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Range",
                             f"bytes {start}-{end}/{file_size}")
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()

            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(remaining, 65536))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)
        finally:
            f.close()

    def handle_test_route(self):
        """Handle special /test/ routes for validate tests."""
        parts = self.path.split("/")
        route = parts[2] if len(parts) > 2 else ""

        if route == "http-error":
            # /test/http-error/<status> — return that HTTP status code
            try:
                status = int(parts[3]) if len(parts) > 3 else 500
            except ValueError:
                status = 500
            self.send_response(status)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if route == "no-range":
            # /test/no-range/media/path — serve file without Range support
            actual_path = "/" + "/".join(parts[3:])
            path = self.translate_path(actual_path)
            try:
                with open(path, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", self.guess_type(path))
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Accept-Ranges", "none")
                self.end_headers()
                self.wfile.write(data)
            except OSError:
                self.send_error(404, "File not found")
            return

        if route == "transient-fail":
            # /test/transient-fail/<count>/media/path — fail <count>
            # times with 503, then serve normally
            try:
                fail_count = int(parts[3]) if len(parts) > 3 else 1
            except ValueError:
                fail_count = 1
            actual_path = "/" + "/".join(parts[4:])
            key = self.path
            if not hasattr(self.server, '_fail_counts'):
                self.server._fail_counts = {}
            current = self.server._fail_counts.get(key, 0)
            self.server._fail_counts[key] = current + 1
            if current < fail_count:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.send_header("Retry-After", "1")
                self.end_headers()
                return
            # Serve normally after exhausting failures
            self.path = actual_path
            return self.do_GET()

        if route == "require-auth":
            # /test/require-auth/media/path — require X-Test-Auth header
            auth = self.headers.get("X-Test-Auth")
            if auth != "secret-token-123":
                self.send_response(403)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            actual_path = "/" + "/".join(parts[3:])
            self.path = actual_path
            return self.do_GET()

        self.send_error(404, "Unknown test route")

    def translate_path(self, path):
        if path.startswith("/media/"):
            return os.path.join(self.media_root, path[len("/media/"):])
        elif path.startswith("/validate/"):
            return os.path.join(self.src_dir, path[len("/validate/"):])
        else:
            return os.path.join(self.builddir, path.lstrip("/"))

    def log_message(self, format, *args):
        pass  # Suppress HTTP logs


class PoolHTTPServer(http.server.HTTPServer):
    """HTTP server using a fixed thread pool to avoid thread exhaustion
    from aborted fetch requests accumulating handler threads."""
    allow_reuse_address = True

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        import concurrent.futures
        self._pool = concurrent.futures.ThreadPoolExecutor(max_workers=4)

    def process_request(self, request, client_address):
        self._pool.submit(self.process_request_thread, request,
                          client_address)

    def process_request_thread(self, request, client_address):
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)

    def server_close(self):
        super().server_close()
        self._pool.shutdown(wait=False)


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

    # Set environment variables via ENV object (plain JS strings)
    if ws_server:
        pre_run_lines.append(
            f'ENV.GST_VALIDATE_SERVER = "{ws_server}";')
    if uuid:
        pre_run_lines.append(
            f'ENV.GST_VALIDATE_UUID = "{uuid}";')

    # Propagate GStreamer debug env vars from host environment
    for env_var in ['GST_DEBUG', 'GST_DEBUG_FILE', 'GST_DEBUG_NO_COLOR']:
        val = os.environ.get(env_var)
        if val:
            val_json = json.dumps(val)
            pre_run_lines.append(
                f'ENV["{env_var}"] = {val_json};')

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

    base_url = media_url.rsplit("/media", 1)[0]
    set_globals = (f'set-globals, GST_WASM_MEDIA_URL="{media_url}", '
                   f'GST_WASM_BASE_URL="{base_url}"\n')
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
    parser.add_argument("--unmute", action="store_true", default=False,
                        help="Run browser in headful (visible) mode")
    args = parser.parse_args()

    # Start HTTP server
    handler = make_handler(args.builddir, args.media_root, args.src_dir)
    httpd = PoolHTTPServer(("localhost", 0), handler)
    port = httpd.server_address[1]

    http_thread = threading.Thread(target=httpd.serve_forever,
                                   kwargs={"poll_interval": 0.05})
    http_thread.daemon = True
    http_thread.start()

    # Parse expect-error directive from test file — if present, the test
    # is expected to produce a pipeline error (non-zero exit code).
    expect_error = False
    with open(args.test_file, "r") as f:
        for line in f:
            if line.strip() == "# expect-error":
                expect_error = True
                break

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
                headless=not args.unmute,
                args=[
                    "--no-sandbox",
                    "--enable-features=SharedArrayBuffer",
                ])
            context = browser.new_context()
            page = context.new_page()

            # Collect console messages and detect test result
            test_result = [None]

            def on_console(msg):
                text = msg.text
                print(f"[browser] {msg.type}: {text}", file=sys.stderr)
                # Detect result from gst_validate_printf output
                if "Return value:" in text:
                    try:
                        val = int(text.split("Return value:")[1]
                                  .strip().rstrip(")"))
                        test_result[0] = val
                    except (ValueError, IndexError):
                        pass

            page.on("console", on_console)

            def on_pageerror(err):
                print(f"[browser] PAGE ERROR: {err}", file=sys.stderr)

            page.on("pageerror", on_pageerror)

            url = f"http://localhost:{port}/_gst_validate_wasm_test_{test_id}.html"
            page.goto(url)

            # Wait for test completion — try window._gstValidateResult
            # first (fast path), fall back to console output detection.
            try:
                page.wait_for_function(
                    "window._gstValidateResult !== undefined",
                    timeout=args.timeout * 1000)
                ret = page.evaluate("window._gstValidateResult")
            except Exception as e:
                if test_result[0] is not None:
                    ret = test_result[0]
                else:
                    print(f"Test timed out or failed: {e}",
                          file=sys.stderr)
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

    if expect_error:
        # Test expects a pipeline error — pass if non-zero, fail if zero
        sys.exit(0 if ret != 0 else 1)
    else:
        sys.exit(ret)


if __name__ == "__main__":
    main()
