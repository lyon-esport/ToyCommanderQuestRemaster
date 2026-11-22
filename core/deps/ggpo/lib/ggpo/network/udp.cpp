/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "ggpo_types.h"
#include "udp.h"
#include <cstdlib>
#include <cstring>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
// tcvr: the browser build. A web page has no UDP: the datagrams go through the page's WebSocket to the local
// server (tcvr/web/netbridge.py), which sends them on the LAN from a UDP socket of its own for this page.
// The page side is window.tcvrNet (tcvr/web/page): bind(port), send(bytes), and q, the datagrams that
// arrived, each one 4 bytes of sender address and 2 of port (both as in a sockaddr_in) then the payload.

Udp::Udp() : _socket(INVALID_SOCKET), _callbacks(NULL) { }

Udp::~Udp(void) { }

void
Udp::Init(uint16 port, Poll *poll, Callbacks *callbacks)
{
   _callbacks = callbacks;
   poll->RegisterLoop(this);
   Log("binding udp port %d through the page.", port);
   int ok = EM_ASM_INT({
      if (!window.tcvrNet) return 0;
      window.tcvrNet.bind($0);
      return 1;
   }, port);
   if (!ok)
      throw GGPOException("No network in this page", GGPO_ERRORCODE_NETWORK_ERROR);
}

void
Udp::SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen)
{
   sockaddr_in *to = (sockaddr_in *)dst;
   EM_ASM({
      var m = new Uint8Array(7 + $3);
      m[0] = 2;
      m.set(HEAPU8.subarray($0, $0 + 4), 1);		// address, as stored
      m.set(HEAPU8.subarray($1, $1 + 2), 5);		// port, network order as stored
      m.set(HEAPU8.subarray($2, $2 + $3), 7);
      window.tcvrNet.send(m);
   }, &to->sin_addr, &to->sin_port, buffer, len);
}

bool
Udp::OnLoopPoll(void *cookie)
{
   uint8          recv_buf[MAX_UDP_PACKET_SIZE];
   sockaddr_in    recv_addr;

   for (;;) {
      memset(&recv_addr, 0, sizeof(recv_addr));
      recv_addr.sin_family = AF_INET;
      int len = EM_ASM_INT({
         var q = window.tcvrNet ? window.tcvrNet.q : null;
         if (!q || q.length == 0) return -1;
         var m = q.shift();
         var n = m.length - 6;
         if (n > $3) return 0;			// not one of ours: dropped
         HEAPU8.set(m.subarray(0, 4), $1);
         HEAPU8.set(m.subarray(4, 6), $2);
         HEAPU8.set(m.subarray(6), $0);
         return n;
      }, recv_buf, &recv_addr.sin_addr, &recv_addr.sin_port, (int)MAX_UDP_PACKET_SIZE);
      if (len < 0)
         break;
      if (len > 0)
         _callbacks->OnMsg(recv_addr, (UdpMsg *)recv_buf, len);
   }
   return true;
}

#else

SOCKET
CreateSocket(uint16 bind_port, int retries)
{
   SOCKET s;
   sockaddr_in sin;
   uint16 port;
   int optval = 1;

   s = socket(AF_INET, SOCK_DGRAM, 0);
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof optval);
   optval = 0;
   setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&optval, sizeof optval);

   // non-blocking...
#ifndef _WIN32
	fcntl(s, F_SETFL, O_NONBLOCK);
#else
	u_long iMode = 1;
	ioctlsocket(s, FIONBIO, &iMode);
#endif

   sin.sin_family = AF_INET;
   // TCVR_NET_LOOPBACK=1 (PC test copies that all talk over 127.0.0.1): listen on loopback only, so the
   // system firewall never asks about each new test copy
   const char *loop = getenv("TCVR_NET_LOOPBACK");
   sin.sin_addr.s_addr = htonl(loop != nullptr && loop[0] == '1' ? INADDR_LOOPBACK : INADDR_ANY);
   for (port = bind_port; port <= bind_port + retries; port++) {
      sin.sin_port = htons(port);
      if (bind(s, (sockaddr *)&sin, sizeof sin) == 0) {
         Log("Udp bound to port: %d.", port);
         return s;
      }
   }
   closesocket(s);
   return INVALID_SOCKET;
}

Udp::Udp() :
   _socket(INVALID_SOCKET),
   _callbacks(NULL)
{
}

Udp::~Udp(void)
{
   if (_socket != INVALID_SOCKET) {
      closesocket(_socket);
      _socket = INVALID_SOCKET;
   }
}

void
Udp::Init(uint16 port, Poll *poll, Callbacks *callbacks)
{
   _callbacks = callbacks;
   poll->RegisterLoop(this);

   Log("binding udp socket to port %d.", port);
   _socket = CreateSocket(port, 0);
   if (_socket == INVALID_SOCKET)
	   throw GGPOException("Socket creation or bind failed", GGPO_ERRORCODE_NETWORK_ERROR);
}

void
Udp::SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen)
{
   struct sockaddr_in *to = (struct sockaddr_in *)dst;

   int res = sendto(_socket, buffer, len, flags, dst, destlen);
   if (res == SOCKET_ERROR) {
	  int err = WSAGetLastError();
      Log("unknown error in sendto (erro: %d  wsaerr: %d).", res, err);
      ASSERT(false && "Unknown error in sendto");
   }
   char dst_ip[1024];
   Log("sent packet length %d to %s:%d (ret:%d).", len, inet_ntop(AF_INET, (void *)&to->sin_addr, dst_ip, ARRAY_SIZE(dst_ip)), ntohs(to->sin_port), res);
}

bool
Udp::OnLoopPoll(void *cookie)
{
   uint8          recv_buf[MAX_UDP_PACKET_SIZE];
   sockaddr_in    recv_addr;
   socklen_t      recv_addr_len;

   for (;;) {
      recv_addr_len = sizeof(recv_addr);
      int len = recvfrom(_socket, (char *)recv_buf, MAX_UDP_PACKET_SIZE, 0, (struct sockaddr *)&recv_addr, &recv_addr_len);

      // TODO: handle len == 0... indicates a disconnect.

      if (len == -1) {
         int error = WSAGetLastError();
         if (error != WSAEWOULDBLOCK) {
            Log("recvfrom WSAGetLastError returned %d (%x).", error, error);
         }
         break;
      } else if (len > 0) {
         char src_ip[1024];
         Log("recvfrom returned (len:%d  from:%s:%d).", len, inet_ntop(AF_INET, (void*)&recv_addr.sin_addr, src_ip, ARRAY_SIZE(src_ip)), ntohs(recv_addr.sin_port) );
         UdpMsg *msg = (UdpMsg *)recv_buf;
         _callbacks->OnMsg(recv_addr, msg, len);
      } 
   }
   return true;
}

#endif
