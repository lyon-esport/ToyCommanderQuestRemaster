/*
	tcvr-server: the internet meeting point for Toy Commander VR multiplayer.

	It does not run the game. It
	- lists the games that players host (what the lobby announces),
	- introduces the players of a game to each other (their public UDP addresses, so they can reach each
	  other directly through their home routers),
	- passes the lobby's connections along, and the match's packets when no direct path works ("relay").
	What players send each other is encrypted end to end (tcvr_inet_proto.h): the server cannot read it.

	One TCP and one UDP port, the same number (default 19800). Its long-term key lives in a key file made
	on the first run; the games show its fingerprint and remember it for the server's name.

	Usage: tcvr-server [--port N] [--key FILE] [--max-clients N] [--relay-kbps N] [--verbose] [--status FILE]

	--status FILE: the server writes its games and connected players there as JSON every 2 seconds (for the
	admin page of tcvr/web/tcvr_host.py, which starts it).

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "network/tcvr_inet_proto.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace tcvr::inet;
using asio::ip::tcp;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;

#ifndef TCVR_SERVER_VERSION
#define TCVR_SERVER_VERSION "1.2"
#endif

namespace
{

bool verbose = false;

void logLine(const char *fmt, ...)
{
	char when[32];
	const std::time_t t = std::time(nullptr);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	std::printf("%s  ", when);
	va_list ap;
	va_start(ap, fmt);
	std::vprintf(fmt, ap);
	va_end(ap);
	std::printf("\n");
	std::fflush(stdout);
}

struct Options
{
	uint16_t port = DefaultServerPort;
	std::string keyFile = "tcvr-server.key";
	size_t maxClients = 200;
	size_t relayBytesPerSecond = 256 * 1024;	// per client; a 4-player match needs about 20 KB/s
	int idleSeconds = 30;						// no frame for this long (clients ping every 5 s): dropped
	GamePolicy policy = GamePolicy::All;		// which games players may host here
	int privateJoinsPerMinute = 10;				// per address: tries to join private games (slows PIN guessing)
	std::string statusFile;						// games and players as JSON, for an admin page ("" = none)
};

// JSON string (names come from players: escape everything that isn't plain printable ASCII)
std::string jsonText(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char ch : s)
	{
		if (ch == '"' || ch == '\\')
		{
			out += '\\';
			out += (char)ch;
		}
		else if (ch < 0x20 || ch >= 0x7f)
		{
			char u[8];
			std::snprintf(u, sizeof(u), "\\u%04x", ch);
			out += u;
		}
		else
			out += (char)ch;
	}
	return out + "\"";
}

// Replaces the file in one step, so a reader never sees half of it
bool writeFileAtomic(const std::string& path, const std::string& text)
{
	const std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out << text;
		if (!out)
			return false;
	}
#ifdef _WIN32
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// The server's long-term key: made on the first run, kept in a file (64 hex digits).
bool loadOrMakeKey(const std::string& path, KeyPair& kp)
{
	std::ifstream in(path);
	if (in)
	{
		std::string hex;
		in >> hex;
		Key secret;
		if (!fromHex(hex, secret.data(), secret.size()))
		{
			logLine("The key file %s is damaged. Move it away to make a new key (players then trust the new one again).", path.c_str());
			return false;
		}
		kp = KeyPair::fromSecret(secret);
		return true;
	}
	kp = KeyPair::generate();
	std::ofstream out(path, std::ios::trunc);
	if (!out)
	{
		logLine("Can't write the key file %s", path.c_str());
		return false;
	}
	out << toHex(kp.secret.data(), kp.secret.size()) << "\n";
	out.close();
#ifndef _WIN32
	chmod(path.c_str(), 0600);
#endif
	logLine("New server key made: %s", path.c_str());
	return true;
}

uint32_t toHostIp(const asio::ip::address& a)
{
	if (a.is_v4())
		return a.to_v4().to_uint();
	if (a.is_v6() && a.to_v6().is_v4_mapped())
		return asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()).to_uint();
	return 0;
}

class Server;

struct Client : std::enable_shared_from_this<Client>
{
	Client(asio::io_context& io, Server& server) : sock(io), server(server) {}
	tcp::socket sock;
	Server& server;
	uint32_t id = 0;
	bool ready = false;			// handshake and Hello done
	bool closed = false;
	Channel channel;
	uint8_t hsBuf[ClientHelloSize];
	size_t hsGot = 0;
	uint8_t buf[4096];
	std::deque<std::vector<uint8_t>> out;
	size_t outBytes = 0;
	bool writing = false;
	Clock::time_point lastRx = Clock::now();
	std::string name;
	std::string appVersion;
	std::time_t since = std::time(nullptr);
	uint64_t relayBytes = 0;	// match packets the server passed on for it (no direct path)
	Key memberKey {};
	Key udpKey {};
	Endpoint tcpFrom;
	Endpoint udp;				// its registered UDP address
	std::vector<Endpoint> lanAddrs;
	uint32_t game = 0;
	// relay budget (token bucket)
	double tokens = 0;
	Clock::time_point tokensAt = Clock::now();
	std::string label() const { return "#" + std::to_string(id) + (name.empty() ? "" : " " + name); }
};
using ClientPtr = std::shared_ptr<Client>;

struct Game
{
	uint32_t id = 0;
	uint32_t owner = 0;			// 0: nobody hosts it right now (the host left; the next one announces)
	std::set<uint32_t> members;
	std::vector<uint8_t> beacon;
	bool listed = false;
	bool isPrivate = false;
	std::time_t since = std::time(nullptr);
};

class Server
{
public:
	Server(asio::io_context& io, const Options& opt, const KeyPair& key)
		: io(io), opt(opt), key(key), acceptor(io), udpSock(io), tick(io), started(std::time(nullptr)) {}

	// One server per port. On Windows SO_REUSEADDR lets a second copy bind the same port and steal
	// connections, so ask for exclusive use there; elsewhere SO_REUSEADDR only allows a quick restart.
	template<typename Sock>
	static void portOptions(Sock& sock, bool tcp, asio::error_code& ec)
	{
#ifdef _WIN32
		BOOL on = TRUE;
		::setsockopt(sock.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
		(void)tcp;
#else
		if (tcp)
			sock.set_option(asio::socket_base::reuse_address(true), ec);
#endif
	}

	bool start()
	{
		asio::error_code ec;
		// IPv6 socket that also takes IPv4 where the system allows; else IPv4 only
		acceptor.open(tcp::v6(), ec);
		if (!ec)
		{
			acceptor.set_option(asio::ip::v6_only(false), ec);
			portOptions(acceptor, true, ec);
			acceptor.bind(tcp::endpoint(tcp::v6(), opt.port), ec);
		}
		if (ec)
		{
			acceptor.close();
			ec.clear();
			acceptor.open(tcp::v4(), ec);
			portOptions(acceptor, true, ec);
			acceptor.bind(tcp::endpoint(tcp::v4(), opt.port), ec);
		}
		if (!ec)
			acceptor.listen(asio::socket_base::max_listen_connections, ec);
		if (ec)
		{
			logLine("Can't listen on TCP port %u (is another tcvr-server already running?): %s", opt.port, ec.message().c_str());
			return false;
		}
		udpSock.open(udp::v6(), ec);
		if (!ec)
		{
			udpSock.set_option(asio::ip::v6_only(false), ec);
			portOptions(udpSock, false, ec);
			udpSock.bind(udp::endpoint(udp::v6(), opt.port), ec);
		}
		if (ec)
		{
			udpSock.close();
			ec.clear();
			udpSock.open(udp::v4(), ec);
			portOptions(udpSock, false, ec);
			udpSock.bind(udp::endpoint(udp::v4(), opt.port), ec);
		}
		if (ec)
		{
			logLine("Can't listen on UDP port %u (is another tcvr-server already running?): %s", opt.port, ec.message().c_str());
			return false;
		}
		accept();
		receiveUdp();
		onTick();
		return true;
	}

	void stop()
	{
		asio::error_code ec;
		acceptor.close(ec);
		udpSock.close(ec);
		tick.cancel();
		for (auto& [id, c] : clients)
			close(c, nullptr);
	}

	void onFrames(const ClientPtr& c, std::vector<std::vector<uint8_t>>& frames)
	{
		for (auto& f : frames)
		{
			if (c->closed)
				return;
			handle(c, f);
		}
	}

	void disconnected(const ClientPtr& c, const char *why)
	{
		close(c, why);
	}

	void write(const ClientPtr& c, const std::vector<uint8_t>& plain)
	{
		if (c->closed)
			return;
		auto frame = c->channel.seal(plain);
		c->outBytes += frame.size();
		if (c->outBytes > 512 * 1024)
		{
			// it doesn't read: drop it rather than hold its backlog
			close(c, "too slow");
			return;
		}
		c->out.push_back(std::move(frame));
		flush(c);
	}

	asio::io_context& io;

private:
	const Options& opt;
	const KeyPair& key;
	tcp::acceptor acceptor;
	udp::socket udpSock;
	asio::steady_timer tick;
	std::map<uint32_t, ClientPtr> clients;		// by id, only once ready
	std::set<ClientPtr> pending;				// handshaking
	std::map<uint32_t, Game> games;
	std::map<uint32_t, std::deque<Clock::time_point>> privateJoins;	// by IPv4 address
	uint32_t nextClient = 1;
	uint32_t nextGame = 1;
	std::time_t started;
	int statusCountdown = 0;
	bool statusFailed = false;
	udp::endpoint udpFrom;
	uint8_t udpBuf[2048];

	void accept()
	{
		auto c = std::make_shared<Client>(io, *this);
		acceptor.async_accept(c->sock, [this, c](const asio::error_code& ec) {
			if (ec == asio::error::operation_aborted)
				return;
			if (!ec)
			{
				if (clients.size() + pending.size() >= opt.maxClients)
				{
					asio::error_code e2;
					c->sock.close(e2);
				}
				else
				{
					asio::error_code e2;
					c->sock.set_option(tcp::no_delay(true), e2);
					const auto ep = c->sock.remote_endpoint(e2);
					c->tcpFrom.ip = toHostIp(ep.address());
					c->tcpFrom.port = ep.port();
					pending.insert(c);
					readHandshake(c);
				}
			}
			accept();
		});
	}

	void readHandshake(const ClientPtr& c)
	{
		c->sock.async_read_some(asio::buffer(c->hsBuf + c->hsGot, ClientHelloSize - c->hsGot),
				[this, c](const asio::error_code& ec, size_t n) {
			if (ec || c->closed)
			{
				close(c, nullptr);
				return;
			}
			c->hsGot += n;
			if (c->hsGot < ClientHelloSize)
			{
				readHandshake(c);
				return;
			}
			std::vector<uint8_t> reply;
			uint8_t keys[64];
			if (!serverHandshake(c->hsBuf, ClientHelloSize, key, reply, keys))
			{
				close(c, nullptr);
				return;
			}
			c->channel.init(keys, true);
			std::memset(keys, 0, sizeof(keys));
			auto data = std::make_shared<std::vector<uint8_t>>(std::move(reply));
			asio::async_write(c->sock, asio::buffer(*data), [this, c, data](const asio::error_code& ec, size_t) {
				if (ec)
					close(c, nullptr);
			});
			c->lastRx = Clock::now();
			readLoop(c);
		});
	}

	void readLoop(const ClientPtr& c)
	{
		c->sock.async_read_some(asio::buffer(c->buf), [this, c](const asio::error_code& ec, size_t n) {
			if (ec || c->closed)
			{
				close(c, c->ready ? "left" : nullptr);
				return;
			}
			c->lastRx = Clock::now();
			std::vector<std::vector<uint8_t>> frames;
			const bool ok = c->channel.feed(c->buf, n, frames);
			onFrames(c, frames);
			if (!ok)
			{
				close(c, "bad frame");
				return;
			}
			if (!c->closed)
				readLoop(c);
		});
	}

	void flush(const ClientPtr& c)
	{
		if (c->writing || c->out.empty() || c->closed)
			return;
		c->writing = true;
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(c->out.front()));
		c->out.pop_front();
		asio::async_write(c->sock, asio::buffer(*data), [this, c, data](const asio::error_code& ec, size_t) {
			c->writing = false;
			c->outBytes -= data->size();
			if (ec)
			{
				close(c, "write failed");
				return;
			}
			flush(c);
		});
	}

	void close(const ClientPtr& c, const char *why)
	{
		if (c->closed)
			return;
		c->closed = true;
		asio::error_code ec;
		c->sock.shutdown(tcp::socket::shutdown_both, ec);
		c->sock.close(ec);
		pending.erase(c);
		if (c->ready)
		{
			leaveGame(c);
			clients.erase(c->id);
			if (why)
				logLine("%s %s (%zu connected)", c->label().c_str(), why, clients.size());
		}
		c->channel.wipe();
	}

	void fail(const ClientPtr& c, const std::string& text)
	{
		write(c, encodeError(text));
		// closed once written
		auto t = std::make_shared<asio::steady_timer>(io);
		t->expires_after(std::chrono::milliseconds(500));
		t->async_wait([this, c, t](const asio::error_code&) { close(c, "refused"); });
	}

	// ---- control messages

	void handle(const ClientPtr& c, const std::vector<uint8_t>& f)
	{
		if (f.empty())
			return;
		Reader r(f.data() + 1, f.size() - 1);
		const Ctl type = (Ctl)f[0];
		if (!c->ready)
		{
			HelloMsg h;
			if (type != Ctl::Hello || !decode(r, h))
			{
				close(c, nullptr);
				return;
			}
			if (h.protocol != ProtocolVersion)
			{
				fail(c, "This server speaks another version of the game");
				return;
			}
			while (nextClient == 0 || clients.count(nextClient) || nextClient >= (1u << 24))
				nextClient = nextClient >= (1u << 24) ? 1 : nextClient + 1;
			c->id = nextClient++;
			c->name = h.name;
			c->appVersion = h.appVersion;
			c->since = std::time(nullptr);
			c->memberKey = h.memberKey;
			c->lanAddrs = h.lanAddrs;
			randomBytes(c->udpKey.data(), c->udpKey.size());
			c->ready = true;
			pending.erase(c);
			clients[c->id] = c;
			WelcomeMsg w;
			w.clientId = c->id;
			w.udpKey = c->udpKey;
			w.udpPort = opt.port;
			w.seenAs = c->tcpFrom;
			w.policy = opt.policy;
			write(c, encode(w));
			logLine("%s connected from %s, app %s (%zu connected)", c->label().c_str(), c->tcpFrom.address().c_str(),
					h.appVersion.c_str(), clients.size());
			return;
		}
		switch (type)
		{
		case Ctl::List:
		{
			std::vector<GameEntry> list;
			for (auto& [id, g] : games)
				if (g.listed && g.owner != 0)
					list.push_back({ g.owner, g.isPrivate, g.beacon });
			write(c, encodeGames(list));
			break;
		}
		case Ctl::Announce:
		{
			const bool isPrivate = (r.u8() & 1) != 0;
			std::vector<uint8_t> beacon = r.blob(512);
			if (!r.done())
				return close(c, "bad announce");
			if (!policyAllows(opt.policy, isPrivate))
			{
				// (the game checks the policy from Welcome first; this one doesn't play by it)
				verboseLog("%s: a %s game refused (%s)", c->label().c_str(), isPrivate ? "private" : "public", policyText(opt.policy));
				break;
			}
			Game *g = c->game ? findGame(c->game) : nullptr;
			if (g == nullptr)
			{
				Game ng;
				ng.id = nextGame++;
				ng.members.insert(c->id);
				games[ng.id] = ng;
				c->game = ng.id;
				g = &games[ng.id];
				logLine("%s hosts game %u (%s)", c->label().c_str(), g->id, isPrivate ? "private" : "public");
			}
			const bool newOwner = g->owner != c->id;
			if (newOwner && g->owner != 0)
			{
				// another member hosts now (the host handover went its way; the old one's lobby is done)
				verboseLog("game %u: %s takes over from #%u", g->id, c->label().c_str(), g->owner);
			}
			g->owner = c->id;
			g->isPrivate = isPrivate;
			g->beacon = std::move(beacon);
			g->listed = true;
			if (newOwner)
				sendMembers(*g);
			break;
		}
		case Ctl::Unannounce:
			if (Game *g = c->game ? findGame(c->game) : nullptr)
				if (g->owner == c->id)
					g->listed = false;
			break;
		case Ctl::Connect:
		{
			const uint32_t target = r.u32();
			if (!r.done())
				return close(c, "bad connect");
			auto it = clients.find(target);
			Game *g = it != clients.end() && it->second->game ? findGame(it->second->game) : nullptr;
			if (g == nullptr)
			{
				write(c, encodeConnected(target, false, "The game is gone"));
				break;
			}
			if (c->game != g->id)
			{
				if (g->isPrivate && !allowPrivateJoin(c->tcpFrom.ip))
				{
					write(c, encodeConnected(target, false, "Too many tries: wait a minute"));
					break;
				}
				if (g->members.size() >= (size_t)MaxMembers)
				{
					write(c, encodeConnected(target, false, "The game is full"));
					break;
				}
				leaveGame(c);
				g = findGame(it->second->game);
				if (g == nullptr)
				{
					write(c, encodeConnected(target, false, "The game is gone"));
					break;
				}
				g->members.insert(c->id);
				c->game = g->id;
				logLine("%s joins game %u", c->label().c_str(), g->id);
				sendMembers(*g);
			}
			write(c, encodeConnected(target, true, ""));
			break;
		}
		case Ctl::LeaveGame:
			leaveGame(c);
			break;
		case Ctl::Stream:
		{
			StreamMsg m;
			if (!decode(r, m))
				return close(c, "bad stream");
			auto it = clients.find(m.peer);
			if (it == clients.end() || c->game == 0 || it->second->game != c->game)
			{
				// not in this player's game (any more): the stream is closed
				if (m.kind != StreamKind::Close)
				{
					StreamMsg back;
					back.peer = m.peer;
					back.streamId = m.streamId;
					back.kind = StreamKind::Close;
					write(c, encode(back));
				}
				break;
			}
			m.peer = c->id;
			write(it->second, encode(m));
			break;
		}
		case Ctl::Ping:
			write(c, encodeSimple(Ctl::Pong));
			break;
		case Ctl::Pong:
			break;
		default:
			close(c, "unknown message");
			break;
		}
	}

	template <typename... Args>
	void verboseLog(const char *fmt, Args... args)
	{
		if (verbose)
			logLine(fmt, args...);
	}

	// Joining a private game: at most privateJoinsPerMinute per address (each try costs a PIN guess)
	bool allowPrivateJoin(uint32_t ip)
	{
		auto& q = privateJoins[ip];
		const auto now = Clock::now();
		while (!q.empty() && now - q.front() > std::chrono::minutes(1))
			q.pop_front();
		if ((int)q.size() >= opt.privateJoinsPerMinute)
			return false;
		q.push_back(now);
		if (privateJoins.size() > 4096)
			privateJoins.erase(privateJoins.begin());
		return true;
	}

	Game *findGame(uint32_t id)
	{
		auto it = games.find(id);
		return it == games.end() ? nullptr : &it->second;
	}

	void leaveGame(const ClientPtr& c)
	{
		if (c->game == 0)
			return;
		Game *g = findGame(c->game);
		c->game = 0;
		if (g == nullptr)
			return;
		g->members.erase(c->id);
		if (g->owner == c->id)
		{
			g->owner = 0;
			g->listed = false;	// until the next host announces it
		}
		if (g->members.empty())
		{
			logLine("game %u closed", g->id);
			games.erase(g->id);
			return;
		}
		sendMembers(*g);
	}

	void sendMembers(const Game& g)
	{
		std::vector<MemberInfo> list;
		for (uint32_t id : g.members)
		{
			auto it = clients.find(id);
			if (it == clients.end())
				continue;
			const Client& c = *it->second;
			MemberInfo m;
			m.clientId = c.id;
			m.name = c.name;
			m.memberKey = c.memberKey;
			m.udp = c.udp;
			m.lanAddrs = c.lanAddrs;
			m.owner = g.owner == c.id;
			list.push_back(m);
		}
		const auto msg = encodeMembers(list);
		for (uint32_t id : g.members)
		{
			auto it = clients.find(id);
			if (it != clients.end())
				write(it->second, msg);
		}
	}

	// ---- UDP

	void receiveUdp()
	{
		udpSock.async_receive_from(asio::buffer(udpBuf), udpFrom, [this](const asio::error_code& ec, size_t n) {
			if (ec == asio::error::operation_aborted)
				return;
			if (!ec)
				onUdp(udpBuf, n, udpFrom);
			receiveUdp();
		});
	}

	void onUdp(const uint8_t *data, size_t n, const udp::endpoint& from)
	{
		UdpType type;
		uint32_t id;
		std::vector<uint8_t> payload;
		ClientPtr c;
		if (!openServerUdp(data, n, type, id, payload, [&](uint32_t cid) -> const Key * {
				auto it = clients.find(cid);
				if (it == clients.end())
					return nullptr;
				c = it->second;
				return &c->udpKey;
			}))
			return;
		Endpoint ep;
		ep.ip = toHostIp(from.address());
		ep.port = from.port();
		if (!ep.valid())
			return;
		switch (type)
		{
		case UdpType::Register:
		{
			Reader r(payload);
			const uint32_t seq = r.u32();
			if (!r.done())
				return;
			const bool changed = c->udp != ep;
			c->udp = ep;
			Writer w;
			w.u32(seq);
			put(w, ep);
			sendUdp(c, UdpType::Registered, w.b, from);
			if (changed)
			{
				verboseLog("%s UDP at %s", c->label().c_str(), ep.text().c_str());
				if (Game *g = c->game ? findGame(c->game) : nullptr)
					sendMembers(*g);
			}
			break;
		}
		case UdpType::Relay:
		{
			if (payload.size() < 4 + PeerOverhead)
				return;
			const uint32_t to = payload[0] | payload[1] << 8 | payload[2] << 16 | (uint32_t)payload[3] << 24;
			auto it = clients.find(to);
			if (it == clients.end() || c->game == 0 || it->second->game != c->game || !it->second->udp.valid())
				return;
			if (!spend(*c, payload.size()))
				return;
			c->relayBytes += payload.size();
			Writer w;
			w.u32(c->id);
			w.bytes(payload.data() + 4, payload.size() - 4);
			const Endpoint& t = it->second->udp;
			sendUdp(it->second, UdpType::Relayed, w.b, udpEndpoint(t, from));
			break;
		}
		default:
			break;
		}
	}

	// the target's endpoint in the family of our socket
	udp::endpoint udpEndpoint(const Endpoint& e, const udp::endpoint& like)
	{
		const auto v4 = asio::ip::address_v4(e.ip);
		if (like.address().is_v6())
			return udp::endpoint(asio::ip::make_address_v6(asio::ip::v4_mapped, v4), e.port);
		return udp::endpoint(v4, e.port);
	}

	bool spend(Client& c, size_t bytes)
	{
		const auto now = Clock::now();
		const double rate = (double)opt.relayBytesPerSecond;
		c.tokens = std::min(rate, c.tokens + rate * std::chrono::duration<double>(now - c.tokensAt).count());
		c.tokensAt = now;
		if (c.tokens < (double)bytes)
			return false;
		c.tokens -= (double)bytes;
		return true;
	}

	// ---- status for an admin page (--status FILE)

	void writeStatus()
	{
		std::string j = "{\"version\":" + jsonText(TCVR_SERVER_VERSION);
		j += ",\"protocol\":" + std::to_string(ProtocolVersion);
		j += ",\"port\":" + std::to_string(opt.port);
		j += ",\"games_policy\":" + jsonText(policyText(opt.policy));
		j += ",\"max_clients\":" + std::to_string(opt.maxClients);
		j += ",\"relay_kbps\":" + std::to_string(opt.relayBytesPerSecond / 1024);
		j += ",\"fingerprint\":" + jsonText(fingerprint(key.pub));
		j += ",\"started\":" + std::to_string((long long)started);
		j += ",\"now\":" + std::to_string((long long)std::time(nullptr));
		j += ",\"handshaking\":" + std::to_string(pending.size());
		j += ",\"clients\":[";
		bool first = true;
		for (auto& [id, c] : clients)
		{
			if (!first)
				j += ",";
			first = false;
			j += "{\"id\":" + std::to_string(id);
			j += ",\"name\":" + jsonText(c->name);
			j += ",\"app\":" + jsonText(c->appVersion);
			j += ",\"address\":" + jsonText(c->tcpFrom.address());
			j += ",\"udp\":" + jsonText(c->udp.valid() ? c->udp.text() : "");
			j += ",\"since\":" + std::to_string((long long)c->since);
			j += ",\"game\":" + std::to_string(c->game);
			j += ",\"relay_bytes\":" + std::to_string(c->relayBytes) + "}";
		}
		j += "],\"games\":[";
		first = true;
		for (auto& [id, g] : games)
		{
			if (!first)
				j += ",";
			first = false;
			j += "{\"id\":" + std::to_string(id);
			j += ",\"owner\":" + std::to_string(g.owner);
			j += std::string(",\"private\":") + (g.isPrivate ? "true" : "false");
			j += std::string(",\"listed\":") + (g.listed ? "true" : "false");
			j += ",\"since\":" + std::to_string((long long)g.since);
			j += ",\"members\":[";
			bool f2 = true;
			for (uint32_t m : g.members)
			{
				if (!f2)
					j += ",";
				f2 = false;
				j += std::to_string(m);
			}
			j += "],\"beacon\":" + jsonText(toHex(g.beacon.data(), g.beacon.size())) + "}";
		}
		j += "]}\n";
		const bool ok = writeFileAtomic(opt.statusFile, j);
		if (!ok && !statusFailed)
			logLine("Can't write the status file %s", opt.statusFile.c_str());
		statusFailed = !ok;
	}

	void sendUdp(const ClientPtr& c, UdpType type, const std::vector<uint8_t>& payload, const udp::endpoint& to)
	{
		auto data = std::make_shared<std::vector<uint8_t>>(sealServerUdp(type, c->id, payload, c->udpKey));
		udpSock.async_send_to(asio::buffer(*data), to, [data](const asio::error_code&, size_t) {});
	}

	// ---- housekeeping

	void onTick()
	{
		tick.expires_after(std::chrono::seconds(1));
		tick.async_wait([this](const asio::error_code& ec) {
			if (ec)
				return;
			const auto now = Clock::now();
			std::vector<ClientPtr> idle;
			for (auto& [id, c] : clients)
				if (now - c->lastRx > std::chrono::seconds(opt.idleSeconds))
					idle.push_back(c);
			for (const ClientPtr& c : pending)
				if (now - c->lastRx > std::chrono::seconds(10))
					idle.push_back(c);
			for (const ClientPtr& c : idle)
				close(c, "timed out");
			if (!opt.statusFile.empty() && --statusCountdown <= 0)
			{
				statusCountdown = 2;
				writeStatus();
			}
			onTick();
		});
	}
};

void usage()
{
	std::printf(
		"tcvr-server %s: the internet meeting point for Toy Commander VR multiplayer.\n"
		"\n"
		"  --port N          TCP and UDP port (default %u). Open both in the firewall and forward them on the router.\n"
		"  --key FILE        the server's key file (default tcvr-server.key, made on the first run)\n"
		"  --max-clients N   at most this many connected players (default 200)\n"
		"  --relay-kbps N    relay budget per player in KB/s (default 256)\n"
		"  --games KIND      which games players may host: all (default), public or private\n"
		"  --verbose         more log lines\n"
		"  --status FILE     write the games and players there as JSON every 2 s (for an admin page)\n"
		"  --fingerprint     print the key's fingerprint and exit\n",
		TCVR_SERVER_VERSION, DefaultServerPort);
}

}

#ifndef TCVR_SERVER_NO_MAIN
int main(int argc, char **argv)
{
	Options opt;
	bool printFingerprint = false;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		auto value = [&]() -> std::string {
			if (i + 1 >= argc)
			{
				usage();
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--port")
			opt.port = (uint16_t)std::atoi(value().c_str());
		else if (a == "--key")
			opt.keyFile = value();
		else if (a == "--max-clients")
			opt.maxClients = (size_t)std::max(1, std::atoi(value().c_str()));
		else if (a == "--relay-kbps")
			opt.relayBytesPerSecond = (size_t)std::max(1, std::atoi(value().c_str())) * 1024;
		else if (a == "--verbose")
			verbose = true;
		else if (a == "--status")
			opt.statusFile = value();
		else if (a == "--games")
		{
			const std::string v = value();
			if (v == "all") opt.policy = GamePolicy::All;
			else if (v == "public") opt.policy = GamePolicy::PublicOnly;
			else if (v == "private") opt.policy = GamePolicy::PrivateOnly;
			else { usage(); return 2; }
		}
		else if (a == "--fingerprint")
			printFingerprint = true;
		else if (a == "--help" || a == "-h" || a == "/?")
		{
			usage();
			return 0;
		}
		else if (a == "--version")
		{
			std::printf("tcvr-server %s (protocol %u)\n", TCVR_SERVER_VERSION, ProtocolVersion);
			return 0;
		}
		else
		{
			usage();
			return 2;
		}
	}
	if (opt.port == 0)
	{
		usage();
		return 2;
	}
	KeyPair key;
	try {
		if (!loadOrMakeKey(opt.keyFile, key))
			return 1;
	} catch (const std::exception& e) {
		logLine("%s", e.what());
		return 1;
	}
	if (printFingerprint)
	{
		std::printf("%s\n", fingerprint(key.pub).c_str());
		return 0;
	}
	asio::io_context io;
	Server server(io, opt, key);
	if (!server.start())
		return 1;
	logLine("tcvr-server %s on port %u (TCP and UDP), %s. Key fingerprint: %s", TCVR_SERVER_VERSION, opt.port,
			policyText(opt.policy), fingerprint(key.pub).c_str());
	logLine("Players enter this computer's public IP address or DNS name in the VR menu (Multiplayer > Server).");
	asio::signal_set signals(io, SIGINT, SIGTERM);
	signals.async_wait([&](const asio::error_code&, int) {
		logLine("Stopping");
		server.stop();
		io.stop();
	});
	io.run();
	key.wipe();
	return 0;
}
#endif
