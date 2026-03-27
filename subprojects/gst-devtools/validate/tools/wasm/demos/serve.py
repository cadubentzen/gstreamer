#!/usr/bin/env python3
"""Simple HTTP server with COOP/COEP headers for SharedArrayBuffer support.

Usage: python3 serve.py [builddir] [port]
  builddir: GStreamer build directory (default: builddir)
  port: port to serve on (default: 8080)

The server serves files from the build directory and symlinks
the media test files from gst-integration-testsuites.
"""
import http.server
import functools
import os
import sys


class COOPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

    def log_message(self, format, *args):
        if '404' in str(args):
            super().log_message(format, *args)


def main():
    builddir = sys.argv[1] if len(sys.argv) > 1 else 'builddir'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

    # Symlink media if not present
    media_link = os.path.join(builddir, 'media')
    if not os.path.exists(media_link):
        media_src = os.path.join(os.path.dirname(__file__), '..', '..', '..',
                                 '..', 'gst-integration-testsuites', 'media',
                                 'defaults')
        media_src = os.path.realpath(media_src)
        if os.path.isdir(media_src):
            os.symlink(media_src, media_link)
            print(f"Symlinked {media_link} -> {media_src}")

    handler = functools.partial(COOPHandler, directory=builddir)
    server = http.server.HTTPServer(('0.0.0.0', port), handler)
    print(f"Serving {builddir} on http://localhost:{port}")
    print("Demo pages:")
    for f in sorted(os.listdir(builddir)):
        if f.endswith('.html') and f.startswith('_'):
            print(f"  http://localhost:{port}/{f}")
    server.serve_forever()


if __name__ == '__main__':
    main()
