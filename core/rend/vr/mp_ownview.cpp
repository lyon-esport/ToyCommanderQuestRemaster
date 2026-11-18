/*
	tcvr: own view in split screen (Toy Commander VR multiplayer, task 2.1). See mp_ownview.h.

	Toy Commander (EU, MK-5102050): active players at 0x8C0A72F4 (1..4), multiplayer flag
	0x8C0A5AC4, four player records of 0x26C bytes from 0x8C0D5904, each with its view record
	at +0x0C (ints x, y, w, h at +4..+16; floats 2 tan(half angle) x, y at +28 / +32: focal =
	w / +28, h / +32, as the game's own projection 0x8C060F20). See tcvr/docs/RE/Addresses.md.

	Which player a picture belongs to: the part of the screen it is drawn in. The PVR's tile
	clip (the region array) or the framebuffer clip of the picture gives it when the game
	limits the picture to the player's part of the screen; otherwise its vertices vote. A
	picture that matches no player's view clearly (a menu, a full-screen page) is shown as it
	is.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "mp_ownview.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/sh4/sh4_mem.h"
#include "network/ggpo.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vr
{
namespace
{

constexpr const char *GameId = "MK-5102050";	// Toy Commander (Europe)
constexpr u32 PlayerCount = 0x8C0A72F4;			// 1..4
constexpr u32 MultiplayerFlag = 0x8C0A5AC4;
constexpr u32 ViewRecord0 = 0x8C0D5910;			// player record 0x8C0D5904 + 0x0C
constexpr u32 PlayerStride = 0x26C;

struct View
{
	glm::ivec4 rect;	// x, y, w, h (DC pixels)
	glm::vec2 focal;
	bool ok;
};

float readFloat(u32 addr)
{
	const u32 v = ReadMem32_nommu(addr);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

View readView(int player)
{
	const u32 base = ViewRecord0 + (u32)player * PlayerStride;
	View v;
	v.rect = glm::ivec4((s32)ReadMem32_nommu(base + 4), (s32)ReadMem32_nommu(base + 8),
			(s32)ReadMem32_nommu(base + 12), (s32)ReadMem32_nommu(base + 16));
	const float tx = readFloat(base + 28);
	const float ty = readFloat(base + 32);
	v.focal = glm::vec2(v.rect.z / tx, v.rect.w / ty);
	v.ok = v.rect.x >= 0 && v.rect.y >= 0 && v.rect.z >= 32 && v.rect.w >= 32
			&& v.rect.x + v.rect.z <= 1280 && v.rect.y + v.rect.w <= 960
			&& std::isfinite(v.focal.x) && std::isfinite(v.focal.y)
			&& v.focal.x > 10.f && v.focal.x < 20000.f && v.focal.y > 10.f && v.focal.y < 20000.f;
	return v;
}

int overlap(const glm::ivec4& a, const glm::ivec4& b)
{
	const int w = std::min(a.x + a.z, b.x + b.z) - std::max(a.x, b.x);
	const int h = std::min(a.y + a.w, b.y + b.w) - std::max(a.y, b.y);
	return w > 0 && h > 0 ? w * h : 0;
}

glm::ivec4 rectOf(const Rect& r) {
	return glm::ivec4(r.origin.x, r.origin.y, r.size.x, r.size.y);
}

// The player whose view a clip rectangle is (most of it inside the view, most of the view
// inside it), or -1.
int byClip(const glm::ivec4& clip, const View *views, int count)
{
	if (clip.z <= 0 || clip.w <= 0)
		return -1;
	for (int i = 0; i < count; i++)
	{
		if (!views[i].ok)
			continue;
		const int o = overlap(clip, views[i].rect);
		const int viewArea = views[i].rect.z * views[i].rect.w;
		const int clipArea = clip.z * clip.w;
		if (o * 10 >= viewArea * 8 && o * 10 >= clipArea * 8)
			return i;
	}
	return -1;
}

// The player whose view holds clearly most of the picture's vertices (3/4), or -1.
int byVertices(const rend_context& ctx, const View *views, int count)
{
	int votes[4] {};
	int total = 0;
	// (every 4th vertex is plenty)
	for (size_t k = 0; k < ctx.verts.size(); k += 4)
	{
		const Vertex& v = ctx.verts[k];
		if (!std::isfinite(v.x) || !std::isfinite(v.y))
			continue;
		total++;
		for (int i = 0; i < count; i++)
		{
			const glm::ivec4& r = views[i].rect;
			if (views[i].ok && v.x >= r.x && v.x < r.x + r.z && v.y >= r.y && v.y < r.y + r.w)
			{
				votes[i]++;
				break;
			}
		}
	}
	if (total < 16)
		return -1;
	const int best = (int)(std::max_element(votes, votes + count) - votes);
	return votes[best] * 4 >= total * 3 ? best : -1;
}

// Pictures per second, for the log (task 2.1 measures the per-player rate).
struct Stats
{
	u32 kept, dropped, unmatched;
	std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

	void tick(int player, int count)
	{
		const auto now = std::chrono::steady_clock::now();
		const float secs = std::chrono::duration<float>(now - since).count();
		if (secs < 10.f)
			return;
		INFO_LOG(RENDERER, "VR own view: player %d of %d, %.1f pictures/s shown, %.1f/s of other players dropped, %.1f/s unmatched (shown as they are)",
				player + 1, count, kept / secs, dropped / secs, unmatched / secs);
		kept = dropped = unmatched = 0;
		since = now;
	}
} stats;

// Logs a new way of matching pictures to players (once per kind), for checking on the PC.
void noteMatch(const rend_context& ctx, int count, const View *views, int clipPlayer, int fbPlayer, int vertPlayer)
{
	static u32 seen[32];
	static int seenCount;
	const u32 key = (u32)count | (u32)(clipPlayer + 1) << 4 | (u32)(fbPlayer + 1) << 8 | (u32)(vertPlayer + 1) << 12
			| (u32)(ctx.tileClip.size.x >= 600) << 16 | (u32)(ctx.tileClip.size.y >= 440) << 17;
	if (std::find(seen, seen + seenCount, key) != seen + seenCount || seenCount >= (int)std::size(seen))
		return;
	seen[seenCount++] = key;
	char text[256];
	int len = 0;
	for (int i = 0; i < count && len < (int)sizeof(text) - 64; i++)
		len += snprintf(text + len, sizeof(text) - len, " P%d %d,%d %dx%d f %.0f,%.0f%s", i + 1,
				views[i].rect.x, views[i].rect.y, views[i].rect.z, views[i].rect.w, views[i].focal.x, views[i].focal.y,
				views[i].ok ? "" : " (bad)");
	NOTICE_LOG(RENDERER, "VR own view: picture tile clip %d,%d %dx%d, fb clip %d,%d %dx%d, sof1 %06x, %u verts -> clip P%d, fb P%d, verts P%d; views%s",
			ctx.tileClip.origin.x, ctx.tileClip.origin.y, ctx.tileClip.size.x, ctx.tileClip.size.y,
			ctx.fbClip.origin.x, ctx.fbClip.origin.y, ctx.fbClip.size.x, ctx.fbClip.size.y,
			ctx.fb_W_SOF1 & 0xffffff, (u32)ctx.verts.size(), clipPlayer + 1, fbPlayer + 1, vertPlayer + 1, text);
}

}	// namespace

int ownViewPlayer()
{
	if (settings.content.gameId != GameId)
		return -1;
	const int opt = config::VrMpLocalPlayer;
	if (opt < 0)
		return -1;
	if (ggpo::active())
		return ggpo::localPlayerIndex();
	return opt >= 1 && opt <= 4 ? opt - 1 : -1;
}

bool ownViewKeep(rend_context& ctx)
{
	ctx.tcvrView = glm::vec4(0.f);
	ctx.tcvrViewFocal = glm::vec2(0.f);
	if (ctx.isRTT)
		return true;
	const int local = ownViewPlayer();
	if (local < 0)
		return true;
	const int count = (int)ReadMem32_nommu(PlayerCount);
	// (a local player who isn't in this battle sees every picture, as stock)
	if (count < 2 || count > 4 || local >= count || ReadMem32_nommu(MultiplayerFlag) == 0)
		return true;

	View views[4];
	for (int i = 0; i < count; i++)
		views[i] = readView(i);
	const int clipPlayer = byClip(rectOf(ctx.tileClip), views, count);
	const int fbPlayer = clipPlayer < 0 ? byClip(rectOf(ctx.fbClip), views, count) : -1;
	const int vertPlayer = clipPlayer < 0 && fbPlayer < 0 ? byVertices(ctx, views, count) : -1;
	noteMatch(ctx, count, views, clipPlayer, fbPlayer, vertPlayer);
	const int player = clipPlayer >= 0 ? clipPlayer : fbPlayer >= 0 ? fbPlayer : vertPlayer;

	bool keep = true;
	if (player < 0)
		stats.unmatched++;
	else if (player != local)
	{
		stats.dropped++;
		keep = false;
	}
	else
	{
		stats.kept++;
		const View& v = views[player];
		ctx.tcvrView = glm::vec4(v.rect);
		ctx.tcvrViewFocal = v.focal;
	}
	stats.tick(local, count);
	return keep;
}

}
