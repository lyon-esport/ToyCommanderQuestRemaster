// Toy Commander browser version: the page's network link for netplay.
// A web page has no UDP: the core's GGPO datagrams go over one WebSocket to the local server
// (tcvr/web/netbridge.py), which sends them on the LAN from a UDP socket it opened for this page.
// The core (core/deps/ggpo/lib/ggpo/network/udp.cpp) calls bind(port) and send(message), and reads q.
// Each entry of q: 4 bytes of sender address, 2 of port (network order), then the datagram.
(function () {
  const net = {
    q: [],
    port: 0,		// this page's UDP port on the LAN, once the server opened it (0 before, or if it failed)
    address: '',	// the server PC's LAN address: other players reach this page at address:port
    ws: null,
    pending: [],
    onbound: null,

    bind(port) {
      this.wanted = port;
      this.open();
      this._send(new Uint8Array([0x01, port >> 8, port & 255]));
    },

    send(message) {	// [0x02, address(4), port(2), payload]
      this._send(message);
    },

    open() {
      if (this.ws) return;
      const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/net';
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => { for (const m of this.pending) ws.send(m); this.pending = []; };
      ws.onmessage = (e) => {
        const m = new Uint8Array(e.data);
        if (m[0] === 0x82)
          this.q.push(m.subarray(1));
        else if (m[0] === 0x81) {
          this.port = (m[1] << 8) | m[2];
          this.address = m[3] + '.' + m[4] + '.' + m[5] + '.' + m[6];
          console.log('[tcvrNet] ' + (this.port ? 'playing on ' + this.address + ':' + this.port : 'UDP port ' + this.wanted + ' not available'));
          if (this.onbound) this.onbound(this.port, this.address);
        }
      };
      ws.onclose = () => { console.warn('[tcvrNet] connection to the server closed'); this.ws = null; };
      this.ws = ws;
    },

    _send(m) {
      if (this.ws && this.ws.readyState === 1) this.ws.send(m);
      else this.pending.push(m);
    },
  };
  // a long queue means the game stopped reading (paused tab): old datagrams are useless to GGPO
  setInterval(() => { if (net.q.length > 2000) net.q.splice(0, net.q.length - 500); }, 1000);
  window.tcvrNet = net;
})();
