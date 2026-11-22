#!/usr/bin/env python3
"""Toy Commander browser speed test: local web server.

Serves the test page, the emulator files and YOUR OWN disc image to this PC only
(127.0.0.1), with the two headers the browser needs (COOP/COEP).
Results the page posts to /report are appended to results.log next to this file.

Usage:  python serve.py --disc "<path to your .chd>" [--port 8642]
"""
import argparse, http.server, json, os, sys, time, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "www")

class Handler(http.server.SimpleHTTPRequestHandler):
    disc = None

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def guess_type(self, path):
        if path.endswith(".wasm"):
            return "application/wasm"
        if path.endswith(".js"):
            return "text/javascript"
        return super().guess_type(path)

    def translate_path(self, path):
        p = urllib.parse.urlparse(path).path
        if p.startswith("/disc/"):
            return Handler.disc
        return super().translate_path(path)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/shot":
            n = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(min(n, 20 * 1024 * 1024))
            f = urllib.parse.parse_qs(u.query).get("f", ["0"])[0]
            os.makedirs(os.path.join(HERE, "shots"), exist_ok=True)
            with open(os.path.join(HERE, "shots", "frame_%07d.png" % int(f)), "wb") as out:
                out.write(data)
            self.send_response(204)
            self.end_headers()
            return
        if u.path != "/report":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(min(n, 65536)).decode("utf-8", "replace")
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + body.replace("\n", " ")
        with open(os.path.join(HERE, "results.log"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
        print(line, flush=True)
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt, *args):
        if "/report" not in str(args[0] if args else ""):
            sys.stderr.write("%s\n" % (fmt % args))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True, help="your own Toy Commander (Europe) disc image (.chd)")
    ap.add_argument("--port", type=int, default=8642)
    a = ap.parse_args()
    if not os.path.isfile(a.disc):
        sys.exit("disc image not found: " + a.disc)
    Handler.disc = os.path.abspath(a.disc)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("Open http://localhost:%d/ in Chrome or Edge (Ctrl+C to stop)" % a.port, flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
