/*
	tcvr: LAN lobby for the VR multiplayer. See tcvr_lobby.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "tcvr_lobby.h"
#include <cstdlib>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace tcvr::lobby
{

const char *const RoomNames[RoomCount] = { "Kitchen and dining room", "Two children's bedrooms", "Hallway and its mezzanine",
	"Parent's bedroom", "Garage and tool room", "Attic", "Living room and winter garden", "Cellar" };
const char *const ModeNames[3] = { "Deathmatch", "Cat and Mice", "Capture the Flag" };

namespace
{
const int PointSteps[LimitCount] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100 };
const int CatSeconds[LimitCount] = { 0, 30, 60, 90, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720, 780, 840, 900,
	960, 1020, 1080, 1140 };
}

int limitValue(int mode, int index)
{
	index = std::clamp(index, 0, LimitCount - 1);
	return mode == 1 ? CatSeconds[index] : PointSteps[index];
}

int limitIndex(int mode, int value)
{
	int best = 0;
	for (int i = 1; i < LimitCount; i++)
		if (std::abs(limitValue(mode, i) - value) < std::abs(limitValue(mode, best) - value))
			best = i;
	return best;
}

std::string limitText(int mode, int value)
{
	const int v = limitValue(mode, limitIndex(mode, value));
	if (v == 0)
		return "no limit";
	if (mode != 1)
		return std::to_string(v) + (v == 1 ? " point to win" : " points to win");
	if (v % 60 == 0)
		return std::to_string(v / 60) + " min time limit";
	return std::to_string(v / 60) + " min " + std::to_string(v % 60) + " s time limit";
}


// ---- codec ----------------------------------------------------------------------------------

namespace
{

constexpr size_t MaxFrame = 2048;
const uint8_t Magic[4] = { 'T', 'C', 'V', 'L' };
enum : uint8_t { KindBeacon = 1, KindQuery = 2 };

struct Writer
{
	std::vector<uint8_t> b;
	void u8(uint8_t v) { b.push_back(v); }
	void u16(uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
	void u32(uint32_t v) { u16(v & 0xffff); u16(v >> 16); }
	void str(const std::string& s)
	{
		const size_t n = std::min<size_t>(s.size(), 255);
		u8((uint8_t)n);
		b.insert(b.end(), s.begin(), s.begin() + n);
	}
	void bytes(const uint8_t *p, size_t n) { b.insert(b.end(), p, p + n); }
	void settings(const Settings& s) { u8(s.mode); u8(s.room); u16(s.points); }
};

struct Reader
{
	const uint8_t *p;
	size_t n;
	size_t i = 0;
	bool ok = true;
	Reader(const uint8_t *p, size_t n) : p(p), n(n) {}
	explicit Reader(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
	bool take(size_t k) { if (!ok || i + k > n) { ok = false; return false; } return true; }
	uint8_t u8() { if (!take(1)) return 0; return p[i++]; }
	uint16_t u16() { if (!take(2)) return 0; uint16_t v = p[i] | (p[i + 1] << 8); i += 2; return v; }
	uint32_t u32() { uint32_t lo = u16(); uint32_t hi = u16(); return lo | (hi << 16); }
	std::string str()
	{
		const size_t len = u8();
		if (!take(len))
			return {};
		std::string s((const char *)p + i, len);
		i += len;
		return s;
	}
	void bytes(uint8_t *out, size_t k) { if (!take(k)) return; memcpy(out, p + i, k); i += k; }
	Settings settings() { Settings s; s.mode = u8(); s.room = u8(); s.points = u16(); return s; }
};

// the text fields come from the network: bounded and printable
std::string cleanText(const std::string& s)
{
	std::string out;
	for (char c : s)
		if (c >= 0x20 && c < 0x7f)
			out += c;
	return out;
}

}	// namespace

std::string cleanName(const std::string& name)
{
	std::string s = cleanText(name);
	if (s.size() > NameMax)
		s.resize(NameMax);
	return s.empty() ? "???" : s;
}

int State::count() const
{
	int n = 0;
	for (const PlayerInfo& p : players)
		n += p.present;
	return n;
}

std::string StartInfo::peerList(const std::string& hostAddress) const
{
	std::string s;
	for (const Peer& p : peers)
	{
		if (!s.empty())
			s += ",";
		s += (p.address.empty() ? hostAddress : p.address) + ":" + std::to_string(p.port);
	}
	return s;
}

std::vector<uint8_t> encodeFrame(MsgType type, const std::vector<uint8_t>& payload)
{
	Writer w;
	w.u16((uint16_t)(1 + payload.size()));
	w.u8((uint8_t)type);
	w.b.insert(w.b.end(), payload.begin(), payload.end());
	return w.b;
}

bool FrameParser::feed(const uint8_t *data, size_t size, std::vector<Frame>& out)
{
	buffer.insert(buffer.end(), data, data + size);
	size_t pos = 0;
	while (buffer.size() - pos >= 2)
	{
		const size_t len = buffer[pos] | (buffer[pos + 1] << 8);
		if (len < 1 || len > MaxFrame)
			return false;
		if (buffer.size() - pos < 2 + len)
			break;
		const uint8_t type = buffer[pos + 2];
		if (type < (uint8_t)MsgType::Hello || type > (uint8_t)MsgType::Start)
			return false;
		Frame f;
		f.type = (MsgType)type;
		f.payload.assign(buffer.begin() + pos + 3, buffer.begin() + pos + 2 + len);
		out.push_back(std::move(f));
		pos += 2 + len;
	}
	buffer.erase(buffer.begin(), buffer.begin() + pos);
	return true;
}

std::vector<uint8_t> encode(const Hello& m)
{
	Writer w;
	w.u8(m.protocol);
	w.str(m.name);
	w.str(m.appVersion);
	w.bytes(m.disc.data(), m.disc.size());
	w.u16(m.ggpoPort);
	return w.b;
}

bool decode(const std::vector<uint8_t>& p, Hello& m)
{
	Reader r(p);
	m.protocol = r.u8();
	m.name = cleanName(r.str());
	m.appVersion = cleanText(r.str());
	r.bytes(m.disc.data(), m.disc.size());
	m.ggpoPort = r.u16();
	return r.ok;
}

std::vector<uint8_t> encodeWelcome(uint8_t slot) { return { slot }; }

bool decodeWelcome(const std::vector<uint8_t>& p, uint8_t& slot)
{
	Reader r(p);
	slot = r.u8();
	return r.ok && slot < MaxPlayers;
}

std::vector<uint8_t> encodeRefuse(RefuseReason reason) { return { (uint8_t)reason }; }

bool decodeRefuse(const std::vector<uint8_t>& p, RefuseReason& reason)
{
	Reader r(p);
	const uint8_t v = r.u8();
	reason = v >= 1 && v <= 4 ? (RefuseReason)v : RefuseReason::None;
	return r.ok;
}

std::vector<uint8_t> encode(const State& m)
{
	Writer w;
	w.u32(m.revision);
	w.settings(m.settings);
	for (const PlayerInfo& p : m.players)
	{
		w.u8(p.present);
		if (p.present)
		{
			w.str(p.name);
			w.u8(p.ready);
			w.str(p.address);
			w.u16(p.ggpoPort);
		}
	}
	return w.b;
}

bool decode(const std::vector<uint8_t>& p, State& m)
{
	Reader r(p);
	m = State();
	m.revision = r.u32();
	m.settings = r.settings();
	for (PlayerInfo& pl : m.players)
	{
		pl.present = r.u8() != 0;
		if (pl.present)
		{
			pl.name = cleanName(r.str());
			pl.ready = r.u8() != 0;
			pl.address = cleanText(r.str());
			pl.ggpoPort = r.u16();
		}
	}
	return r.ok;
}

std::vector<uint8_t> encodeReady(bool ready) { return { (uint8_t)ready }; }

bool decodeReady(const std::vector<uint8_t>& p, bool& ready)
{
	Reader r(p);
	ready = r.u8() != 0;
	return r.ok;
}

std::vector<uint8_t> encode(const StartInfo& m)
{
	Writer w;
	w.u8((uint8_t)m.peers.size());
	for (const Peer& p : m.peers)
	{
		w.str(p.address);
		w.u16(p.port);
	}
	w.u8((uint8_t)m.localIndex);
	w.u8(m.inputDelay);
	w.u32(m.sessionId);
	w.settings(m.settings);
	return w.b;
}

bool decode(const std::vector<uint8_t>& p, StartInfo& m)
{
	Reader r(p);
	m = StartInfo();
	const int count = r.u8();
	if (count < 2 || count > MaxPlayers)
		return false;
	for (int i = 0; i < count; i++)
	{
		Peer peer;
		peer.address = cleanText(r.str());
		peer.port = r.u16();
		m.peers.push_back(peer);
	}
	m.localIndex = r.u8();
	m.inputDelay = r.u8();
	m.sessionId = r.u32();
	m.settings = r.settings();
	return r.ok && m.localIndex < count;
}

std::vector<uint8_t> encode(const Beacon& m)
{
	Writer w;
	w.bytes(Magic, 4);
	w.u8(KindBeacon);
	w.u8(m.protocol);
	w.u32(m.hostId);
	w.str(m.appVersion);
	w.str(m.gameId);
	w.str(m.hostName);
	w.settings(m.settings);
	w.u8(m.players);
	w.u8(m.maxPlayers);
	w.u16(m.lobbyPort);
	w.u8(m.started);
	return w.b;
}

std::vector<uint8_t> encodeQuery()
{
	Writer w;
	w.bytes(Magic, 4);
	w.u8(KindQuery);
	w.u8(ProtocolVersion);
	return w.b;
}

Datagram decodeDatagram(const uint8_t *data, size_t size, Beacon& beacon)
{
	Reader r(data, size);
	uint8_t magic[4] {};
	r.bytes(magic, 4);
	if (!r.ok || memcmp(magic, Magic, 4) != 0)
		return Datagram::Invalid;
	const uint8_t kind = r.u8();
	const uint8_t protocol = r.u8();
	if (!r.ok)
		return Datagram::Invalid;
	if (kind == KindQuery)
		return Datagram::Query;
	if (kind != KindBeacon)
		return Datagram::Invalid;
	Beacon b;
	b.protocol = protocol;
	b.hostId = r.u32();
	b.appVersion = cleanText(r.str());
	b.gameId = cleanText(r.str());
	b.hostName = cleanName(r.str());
	b.settings = r.settings();
	b.players = r.u8();
	b.maxPlayers = r.u8();
	b.lobbyPort = r.u16();
	b.started = r.u8() != 0;
	if (!r.ok || b.lobbyPort == 0 || b.players > MaxPlayers || b.maxPlayers > MaxPlayers)
		return Datagram::Invalid;
	beacon = b;
	return Datagram::Beacon;
}

const char *refuseText(RefuseReason reason)
{
	switch (reason)
	{
	case RefuseReason::Full: return "This game is full";
	case RefuseReason::Version: return "The host has a different version of the game app";
	case RefuseReason::Disc: return "The host has a different disc";
	case RefuseReason::Started: return "This game has already started";
	default: return "The host refused";
	}
}

// ---- the lobby ------------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
using asio::ip::tcp;
using asio::ip::udp;

namespace
{

// The address other computers reach this one by (nothing is sent: a UDP "connect" only picks the route).
std::string localAddress()
{
	try {
		asio::io_context io;
		udp::socket s(io);
		s.open(udp::v4());
		s.connect(udp::endpoint(asio::ip::make_address_v4("192.0.2.1"), 9));
		return s.local_endpoint().address().to_string();
	} catch (...) {
		return "127.0.0.1";
	}
}

bool isLoopback(const std::string& a)
{
	return a.rfind("127.", 0) == 0;
}

}

struct Lobby::Impl
{
	struct Conn
	{
		explicit Conn(asio::io_context& io) : sock(io) {}
		tcp::socket sock;
		FrameParser parser;
		uint8_t buf[2048];
		std::deque<std::vector<uint8_t>> out;
		bool writing = false;
		bool closed = false;
		int slot = -1;
		std::string address;
		uint16_t ggpoPort = 0;
		Clock::time_point lastRx = Clock::now();
		Clock::time_point accepted = Clock::now();
	};
	using ConnPtr = std::shared_ptr<Conn>;

	struct Seen
	{
		FoundGame game;
		Clock::time_point at;
	};

	Timing timing;
	asio::io_context io;
	std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard;
	std::thread thread;

	// shared with the menu thread
	std::mutex mutex;
	View v;
	std::map<uint32_t, Seen> found;

	// the io thread only
	std::function<void(bool)> broadcastHook;
	std::vector<std::string> extraTargets;
	uint16_t discoveryPort = DiscoveryPort;
	std::unique_ptr<udp::socket> disc;
	udp::endpoint discFrom;
	uint8_t discBuf[1500];
	bool searching = false;
	asio::steady_timer beaconTimer;
	asio::steady_timer tickTimer;
	std::unique_ptr<tcp::acceptor> acceptor;
	std::vector<ConnPtr> conns;			// host: every connection (slot >= 1 after HELLO)
	ConnPtr hostConn;					// joiner
	std::array<ConnPtr, MaxPlayers> slots;
	Identity me;
	Settings settings;
	uint32_t hostId = 0;
	bool hosting = false;
	bool started = false;
	bool beaconRunning = false;
	bool tickRunning = false;
	Clock::time_point lastPing = Clock::now();
	Clock::time_point connectStart = Clock::now();
	uint32_t revision = 0;
	std::mt19937 rng { std::random_device()() };

	explicit Impl(const Timing& t) : timing(t), beaconTimer(io), tickTimer(io)
	{
		guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io.get_executor());
		thread = std::thread([this] { io.run(); });
	}

	~Impl()
	{
		run([this] { reset(false); closeDiscovery(); });
		guard.reset();
		io.stop();
		if (thread.joinable())
			thread.join();
	}

	// run f on the io thread and wait for it
	template <typename F>
	auto run(F f) -> decltype(f())
	{
		using R = decltype(f());
		std::packaged_task<R()> task(std::move(f));
		auto fut = task.get_future();
		asio::post(io, [&task] { task(); });
		return fut.get();
	}

	template <typename F>
	void withView(F f)
	{
		std::lock_guard<std::mutex> lock(mutex);
		f(v);
	}

	// ---- discovery

	void openDiscovery()
	{
		if (disc)
			return;
		try {
			auto s = std::make_unique<udp::socket>(io);
			s->open(udp::v4());
			s->set_option(asio::socket_base::reuse_address(true));
			s->set_option(asio::socket_base::broadcast(true));
			s->bind(udp::endpoint(udp::v4(), discoveryPort));
			disc = std::move(s);
			if (broadcastHook)
				broadcastHook(true);
			receiveDiscovery();
		} catch (const std::exception& e) {
			disc.reset();
			withView([&](View& view) { if (view.phase == Phase::Idle) view.message = "Can't listen for games on this network"; });
		}
	}

	void closeDiscovery()
	{
		if (!disc)
			return;
		asio::error_code ec;
		disc->close(ec);
		disc.reset();
		if (broadcastHook)
			broadcastHook(false);
	}

	void updateDiscovery()
	{
		if (searching || hosting)
			openDiscovery();
		else
			closeDiscovery();
	}

	void receiveDiscovery()
	{
		if (!disc)
			return;
		udp::socket *s = disc.get();
		s->async_receive_from(asio::buffer(discBuf), discFrom, [this, s](const asio::error_code& ec, size_t n) {
			if (disc.get() != s)
				return;		// closed (and maybe opened again) meanwhile
			if (ec == asio::error::operation_aborted)
				return;
			if (!ec)
				onDatagram(discBuf, n, discFrom);
			receiveDiscovery();
		});
	}

	void onDatagram(const uint8_t *data, size_t n, const udp::endpoint& from)
	{
		Beacon b;
		switch (decodeDatagram(data, n, b))
		{
		case Datagram::Beacon:
			if (!searching || (hosting && b.hostId == hostId))
				return;
			{
				const std::string addr = from.address().to_string();
				std::lock_guard<std::mutex> lock(mutex);
				Seen& s = found[b.hostId];
				const bool recent = Clock::now() - s.at < std::chrono::milliseconds(timing.visibleMs);
				// the same game heard twice (broadcast, and this computer's own address): keep the real one
				if (!(recent && !s.game.address.empty() && !isLoopback(s.game.address) && isLoopback(addr)))
					s.game.address = addr;
				s.game.hostId = b.hostId;
				s.game.beacon = b;
				s.at = Clock::now();
			}
			break;
		case Datagram::Query:
			if (hosting && !started)
			{
				auto data = std::make_shared<std::vector<uint8_t>>(encode(makeBeacon()));
				disc->async_send_to(asio::buffer(*data), from, [data](const asio::error_code&, size_t) {});
			}
			break;
		default:
			break;
		}
	}

	Beacon makeBeacon()
	{
		Beacon b;
		b.hostId = hostId;
		b.appVersion = me.appVersion;
		b.gameId = me.gameId;
		b.hostName = cleanName(me.name);
		b.settings = settings;
		b.players = (uint8_t)currentState().count();
		b.maxPlayers = MaxPlayers;
		b.lobbyPort = acceptor ? acceptor->local_endpoint().port() : 0;
		b.started = started;
		return b;
	}

	void sendBeacon()
	{
		if (!disc)
			return;
		auto data = std::make_shared<std::vector<uint8_t>>(encode(makeBeacon()));
		std::vector<std::string> targets { "255.255.255.255", "127.0.0.1" };
		targets.insert(targets.end(), extraTargets.begin(), extraTargets.end());
		for (const std::string& t : targets)
		{
			asio::error_code ec;
			const auto addr = asio::ip::make_address_v4(t, ec);
			if (ec)
				continue;
			disc->async_send_to(asio::buffer(*data), udp::endpoint(addr, discoveryPort), [data](const asio::error_code&, size_t) {});
		}
	}

	void beaconTick()
	{
		if (!hosting || started)
		{
			beaconRunning = false;
			return;
		}
		beaconRunning = true;
		sendBeacon();
		beaconTimer.expires_after(std::chrono::milliseconds(timing.beaconMs));
		beaconTimer.async_wait([this](const asio::error_code& ec) {
			if (ec)
			{
				beaconRunning = false;
				return;
			}
			beaconTick();
		});
	}

	// ---- connections

	void closeConn(const ConnPtr& c)
	{
		if (!c || c->closed)
			return;
		c->closed = true;
		asio::error_code ec;
		c->sock.shutdown(tcp::socket::shutdown_both, ec);
		c->sock.close(ec);
	}

	void send(const ConnPtr& c, MsgType type, const std::vector<uint8_t>& payload = {})
	{
		if (!c || c->closed)
			return;
		c->out.push_back(encodeFrame(type, payload));
		flush(c);
	}

	void flush(const ConnPtr& c)
	{
		if (c->writing || c->out.empty() || c->closed)
			return;
		c->writing = true;
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(c->out.front()));
		c->out.pop_front();
		asio::async_write(c->sock, asio::buffer(*data), [this, c, data](const asio::error_code& ec, size_t) {
			c->writing = false;
			if (ec)
			{
				onDisconnect(c);
				return;
			}
			flush(c);
		});
	}

	void readLoop(const ConnPtr& c)
	{
		c->sock.async_read_some(asio::buffer(c->buf), [this, c](const asio::error_code& ec, size_t n) {
			if (ec || c->closed)
			{
				if (ec != asio::error::operation_aborted)
					onDisconnect(c);
				return;
			}
			c->lastRx = Clock::now();
			std::vector<FrameParser::Frame> frames;
			if (!c->parser.feed(c->buf, n, frames))
			{
				onDisconnect(c);
				return;
			}
			for (auto& f : frames)
			{
				if (c->closed)
					return;
				if (hosting)
					hostFrame(c, f);
				else
					joinerFrame(c, f);
			}
			if (!c->closed)
				readLoop(c);
		});
	}

	void onDisconnect(const ConnPtr& c)
	{
		if (c->closed)
			return;
		closeConn(c);
		if (hosting)
		{
			if (c->slot > 0 && slots[c->slot] == c)
			{
				slots[c->slot].reset();
				c->slot = -1;
				publishState();
			}
			conns.erase(std::remove(conns.begin(), conns.end(), c), conns.end());
		}
		else if (c == hostConn)
		{
			withView([&](View& view) {
				if (view.phase == Phase::Connecting)
				{
					view.phase = Phase::Failed;
					view.message = "Could not reach the host";
				}
				else if (view.phase == Phase::InLobby)
				{
					view.phase = Phase::HostLeft;
					view.message = "The host left";
				}
			});
		}
	}

	void startTick()
	{
		if (tickRunning)
			return;
		tickRunning = true;
		tickTimer.expires_after(std::chrono::milliseconds(250));
		tickTimer.async_wait([this](const asio::error_code& ec) {
			tickRunning = false;
			if (ec)
				return;
			tick();
		});
	}

	void tick()
	{
		const auto now = Clock::now();
		const auto drop = std::chrono::milliseconds(timing.dropMs);
		bool active = false;
		Phase phase = Phase::Idle;
		withView([&](View& view) { phase = view.phase; });
		if (hosting && !started)
		{
			active = true;
			const bool ping = now - lastPing >= std::chrono::milliseconds(timing.pingMs);
			if (ping)
				lastPing = now;
			const auto list = conns;		// onDisconnect edits conns
			for (const ConnPtr& c : list)
			{
				if (now - c->lastRx > drop)
					onDisconnect(c);
				else if (ping)
					send(c, MsgType::Ping);
			}
		}
		else if (!hosting && hostConn && (phase == Phase::Connecting || phase == Phase::InLobby))
		{
			active = true;
			if (phase == Phase::Connecting && now - connectStart > std::chrono::milliseconds(timing.connectMs))
			{
				closeConn(hostConn);
				withView([&](View& view) { view.phase = Phase::Failed; view.message = "The host doesn't answer"; });
			}
			else if (now - hostConn->lastRx > drop)
				onDisconnect(hostConn);
			else if (now - lastPing >= std::chrono::milliseconds(timing.pingMs))
			{
				lastPing = now;
				send(hostConn, MsgType::Ping);
			}
		}
		if (active)
			startTick();
	}

	// ---- the host

	State currentState()
	{
		State s;
		s.revision = revision;
		s.settings = settings;
		for (int i = 0; i < MaxPlayers; i++)
		{
			PlayerInfo& p = s.players[i];
			if (i == 0)
			{
				p.present = true;
				p.name = cleanName(me.name);
				p.ready = true;
				p.ggpoPort = me.ggpoPort;
			}
			else if (slots[i])
			{
				p.present = true;
				p.name = slotNames[i];
				p.ready = slotReady[i];
				p.address = slots[i]->address;
				p.ggpoPort = slots[i]->ggpoPort;
			}
		}
		return s;
	}
	std::array<std::string, MaxPlayers> slotNames;
	std::array<bool, MaxPlayers> slotReady {};

	void publishState()
	{
		revision++;
		const State s = currentState();
		withView([&](View& view) { view.state = s; });
		const auto payload = encode(s);
		for (int i = 1; i < MaxPlayers; i++)
			if (slots[i])
				send(slots[i], MsgType::State, payload);
	}

	void hostFrame(const ConnPtr& c, const FrameParser::Frame& f)
	{
		switch (f.type)
		{
		case MsgType::Hello:
		{
			Hello h;
			if (c->slot >= 0 || !decode(f.payload, h))
			{
				onDisconnect(c);
				return;
			}
			RefuseReason why = RefuseReason::None;
			int slot = -1;
			for (int i = 1; i < MaxPlayers && slot < 0; i++)
				if (!slots[i])
					slot = i;
			if (started)
				why = RefuseReason::Started;
			else if (h.protocol != ProtocolVersion || h.appVersion != me.appVersion)
				why = RefuseReason::Version;
			else if (h.disc != me.disc)
				why = RefuseReason::Disc;
			else if (slot < 0)
				why = RefuseReason::Full;
			if (why != RefuseReason::None)
			{
				send(c, MsgType::Refuse, encodeRefuse(why));
				// closed once the refusal has been written
				asio::post(io, [this, c] { closeLater(c); });
				return;
			}
			c->slot = slot;
			c->ggpoPort = h.ggpoPort;
			slots[slot] = c;
			slotNames[slot] = h.name;
			slotReady[slot] = false;
			send(c, MsgType::Welcome, encodeWelcome((uint8_t)slot));
			publishState();
			break;
		}
		case MsgType::Ready:
		{
			bool ready;
			if (c->slot > 0 && decodeReady(f.payload, ready) && slotReady[c->slot] != ready)
			{
				slotReady[c->slot] = ready;
				publishState();
			}
			break;
		}
		case MsgType::Leave:
			onDisconnect(c);
			break;
		default:	// Ping (its arrival is what counts); anything else is not for a host
			break;
		}
	}

	void closeLater(const ConnPtr& c)
	{
		if (c->writing || !c->out.empty())
		{
			// not written yet: look again shortly
			auto t = std::make_shared<asio::steady_timer>(io);
			t->expires_after(std::chrono::milliseconds(50));
			t->async_wait([this, c, t](const asio::error_code&) { closeLater(c); });
			return;
		}
		onDisconnect(c);
	}

	void acceptLoop()
	{
		if (!acceptor)
			return;
		auto c = std::make_shared<Conn>(io);
		acceptor->async_accept(c->sock, [this, c, a = acceptor.get()](const asio::error_code& ec) {
			if (acceptor.get() != a || ec == asio::error::operation_aborted)
				return;
			if (!ec)
			{
				asio::error_code e2;
				c->address = c->sock.remote_endpoint(e2).address().to_string();
				c->sock.set_option(tcp::no_delay(true), e2);
				c->lastRx = Clock::now();
				conns.push_back(c);
				readLoop(c);
			}
			acceptLoop();
		});
	}

	// ---- the joiner

	void joinerFrame(const ConnPtr& c, const FrameParser::Frame& f)
	{
		switch (f.type)
		{
		case MsgType::Welcome:
		{
			uint8_t slot;
			if (!decodeWelcome(f.payload, slot))
			{
				onDisconnect(c);
				return;
			}
			withView([&](View& view) {
				view.mySlot = slot;
				view.phase = Phase::InLobby;
				view.message.clear();
			});
			break;
		}
		case MsgType::Refuse:
		{
			RefuseReason why;
			decodeRefuse(f.payload, why);
			withView([&](View& view) {
				view.phase = Phase::Refused;
				view.refuse = why;
				view.message = refuseText(why);
			});
			closeConn(c);
			break;
		}
		case MsgType::State:
		{
			State s;
			if (!decode(f.payload, s))
			{
				onDisconnect(c);
				return;
			}
			withView([&](View& view) {
				if (s.revision >= view.state.revision)
					view.state = s;
			});
			break;
		}
		case MsgType::Start:
		{
			StartInfo info;
			if (!decode(f.payload, info))
			{
				onDisconnect(c);
				return;
			}
			withView([&](View& view) {
				view.start = info;
				view.phase = Phase::Started;
			});
			break;
		}
		case MsgType::Leave:
			onDisconnect(c);
			break;
		default:
			break;
		}
	}

	// ---- starting and stopping

	// Everything off, back to Idle. keepView: leave the view as it is (a refusal, a start).
	void reset(bool keepView)
	{
		if (hostConn)
			closeConn(hostConn);
		hostConn.reset();
		for (const ConnPtr& c : conns)
			closeConn(c);
		conns.clear();
		for (auto& s : slots)
			s.reset();
		if (acceptor)
		{
			asio::error_code ec;
			acceptor->close(ec);
			acceptor.reset();
		}
		hosting = false;
		started = false;
		beaconTimer.cancel();
		tickTimer.cancel();
		if (!keepView)
			withView([&](View& view) { view = View(); });
		updateDiscovery();
	}

	bool canStart(std::string& why)
	{
		if (!hosting || started)
		{
			why = "Not hosting";
			return false;
		}
		int n = 1;
		bool allReady = true;
		for (int i = 1; i < MaxPlayers; i++)
			if (slots[i])
			{
				n++;
				allReady = allReady && slotReady[i];
			}
		if (n < 2)
		{
			why = "Waiting for another player";
			return false;
		}
		if (!allReady)
		{
			why = "Waiting for the players to be ready";
			return false;
		}
		why.clear();
		return true;
	}
};

Lobby::Lobby(const Timing& timing) : impl(std::make_unique<Impl>(timing)) {}
Lobby::~Lobby() = default;

void Lobby::setBroadcastHook(std::function<void(bool)> hook)
{
	impl->run([&] { impl->broadcastHook = std::move(hook); });
}

void Lobby::addBeaconTarget(const std::string& address)
{
	impl->run([&] { impl->extraTargets.push_back(address); });
}

void Lobby::setDiscoveryPort(uint16_t port)
{
	impl->run([&] { impl->discoveryPort = port; });
}

void Lobby::startSearch()
{
	impl->run([&] {
		{
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->found.clear();
		}
		impl->searching = true;
		impl->openDiscovery();
	});
}

void Lobby::stopSearch()
{
	impl->run([&] {
		impl->searching = false;
		impl->updateDiscovery();
	});
}

std::vector<FoundGame> Lobby::games()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	const auto now = Clock::now();
	std::vector<std::pair<Clock::time_point, FoundGame>> list;
	for (auto it = impl->found.begin(); it != impl->found.end();)
	{
		if (now - it->second.at > std::chrono::milliseconds(impl->timing.visibleMs))
			it = impl->found.erase(it);
		else
		{
			list.push_back({ it->second.at, it->second.game });
			++it;
		}
	}
	std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second.hostId < b.second.hostId; });
	std::vector<FoundGame> out;
	for (auto& p : list)
		out.push_back(p.second);
	return out;
}

void Lobby::query(const std::string& address)
{
	impl->run([&] {
		if (!impl->disc)
			return;
		asio::error_code ec;
		const auto addr = asio::ip::make_address_v4(address, ec);
		if (ec)
			return;
		auto data = std::make_shared<std::vector<uint8_t>>(encodeQuery());
		impl->disc->async_send_to(asio::buffer(*data), udp::endpoint(addr, impl->discoveryPort), [data](const asio::error_code&, size_t) {});
	});
}

bool Lobby::host(const Identity& me, const Settings& settings, std::string& error)
{
	return impl->run([&] {
		impl->reset(false);
		try {
			auto acc = std::make_unique<tcp::acceptor>(impl->io);
			acc->open(tcp::v4());
			acc->set_option(asio::socket_base::reuse_address(true));
			acc->bind(tcp::endpoint(tcp::v4(), 0));
			acc->listen();
			impl->acceptor = std::move(acc);
		} catch (const std::exception& e) {
			error = "Can't open the game on this network";
			return false;
		}
		impl->me = me;
		impl->me.name = cleanName(me.name);
		impl->settings = settings;
		impl->hostId = impl->rng();
		impl->hosting = true;
		impl->started = false;
		impl->revision = 0;
		impl->slotNames = {};
		impl->slotReady = {};
		impl->lastPing = Clock::now();
		const std::string addr = localAddress();
		const State s = impl->currentState();
		const uint16_t port = impl->acceptor->local_endpoint().port();
		impl->withView([&](View& view) {
			view = View();
			view.role = Role::Host;
			view.phase = Phase::InLobby;
			view.mySlot = 0;
			view.state = s;
			view.hostAddress = addr;
			view.lobbyPort = port;
		});
		impl->updateDiscovery();
		if (!impl->disc)
		{
			// no beacon, so nobody finds this game; still hostable by address? No: the query needs it too
			impl->reset(false);
			error = "Can't use the network for announcing the game";
			return false;
		}
		impl->acceptLoop();
		impl->beaconTick();
		impl->startTick();
		return true;
	});
}

void Lobby::setSettings(const Settings& settings)
{
	impl->run([&] {
		if (!impl->hosting || impl->started || impl->settings == settings)
			return;
		impl->settings = settings;
		impl->publishState();
	});
}

bool Lobby::canStart(std::string& why)
{
	return impl->run([&] { return impl->canStart(why); });
}

bool Lobby::start(uint8_t inputDelay, std::string& error)
{
	return impl->run([&] {
		if (!impl->canStart(error))
			return false;
		// players in slot order, holes closed
		std::vector<int> order;
		for (int i = 0; i < MaxPlayers; i++)
			if (i == 0 || impl->slots[i])
				order.push_back(i);
		const State s = impl->currentState();
		StartInfo base;
		base.inputDelay = inputDelay;
		base.sessionId = impl->rng();
		base.settings = impl->settings;
		for (int i : order)
			base.peers.push_back({ i == 0 ? std::string() : s.players[i].address, s.players[i].ggpoPort });
		impl->started = true;
		for (size_t k = 0; k < order.size(); k++)
		{
			StartInfo mine = base;
			mine.localIndex = (int)k;
			if (order[k] == 0)
			{
				impl->withView([&](View& view) { view.start = mine; view.phase = Phase::Started; });
			}
			else
				impl->send(impl->slots[order[k]], MsgType::Start, encode(mine));
		}
		return true;
	});
}

void Lobby::join(const Identity& me, const std::string& address, uint16_t lobbyPort)
{
	impl->run([&] {
		impl->reset(false);
		impl->me = me;
		impl->me.name = cleanName(me.name);
		impl->hosting = false;
		impl->withView([&](View& view) {
			view = View();
			view.role = Role::Joiner;
			view.phase = Phase::Connecting;
			view.hostAddress = address;
			view.lobbyPort = lobbyPort;
			view.message = "Connecting...";
		});
		asio::error_code ec;
		auto addr = asio::ip::make_address_v4(address, ec);
		if (ec)
		{
			impl->withView([&](View& view) { view.phase = Phase::Failed; view.message = "Not a valid address"; });
			return;
		}
		auto c = std::make_shared<Impl::Conn>(impl->io);
		impl->hostConn = c;
		impl->connectStart = Clock::now();
		impl->lastPing = Clock::now();
		c->sock.async_connect(tcp::endpoint(addr, lobbyPort), [impl = impl.get(), c](const asio::error_code& ec) {
			if (c->closed)
				return;
			if (ec)
			{
				impl->onDisconnect(c);
				return;
			}
			asio::error_code e2;
			c->sock.set_option(tcp::no_delay(true), e2);
			c->lastRx = Clock::now();
			Hello h;
			h.name = impl->me.name;
			h.appVersion = impl->me.appVersion;
			h.disc = impl->me.disc;
			h.ggpoPort = impl->me.ggpoPort;
			impl->send(c, MsgType::Hello, encode(h));
			impl->readLoop(c);
		});
		impl->startTick();
	});
}

void Lobby::setReady(bool ready)
{
	impl->run([&] {
		if (impl->hosting || !impl->hostConn)
			return;
		impl->send(impl->hostConn, MsgType::Ready, encodeReady(ready));
	});
}

void Lobby::leave()
{
	impl->run([&] {
		// the goodbyes are written at once (the sockets close right after)
		const auto bye = encodeFrame(MsgType::Leave);
		auto sayBye = [&](const Impl::ConnPtr& c) {
			if (c && !c->closed && c->sock.is_open())
			{
				asio::error_code ec;
				asio::write(c->sock, asio::buffer(bye), ec);
			}
		};
		if (impl->hosting)
		{
			for (int i = 1; i < MaxPlayers; i++)
				sayBye(impl->slots[i]);
		}
		else if (impl->hostConn)
		{
			bool inLobby;
			impl->withView([&](View& view) { inLobby = view.phase == Phase::InLobby; });
			if (inLobby)
				sayBye(impl->hostConn);
		}
		impl->reset(false);
	});
}

View Lobby::view()
{
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->v;
}

}
