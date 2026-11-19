/*
	tcvr: tests of internet play (core/network/tcvr_inet*): the cryptography and wire formats, then a real
	tcvr-server (tcvr/server/tcvr_server.cpp, built into this test) with real lobbies talking through it
	over this computer's loopback: listing, joining, the lobby's messages end to end, the match's packets.
	The NAT cases (direct paths, relay, host handover) are in tcvr/tools/inet-nat-test.sh.
*/
#include "gtest/gtest.h"
#include "network/tcvr_inet.h"
#include "network/tcvr_inet_proto.h"
#include "network/tcvr_lobby.h"

#define TCVR_SERVER_NO_MAIN
#include "../../tcvr/server/tcvr_server.cpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

using namespace tcvr;

namespace
{

bool waitUntil(const std::function<bool()>& cond, int ms = 8000)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end)
	{
		if (cond())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return cond();
}

// a tcvr-server on 127.0.0.1, on its own thread
struct TestServer
{
	asio::io_context io;
	Options opt;
	inet::KeyPair key = inet::KeyPair::generate();
	std::unique_ptr<Server> server;
	std::thread thread;
	explicit TestServer(uint16_t port)
	{
		opt.port = port;
		server = std::make_unique<Server>(io, opt, key);
		started = server->start();
		thread = std::thread([this] { io.run(); });
	}
	~TestServer()
	{
		asio::post(io, [this] { server->stop(); io.stop(); });
		thread.join();
	}
	bool started = false;
};

}

TEST(TcvrInetTest, handshakeAndChannel)
{
	const inet::KeyPair server = inet::KeyPair::generate();
	const inet::KeyPair client = inet::KeyPair::generate();
	const auto hello = inet::clientHello(client);
	std::vector<uint8_t> reply;
	uint8_t ks[64], kc[64];
	ASSERT_TRUE(inet::serverHandshake(hello.data(), hello.size(), server, reply, ks));
	inet::Key seen;
	ASSERT_TRUE(inet::clientHandshake(client, reply.data(), reply.size(), seen, kc));
	EXPECT_EQ(seen, server.pub);
	EXPECT_EQ(0, memcmp(ks, kc, 64));

	inet::Channel a, b;
	a.init(kc, false);
	b.init(ks, true);
	std::vector<uint8_t> stream;
	for (int i = 0; i < 3; i++)
	{
		auto f = a.seal({ (uint8_t)i, 2, 3 });
		stream.insert(stream.end(), f.begin(), f.end());
	}
	std::vector<std::vector<uint8_t>> out;
	// in pieces, as TCP delivers it
	ASSERT_TRUE(b.feed(stream.data(), 5, out));
	ASSERT_TRUE(b.feed(stream.data() + 5, stream.size() - 5, out));
	ASSERT_EQ(3u, out.size());
	EXPECT_EQ(2, out[2][0]);
	// a changed byte is refused
	auto f = a.seal({ 9, 9 });
	f[3] ^= 1;
	out.clear();
	EXPECT_FALSE(b.feed(f.data(), f.size(), out));
	// another server key: other channel keys (a man in the middle can't read on)
	const inet::KeyPair other = inet::KeyPair::generate();
	std::vector<uint8_t> reply2;
	uint8_t ko[64];
	ASSERT_TRUE(inet::serverHandshake(hello.data(), hello.size(), other, reply2, ko));
	EXPECT_NE(0, memcmp(ko, kc, 64));
}

TEST(TcvrInetTest, pairKeysAndPackets)
{
	const inet::KeyPair a = inet::KeyPair::generate();
	const inet::KeyPair b = inet::KeyPair::generate();
	const inet::PairKey ka = inet::derivePairKey(a, b.pub, "");
	const inet::PairKey kb = inet::derivePairKey(b, a.pub, "");
	ASSERT_TRUE(ka.valid && kb.valid);
	EXPECT_EQ(ka.key, kb.key);
	EXPECT_NE(ka.dir, kb.dir);
	const auto pkt = inet::sealPeer(ka, 0, 7, 1, { 1, 2, 3, 4 });
	uint32_t from;
	ASSERT_TRUE(inet::peekPeerFrom(pkt.data(), pkt.size(), from));
	EXPECT_EQ(7u, from);
	uint64_t counter;
	std::vector<uint8_t> plain;
	ASSERT_TRUE(inet::openPeer(kb, 0, pkt.data(), pkt.size(), counter, plain));
	EXPECT_EQ(1u, counter);
	EXPECT_EQ(4u, plain.size());
	// its own packet doesn't open as the other's (direction), nor in the other domain
	EXPECT_FALSE(inet::openPeer(ka, 0, pkt.data(), pkt.size(), counter, plain));
	EXPECT_FALSE(inet::openPeer(kb, 1, pkt.data(), pkt.size(), counter, plain));
	// another password: nothing opens
	const inet::PairKey kc = inet::derivePairKey(b, a.pub, "red");
	EXPECT_FALSE(inet::openPeer(kc, 0, pkt.data(), pkt.size(), counter, plain));
	// the server's UDP MAC
	inet::Key k {};
	k[0] = 1;
	auto d = inet::sealServerUdp(inet::UdpType::Register, 5, { 1, 2, 3, 4 }, k);
	EXPECT_TRUE(inet::checkServerUdpMac(d.data(), d.size(), k));
	d[6] ^= 1;
	EXPECT_FALSE(inet::checkServerUdpMac(d.data(), d.size(), k));
}

