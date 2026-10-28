"""Static file server with cross-origin isolation headers (needed for SharedArrayBuffer).

Usage: python3 serve_isolated.py PORT [DIRECTORY]
"""
import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


Handler.extensions_map[".wasm"] = "application/wasm"
directory = sys.argv[2] if len(sys.argv) > 2 else "."
http.server.ThreadingHTTPServer(
    ("127.0.0.1", int(sys.argv[1])), functools.partial(Handler, directory=directory)
).serve_forever()
