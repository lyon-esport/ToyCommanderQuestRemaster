#!/bin/sh
# Builds and installs tcvr-server as a systemd service on Debian (or Ubuntu). Run from the repository:
#   sudo tcvr/server/debian/install.sh [port]
# Then: systemctl status tcvr-server; journalctl -u tcvr-server -f (shows the key fingerprint).
# Open TCP and UDP <port> (default 19800) in the firewall and forward them on the router if any.
set -eu
PORT=${1:-19800}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }

apt-get install -y --no-install-recommends g++ cmake make
BUILD=$(mktemp -d)
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DTCVR_INET_PEER=OFF
cmake --build "$BUILD" --parallel
install -m 0755 "$BUILD/tcvr-server" /usr/local/bin/tcvr-server
rm -rf "$BUILD"

id tcvr >/dev/null 2>&1 || useradd --system --home-dir /var/lib/tcvr-server --shell /usr/sbin/nologin tcvr
install -d -o tcvr -g tcvr -m 0700 /var/lib/tcvr-server
if [ ! -f /etc/default/tcvr-server ]; then
	printf 'TCVR_PORT=%s\nTCVR_ARGS=\n' "$PORT" > /etc/default/tcvr-server
fi
install -m 0644 "$HERE/tcvr-server.service" /etc/systemd/system/tcvr-server.service
systemctl daemon-reload
systemctl enable --now tcvr-server
sleep 1
systemctl --no-pager status tcvr-server | head -5 || true
echo
echo "Key fingerprint (the headsets show it in Multiplayer > Server):"
sudo -u tcvr /usr/local/bin/tcvr-server --key /var/lib/tcvr-server/tcvr-server.key --fingerprint
if command -v ufw >/dev/null 2>&1; then
	echo "ufw found: allow the port with   ufw allow $PORT/tcp && ufw allow $PORT/udp"
fi