TEST(TcvrInetTest, replayWindow)
{
	inet::ReplayWindow w;
	EXPECT_TRUE(w.accept(1));
	EXPECT_FALSE(w.accept(1));
	EXPECT_TRUE(w.accept(5));
	EXPECT_TRUE(w.accept(3));		// late but new
	EXPECT_FALSE(w.accept(3));
	EXPECT_TRUE(w.accept(3000));
	EXPECT_FALSE(w.accept(5));		// too old now
	EXPECT_TRUE(w.accept(2999));
	EXPECT_FALSE(w.accept(0));
}

TEST(TcvrInetTest, addressesAndServerNames)
{
	EXPECT_EQ("10.1.2.3", inet::virtualAddress(0x010203));	// leakscan:allow (a made-up player number, never routed)
	EXPECT_EQ(0x010203u, inet::clientIdOf("10.1.2.3"));	// leakscan:allow (the same)
	EXPECT_EQ(0u, inet::clientIdOf("192.0.2.1"));
	std::string host;
	uint16_t port;
	ASSERT_TRUE(inet::splitServer("game.example.org", host, port));
	EXPECT_EQ("game.example.org", host);
	EXPECT_EQ(inet::DefaultServerPort, port);
	ASSERT_TRUE(inet::splitServer(" 192.0.2.7:20000 ", host, port));
	EXPECT_EQ("192.0.2.7", host);
	EXPECT_EQ(20000, port);
	EXPECT_FALSE(inet::splitServer("host:99999", host, port));
	EXPECT_FALSE(inet::splitServer("", host, port));
}

