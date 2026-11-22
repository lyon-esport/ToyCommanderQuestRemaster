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
#include "rend/transform_matrix.h"

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
		NOTICE_LOG(RENDERER, "VR own view: player %d of %d, %.1f pictures/s shown, %.1f/s of other players dropped, %.1f/s unmatched (shown as they are)",
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

// the game's safe-area inset of a HUD row from the screen's edge, in lines of a 480-line screen (measured)
constexpr float SafeInset = 34.f;

// The HUD of a view that is not the screen's shape (two players: a band the full width, half the
// height). The 2D is scaled up by the smaller ratio only (x1 for a band), so its parts would stay in
// a strip across the middle of the plane: each 2D polygon is moved instead so that its centre takes
// the same place on the whole screen as in the band (the top of the band to the top of the screen),
// at its own size. Only this picture's vertices are changed (the renderer's copy): nothing the game
// reads.
//
// Done at parse time, while every polygon still has its own run of vertices (pp.first, pp.count index
// ctx.verts there; once the index is made they index ctx.idx and merged strips share runs: moving by
// them, as the first version did after the parse, moved other polygons' vertices, half a bar here, an
// icon there, a different mix in each picture: the HUD that shook in two-player matches).
//
// Only what is near the band's edges goes with them (gauges, icons, counters). What the game puts
// over the 3D view near its middle (the lock-on mark on a target, the sight) stays where it is drawn:
// the 3D is rebuilt about the same centre at the same scale, so it stays on what it marks.
void placeHudByEdge(rend_context& ctx, const glm::vec4& view, const RenderPass& pass, const RenderPass& previousPass)
{
	int width, height;
	getPvrFramebufferSize(ctx, width, height);
	const glm::vec2 ratio(width / view.z, height / view.w);
	const float scale = std::min(ratio.x, ratio.y);	// as vr_reproject's update()
	if (std::abs(ratio.x - ratio.y) < 0.1f * scale)
		return;		// a quarter (4 players, 3): scaled up evenly already
	const glm::vec2 centre(view.x + view.z * 0.5f, view.y + view.w * 0.5f);
	const glm::vec2 half(view.z * 0.5f, view.w * 0.5f);
	// shown at (p - centre) * scale: a centre c goes where (c - centre) * ratio is wanted
	const glm::vec2 move = ratio / scale - 1.f;		// (0, 1) for a band
	const float hudW = config::VrHudW;
	auto onPlane = [hudW](const Vertex& v) { return v.z > 0.f && std::abs(1.f / v.z - hudW) < 0.002f; };
	auto list = [&](std::vector<PolyParam>& polys, u32 from, u32 to) {
		to = std::min<u32>(to, (u32)polys.size());
		for (u32 n = from; n < to; n++)
		{
			const PolyParam& pp = polys[n];
			if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
				continue;
			glm::vec2 lo(1e9f), hi(-1e9f);
			bool flat = true;
			for (u32 i = pp.first; i < pp.first + pp.count && flat; i++)
			{
				const Vertex& v = ctx.verts[i];
				flat = onPlane(v) && std::isfinite(v.x) && std::isfinite(v.y);
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
			}
			if (!flat)
				continue;
			const glm::vec2 c = (lo + hi) * 0.5f;
			const glm::vec2 offset = c - centre;
			glm::vec2 delta(0.f);
			for (int a = 0; a < 2; a++)
			{
				if (move[a] < 0.1f)
				{
					// not the band's long axis: 0 in the middle 40%, 1 from 70% out, smooth between
					const float out = std::clamp((std::abs(offset[a]) / half[a] - 0.4f) / 0.3f, 0.f, 1.f);
					delta[a] = offset[a] * move[a] * out * out * (3.f - 2.f * out);
					continue;
				}
				// The band's long axis. The game insets each band's HUD from the screen's edges (the TV's safe
				// area), not from the band: on the band edge that is a screen edge its rows sit SafeInset in,
				// on the inner edge (between the two bands) flush with it. Each row goes to the nearer edge
				// of the whole screen, at the same distance from it as a one-player HUD: the screen-edge rows
				// where they are drawn, the inner-edge rows SafeInset further in. The same place for player 1
				// and player 2 (before, by the distance from the band's centre: player 2's HUD about 100 px
				// off player 1's).
				const float full = a == 0 ? (float)width : (float)height;
				const float safe = SafeInset * full / 480.f;
				const float b0 = view[a], b1 = view[a] + view[a + 2];
				const bool nearStart = c[a] - b0 < b1 - c[a];
				const bool screenEdge = nearStart ? b0 <= 2.f : b1 >= full - 2.f;
				const float d = nearStart ? c[a] - b0 : b1 - c[a];		// from its band edge
				const float target = nearStart ? (screenEdge ? d : safe + d) : full - (screenEdge ? d : safe + d);
				// only rows near the edges (gauges, icons, counters): the sight and the lock-on marks over
				// the 3D stay; 1 up to 40% of the half band from the edge (the safe inset left out), 0 from 60%
				const float inner = (d - (screenEdge ? safe : 0.f)) / half[a];
				const float out = std::clamp((0.6f - inner) / 0.2f, 0.f, 1.f);
				// shown at (p - centre) * scale + full / 2
				const float wanted = centre[a] + (target - full * 0.5f) / scale;
				delta[a] = (wanted - c[a]) * out * out * (3.f - 2.f * out);
			}
			if (delta.x == 0.f && delta.y == 0.f)
				continue;
			for (u32 i = pp.first; i < pp.first + pp.count; i++)
			{
				ctx.verts[i].x += delta.x;
				ctx.verts[i].y += delta.y;
			}
		}
	};
	list(ctx.global_param_op, previousPass.op_count, pass.op_count);
	list(ctx.global_param_pt, previousPass.pt_count, pass.pt_count);
	list(ctx.global_param_tr, previousPass.tr_count, pass.tr_count);
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
	// A picture the game clips to the whole screen is a page for everybody (the results page: both
	// players' boxes in its top half once won the vertex vote for player 1, and the other headset
	// kept its last battle picture): shown as it is. The battle's pictures are clipped to their part.
	const bool wholeScreen = ctx.fbClip.origin.x <= 0 && ctx.fbClip.origin.y <= 0
			&& ctx.fbClip.size.x >= 600 && ctx.fbClip.size.y >= 440;
	const int vertPlayer = clipPlayer < 0 && fbPlayer < 0 && !wholeScreen ? byVertices(ctx, views, count) : -1;
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
		// (its HUD was placed at parse time: ownViewPlaceHud)
	}
	stats.tick(local, count);
	return keep;
}

void ownViewPlaceHud(rend_context& ctx, const RenderPass& pass, const RenderPass& previousPass)
{
	if (ctx.isRTT || !config::VrReproject)
		return;
	const int local = ownViewPlayer();
	if (local < 0)
		return;
	const int count = (int)ReadMem32_nommu(PlayerCount);
	if (count != 2 || local >= count || ReadMem32_nommu(MultiplayerFlag) == 0)
		return;		// (only the two-player bands are not the screen's shape)
	View views[4];
	for (int i = 0; i < count; i++)
		views[i] = readView(i);
	// the picture's own part of the screen (as ownViewKeep; the framebuffer clip is set before the parse)
	const int player = byClip(rectOf(ctx.fbClip), views, count);
	if (player != local)
		return;		// another player's picture (not shown), or a page for everybody
	placeHudByEdge(ctx, glm::vec4(views[player].rect), pass, previousPass);
}

}
