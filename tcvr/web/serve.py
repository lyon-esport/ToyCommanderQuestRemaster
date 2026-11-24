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

def lan_addresses():
    """This computer's IPv4 addresses that other devices on the network can use."""
    import socket
    found = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))	# no packet is sent: picks the interface of the default route
        found.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in found and not ip.startswith("127."):
                found.append(ip)
    except OSError:
        pass
    return found

def make_certificate(folder, addresses):
    """A self-signed certificate for this computer's addresses (HTTPS lets browsers use gamepads).
    Made once, kept in cert/ next to this file; made again when the addresses change."""
    import datetime, ipaddress, socket
    cert, key = os.path.join(folder, "cert.pem"), os.path.join(folder, "key.pem")
    names = os.path.join(folder, "addresses.txt")
    want = ",".join(sorted(set(addresses + ["127.0.0.1"])))
    if os.path.isfile(cert) and os.path.isfile(key) and os.path.isfile(names) and open(names).read() == want:
        return cert, key
    try:
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        return None
    k = ec.generate_private_key(ec.SECP256R1())
    host = socket.gethostname()
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Toy Commander local server")])
    san = [x509.DNSName("localhost"), x509.DNSName(host)] + [x509.IPAddress(ipaddress.ip_address(ip)) for ip in want.split(",")]
    now = datetime.datetime.now(datetime.timezone.utc)
    c = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(k.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
         .not_valid_after(now + datetime.timedelta(days=3650))
         .add_extension(x509.SubjectAlternativeName(san), critical=False)
         .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
         .sign(k, hashes.SHA256()))
    os.makedirs(folder, exist_ok=True)
    with open(key, "wb") as f:
        f.write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    with open(cert, "wb") as f:
        f.write(c.public_bytes(serialization.Encoding.PEM))
    with open(names, "w") as f:
        f.write(want)
    return cert, key

def main():
    import ssl, threading
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True, help="your own Toy Commander (Europe) disc image (.chd)")
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--https-port", type=int, default=8643, help="HTTPS port (0: no HTTPS)")
    ap.add_argument("--host", default="127.0.0.1", help="address to serve on (default: this PC only; 0.0.0.0: the whole network)")
    a = ap.parse_args()
    if not os.path.isfile(a.disc):
        sys.exit("disc image not found: " + a.disc)
    Handler.disc = os.path.abspath(a.disc)
    class Server(http.server.ThreadingHTTPServer):
        # several players' pages load at once: the default queue (5) refused some connections
        request_queue_size = 128
        daemon_threads = True
    srv = Server((a.host, a.port), Handler)
    addresses = lan_addresses() if a.host == "0.0.0.0" else []
    print("On this computer: http://localhost:%d/play.html (Ctrl+C to stop)" % a.port, flush=True)
    for ip in addresses:
        print("Other devices on the network: http://%s:%d/play.html" % (ip, a.port), flush=True)
    if a.https_port:
        made = make_certificate(os.path.join(HERE, "cert"), addresses)
        if made is None:
            print("No HTTPS (gamepads on tablets and phones need it): run  python -m pip install cryptography  and start again", flush=True)
        else:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(*made)
            tls = Server((a.host, a.https_port), Handler)
            tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
            threading.Thread(target=tls.serve_forever, daemon=True).start()
            for ip in addresses or ["localhost"]:
                print("With gamepads (the browser warns once about the certificate: choose Advanced, then Proceed): https://%s:%d/play.html" % (ip, a.https_port), flush=True)
    srv.serve_forever()

if __name__ == "__main__":
    main()
