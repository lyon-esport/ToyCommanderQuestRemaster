/*
	tcvr: internet play, wire formats and cryptography. See tcvr_inet_proto.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "tcvr_inet_proto.h"
#include "monocypher/monocypher.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tcvr::inet
{

// ---- primitives ------------------------------------------------------------------------------

void randomBytes(void *out, size_t size)
{
#ifdef _WIN32
	if (BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		throw std::runtime_error("no secure random source");
#else
	int fd = ::open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		throw std::runtime_error("no secure random source");
	uint8_t *p = (uint8_t *)out;
	size_t got = 0;
	while (got < size)
	{
		ssize_t n = ::read(fd, p + got, size - got);
		if (n <= 0)
		{
			::close(fd);
			throw std::runtime_error("no secure random source");
		}
		got += (size_t)n;
	}
	::close(fd);
#endif
}

uint32_t randomU32()
{
	uint32_t v;
	randomBytes(&v, sizeof(v));
	return v;
}

KeyPair KeyPair::generate()
{
	Key secret;
	randomBytes(secret.data(), secret.size());
	KeyPair kp = fromSecret(secret);
	crypto_wipe(secret.data(), secret.size());
	return kp;
}

KeyPair KeyPair::fromSecret(const Key& secret)
{
	KeyPair kp;
	kp.secret = secret;
	crypto_x25519_public_key(kp.pub.data(), kp.secret.data());
	return kp;
}

void KeyPair::wipe()
{
	crypto_wipe(secret.data(), secret.size());
}

std::string toHex(const uint8_t *data, size_t size)
{
	static const char *digits = "0123456789abcdef";
	std::string s;
	for (size_t i = 0; i < size; i++)
	{
		s += digits[data[i] >> 4];
		s += digits[data[i] & 15];
	}
	return s;
}

bool fromHex(const std::string& hex, uint8_t *out, size_t size)
{
	if (hex.size() != size * 2)
		return false;
	auto val = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < size; i++)
	{
		int hi = val(hex[2 * i]), lo = val(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return true;
}

std::string fingerprint(const Key& pub)
{
	uint8_t h[8];
	crypto_blake2b(h, sizeof(h), pub.data(), pub.size());
	const std::string hex = toHex(h, sizeof(h));
	return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
}

// ---- byte helpers ------------------------------------------------------------------------------

void Writer::str(const std::string& s)
{
	const size_t n = std::min<size_t>(s.size(), 255);
	u8((uint8_t)n);
	bytes((const uint8_t *)s.data(), n);
}

void Writer::blob(const std::vector<uint8_t>& v)
{
	const size_t n = std::min<size_t>(v.size(), 65535);
	u16((uint16_t)n);
	bytes(v.data(), n);
}

std::string Reader::str(size_t maxLen)
{
	const size_t len = u8();
	if (!need(len) || len > maxLen)
	{
		ok = false;
		return std::string();
	}
	std::string s((const char *)p + at, len);
	at += len;
	return s;
}

std::vector<uint8_t> Reader::blob(size_t maxLen)
{
	const size_t len = u16();
	if (!need(len) || len > maxLen)
	{
		ok = false;
		return {};
	}
	std::vector<uint8_t> v(p + at, p + at + len);
	at += len;
	return v;
}

// ---- the control channel ---------------------------------------------------------------------

static const uint8_t Magic[4] = { 'T', 'C', 'V', 'R' };

std::vector<uint8_t> clientHello(const KeyPair& ephemeral)
{
	Writer w;
	w.bytes(Magic, 4);
	w.u8(ProtocolVersion);
	w.key(ephemeral.pub);
	return w.b;
}

static void deriveChannelKeys(const uint8_t dh1[32], const uint8_t dh2[32], const Key& ce, const Key& se, const Key& ss,
		uint8_t keys[64])
{
	static const char label[] = "tcvr-inet-v1";
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, 64);
	crypto_blake2b_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
	crypto_blake2b_update(&ctx, dh1, 32);
	crypto_blake2b_update(&ctx, dh2, 32);
	crypto_blake2b_update(&ctx, ce.data(), 32);
	crypto_blake2b_update(&ctx, se.data(), 32);
	crypto_blake2b_update(&ctx, ss.data(), 32);
	crypto_blake2b_final(&ctx, keys);
}

static bool allZero(const uint8_t *p, size_t n)
{
	uint8_t acc = 0;
	for (size_t i = 0; i < n; i++)
		acc |= p[i];
	return acc == 0;
}

bool serverHandshake(const uint8_t *hello, size_t size, const KeyPair& serverStatic, std::vector<uint8_t>& reply, uint8_t keys[64])
{
	if (size != ClientHelloSize || memcmp(hello, Magic, 4) != 0 || hello[4] != ProtocolVersion)
		return false;
	Key ce;
	memcpy(ce.data(), hello + 5, 32);
	KeyPair se = KeyPair::generate();
	uint8_t dh1[32], dh2[32];
	crypto_x25519(dh1, se.secret.data(), ce.data());
	crypto_x25519(dh2, serverStatic.secret.data(), ce.data());
	const bool bad = allZero(dh1, 32) || allZero(dh2, 32);	// a low-order point: refuse
	deriveChannelKeys(dh1, dh2, ce, se.pub, serverStatic.pub, keys);
	crypto_wipe(dh1, 32);
	crypto_wipe(dh2, 32);
	Writer w;
	w.bytes(Magic, 4);
	w.u8(ProtocolVersion);
	w.key(se.pub);
	w.key(serverStatic.pub);
	reply = w.b;
	se.wipe();
	return !bad;
}

bool clientHandshake(const KeyPair& ephemeral, const uint8_t *reply, size_t size, Key& serverStatic, uint8_t keys[64])
{
	if (size != ServerReplySize || memcmp(reply, Magic, 4) != 0 || reply[4] != ProtocolVersion)
		return false;
	Key se;
	memcpy(se.data(), reply + 5, 32);
	memcpy(serverStatic.data(), reply + 37, 32);
	uint8_t dh1[32], dh2[32];
	crypto_x25519(dh1, ephemeral.secret.data(), se.data());
	crypto_x25519(dh2, ephemeral.secret.data(), serverStatic.data());
	const bool bad = allZero(dh1, 32) || allZero(dh2, 32);
	deriveChannelKeys(dh1, dh2, ephemeral.pub, se, serverStatic, keys);
	crypto_wipe(dh1, 32);
	crypto_wipe(dh2, 32);
	return !bad;
}

static void counterNonce(uint8_t nonce[24], uint8_t tag, uint64_t counter)
{
	memset(nonce, 0, 24);
	nonce[0] = tag;
	for (int i = 0; i < 8; i++)
		nonce[16 + i] = (uint8_t)(counter >> (8 * i));
}

void Channel::init(const uint8_t keys[64], bool server)
{
	memcpy(server ? recvKey.data() : sendKey.data(), keys, 32);
	memcpy(server ? sendKey.data() : recvKey.data(), keys + 32, 32);
	sendCount = recvCount = 0;
	buffer.clear();
}

std::vector<uint8_t> Channel::seal(const std::vector<uint8_t>& plain)
{
	const size_t len = plain.size() + 16;
	std::vector<uint8_t> out(2 + len);
	out[0] = (uint8_t)len;
	out[1] = (uint8_t)(len >> 8);
	uint8_t nonce[24];
	counterNonce(nonce, 0xC0, sendCount++);
	crypto_aead_lock(out.data() + 2, out.data() + 2 + plain.size(), sendKey.data(), nonce, out.data(), 2,
			plain.data(), plain.size());
	return out;
}

bool Channel::feed(const uint8_t *data, size_t size, std::vector<std::vector<uint8_t>>& out)
{
	buffer.insert(buffer.end(), data, data + size);
	size_t pos = 0;
	bool ok = true;
	while (buffer.size() - pos >= 2)
	{
		const size_t len = buffer[pos] | (buffer[pos + 1] << 8);
		if (len < 16 || len > MaxFrame + 16)
		{
			ok = false;
			break;
		}
		if (buffer.size() - pos - 2 < len)
			break;
		std::vector<uint8_t> plain(len - 16);
		uint8_t nonce[24];
		counterNonce(nonce, 0xC0, recvCount);
		if (crypto_aead_unlock(plain.data(), buffer.data() + pos + 2 + plain.size(), recvKey.data(), nonce,
				buffer.data() + pos, 2, buffer.data() + pos + 2, plain.size()) != 0)
		{
			ok = false;
			break;
		}
		recvCount++;
		out.push_back(std::move(plain));
		pos += 2 + len;
	}
	buffer.erase(buffer.begin(), buffer.begin() + pos);
	return ok;
}

void Channel::wipe()
{
	crypto_wipe(sendKey.data(), 32);
	crypto_wipe(recvKey.data(), 32);
}

// ---- control messages ------------------------------------------------------------------------

std::string Endpoint::address() const
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	return buf;
}

std::string Endpoint::text() const
{
	return address() + ":" + std::to_string(port);
}

bool Endpoint::parse(const std::string& address, uint16_t port, Endpoint& out)
{
	unsigned a, b, c, d;
	char extra;
	if (sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	out.ip = a << 24 | b << 16 | c << 8 | d;
	out.port = port;
	return true;
}

void put(Writer& w, const Endpoint& e)
{
	w.u32(e.ip);
	w.u16(e.port);
}

Endpoint getEndpoint(Reader& r)
{
	Endpoint e;
	e.ip = r.u32();
	e.port = r.u16();
	return e;
}

static void putLan(Writer& w, const std::vector<Endpoint>& lan)
{
	const size_t n = std::min(lan.size(), MaxLanEndpoints);
	w.u8((uint8_t)n);
	for (size_t i = 0; i < n; i++)
		put(w, lan[i]);
}

static bool getLan(Reader& r, std::vector<Endpoint>& lan)
{
	const size_t n = r.u8();
	if (n > MaxLanEndpoints)
		return false;
	lan.clear();
	for (size_t i = 0; i < n; i++)
		lan.push_back(getEndpoint(r));
	return r.ok;
}

std::vector<uint8_t> encode(const HelloMsg& m)
{
	Writer w;
	w.u8((uint8_t)Ctl::Hello);
	w.u8(m.protocol);
	w.str(m.appVersion);
	w.str(m.name.substr(0, MaxNameLen));
	w.key(m.memberKey);
	putLan(w, m.lanAddrs);
	return w.b;
}

bool decode(Reader& r, HelloMsg& m)
{
	m.protocol = r.u8();
	m.appVersion = r.str(64);
	m.name = r.str(MaxNameLen);
	m.memberKey = r.key();
	return getLan(r, m.lanAddrs) && r.done();
}

std::vector<uint8_t> encode(const WelcomeMsg& m)
{
	Writer w;
	w.u8((uint8_t)Ctl::Welcome);
	w.u32(m.clientId);
	w.key(m.udpKey);
	w.u16(m.udpPort);
	put(w, m.seenAs);
	w.u8((uint8_t)m.policy);
	return w.b;
}

bool decode(Reader& r, WelcomeMsg& m)
{
	m.clientId = r.u32();
	m.udpKey = r.key();
	m.udpPort = r.u16();
	m.seenAs = getEndpoint(r);
	m.policy = (GamePolicy)r.u8();
	return r.done() && (uint8_t)m.policy <= 2;
}

std::vector<uint8_t> encodeGames(const std::vector<GameEntry>& games)
{
	Writer w;
	w.u8((uint8_t)Ctl::Games);
	const size_t n = std::min<size_t>(games.size(), 64);
	w.u8((uint8_t)n);
	for (size_t i = 0; i < n; i++)
	{
		w.u32(games[i].ownerId);
		w.u8(games[i].isPrivate ? 1 : 0);
		w.blob(games[i].beacon);
	}
	return w.b;
}

bool decodeGames(Reader& r, std::vector<GameEntry>& games)
{
	const size_t n = r.u8();
	games.clear();
	for (size_t i = 0; i < n && r.ok; i++)
	{
		GameEntry g;
		g.ownerId = r.u32();
		g.isPrivate = (r.u8() & 1) != 0;
		g.beacon = r.blob(512);
		games.push_back(std::move(g));
	}
	return r.done();
}

bool validPin(const std::string& pin)
{
	if (pin.empty())
		return true;
	if (pin.size() != PinLength)
		return false;
	for (char c : pin)
		if (c < '0' || c > '9')
			return false;
	return true;
}

const char *policyText(GamePolicy p)
{
	switch (p)
	{
	case GamePolicy::PublicOnly: return "public games only";
	case GamePolicy::PrivateOnly: return "private games only";
	default: return "public and private games";
	}
}

bool policyAllows(GamePolicy p, bool isPrivate)
{
	return p == GamePolicy::All || (p == GamePolicy::PrivateOnly) == isPrivate;
}

std::vector<uint8_t> encodeAnnounce(const std::vector<uint8_t>& beacon, bool isPrivate)
{
	Writer w;
	w.u8((uint8_t)Ctl::Announce);
	w.u8(isPrivate ? 1 : 0);
	w.blob(beacon);
	return w.b;
}

std::vector<uint8_t> encodeConnect(uint32_t target)
{
	Writer w;
	w.u8((uint8_t)Ctl::Connect);
	w.u32(target);
	return w.b;
}

std::vector<uint8_t> encodeConnected(uint32_t target, bool ok, const std::string& why)
{
	Writer w;
	w.u8((uint8_t)Ctl::Connected);
	w.u32(target);
	w.u8(ok ? 1 : 0);
	w.str(why);
	return w.b;
}

std::vector<uint8_t> encodeMembers(const std::vector<MemberInfo>& members)
{
	Writer w;
	w.u8((uint8_t)Ctl::Members);
	const size_t n = std::min<size_t>(members.size(), MaxMembers);
	w.u8((uint8_t)n);
	for (size_t i = 0; i < n; i++)
	{
		const MemberInfo& m = members[i];
		w.u32(m.clientId);
		w.str(m.name.substr(0, MaxNameLen));
		w.key(m.memberKey);
		put(w, m.udp);
		putLan(w, m.lanAddrs);
		w.u8(m.owner ? 1 : 0);
	}
	return w.b;
}

bool decodeMembers(Reader& r, std::vector<MemberInfo>& members)
{
	const size_t n = r.u8();
	if (n > (size_t)MaxMembers)
		return false;
	members.clear();
	for (size_t i = 0; i < n && r.ok; i++)
	{
		MemberInfo m;
		m.clientId = r.u32();
		m.name = r.str(MaxNameLen);
		m.memberKey = r.key();
		m.udp = getEndpoint(r);
		if (!getLan(r, m.lanAddrs))
			return false;
		m.owner = r.u8() != 0;
		members.push_back(std::move(m));
	}
	return r.done();
}

std::vector<uint8_t> encode(const StreamMsg& m)
{
	Writer w;
	w.u8((uint8_t)Ctl::Stream);
	w.u32(m.peer);
	w.u32(m.streamId);
	w.u8((uint8_t)m.kind);
	w.blob(m.data);
	return w.b;
}

bool decode(Reader& r, StreamMsg& m)
{
	m.peer = r.u32();
	m.streamId = r.u32();
	m.kind = (StreamKind)r.u8();
	m.data = r.blob(MaxFrame - 32);
	return r.done() && m.kind >= StreamKind::Open && m.kind <= StreamKind::Refused;
}

std::vector<uint8_t> encodeError(const std::string& text)
{
	Writer w;
	w.u8((uint8_t)Ctl::Error);
	w.str(text);
	return w.b;
}

std::vector<uint8_t> encodeSimple(Ctl type)
{
	return { (uint8_t)type };
}

// ---- UDP -------------------------------------------------------------------------------------

std::vector<uint8_t> sealServerUdp(UdpType type, uint32_t clientId, const std::vector<uint8_t>& payload, const Key& udpKey)
{
	Writer w;
	w.u8((uint8_t)type);
	w.u32(clientId);
	w.bytes(payload);
	uint8_t mac[UdpMacSize];
	crypto_blake2b_keyed(mac, sizeof(mac), udpKey.data(), udpKey.size(), w.b.data(), w.b.size());
	w.bytes(mac, sizeof(mac));
	return w.b;
}

bool checkServerUdpMac(const uint8_t *data, size_t size, const Key& udpKey)
{
	if (size < UdpMacSize + 5)
		return false;
	uint8_t mac[UdpMacSize];
	crypto_blake2b_keyed(mac, sizeof(mac), udpKey.data(), udpKey.size(), data, size - UdpMacSize);
	return crypto_verify16(mac, data + size - UdpMacSize) == 0;
}

void PairKey::wipe()
{
	crypto_wipe(key.data(), key.size());
	valid = false;
}

PairKey derivePairKey(const KeyPair& mine, const Key& theirs, const std::string& password)
{
	PairKey k;
	uint8_t dh[32];
	crypto_x25519(dh, mine.secret.data(), theirs.data());
	if (allZero(dh, 32))
		return k;
	const bool first = std::lexicographical_compare(mine.pub.begin(), mine.pub.end(), theirs.begin(), theirs.end());
	const Key& a = first ? mine.pub : theirs;
	const Key& b = first ? theirs : mine.pub;
	uint8_t pw[32];
	static const char pwLabel[] = "tcvr-game-password-v1";
	crypto_blake2b_keyed(pw, sizeof(pw), (const uint8_t *)pwLabel, sizeof(pwLabel) - 1,
			(const uint8_t *)password.data(), password.size());
	static const char label[] = "tcvr-pair-v1";
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, 32);
	crypto_blake2b_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
	crypto_blake2b_update(&ctx, dh, 32);
	crypto_blake2b_update(&ctx, a.data(), 32);
	crypto_blake2b_update(&ctx, b.data(), 32);
	crypto_blake2b_update(&ctx, pw, 32);
	crypto_blake2b_final(&ctx, k.key.data());
	crypto_wipe(dh, 32);
	crypto_wipe(pw, 32);
	k.dir = first ? 0 : 1;
	k.valid = true;
	return k;
}

static void peerNonce(uint8_t nonce[24], uint8_t domain, uint8_t dir, uint64_t counter)
{
	counterNonce(nonce, (uint8_t)(0x10 | domain << 1 | dir), counter);
}

std::vector<uint8_t> sealPeer(const PairKey& k, uint8_t domain, uint32_t fromId, uint64_t counter, const std::vector<uint8_t>& plain)
{
	Writer w;
	w.u8((uint8_t)UdpType::Peer);
	w.u32(fromId);
	w.u64(counter);
	std::vector<uint8_t> out = std::move(w.b);
	out.resize(PeerHeaderSize + plain.size() + 16);
	uint8_t nonce[24];
	peerNonce(nonce, domain, k.dir, counter);
	crypto_aead_lock(out.data() + PeerHeaderSize, out.data() + PeerHeaderSize + plain.size(), k.key.data(), nonce,
			out.data(), PeerHeaderSize, plain.data(), plain.size());
	return out;
}

bool peekPeerFrom(const uint8_t *data, size_t size, uint32_t& fromId)
{
	if (size < PeerOverhead || data[0] != (uint8_t)UdpType::Peer)
		return false;
	fromId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
	return true;
}

bool openPeer(const PairKey& k, uint8_t domain, const uint8_t *data, size_t size, uint64_t& counter, std::vector<uint8_t>& plain)
{
	if (!k.valid || size < PeerOverhead || data[0] != (uint8_t)UdpType::Peer)
		return false;
	counter = 0;
	for (int i = 0; i < 8; i++)
		counter |= (uint64_t)data[5 + i] << (8 * i);
	plain.resize(size - PeerOverhead);
	uint8_t nonce[24];
	peerNonce(nonce, domain, (uint8_t)(k.dir ^ 1), counter);	// sent by the other side
	return crypto_aead_unlock(plain.data(), data + size - 16, k.key.data(), nonce, data, PeerHeaderSize,
			data + PeerHeaderSize, plain.size()) == 0;
}

bool ReplayWindow::accept(uint64_t counter)
{
	constexpr uint64_t Size = 64 * 16;
	if (counter == 0)
		return false;
	if (counter > top)
	{
		const uint64_t shift = counter - top;
		if (shift >= Size)
			bits.fill(0);
		else
			for (uint64_t c = top + 1; c < counter; c++)
				bits[(c / 64) % 16] &= ~(1ull << (c % 64));
		top = counter;
		bits[(counter / 64) % 16] |= 1ull << (counter % 64);
		return true;
	}
	if (top - counter >= Size)
		return false;
	uint64_t& word = bits[(counter / 64) % 16];
	const uint64_t bit = 1ull << (counter % 64);
	if (word & bit)
		return false;
	word |= bit;
	return true;
}

// ---- virtual addresses -------------------------------------------------------------------------

std::string virtualAddress(uint32_t clientId)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "10.%u.%u.%u", (clientId >> 16) & 255, (clientId >> 8) & 255, clientId & 255);
	return buf;
}

uint32_t clientIdOf(const std::string& address)
{
	unsigned a, b, c, d;
	char extra;
	if (sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a != 10 || b > 255 || c > 255 || d > 255)
		return 0;
	return b << 16 | c << 8 | d;
}

}
