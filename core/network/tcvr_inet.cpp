/*
	tcvr: internet play, the game's side. See tcvr_inet.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "tcvr_inet.h"

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace tcvr::inet
{

using asio::ip::tcp;
using asio::ip::udp;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

bool splitServer(const std::string& text, std::string& host, uint16_t& port)
{
	std::string t = text;
	t.erase(0, t.find_first_not_of(" \t"));
	t.erase(t.find_last_not_of(" \t") + 1 > t.size() ? t.size() : t.find_last_not_of(" \t") + 1);
	port = DefaultServerPort;
	const size_t colon = t.rfind(':');
	if (colon != std::string::npos && t.find(':') == colon)	// one colon: host:port (no IPv6 literals)
	{
		const std::string p = t.substr(colon + 1);
		t = t.substr(0, colon);
		if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos)
			return false;
		const int v = std::atoi(p.c_str());
		if (v <= 0 || v > 65535)
			return false;
		port = (uint16_t)v;
	}
	host = t;
	return !host.empty();
}

namespace
{

constexpr int ReconnectMs = 3000;
constexpr int ServerPingMs = 5000;
constexpr int ServerSilentMs = 20000;	// no frame from the server for this long: reconnect
constexpr int ListMs = 1000;
constexpr int AnnounceMs = 5000;		// the same beacon again (a changed one goes at once)
constexpr int RegisterMs = 1000;		// until registered; then KeepAliveMs
constexpr int KeepAliveMs = 5000;
constexpr int PunchFastMs = 250;
constexpr int PunchSlowMs = 1000;
constexpr int PunchFastPhaseMs = 10000;
constexpr int DirectFreshMs = 3000;		// the other side heard our direct packets this recently: send direct
constexpr int AcceptTimeoutMs = 10000;
constexpr int GamesVisibleMs = 3000;
constexpr int PinRetryMs = 2000;		// after a wrong PIN, the same player waits this long before the next try (Manu)

enum PeerKind : uint8_t { Punch = 1, Ggpo = 2, Pong = 3 };

uint32_t nowMs()
{
	return (uint32_t)std::chrono::duration_cast<milliseconds>(Clock::now().time_since_epoch()).count();
}

}

struct InternetClient::Impl
{
	asio::io_context io;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard;
	std::thread thread;

	// ---- shared with the other threads (mutex)
	std::mutex mutex;
	Status st = Status::Off;
	std::string stLine;
	uint32_t myId = 0;
	std::string keyHex, keyPrint;
	std::vector<std::pair<Clock::time_point, lobby::FoundGame>> found;
	std::map<uint16_t, uint32_t> portOwner;		// local port of a connection we opened into the lobby -> player
	std::vector<PeerPath> pathList;
	std::function<void(const std::string&, const std::string&)> newKeyHook;
	GamePolicy serverPolicy = GamePolicy::All;
	std::string refusal;

	// ---- the io thread only
	ClientConfig cfg;
	bool configured = false;
	uint32_t generation = 0;		// bumps on every (re)connect: late callbacks of an old one do nothing
	tcp::resolver resolver;
	std::unique_ptr<tcp::socket> ctl;
	Channel channel;
	KeyPair ephemeral;
	KeyPair memberKp;
	uint8_t hsReply[ServerReplySize];
	uint8_t ctlBuf[4096];
	std::deque<std::vector<uint8_t>> ctlOut;
	bool ctlWriting = false;
	bool online = false;
	Clock::time_point ctlLastRx, ctlLastPing;
	asio::ip::address_v4 serverIp;
	Key udpKey {};
	udp::endpoint serverUdp;
	bool registered = false;
	uint32_t registerSeq = 0;
	Clock::time_point lastRegister;
	asio::steady_timer reconnectTimer;
	asio::steady_timer tickTimer;

	std::unique_ptr<udp::socket> udpSock;
	udp::endpoint udpFrom;
	uint8_t udpBuf[2048];

	bool searching = false;
	Clock::time_point lastList;
	std::vector<uint8_t> beacon;		// what we announce (empty: nothing)
	Clock::time_point lastAnnounce;
	std::set<uint16_t> lobbyPorts;		// where other players' streams may go

	struct Peer
	{
		MemberInfo info;
		PairKey pk;
		uint64_t udpSend = 0;
		ReplayWindow udpWin;
		uint64_t streamSend = 0;
		uint64_t streamRecv = 0;
		std::vector<Endpoint> base;		// from the server: its public address, then its LAN ones
		std::vector<Endpoint> cands;	// base, then addresses its packets came from that base doesn't have
		int sendCand = -1;				// the candidate it says our packets reach
		Clock::time_point heardMeAt;	// when it said so
		int heardIdx = -1;				// the candidate of ours its last direct punch came to
		Clock::time_point heardAt;
		Clock::time_point since = Clock::now();
		Clock::time_point lastPunch, lastRelayPunch;
		int rtt = -1;
		std::unique_ptr<udp::socket> proxy;	// GGPO's way to this player during a match
		uint8_t proxyBuf[2048];
		udp::endpoint proxyFrom;
	};
	std::map<uint32_t, std::unique_ptr<Peer>> peers;
	uint16_t localGgpoPort = 0;
	std::string pin;					// the game's PIN ("" public)
	struct PinTries
	{
		int wrong = 0;
		Clock::time_point until;		// no try before
	};
	std::map<uint32_t, PinTries> pinTries;	// by player
	struct Stream
	{
		explicit Stream(asio::io_context& io) : sock(io) {}
		uint32_t peer = 0;
		uint32_t sid = 0;
		bool mine = false;				// we opened it (the lobby connected to us)
		tcp::socket sock;
		std::unique_ptr<tcp::acceptor> acc;
		uint16_t remotePort = 0;		// mine: the port on the other side
		bool connected = false;			// the local socket is up
		bool open = false;				// mine: Open sent
		bool closing = false;			// the other side closed: close once written
		bool closed = false;
		std::deque<std::vector<uint8_t>> toLocal;
		bool writing = false;
		uint8_t buf[2048];
		Clock::time_point made = Clock::now();
	};
	using StreamPtr = std::shared_ptr<Stream>;
	using StreamKey = std::tuple<uint32_t, uint32_t, bool>;
	std::map<StreamKey, StreamPtr> streams;
	uint32_t nextSid = 1;
	std::map<uint32_t, std::vector<StreamPtr>> waitingConnect;		// target -> streams waiting for Connected

	Impl() : resolver(io), reconnectTimer(io), tickTimer(io)
	{
		guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io.get_executor());
		thread = std::thread([this] { io.run(); });
	}

	~Impl()
	{
		run([this] { shutdownAll(Status::Off, ""); });
		guard.reset();
		io.stop();
		if (thread.joinable())
			thread.join();
	}

	template <typename F>
	auto run(F f) -> decltype(f())
	{
		using R = decltype(f());
		if (std::this_thread::get_id() == thread.get_id())
			return f();
		std::packaged_task<R()> task(std::move(f));
		auto fut = task.get_future();
		asio::post(io, [&task] { task(); });
		return fut.get();
	}

	void setStatus(Status s, const std::string& line)
	{
		std::lock_guard<std::mutex> lock(mutex);
		st = s;
		stLine = line;
	}

	// ---- connection to the server

	void configure(const ClientConfig& c)
	{
		const bool same = configured && c.server == cfg.server && c.name == cfg.name && c.appVersion == cfg.appVersion
				&& c.udpPort == cfg.udpPort && c.forceRelay == cfg.forceRelay
				&& c.noLan == cfg.noLan && (c.pinnedKey == cfg.pinnedKey || c.pinnedKey.empty());
		if (same)
		{
			Status s;
			{
				std::lock_guard<std::mutex> lock(mutex);
				s = st;
			}
			if (s != Status::Failed)
				return;		// running, or stopped for a reason a new try won't change
		}
		shutdownAll(Status::Off, "");
		cfg = c;
		configured = true;
		startTick();
		connect();
	}

	void connect()
	{
		const uint32_t gen = ++generation;
		std::string host;
		uint16_t port;
		if (!splitServer(cfg.server, host, port))
		{
			setStatus(Status::Refused, "Enter the server's address (IP or DNS name)");
			return;
		}
		openUdp();
		setStatus(Status::Resolving, "Looking up " + host + "...");
		resolver.async_resolve(tcp::v4(), host, std::to_string(port),
				[this, gen, host](const asio::error_code& ec, tcp::resolver::results_type results) {
			if (gen != generation)
				return;
			if (ec || results.empty())
			{
				retryLater("Can't find the server " + host);
				return;
			}
			const tcp::endpoint ep = *results.begin();
			serverIp = ep.address().to_v4();
			setStatus(Status::Connecting, "Connecting to " + host + "...");
			ctl = std::make_unique<tcp::socket>(io);
			ctl->async_connect(ep, [this, gen, host](const asio::error_code& ec) {
				if (gen != generation)
					return;
				if (ec)
				{
					retryLater("Can't reach the server " + host);
					return;
				}
				asio::error_code e2;
				ctl->set_option(tcp::no_delay(true), e2);
				ephemeral = KeyPair::generate();
				auto hello = std::make_shared<std::vector<uint8_t>>(clientHello(ephemeral));
				asio::async_write(*ctl, asio::buffer(*hello), [this, gen, hello](const asio::error_code& ec, size_t) {
					if (gen == generation && ec)
						retryLater("The server closed the connection");
				});
				asio::async_read(*ctl, asio::buffer(hsReply), [this, gen, host](const asio::error_code& ec, size_t) {
					if (gen != generation)
						return;
					if (ec)
					{
						retryLater("The server closed the connection");
						return;
					}
					onHandshake(host);
				});
			});
		});
	}

	void onHandshake(const std::string& host)
	{
		Key serverKey;
		uint8_t keys[64];
		const bool ok = clientHandshake(ephemeral, hsReply, sizeof(hsReply), serverKey, keys);
		ephemeral.wipe();
		if (!ok)
		{
			stopWith(Status::Refused, host + " is not a Toy Commander VR server");
			return;
		}
		const std::string hex = toHex(serverKey.data(), serverKey.size());
		{
			std::lock_guard<std::mutex> lock(mutex);
			keyHex = hex;
			keyPrint = fingerprint(serverKey);
		}
		if (!cfg.pinnedKey.empty() && cfg.pinnedKey != hex)
		{
			stopWith(Status::KeyChanged, "The server's key changed (" + fingerprint(serverKey)
					+ "). If you trust it, enter the server address again.");
			return;
		}
		if (cfg.pinnedKey.empty())
		{
			cfg.pinnedKey = hex;
			std::function<void(const std::string&, const std::string&)> hook;
			{
				std::lock_guard<std::mutex> lock(mutex);
				hook = newKeyHook;
			}
			if (hook)
				hook(cfg.server, hex);
		}
		channel.init(keys, false);
		std::memset(keys, 0, sizeof(keys));
		memberKp = KeyPair::generate();
		HelloMsg h;
		h.appVersion = cfg.appVersion;
		h.name = cfg.name;
		h.memberKey = memberKp.pub;
		h.lanAddrs = lanEndpoints();
		ctlLastRx = Clock::now();
		ctlLastPing = Clock::now();
		send(encode(h));
		readCtl(generation);
	}

	std::vector<Endpoint> lanEndpoints()
	{
		std::vector<Endpoint> list;
		if (cfg.noLan || !udpSock)
			return list;
		asio::error_code ec;
		const uint16_t port = udpSock->local_endpoint(ec).port();
		std::vector<lobby::NetInterface> nifs;
		try {
			nifs = lobby::listInterfaces();
		} catch (...) {
		}
		std::sort(nifs.begin(), nifs.end(), [](const lobby::NetInterface& a, const lobby::NetInterface& b) {
			return lobby::lanScore(a) > lobby::lanScore(b);
		});
		for (const lobby::NetInterface& n : nifs)
		{
			Endpoint e;
			if (lobby::lanScore(n) < 0 || !Endpoint::parse(n.address, port, e))
				continue;
			if (std::find(list.begin(), list.end(), e) == list.end())
				list.push_back(e);
			if (list.size() >= MaxLanEndpoints)
				break;
		}
		return list;
	}

	void retryLater(const std::string& line)
	{
		const Status keep = Status::Failed;
		dropConnection();
		setStatus(keep, line + ". Trying again...");
		const uint32_t gen = generation;
		reconnectTimer.expires_after(milliseconds(ReconnectMs));
		reconnectTimer.async_wait([this, gen](const asio::error_code& ec) {
			if (!ec && gen == generation && configured)
				connect();
		});
	}

	void stopWith(Status s, const std::string& line)
	{
		dropConnection();
		++generation;
		setStatus(s, line);
	}

	// the server connection and everything that goes through it
	void dropConnection()
	{
		if (ctl)
		{
			asio::error_code ec;
			ctl->shutdown(tcp::socket::shutdown_both, ec);
			ctl->close(ec);
			ctl.reset();
		}
		resolver.cancel();
		channel.wipe();
		ctlOut.clear();
		ctlWriting = false;
		online = false;
		registered = false;
		for (auto& [k, s] : std::map<StreamKey, StreamPtr>(streams))
			closeStream(s, false);
		streams.clear();
		waitingConnect.clear();
		for (auto& [id, p] : peers)
			closePeer(*p);
		peers.clear();
		memberKp.wipe();
		std::lock_guard<std::mutex> lock(mutex);
		myId = 0;
		found.clear();
		portOwner.clear();
		pathList.clear();
	}

	void shutdownAll(Status s, const std::string& line)
	{
		++generation;
		configured = false;
		dropConnection();
		reconnectTimer.cancel();
		tickTimer.cancel();
		tickRunning = false;
		if (udpSock)
		{
			asio::error_code ec;
			udpSock->close(ec);
			udpSock.reset();
		}
		beacon.clear();
		setStatus(s, line);
	}

	void send(const std::vector<uint8_t>& plain)
	{
		if (!ctl)
			return;
		ctlOut.push_back(channel.seal(plain));
		flushCtl(generation);
	}

	void flushCtl(uint32_t gen)
	{
		if (ctlWriting || ctlOut.empty() || !ctl)
			return;
		ctlWriting = true;
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(ctlOut.front()));
		ctlOut.pop_front();
		asio::async_write(*ctl, asio::buffer(*data), [this, gen, data](const asio::error_code& ec, size_t) {
			if (gen != generation)
				return;
			ctlWriting = false;
			if (ec)
			{
				retryLater("Lost the server");
				return;
			}
			flushCtl(gen);
		});
	}

	void readCtl(uint32_t gen)
	{
		ctl->async_read_some(asio::buffer(ctlBuf), [this, gen](const asio::error_code& ec, size_t n) {
			if (gen != generation)
				return;
			if (ec)
			{
				retryLater("Lost the server");
				return;
			}
			ctlLastRx = Clock::now();
			std::vector<std::vector<uint8_t>> frames;
			const bool ok = channel.feed(ctlBuf, n, frames);
			for (auto& f : frames)
			{
				onCtl(f);
				if (gen != generation)
					return;
			}
			if (!ok)
			{
				retryLater("Bad data from the server");
				return;
			}
			readCtl(gen);
		});
	}

	void onCtl(const std::vector<uint8_t>& f)
	{
		if (f.empty())
			return;
		Reader r(f.data() + 1, f.size() - 1);
		switch ((Ctl)f[0])
		{
		case Ctl::Welcome:
		{
			WelcomeMsg w;
			if (!decode(r, w) || w.clientId == 0 || w.clientId >= (1u << 24))
			{
				retryLater("Bad data from the server");
				return;
			}
			udpKey = w.udpKey;
			{
				std::lock_guard<std::mutex> lock(mutex);
				serverPolicy = w.policy;
			}
			serverUdp = udp::endpoint(serverIp, w.udpPort);
			online = true;
			{
				std::lock_guard<std::mutex> lock(mutex);
				myId = w.clientId;
			}
			std::string host;
			uint16_t port;
			splitServer(cfg.server, host, port);
			setStatus(Status::Online, "Online: " + host + "   key " + keyPrintCopy());
			registerUdp();
			if (!beacon.empty())
				send(encodeAnnounce(beacon, !pin.empty()));
			if (searching)
				send(encodeSimple(Ctl::List));
			break;
		}
		case Ctl::Games:
		{
			std::vector<GameEntry> list;
			if (!decodeGames(r, list))
				break;
			const auto now = Clock::now();
			std::lock_guard<std::mutex> lock(mutex);
			found.clear();
			for (const GameEntry& g : list)
			{
				lobby::Beacon b;
				if (lobby::decodeDatagram(g.beacon.data(), g.beacon.size(), b) != lobby::Datagram::Beacon)
					continue;
				lobby::FoundGame fg;
				fg.hostId = b.hostId;
				fg.address = virtualAddress(g.ownerId);
				fg.beacon = b;
				fg.isPrivate = g.isPrivate;
				found.push_back({ now, fg });
			}
			break;
		}
		case Ctl::Members:
		{
			std::vector<MemberInfo> list;
			if (decodeMembers(r, list))
				onMembers(list);
			break;
		}
		case Ctl::Connected:
		{
			const uint32_t target = r.u32();
			const bool ok = r.u8() != 0;
			const std::string why = r.str();
			if (!ok && !why.empty())
			{
				std::lock_guard<std::mutex> lock(mutex);
				refusal = why;
			}
			auto it = waitingConnect.find(target);
			if (it == waitingConnect.end())
				break;
			std::vector<StreamPtr> list = std::move(it->second);
			waitingConnect.erase(it);
			for (const StreamPtr& s : list)
			{
				if (ok && peers.count(s->peer))
					sendOpen(s);
				else
					closeStream(s, false);
			}
			break;
		}
		case Ctl::Stream:
		{
			StreamMsg m;
			if (decode(r, m))
				onStream(m);
			break;
		}
		case Ctl::Error:
		{
			const std::string text = r.str();
			stopWith(Status::Refused, "The server says: " + text);
			break;
		}
		case Ctl::Ping:
			send(encodeSimple(Ctl::Pong));
			break;
		default:
			break;
		}
	}

	std::string keyPrintCopy()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return keyPrint;
	}

	// ---- the members of our game

	void onMembers(const std::vector<MemberInfo>& list)
	{
		uint32_t me;
		{
			std::lock_guard<std::mutex> lock(mutex);
			me = myId;
		}
		std::set<uint32_t> still;
		for (const MemberInfo& m : list)
		{
			if (m.clientId == me || m.clientId == 0)
				continue;
			still.insert(m.clientId);
			auto& slot = peers[m.clientId];
			if (!slot || slot->info.memberKey != m.memberKey)
			{
				if (slot)
				{
					dropPeerStreams(m.clientId);
					closePeer(*slot);
				}
				slot = std::make_unique<Peer>();
				slot->pk = derivePairKey(memberKp, m.memberKey, "");
			}
			Peer& p = *slot;
			p.info = m;
			std::vector<Endpoint> cands;
			if (m.udp.valid())
				cands.push_back(m.udp);
			for (const Endpoint& e : m.lanAddrs)
				if (e.valid() && std::find(cands.begin(), cands.end(), e) == cands.end())
					cands.push_back(e);
			if (cands != p.base)
			{
				p.base = cands;
				p.cands = cands;
				p.sendCand = -1;
				p.since = Clock::now();		// punch fast again
			}
		}
		for (auto it = peers.begin(); it != peers.end();)
		{
			if (!still.count(it->first))
			{
				dropPeerStreams(it->first);
				closePeer(*it->second);
				it = peers.erase(it);
			}
			else
				++it;
		}
		updatePaths();
	}

	void closePeer(Peer& p)
	{
		if (p.proxy)
		{
			asio::error_code ec;
			p.proxy->close(ec);
			p.proxy.reset();
		}
		p.pk.wipe();
	}

	void dropPeerStreams(uint32_t id)
	{
		for (auto& [k, s] : std::map<StreamKey, StreamPtr>(streams))
			if (s->peer == id)
				closeStream(s, false);
		waitingConnect.erase(id);
	}

	bool directUsable(const Peer& p) const
	{
		return !cfg.forceRelay && p.sendCand >= 0 && p.sendCand < (int)p.cands.size()
				&& Clock::now() - p.heardMeAt < milliseconds(DirectFreshMs);
	}

	void updatePaths()
	{
		std::vector<PeerPath> list;
		for (auto& [id, p] : peers)
			list.push_back({ id, p->info.name, directUsable(*p), p->rtt });
		std::lock_guard<std::mutex> lock(mutex);
		pathList = list;
	}

	// ---- lobby streams

	static uint32_t wireSid(const Stream& s)
	{
		return s.sid | (s.mine ? 0 : 0x80000000u);
	}

	void sendStream(const Stream& s, StreamKind kind, const std::vector<uint8_t>& plain)
	{
		auto it = peers.find(s.peer);
		if (it == peers.end() || !online)
			return;
		Peer& p = *it->second;
		StreamMsg m;
		m.peer = s.peer;
		m.streamId = wireSid(s);
		m.kind = kind;
		if (kind != StreamKind::Close)
		{
			uint32_t me;
			{
				std::lock_guard<std::mutex> lock(mutex);
				me = myId;
			}
			m.data = sealPeer(p.pk, 1, me, ++p.streamSend, plain);
		}
		send(encode(m));
	}

	void closeStream(const StreamPtr& s, bool tell)
	{
		if (s->closed)
			return;
		s->closed = true;
		if (tell)
			sendStream(*s, StreamKind::Close, {});
		asio::error_code ec;
		if (s->acc)
			s->acc->close(ec);
		s->sock.shutdown(tcp::socket::shutdown_both, ec);
		s->sock.close(ec);
		streams.erase(StreamKey(s->peer, s->sid, s->mine));
	}

	// the lobby asks for a connection to a player: a local port for it
	uint16_t route(const std::string& address, uint16_t port)
	{
		const uint32_t target = clientIdOf(address);
		uint32_t me;
		{
			std::lock_guard<std::mutex> lock(mutex);
			me = myId;
		}
		if (!online || target == 0 || target == me || port == 0)
			return 0;
		auto s = std::make_shared<Stream>(io);
		s->peer = target;
		s->sid = nextSid++ & 0x7fffffffu;
		s->mine = true;
		s->remotePort = port;
		try {
			s->acc = std::make_unique<tcp::acceptor>(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
		} catch (const std::exception&) {
			return 0;
		}
		const uint16_t local = s->acc->local_endpoint().port();
		streams[StreamKey(s->peer, s->sid, true)] = s;
		s->acc->async_accept(s->sock, [this, s](const asio::error_code& ec) {
			if (s->closed)
				return;
			asio::error_code e2;
			s->acc->close(e2);
			if (ec)
			{
				closeStream(s, false);
				return;
			}
			s->sock.set_option(tcp::no_delay(true), e2);
			s->connected = true;
			if (peers.count(s->peer))
				sendOpen(s);
			else
			{
				// not in its game yet: the server puts us there, then Open
				const bool first = waitingConnect[s->peer].empty();
				waitingConnect[s->peer].push_back(s);
				if (first)
					send(encodeConnect(s->peer));
			}
		});
		return local;
	}

	void sendOpen(const StreamPtr& s)
	{
		if (s->closed)
			return;
		Writer w;
		w.u16(s->remotePort);
		w.str(pin);
		sendStream(*s, StreamKind::Open, w.b);
		s->open = true;
		readLocal(s);
	}

	void readLocal(const StreamPtr& s)
	{
		s->sock.async_read_some(asio::buffer(s->buf), [this, s](const asio::error_code& ec, size_t n) {
			if (s->closed)
				return;
			if (ec)
			{
				closeStream(s, true);
				return;
			}
			sendStream(*s, StreamKind::Data, std::vector<uint8_t>(s->buf, s->buf + n));
			readLocal(s);
		});
	}

	void writeLocal(const StreamPtr& s)
	{
		if (s->writing || !s->connected || s->closed)
			return;
		if (s->toLocal.empty())
		{
			if (s->closing)
				closeStream(s, false);
			return;
		}
		s->writing = true;
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(s->toLocal.front()));
		s->toLocal.pop_front();
		asio::async_write(s->sock, asio::buffer(*data), [this, s, data](const asio::error_code& ec, size_t) {
			s->writing = false;
			if (ec)
			{
				closeStream(s, true);
				return;
			}
			writeLocal(s);
		});
	}

	void onStream(const StreamMsg& m)
	{
		const bool mine = (m.streamId & 0x80000000u) != 0;
		const uint32_t sid = m.streamId & 0x7fffffffu;
		auto pit = peers.find(m.peer);
		auto sit = streams.find(StreamKey(m.peer, sid, mine));
		StreamPtr s = sit == streams.end() ? nullptr : sit->second;
		if (m.kind == StreamKind::Close)
		{
			if (s)
			{
				s->closing = true;
				writeLocal(s);
			}
			return;
		}
		if (m.kind == StreamKind::Refused && (!s || !mine))
			return;
		std::vector<uint8_t> plain;
		uint64_t counter;
		if (pit == peers.end() || !openPeer(pit->second->pk, 1, m.data.data(), m.data.size(), counter, plain)
				|| counter <= pit->second->streamRecv)
		{
			// not from a member, or forged: ignore (a real stream then times out or closes)
			return;
		}
		pit->second->streamRecv = counter;
		if (m.kind == StreamKind::Refused)
		{
			// the host's copy turned us away
			const RefuseWhy why = plain.size() == 1 ? (RefuseWhy)plain[0] : RefuseWhy::WrongPin;
			{
				std::lock_guard<std::mutex> lock(mutex);
				refusal = why == RefuseWhy::TooManyTries ? "Wait 2 seconds before trying again" : "Wrong PIN";
			}
			closeStream(s, false);
			return;
		}
		if (m.kind == StreamKind::Open)
		{
			Reader r(plain);
			const uint16_t port = r.u16();
			const std::string theirPin = r.str(PinLength);
			// a private game: the PIN first (and 2 s between tries after a wrong one, against guessing)
			if (!mine && !s && r.ok && !pin.empty())
			{
				PinTries& t = pinTries[m.peer];
				const auto now = Clock::now();
				RefuseWhy why = RefuseWhy::WrongPin;
				bool refuse = false;
				if (now < t.until)
				{
					refuse = true;
					why = RefuseWhy::TooManyTries;
				}
				else if (theirPin != pin)
				{
					refuse = true;
					t.wrong++;
					t.until = now + milliseconds(PinRetryMs);
				}
				if (refuse)
				{
					Writer w;
					w.u8((uint8_t)why);
					StreamMsg back;
					back.peer = m.peer;
					back.streamId = m.streamId ^ 0x80000000u;
					back.kind = StreamKind::Refused;
					back.data = sealPeer(pit->second->pk, 1, me(), ++pit->second->streamSend, w.b);
					send(encode(back));
					return;
				}
			}
			if (mine || s || !r.done() || !lobbyPorts.count(port))
			{
				StreamMsg back;
				back.peer = m.peer;
				back.streamId = m.streamId ^ 0x80000000u;
				back.kind = StreamKind::Close;
				send(encode(back));
				return;
			}
			s = std::make_shared<Stream>(io);
			s->peer = m.peer;
			s->sid = sid;
			s->mine = false;
			streams[StreamKey(s->peer, s->sid, false)] = s;
			asio::error_code ec;
			s->sock.open(tcp::v4(), ec);
			if (!ec)
				s->sock.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
			if (ec)
			{
				closeStream(s, true);
				return;
			}
			{
				// before connecting: the lobby may accept before our connect handler runs
				std::lock_guard<std::mutex> lock(mutex);
				portOwner[s->sock.local_endpoint(ec).port()] = m.peer;
				if (portOwner.size() > 256)
					portOwner.erase(portOwner.begin());
			}
			s->sock.async_connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), [this, s](const asio::error_code& ec) {
				if (s->closed)
					return;
				if (ec)
				{
					closeStream(s, true);
					return;
				}
				asio::error_code e2;
				s->sock.set_option(tcp::no_delay(true), e2);
				s->connected = true;
				writeLocal(s);
				readLocal(s);
			});
			return;
		}
		// Data
		if (!s)
			return;
		s->toLocal.push_back(std::move(plain));
		writeLocal(s);
	}

	// ---- UDP

	void openUdp()
	{
		if (udpSock)
			return;
		try {
			auto s = std::make_unique<udp::socket>(io);
			s->open(udp::v4());
			s->bind(udp::endpoint(asio::ip::address_v4::any(), cfg.udpPort));
			udpSock = std::move(s);
			receiveUdp();
		} catch (const std::exception&) {
			udpSock.reset();
		}
	}

	void receiveUdp()
	{
		udp::socket *sock = udpSock.get();
		sock->async_receive_from(asio::buffer(udpBuf), udpFrom, [this, sock](const asio::error_code& ec, size_t n) {
			if (udpSock.get() != sock || ec == asio::error::operation_aborted)
				return;
			if (!ec)
				onUdp(udpBuf, n, udpFrom);
			receiveUdp();
		});
	}

	// TEST ONLY: TCVR_INET_NETEM="delay_ms,loss_percent" delays and drops this copy's outgoing UDP (for
	// PC/cloud tests where the system can't; never set by the game)
	int testDelayMs = -1;
	int testLossPct = 0;
	std::minstd_rand testRng { 12345 };

	void sendUdp(const std::vector<uint8_t>& data, const udp::endpoint& to)
	{
		if (!udpSock || data.size() > MaxDatagram)
			return;
		if (testDelayMs < 0)
		{
			testDelayMs = 0;
			if (const char *e = std::getenv("TCVR_INET_NETEM"))
				if (std::sscanf(e, "%d,%d", &testDelayMs, &testLossPct) < 1)
					testDelayMs = 0;
		}
		if (testLossPct > 0 && (int)(testRng() % 100) < testLossPct)
			return;
		auto d = std::make_shared<std::vector<uint8_t>>(data);
		if (testDelayMs > 0)
		{
			auto t = std::make_shared<asio::steady_timer>(io, milliseconds(testDelayMs));
			udp::socket *sock = udpSock.get();
			t->async_wait([this, t, d, to, sock](const asio::error_code& ec) {
				if (!ec && udpSock.get() == sock)
					udpSock->async_send_to(asio::buffer(*d), to, [d](const asio::error_code&, size_t) {});
			});
			return;
		}
		udpSock->async_send_to(asio::buffer(*d), to, [d](const asio::error_code&, size_t) {});
	}

	static udp::endpoint toUdp(const Endpoint& e)
	{
		return udp::endpoint(asio::ip::address_v4(e.ip), e.port);
	}

	uint32_t me()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return myId;
	}

	void registerUdp()
	{
		if (!online)
			return;
		Writer w;
		w.u32(++registerSeq);
		sendUdp(sealServerUdp(UdpType::Register, me(), w.b, udpKey), serverUdp);
		lastRegister = Clock::now();
	}

	// one packet to a player: direct when it hears us, else through the server
	void sendPeer(Peer& p, uint32_t to, const std::vector<uint8_t>& plain, int cand = -2)
	{
		if (!p.pk.valid)
			return;
		const auto pkt = sealPeer(p.pk, 0, me(), ++p.udpSend, plain);
		if (cand == -2)
			cand = directUsable(p) ? p.sendCand : -1;
		if (cand >= 0 && cand < (int)p.cands.size())
		{
			sendUdp(pkt, toUdp(p.cands[cand]));
			return;
		}
		if (!online || !registered)
			return;
		Writer w;
		w.u32(to);
		w.bytes(pkt);
		sendUdp(sealServerUdp(UdpType::Relay, me(), w.b, udpKey), serverUdp);
	}

	std::vector<uint8_t> punch(const Peer& p, int cand)
	{
		Writer w;
		w.u8(Punch);
		w.u8((uint8_t)(cand < 0 ? 0xFF : cand));
		const bool heard = p.heardIdx >= 0 && Clock::now() - p.heardAt < milliseconds(DirectFreshMs);
		w.u8((uint8_t)(heard ? p.heardIdx : 0xFF));
		w.u32(nowMs());
		return w.b;
	}

	void onUdp(const uint8_t *data, size_t n, const udp::endpoint& from)
	{
		if (n < 1)
			return;
		const uint8_t type = data[0];
		if (type == (uint8_t)UdpType::Peer)
		{
			Endpoint src;
			if (from.address().is_v4())
			{
				src.ip = from.address().to_v4().to_uint();
				src.port = from.port();
			}
			onPeerPacket(data, n, true, 0, src);
			return;
		}
		if (!online || from.address() != serverUdp.address() || from.port() != serverUdp.port())
			return;
		if (!checkServerUdpMac(data, n, udpKey))
			return;
		Reader r(data + 5, n - 5 - UdpMacSize);
		const uint32_t to = (uint32_t)data[1] | (uint32_t)data[2] << 8 | (uint32_t)data[3] << 16 | (uint32_t)data[4] << 24;
		if (to != me())
			return;
		if (type == (uint8_t)UdpType::Registered)
		{
			r.u32();
			getEndpoint(r);
			registered = r.ok;
		}
		else if (type == (uint8_t)UdpType::Relayed)
		{
			const uint32_t from = r.u32();
			const std::vector<uint8_t> pkt = r.rest();
			if (r.ok)
				onPeerPacket(pkt.data(), pkt.size(), false, from);
		}
	}

	// A direct packet from an address the server didn't give (its router chose another port for us than for
	// the server): try that one too ("peer reflexive" in ICE words).
	void learn(Peer& p, const Endpoint& from)
	{
		if (!from.valid() || p.cands.size() >= 8 || std::find(p.cands.begin(), p.cands.end(), from) != p.cands.end())
			return;
		p.cands.push_back(from);
		p.since = Clock::now();
	}

	void onPeerPacket(const uint8_t *data, size_t n, bool direct, uint32_t relayedFrom, const Endpoint& sourceOf = Endpoint())
	{
		uint32_t from;
		if (!peekPeerFrom(data, n, from) || (!direct && from != relayedFrom))
			return;
		auto it = peers.find(from);
		if (it == peers.end())
			return;
		Peer& p = *it->second;
		uint64_t counter;
		std::vector<uint8_t> plain;
		if (!openPeer(p.pk, 0, data, n, counter, plain) || !p.udpWin.accept(counter) || plain.empty())
			return;
		switch (plain[0])
		{
		case Punch:
		{
			if (plain.size() != 7)
				return;
			const int candIdx = plain[1];
			const int heardIdx = plain[2];
			if (direct && candIdx != 0xFF)
			{
				p.heardIdx = candIdx;
				p.heardAt = Clock::now();
			}
			if (direct)
				learn(p, sourceOf);
			if (heardIdx != 0xFF && heardIdx < (int)p.cands.size())
			{
				const bool was = directUsable(p);
				p.sendCand = heardIdx;
				p.heardMeAt = Clock::now();
				if (!was)
					updatePaths();
			}
			Writer w;
			w.u8(Pong);
			w.bytes(plain.data() + 3, 4);
			sendPeer(p, from, w.b);
			break;
		}
		case Pong:
			if (plain.size() == 5)
			{
				const uint32_t ts = plain[1] | plain[2] << 8 | plain[3] << 16 | (uint32_t)plain[4] << 24;
				const uint32_t rtt = nowMs() - ts;
				if (rtt < 10000)
				{
					p.rtt = (int)rtt;
					updatePaths();
				}
			}
			break;
		case Ggpo:
			if (p.proxy && localGgpoPort != 0)
			{
				auto d = std::make_shared<std::vector<uint8_t>>(plain.begin() + 1, plain.end());
				p.proxy->async_send_to(asio::buffer(*d), udp::endpoint(asio::ip::address_v4::loopback(), localGgpoPort),
						[d](const asio::error_code&, size_t) {});
			}
			break;
		default:
			break;
		}
	}

	// ---- GGPO's local ports

	std::string ggpoPeers(const lobby::StartInfo& info, const std::string& hostAddress, uint16_t myGgpoPort)
	{
		std::string list;
		localGgpoPort = myGgpoPort;
		for (size_t k = 0; k < info.peers.size(); k++)
		{
			if (!list.empty())
				list += ",";
			if ((int)k == info.localIndex)
			{
				list += "127.0.0.1:" + std::to_string(myGgpoPort);
				continue;
			}
			const std::string addr = info.peers[k].address.empty() ? hostAddress : info.peers[k].address;
			const uint32_t id = clientIdOf(addr);
			auto it = peers.find(id);
			if (id == 0 || it == peers.end())
				return std::string();
			Peer& p = *it->second;
			if (!p.proxy)
			{
				try {
					p.proxy = std::make_unique<udp::socket>(io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
				} catch (const std::exception&) {
					return std::string();
				}
				receiveProxy(id, p.proxy.get());
			}
			list += "127.0.0.1:" + std::to_string(p.proxy->local_endpoint().port());
		}
		return list;
	}

	void receiveProxy(uint32_t id, udp::socket *sock)
	{
		auto it = peers.find(id);
		if (it == peers.end() || it->second->proxy.get() != sock)
			return;
		Peer& p = *it->second;
		sock->async_receive_from(asio::buffer(p.proxyBuf, sizeof(p.proxyBuf) - 1), p.proxyFrom,
				[this, id, sock](const asio::error_code& ec, size_t n) {
			if (ec == asio::error::operation_aborted)
				return;
			auto it = peers.find(id);
			if (it == peers.end() || it->second->proxy.get() != sock)
				return;
			Peer& p = *it->second;
			if (!ec && p.proxyFrom.address().is_loopback() && p.proxyFrom.port() == localGgpoPort
					&& n + PeerOverhead + 1 + 4 + 5 + UdpMacSize <= MaxDatagram)
			{
				std::vector<uint8_t> plain(n + 1);
				plain[0] = Ggpo;
				std::memcpy(plain.data() + 1, p.proxyBuf, n);
				sendPeer(p, id, plain);
			}
			receiveProxy(id, sock);
		});
	}

	void matchOver()
	{
		for (auto& [id, p] : peers)
			if (p->proxy)
			{
				asio::error_code ec;
				p->proxy->close(ec);
				p->proxy.reset();
			}
		localGgpoPort = 0;
	}

	// ---- the clock

	bool tickRunning = false;
	Clock::time_point lastPaths;

	void startTick()
	{
		if (tickRunning)
			return;
		tickRunning = true;
		tickTimer.expires_after(milliseconds(100));
		tickTimer.async_wait([this](const asio::error_code& ec) {
			tickRunning = false;
			if (ec || !configured)
				return;
			tick();
			startTick();
		});
	}

	void tick()
	{
		const auto now = Clock::now();
		if (!online)
			return;
		if (now - ctlLastRx > milliseconds(ServerSilentMs))
		{
			retryLater("The server stopped answering");
			return;
		}
		if (now - ctlLastPing > milliseconds(ServerPingMs))
		{
			ctlLastPing = now;
			send(encodeSimple(Ctl::Ping));
		}
		if (now - lastRegister > milliseconds(registered ? KeepAliveMs : RegisterMs))
			registerUdp();
		if (searching && now - lastList > milliseconds(ListMs))
		{
			lastList = now;
			send(encodeSimple(Ctl::List));
		}
		if (!beacon.empty() && now - lastAnnounce > milliseconds(AnnounceMs))
		{
			lastAnnounce = now;
			send(encodeAnnounce(beacon, !pin.empty()));
		}
		// punching
		bool changed = false;
		for (auto& [id, pp] : peers)
		{
			Peer& p = *pp;
			const bool direct = directUsable(p);
			if (!cfg.forceRelay)
			{
				const int every = direct ? PunchSlowMs
						: (now - p.since < milliseconds(PunchFastPhaseMs) ? PunchFastMs : PunchSlowMs);
				if (now - p.lastPunch >= milliseconds(every))
				{
					p.lastPunch = now;
					if (direct)
						sendPeer(p, id, punch(p, p.sendCand), p.sendCand);
					else
						for (int c = 0; c < (int)p.cands.size(); c++)
							sendPeer(p, id, punch(p, c), c);
				}
			}
			// through the server too, so what each side heard gets across even before a direct path works
			if (now - p.lastRelayPunch >= milliseconds(PunchSlowMs))
			{
				p.lastRelayPunch = now;
				sendPeer(p, id, punch(p, -1), -1);
			}
			if (direct != directUsable(p))
				changed = true;
		}
		if (changed || now - lastPaths > milliseconds(1000))
		{
			lastPaths = now;
			updatePaths();
		}
		// streams whose local connection never came
		for (auto& [k, s] : std::map<StreamKey, StreamPtr>(streams))
			if (s->mine && !s->connected && now - s->made > milliseconds(AcceptTimeoutMs))
				closeStream(s, false);
	}
};

InternetClient::InternetClient() : impl(std::make_unique<Impl>()) {}
InternetClient::~InternetClient() = default;

void InternetClient::configure(const ClientConfig& config)
{
	impl->run([&] { impl->configure(config); });
}

void InternetClient::stop()
{
	impl->run([&] { impl->shutdownAll(Status::Off, ""); });
}

Status InternetClient::status(std::string *line)
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (line)
		*line = impl->stLine;
	return impl->st;
}

std::string InternetClient::serverKey()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->keyHex;
}

std::string InternetClient::serverFingerprint()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->keyPrint;
}

void InternetClient::onNewServerKey(std::function<void(const std::string&, const std::string&)> f)
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->newKeyHook = std::move(f);
}

std::string InternetClient::ggpoPeers(const lobby::StartInfo& info, const std::string& hostAddress, uint16_t myGgpoPort)
{
	return impl->run([&] { return impl->ggpoPeers(info, hostAddress, myGgpoPort); });
}

void InternetClient::setGamePin(const std::string& pin)
{
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->refusal.clear();
	}
	impl->run([&] {
		if (validPin(pin))
			impl->pin = pin;
	});
}

GamePolicy InternetClient::policy()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->serverPolicy;
}

std::string InternetClient::lastRefusal()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->refusal;
}

void InternetClient::matchOver()
{
	impl->run([&] { impl->matchOver(); });
}

std::vector<PeerPath> InternetClient::paths()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->pathList;
}

bool InternetClient::ready(std::string& why)
{
	std::string line;
	const Status s = status(&line);
	if (s == Status::Online)
		return true;
	why = line.empty() ? "Not connected to the server" : line;
	return false;
}

std::string InternetClient::localAddress()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->myId ? virtualAddress(impl->myId) : std::string();
}

uint16_t InternetClient::route(const std::string& address, uint16_t port)
{
	return impl->run([&] { return impl->route(address, port); });
}

std::string InternetClient::peerOf(uint16_t localPort)
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	auto it = impl->portOwner.find(localPort);
	return it == impl->portOwner.end() ? std::string() : virtualAddress(it->second);
}

void InternetClient::listening(uint16_t port, bool on)
{
	asio::post(impl->io, [this, port, on] {
		if (on)
			impl->lobbyPorts.insert(port);
		else
			impl->lobbyPorts.erase(port);
	});
}

void InternetClient::announce(const lobby::Beacon *beacon)
{
	std::vector<uint8_t> b = beacon ? lobby::encode(*beacon) : std::vector<uint8_t>();
	asio::post(impl->io, [this, b = std::move(b)] {
		if (b == impl->beacon)
			return;
		const bool was = !impl->beacon.empty();
		impl->beacon = b;
		if (!impl->online)
			return;
		if (!b.empty())
		{
			impl->lastAnnounce = Clock::now();
			impl->send(encodeAnnounce(b, !impl->pin.empty()));
		}
		else if (was)
			impl->send(encodeSimple(Ctl::Unannounce));
	});
}

void InternetClient::search(bool on)
{
	asio::post(impl->io, [this, on] {
		if (on && !impl->searching && impl->online)
		{
			impl->lastList = Clock::now();
			impl->send(encodeSimple(Ctl::List));
		}
		impl->searching = on;
		if (!on)
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->found.clear();
		}
	});
}

std::vector<lobby::FoundGame> InternetClient::games()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	std::vector<lobby::FoundGame> list;
	const auto now = Clock::now();
	for (auto& [at, g] : impl->found)
		if (now - at < milliseconds(GamesVisibleMs))
			list.push_back(g);
	std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.hostId < b.hostId; });
	return list;
}

void InternetClient::idle()
{
	asio::post(impl->io, [this] {
		impl->beacon.clear();
		impl->matchOver();
		if (impl->online)
			impl->send(encodeSimple(Ctl::LeaveGame));
		// (the server sends Members no more: forget the others)
		for (auto& [id, p] : impl->peers)
		{
			impl->dropPeerStreams(id);
			impl->closePeer(*p);
		}
		impl->peers.clear();
		impl->updatePaths();
	});
}

}
