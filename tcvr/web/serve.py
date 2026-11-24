#!/usr/bin/env python3
"""Toy Commander browser speed test: local web server.

Serves the test page, the emulator files and YOUR OWN disc image to this PC only
(127.0.0.1), with the two headers the browser needs (COOP/COEP).
Results the page posts to /report are appended to results.log next to this file.

Usage:  python serve.py --disc "<path to your .chd>" [--port 8642]
"""
import argparse, base64, http.server, json, os, re, sys, time, urllib.parse

import lobby, netbridge

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "www")

LOBBIES = lobby.Lobbies(log=lambda m: print(m, flush=True))

# Player profiles: each browser player's save files (the game's VMU) are kept here, on this computer, under
# the player's 3-letter name, so a player who comes back gets them from any browser or device.
PROFILES = os.path.join(HERE, "profiles")
PROFILE_MAX = 4 * 1024 * 1024
NAME_OK = re.compile(r"^[A-Z0-9]{1,3}$")
FILE_OK = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")

def profile_path(name):
    return os.path.join(PROFILES, name + ".json") if NAME_OK.match(name or "") else None

def profile_read(name):
    path = profile_path(name)
    if path is None or not os.path.isfile(path):
        return {"files": {}}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def profile_write(name, body):
    path = profile_path(name)
    if path is None:
        return False
    obj = json.loads(body.decode("utf-8"))
    files = obj.get("files")
    if not isinstance(files, dict) or not all(FILE_OK.match(k) and isinstance(v, str) for k, v in files.items()):
        return False
    for v in files.values():
        base64.b64decode(v, validate=True)
    os.makedirs(PROFILES, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"name": name, "saved": time.strftime("%Y-%m-%d %H:%M:%S"), "files": files}, f)
    os.replace(tmp, path)
    return True

class Handler(http.server.SimpleHTTPRequestHandler):
    disc = None

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def lobby_get(self, u):
        # the LAN lobby for browser players (lobby.py)
        q = urllib.parse.parse_qs(u.query)
        cid = q.get("cid", [""])[0][:64]
        if u.path == "/lobby/games":
            self.send_json(LOBBIES.games())
        elif u.path == "/lobby/view" and cid:
            v = LOBBIES.joiner(cid).get_view()
            v["peers"] = lobby.peer_list(v)
            self.send_json(v)
        elif u.path == "/lobby/snap" and cid:
            data = LOBBIES.joiner(cid).snapshot(q.get("id", [""])[0])
            if data is not None:
                data = lobby.raw_state(data)	# the browser core loads raw state bytes
            if data is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_error(404)

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

    def do_GET(self):
        if urllib.parse.urlparse(self.path).path == "/profile":
            name = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query).get("name", [""])[0]
            if profile_path(name) is None:
                self.send_error(400)
                return
            self.send_json(profile_read(name))
            return
        if urllib.parse.urlparse(self.path).path.startswith("/lobby/"):
            self.lobby_get(urllib.parse.urlparse(self.path))
            return
        if urllib.parse.urlparse(self.path).path == "/net":
            netbridge.handle_upgrade(self, log=lambda m: print(m, flush=True))	# network play (netbridge.py)
            return
        super().do_GET()

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
        if u.path == "/dump":
            # a netplay test aid (page ?syncdump=frame,...): a frame's saved state or main RAM,
            # to dumps/p<player>.<frame>.<state|ram> (as the PC build's hotd2vr.SyncDump)
            n = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(min(n, 40 * 1024 * 1024))
            q = urllib.parse.parse_qs(u.query)
            p, f, k = int(q.get("p", ["0"])[0]), int(q.get("f", ["0"])[0]), q.get("k", ["ram"])[0]
            if k not in ("ram", "state"):
                self.send_error(400)
                return
            os.makedirs(os.path.join(HERE, "dumps"), exist_ok=True)
            with open(os.path.join(HERE, "dumps", "p%d.%d.%s" % (p, f, k)), "wb") as out:
                out.write(data)
            self.send_response(204)
            self.end_headers()
            return
        if u.path == "/profile":
            n = int(self.headers.get("Content-Length", "0"))
            name = urllib.parse.parse_qs(u.query).get("name", [""])[0]
            if n > PROFILE_MAX:
                self.send_error(413)
                return
            try:
                ok = profile_write(name, self.rfile.read(n))
            except (ValueError, UnicodeDecodeError, OSError):
                ok = False
            self.send_json({"ok": ok}, 200 if ok else 400)
            return
        if u.path == "/lobby/cmd":
            n = int(self.headers.get("Content-Length", "0"))
            cid = urllib.parse.parse_qs(u.query).get("cid", [""])[0][:64]
            try:
                cmd = json.loads(self.rfile.read(min(n, 4096)).decode("utf-8"))
                ok = bool(cid) and LOBBIES.command(cid, cmd)
            except (ValueError, KeyError, TypeError):
                ok = False
            self.send_json({"ok": ok}, 200 if ok else 400)
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
    ap.add_argument("--host", default="127.0.0.1", help="address to serve on (default: this PC only)")
    a = ap.parse_args()
    if not os.path.isfile(a.disc):
        sys.exit("disc image not found: " + a.disc)
    Handler.disc = os.path.abspath(a.disc)
    class Server(http.server.ThreadingHTTPServer):
        # several players' pages load at once: the default queue (5) refused some connections
        request_queue_size = 128
    srv = Server((a.host, a.port), Handler)
    srv.daemon_threads = True
    print("Open http://localhost:%d/ in Chrome or Edge (Ctrl+C to stop)" % a.port, flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
