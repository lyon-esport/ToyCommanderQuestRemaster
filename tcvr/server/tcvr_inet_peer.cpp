/*
	tcvr-inet-peer: a test player for internet play, without the emulator. It runs the real lobby
	(tcvr_lobby) through the real internet client (tcvr_inet) against a tcvr-server, and plays a fake
	match: 60 packets a second to every other player through the same local UDP ports GGPO would use,
	each answered, so it can tell how many arrived and the round trip. Used by tcvr/tools/inet-nat-test.sh.

	  tcvr-inet-peer --server HOST[:PORT] --name AAA (--host | --join) [--players N] [--seconds S]
	                 [--password P] [--force-relay] [--no-lan] [--leave-at-full] [--timeout S]

	--host           create the game; start it when N players (with this one) are there and ready
	--join           join the first game the server lists; become host and start it if the host leaves
	--players N      how many players start the match (default 2)
	--leave-at-full  (host) leave the lobby as soon as N players are in: tests the host handover
	--seconds S      length of the fake match (default 20)

	Last line printed: RESULT name=... role=... ok=0|1 then per other player: sent, received, lost %, rtt.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "network/tcvr_inet.h"
#include "network/tcvr_lobby.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

using namespace tcvr;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

namespace
{

void say(const std::string& name, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::printf("[%s] %s\n", name.c_str(), buf);
	std::fflush(stdout);
}

struct PeerStats
{
	std::string name;
	uint32_t sent = 0;
	uint32_t got = 0;		// its packets that reached us
	uint32_t pongs = 0;		// answers to ours
	double rttSum = 0;
	int rttMax = 0;
};

uint32_t nowUs()
{
	return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

}

int main(int argc, char **argv)
{
	inet::ClientConfig cfg;
	cfg.appVersion = "inet-test";
	bool hostMode = false, joinMode = false, leaveAtFull = false;
	int players = 2;
	int seconds = 20;
	int timeout = 90;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--server") cfg.server = next();
		else if (a == "--name") cfg.name = next();
		else if (a == "--password") cfg.password = next();
		else if (a == "--host") hostMode = true;
		else if (a == "--join") joinMode = true;
		else if (a == "--players") players = std::atoi(next().c_str());
		else if (a == "--seconds") seconds = std::atoi(next().c_str());
		else if (a == "--timeout") timeout = std::atoi(next().c_str());
		else if (a == "--force-relay") cfg.forceRelay = true;
		else if (a == "--no-lan") cfg.noLan = true;
		else if (a == "--leave-at-full") leaveAtFull = true;
		else
		{
			std::fprintf(stderr, "unknown argument %s\n", a.c_str());
			return 2;
		}
	}
	if (cfg.server.empty() || cfg.name.empty() || hostMode == joinMode)
	{
		std::fprintf(stderr, "usage: see the top of tcvr_inet_peer.cpp\n");
		return 2;
	}
	const std::string me = cfg.name;
	auto client = std::make_shared<inet::InternetClient>();
	client->configure(cfg);
	lobby::Lobby lob;
	lob.setRouter(client);

	// the fake GGPO socket
	asio::io_context io;
	asio::ip::udp::socket ggpo(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
	ggpo.non_blocking(true);
	const uint16_t ggpoPort = ggpo.local_endpoint().port();

	lobby::Identity id;
	id.name = me;
	id.appVersion = "inet-test";
	id.gameId = "TEST";
	id.ggpoPort = ggpoPort;

	const auto deadline = Clock::now() + std::chrono::seconds(timeout);
	std::string line, lastLine;
	// online
	while (client->status(&line) != inet::Status::Online)
	{
		if (line != lastLine)
			say(me, "%s", (lastLine = line).c_str());
		if (Clock::now() > deadline || client->status() == inet::Status::KeyChanged || client->status() == inet::Status::Refused)
		{
			std::printf("RESULT name=%s ok=0 why=offline\n", me.c_str());
			return 1;
		}
		std::this_thread::sleep_for(milliseconds(100));
	}
	say(me, "%s, address %s", line.c_str(), client->localAddress().c_str());

	std::string error;
	bool left = false;
	if (hostMode)
	{
		lobby::Settings st;
		st.mode = 0;
		st.room = 3;
		if (!lob.host(id, st, error))
		{
			std::printf("RESULT name=%s ok=0 why=host:%s\n", me.c_str(), error.c_str());
			return 1;
		}
		say(me, "hosting");
	}
	else
	{
		lob.startSearch();
		while (lob.games().empty())
		{
			if (Clock::now() > deadline)
			{
				std::printf("RESULT name=%s ok=0 why=no-game\n", me.c_str());
				return 1;
			}
			std::this_thread::sleep_for(milliseconds(200));
		}
		const auto g = lob.games().front();
		lob.stopSearch();
		say(me, "joining %s's game at %s", g.beacon.hostName.c_str(), g.address.c_str());
		lob.join(id, g.address, g.beacon.lobbyPort);
	}

	// the lobby, until START
	bool readySent = false;
	lobby::Role lastRole = lobby::Role::None;
	std::string lastMsg;
	int lastCount = 0;
	while (true)
	{
		const lobby::View v = lob.view();
		if (v.role != lastRole || v.message != lastMsg || v.state.count() != lastCount)
		{
			lastRole = v.role;
			lastMsg = v.message;
			lastCount = v.state.count();
			say(me, "%s, %d players%s%s", v.role == lobby::Role::Host ? "host" : "joiner", lastCount,
					v.message.empty() ? "" : ": ", v.message.c_str());
		}
		if (v.phase == lobby::Phase::Started)
			break;
		if (v.phase == lobby::Phase::Refused || v.phase == lobby::Phase::HostLeft || v.phase == lobby::Phase::Failed)
		{
			std::printf("RESULT name=%s ok=0 why=lobby:%s\n", me.c_str(), v.message.c_str());
			return 1;
		}
		if (v.phase == lobby::Phase::InLobby && v.role == lobby::Role::Joiner && !readySent)
		{
			lob.setReady(true);
			readySent = true;
		}
		if (v.phase == lobby::Phase::Connecting)
			readySent = false;		// (rejoining a new host: ready again)
		if (v.phase == lobby::Phase::InLobby && v.role == lobby::Role::Host)
		{
			if (leaveAtFull && v.state.count() >= players)
			{
				say(me, "leaving the lobby (handover test)");
				std::this_thread::sleep_for(milliseconds(1500));
				lob.leave();
				left = true;
				break;
			}
			std::string why;
			if (v.state.count() >= players && lob.canStart(why))
			{
				say(me, "starting the match");
				if (!lob.start(2, error))
					say(me, "start: %s", error.c_str());
			}
		}
		if (Clock::now() > deadline)
		{
			std::printf("RESULT name=%s ok=0 why=lobby-timeout\n", me.c_str());
			return 1;
		}
		std::this_thread::sleep_for(milliseconds(100));
	}
	if (left)
	{
		std::this_thread::sleep_for(milliseconds(3000));
		std::printf("RESULT name=%s role=left ok=1\n", me.c_str());
		return 0;
	}

	// the fake match
	const lobby::View v = lob.view();
	const std::string hostAddress = v.role == lobby::Role::Host ? client->localAddress() : v.hostAddress;
	const std::string list = client->ggpoPeers(v.start, hostAddress, ggpoPort);
	say(me, "match: player %d of %d, GGPO peers %s", v.start.localIndex + 1, (int)v.start.peers.size(), list.c_str());
	if (list.empty())
	{
		std::printf("RESULT name=%s ok=0 why=no-peers\n", me.c_str());
		return 1;
	}
	std::vector<std::string> names;
	for (const lobby::PlayerInfo& p : v.state.players)
		if (p.present)
			names.push_back(p.name);
	std::map<uint16_t, PeerStats> stats;		// by proxy port
	{
		size_t start = 0;
		int k = 0;
		while (start < list.size())
		{
			size_t end = list.find(',', start);
			if (end == std::string::npos)
				end = list.size();
			const std::string item = list.substr(start, end - start);
			const uint16_t port = (uint16_t)std::atoi(item.substr(item.rfind(':') + 1).c_str());
			if (k != v.start.localIndex)
				stats[port].name = k < (int)names.size() ? names[k] : "?";
			start = end + 1;
			k++;
		}
	}
	const auto matchEnd = Clock::now() + std::chrono::seconds(seconds);
	auto nextSend = Clock::now();
	uint32_t seq = 0;
	uint8_t buf[1500];
	std::vector<inet::PeerPath> paths;
	while (Clock::now() < matchEnd + milliseconds(1500))
	{
		const auto now = Clock::now();
		if (paths.empty() && now >= matchEnd - milliseconds(500))
			paths = client->paths();		// (before the others leave the game)
		if (now >= nextSend && now < matchEnd)
		{
			nextSend += milliseconds(16);
			seq++;
			for (auto& [port, s] : stats)
			{
				uint8_t pkt[48] {};
				pkt[0] = 'P';
				const uint32_t t = nowUs();
				std::memcpy(pkt + 1, &seq, 4);
				std::memcpy(pkt + 5, &t, 4);
				asio::error_code ec;
				ggpo.send_to(asio::buffer(pkt), asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port), 0, ec);
				s.sent++;
			}
		}
		asio::ip::udp::endpoint from;
		asio::error_code ec;
		const size_t n = ggpo.receive_from(asio::buffer(buf), from, 0, ec);
		if (!ec && n == 48)
		{
			auto it = stats.find(from.port());
			if (it != stats.end())
			{
				if (buf[0] == 'P')
				{
					it->second.got++;
					buf[0] = 'R';
					ggpo.send_to(asio::buffer(buf, n), from, 0, ec);
				}
				else if (buf[0] == 'R')
				{
					uint32_t t;
					std::memcpy(&t, buf + 5, 4);
					const int rtt = (int)((nowUs() - t) / 1000);
					it->second.pongs++;
					it->second.rttSum += rtt;
					it->second.rttMax = std::max(it->second.rttMax, rtt);
				}
			}
			continue;
		}
		std::this_thread::sleep_for(milliseconds(1));
	}
	bool ok = true;
	std::string out = "RESULT name=" + me + (v.role == lobby::Role::Host ? " role=host" : " role=joiner");
	std::string detail;
	for (auto& [port, s] : stats)
	{
		const double lost = s.sent ? 100.0 * (1.0 - (double)s.pongs / s.sent) : 100.0;
		std::string path = "?";
		for (const inet::PeerPath& p : paths)
			if (p.name == s.name)
				path = p.direct ? "direct" : "relay";
		char b[256];
		snprintf(b, sizeof(b), " | %s path=%s sent=%u got=%u answered=%u lost=%.1f%% rtt_avg=%.0fms rtt_max=%dms", s.name.c_str(),
				path.c_str(), s.sent, s.got, s.pongs, lost, s.pongs ? s.rttSum / s.pongs : -1.0, s.rttMax);
		detail += b;
		if (s.got < s.sent / 2 || s.pongs < s.sent / 2)
			ok = false;
	}
	std::printf("%s ok=%d%s\n", out.c_str(), ok ? 1 : 0, detail.c_str());
	client->matchOver();
	lob.matchOver();
	lob.leave();
	return ok ? 0 : 1;
}
