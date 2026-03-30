#!/usr/bin/env python3
"""Simple HTTP server with COOP/COEP headers for SharedArrayBuffer support.

Usage: python3 serve.py [builddir] [port]
  builddir: GStreamer build directory (default: builddir)
  port: port to serve on (default: 8080)

The server serves files from the build directory, symlinking demo HTML
pages and test media from gst-integration-testsuites automatically.
"""
import http.server
import functools
import glob
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


def setup_symlinks(builddir):
    """Create symlinks for demo HTML pages and test media."""
    demos_dir = os.path.dirname(os.path.realpath(__file__))

    # Symlink demo HTML pages
    for html in glob.glob(os.path.join(demos_dir, '*.html')):
        link = os.path.join(builddir, '_demo_' + os.path.basename(html))
        if not os.path.exists(link):
            os.symlink(os.path.realpath(html), link)

    # Symlink test media
    media_link = os.path.join(builddir, 'media')
    if not os.path.exists(media_link):
        media_src = os.path.realpath(
            os.path.join(demos_dir, '..', '..', '..', '..', '..',
                         'gst-integration-testsuites', 'media', 'defaults'))
        if os.path.isdir(media_src):
            os.symlink(os.path.realpath(media_src), media_link)
            print(f"Symlinked {media_link} -> {media_src}")
        else:
            print(f"Warning: media directory not found at {media_src}")
            print("  Run: git submodule update --init"
                  " subprojects/gst-integration-testsuites/media")


def main():
    builddir = sys.argv[1] if len(sys.argv) > 1 else 'builddir'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

    setup_symlinks(builddir)

    handler = functools.partial(COOPHandler, directory=builddir)
    server = http.server.HTTPServer(('0.0.0.0', port), handler)
    print(f"Serving {builddir} on http://localhost:{port}")
    print("Demo pages:")
    for f in sorted(os.listdir(builddir)):
        if f.endswith('.html') and f.startswith('_demo_'):
            print(f"  http://localhost:{port}/{f}")
    server.serve_forever()


if __name__ == '__main__':
    main()