// Two lobbies through a real server: the game is listed, joined, readied, started; then the match's
// packets go both ways through the local ports GGPO would use.
TEST(TcvrInetTest, lobbyAndMatchThroughServer)
{
	TestServer srv(19871);
	ASSERT_TRUE(srv.started);
	auto makeClient = [](const char *name) {
		auto c = std::make_shared<inet::InternetClient>();
		inet::ClientConfig cfg;
		cfg.server = "127.0.0.1:19871";
		cfg.name = name;
		cfg.appVersion = "test";
		c->configure(cfg);
		return c;
	};
	auto ca = makeClient("AAA");
	auto cb = makeClient("BBB");
	ASSERT_TRUE(waitUntil([&] { return ca->status() == inet::Status::Online && cb->status() == inet::Status::Online; }));
	EXPECT_EQ(inet::fingerprint(srv.key.pub), ca->serverFingerprint());

	lobby::Timing t;
	lobby::Lobby la(t), lb(t);
	la.setRouter(ca);
	lb.setRouter(cb);
	lobby::Identity ia, ib;
	ia.name = "AAA"; ia.appVersion = "test"; ia.ggpoPort = 0;
	ib.name = "BBB"; ib.appVersion = "test"; ib.ggpoPort = 0;
	asio::io_context io;
	asio::ip::udp::socket ga(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
	asio::ip::udp::socket gb(io, asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
	ia.ggpoPort = ga.local_endpoint().port();
	ib.ggpoPort = gb.local_endpoint().port();

	std::string error;
	ASSERT_TRUE(la.host(ia, { 1, 2, 60 }, error)) << error;
	lb.startSearch();
	ASSERT_TRUE(waitUntil([&] { return !lb.games().empty(); }));
	const lobby::FoundGame g = lb.games().front();
	EXPECT_EQ("AAA", g.beacon.hostName);
	EXPECT_EQ(ca->localAddress(), g.address);
	lb.stopSearch();
	lb.join(ib, g.address, g.beacon.lobbyPort);
	ASSERT_TRUE(waitUntil([&] { return lb.view().phase == lobby::Phase::InLobby && la.view().state.count() == 2; }));
	EXPECT_EQ(cb->localAddress(), la.view().state.players[1].address);
	lb.setReady(true);
	ASSERT_TRUE(waitUntil([&] { std::string why; return la.canStart(why); }));
	ASSERT_TRUE(la.start(2, error)) << error;
	ASSERT_TRUE(waitUntil([&] { return lb.view().phase == lobby::Phase::Started; }));

	const lobby::View va = la.view(), vb = lb.view();
	const std::string pa = ca->ggpoPeers(va.start, ca->localAddress(), ia.ggpoPort);
	const std::string pb = cb->ggpoPeers(vb.start, vb.hostAddress, ib.ggpoPort);
	ASSERT_FALSE(pa.empty());
	ASSERT_FALSE(pb.empty());
	auto portOf = [](const std::string& list, int index) {
		size_t at = 0;
		for (int i = 0; i < index; i++)
			at = list.find(',', at) + 1;
		const size_t end = list.find(',', at);
		const std::string item = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
		return (uint16_t)std::atoi(item.substr(item.rfind(':') + 1).c_str());
	};
	// A (player 1) sends to its port for player 2; B's GGPO socket receives it from B's port for player 1
	const uint16_t aToB = portOf(pa, 1), bToA = portOf(pb, 0);
	const asio::ip::address lo = asio::ip::address_v4::loopback();
	gb.non_blocking(true);
	bool got = false;
	asio::ip::udp::endpoint from;
	uint8_t buf[64];
	for (int i = 0; i < 100 && !got; i++)
	{
		ga.send_to(asio::buffer("hello", 5), asio::ip::udp::endpoint(lo, aToB));
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		asio::error_code ec;
		const size_t n = gb.receive_from(asio::buffer(buf), from, 0, ec);
		got = !ec && n == 5 && memcmp(buf, "hello", 5) == 0;
	}
	ASSERT_TRUE(got);
	EXPECT_EQ(bToA, from.port());

	ca->matchOver();
	cb->matchOver();
	la.leave();
	lb.leave();
}

TEST(TcvrInetTest, pinsAndPolicy)
{
	EXPECT_TRUE(inet::validPin(""));
	EXPECT_TRUE(inet::validPin("0042"));
	EXPECT_FALSE(inet::validPin("123"));
	EXPECT_FALSE(inet::validPin("12a4"));
	EXPECT_TRUE(inet::policyAllows(inet::GamePolicy::All, true));
	EXPECT_FALSE(inet::policyAllows(inet::GamePolicy::PublicOnly, true));
	EXPECT_TRUE(inet::policyAllows(inet::GamePolicy::PrivateOnly, true));
	EXPECT_FALSE(inet::policyAllows(inet::GamePolicy::PrivateOnly, false));
	std::vector<inet::GameEntry> in { { 5, true, { 1, 2 } }, { 6, false, {} } }, out;
	const auto msg = inet::encodeGames(in);
	inet::Reader r(msg.data() + 1, msg.size() - 1);
	ASSERT_TRUE(inet::decodeGames(r, out));
	ASSERT_EQ(2u, out.size());
	EXPECT_TRUE(out[0].isPrivate);
	EXPECT_FALSE(out[1].isPrivate);
}

// A private game: the wrong PIN is turned away by the host's copy ("Wrong PIN"), the right one gets in.
TEST(TcvrInetTest, privateGamePin)
{
	TestServer srv(19873);
	ASSERT_TRUE(srv.started);
	auto makeClient = [](const char *name) {
		auto c = std::make_shared<inet::InternetClient>();
		inet::ClientConfig cfg;
		cfg.server = "127.0.0.1:19873";
		cfg.name = name;
		cfg.appVersion = "test";
		c->configure(cfg);
		return c;
	};
	auto ca = makeClient("AAA"), cb = makeClient("BBB"), cc = makeClient("CCC");
	ASSERT_TRUE(waitUntil([&] { return ca->status() == inet::Status::Online && cb->status() == inet::Status::Online
			&& cc->status() == inet::Status::Online; }));
	lobby::Lobby la, lb, lc;
	la.setRouter(ca);
	lb.setRouter(cb);
	lc.setRouter(cc);
	lobby::Identity id;
	id.appVersion = "test";
	std::string error;
	ca->setGamePin("2580");
	id.name = "AAA";
	ASSERT_TRUE(la.host(id, {}, error)) << error;
	lb.startSearch();
	ASSERT_TRUE(waitUntil([&] { return !lb.games().empty(); }));
	const lobby::FoundGame g = lb.games().front();
	EXPECT_TRUE(g.isPrivate);
	cb->setGamePin("1111");
	id.name = "BBB";
	lb.join(id, g.address, g.beacon.lobbyPort);
	ASSERT_TRUE(waitUntil([&] { return lb.view().phase == lobby::Phase::Failed; }));
	EXPECT_EQ("Wrong PIN", cb->lastRefusal());
	cc->setGamePin("2580");
	id.name = "CCC";
	lc.join(id, g.address, g.beacon.lobbyPort);
	ASSERT_TRUE(waitUntil([&] { return lc.view().phase == lobby::Phase::InLobby && la.view().state.count() == 2; }));
	// BBB again at once: still waiting after its wrong PIN
	cb->setGamePin("2580");
	lb.join(id, g.address, g.beacon.lobbyPort);
	ASSERT_TRUE(waitUntil([&] { return lb.view().phase == lobby::Phase::Failed; }));
	EXPECT_EQ("Too many wrong PINs: wait a minute", cb->lastRefusal());
}

// A changed server key stops the client (a man in the middle, or a server that lost its key).
TEST(TcvrInetTest, pinnedKeyMismatch)
{
	TestServer srv(19872);
	ASSERT_TRUE(srv.started);
	inet::InternetClient c;
	inet::ClientConfig cfg;
	cfg.server = "127.0.0.1:19872";
	cfg.name = "AAA";
	cfg.appVersion = "test";
	cfg.pinnedKey = std::string(64, 'a');
	c.configure(cfg);
	ASSERT_TRUE(waitUntil([&] { return c.status() == inet::Status::KeyChanged; }));
}
