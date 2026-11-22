/*
	tcvr: the Multiplayer pages of the VR menu, drawn as cards and buttons. See xr_mp_pages.h.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_mp_pages.h"
#include <algorithm>
#include <cmath>

namespace vr::xr::mp
{
namespace
{

constexpr float W = 1536.f, H = 1000.f;
constexpr float Margin = 64.f;
constexpr float FootTop = 922.f;

glm::vec4 rgb(unsigned hex, float a = 1.f)
{
	return glm::vec4(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}
glm::vec3 rgb3(unsigned hex) { return glm::vec3(rgb(hex)); }

// the colours of the mock-ups Manu picked
const glm::vec4 Card = rgb(0x1a2029), CardEdge = rgb(0x2a313c), Button = rgb(0x1d232c);
const glm::vec4 PickedEdge = rgb(0xffd24a), PickedFill = rgb(0x2a2a1a);
const glm::vec4 Go = rgb(0x1f6b3a), GoEdge = rgb(0x2f8f4e), GoPicked = rgb(0x2a8a4c);
const glm::vec3 Brand = rgb3(0xf43d2e), White = rgb3(0xffffff), Text = rgb3(0xe9e9f0), Soft = rgb3(0xa8a8b3),
		Label = rgb3(0x8b8f99), Yellow = rgb3(0xffd24a), Faint = rgb3(0x6b707b), LeaveText = rgb3(0xffb4ab);
const unsigned AvatarColours[] { 0xb9362b, 0x3a6fb0, 0x7a4fb0, 0x2f8f4e };

struct Painter
{
	Panel& panel;

	void box(float x0, float y0, float x1, float y1, float r, glm::vec4 fill, glm::vec4 edge = glm::vec4(0.f), float edgePx = 0.f, bool dashed = false)
	{
		PanelShape s;
		s.x0 = x0; s.y0 = y0; s.x1 = x1; s.y1 = y1;
		s.radius = r;
		s.fill = fill;
		s.border = edge;
		s.borderPx = edgePx;
		s.dashed = dashed;
		panel.shapes.push_back(s);
	}
	void triangle(float ax, float ay, float bx, float by, float cx, float cy, glm::vec3 colour)
	{
		PanelShape s;
		s.kind = PanelShape::Triangle;
		s.x0 = ax; s.y0 = ay; s.x1 = bx; s.y1 = by; s.x2 = cx; s.y2 = cy;
		s.fill = glm::vec4(colour, 1.f);
		panel.shapes.push_back(s);
	}
	// left (dir -1) or right (+1) pointing, centred on (x, y)
	void arrow(float x, float y, float size, int dir, glm::vec3 colour)
	{
		const float h = size * 0.5f, w = size * 0.45f;
		if (dir < 0)
			triangle(x + w, y - h, x + w, y + h, x - w, y, colour);
		else
			triangle(x - w, y - h, x - w, y + h, x + w, y, colour);
	}
	void text(const std::string& s, float x, float y, float px, glm::vec3 colour, int align = 0, float maxWidth = 0.f)
	{
		PanelText t;
		t.text = s;
		t.x = x; t.y = y; t.px = px;
		t.colour = colour;
		t.align = align;
		t.maxWidth = maxWidth;
		panel.texts.push_back(t);
	}
	// a text cut with "..." to fit a width
	std::string fit(const std::string& s, float px, float width)
	{
		if (panelTextWidth(s, px) <= width)
			return s;
		std::string cut = s;
		while (!cut.empty() && panelTextWidth(cut + "...", px) > width)
		{
			cut.pop_back();
			while (!cut.empty() && ((unsigned char)cut.back() & 0xC0) == 0x80)	// (a whole UTF-8 letter)
				cut.pop_back();
			if (!cut.empty() && (unsigned char)cut.back() >= 0xC0)
				cut.pop_back();
		}
		return cut + "...";
	}
	// a rounded label; x is its left edge (align 0) or right edge (align 2); returns its width
	float chip(const std::string& s, float x, float yCentre, glm::vec4 fill, glm::vec3 colour, float px = 26.f, int align = 0)
	{
		const float w = panelTextWidth(s, px) + px * 1.3f, h = px * 1.55f;
		const float x0 = align == 2 ? x - w : x;
		box(x0, yCentre - h * 0.5f, x0 + w, yCentre + h * 0.5f, h * 0.5f, fill);
		text(s, x0 + w * 0.5f, yCentre + px * 0.36f, px, colour, 1);
		return w;
	}
	void avatar(const std::string& name, float cx, float cy, float r, unsigned colour)
	{
		box(cx - r, cy - r, cx + r, cy + r, r, rgb(colour));
		text(name.empty() ? std::string("?") : name.substr(0, 1), cx, cy + r * 0.4f, r * 1.05f, White, 1);
	}
	// a button's frame: picked (yellow edge), "go" (green) or plain
	void frame(const Rect& r, bool picked, bool go = false, bool dimmed = false, float radius = 22.f)
	{
		glm::vec4 fill = go ? (picked ? GoPicked : Go) : picked ? PickedFill : Button;
		glm::vec4 edge = picked ? PickedEdge : go ? GoEdge : CardEdge;
		if (dimmed)
		{
			fill.a *= 0.5f;
			edge = picked ? PickedEdge : glm::vec4(glm::vec3(edge), 0.5f);
		}
		if (picked)
			box(r.x0 - 8, r.y0 - 8, r.x1 + 8, r.y1 + 8, radius + 8, rgb(0xffd24a, 0.16f));
		box(r.x0, r.y0, r.x1, r.y1, radius, fill, edge, 4.f);
	}
	void centred(const Rect& r, const std::string& s, float px, glm::vec3 colour, float dy = 0.f)
	{
		text(fit(s, px, r.x1 - r.x0 - 40.f), (r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f + px * 0.36f + dy, px, colour, 1);
	}
};

std::string upper(std::string s)
{
	for (char& c : s)
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
	return s;
}

void header(Painter& p, const Screen& s)
{
	p.text("TOY COMMANDER VR", Margin, 96.f, 44.f, Brand);
	const float x = Margin + panelTextWidth("TOY COMMANDER VR", 44.f) + 30.f;
	// the crumb's last part in white
	const std::string sep = " \xE2\x80\xBA ";	// " › "
	const size_t at = s.crumb.rfind(sep);
	if (at == std::string::npos)
		p.text(s.crumb, x, 96.f, 34.f, White);
	else
	{
		const std::string head = s.crumb.substr(0, at + sep.size());
		p.text(head, x, 96.f, 34.f, Soft);
		p.text(s.crumb.substr(at + sep.size()), x + panelTextWidth(head, 34.f), 96.f, 34.f, White);
	}
}

// the round key and what it does, from x; returns the next x
float key(Painter& p, float x, const std::string& k, glm::vec4 colour, const std::string& what)
{
	const float y = FootTop + 39.f;
	float w = 46.f;
	if (k == "<>")
	{
		w = 60.f;
		p.box(x, y - 23.f, x + w, y + 23.f, 23.f, colour);
		p.arrow(x + 18.f, y, 18.f, -1, White);
		p.arrow(x + 42.f, y, 18.f, 1, White);
	}
	else if (k == "^v")
	{
		w = 46.f;
		p.box(x, y - 23.f, x + w, y + 23.f, 23.f, colour);
		p.triangle(x + 23.f, y - 15.f, x + 14.f, y - 3.f, x + 32.f, y - 3.f, White);
		p.triangle(x + 23.f, y + 15.f, x + 14.f, y + 3.f, x + 32.f, y + 3.f, White);
	}
	else
	{
		w = std::max(46.f, panelTextWidth(k, 26.f) + 20.f);
		p.box(x, y - 23.f, x + w, y + 23.f, 23.f, colour);
		p.text(k, x + w * 0.5f, y + 9.f, 26.f, White, 1);
	}
	p.text(what, x + w + 12.f, y + 10.f, 28.f, rgb3(0xb9bcc6));
	return x + w + 12.f + panelTextWidth(what, 28.f) + 40.f;
}

void footer(Painter& p, const Screen& s)
{
	p.box(11.f, FootTop, W - 11.f, H - 11.f, 0.f, rgb(0x151a21));	// (inside the panel's red frame)
	float x = Margin;
	const glm::vec4 a = rgb(0x2f8f4e), b = rgb(0xb9362b), stick = rgb(0x2b313b);
	if (s.editing)
	{
		x = key(p, x, "^v", stick, "Letter");
		x = key(p, x, "<>", stick, "Move");
		x = key(p, x, "A", a, "Keep");
		x = key(p, x, "B", b, "Cancel");
	}
	else
	{
		x = key(p, x, "A", a, "Select");
		x = key(p, x, "B", b, "Back");
		x = key(p, x, "<>", stick, "Change");
	}
	const bool status = !s.status.empty();
	const std::string right = status ? s.status : s.rates;
	p.text(p.fit(right, 28.f, W - Margin - x), W - Margin, FootTop + 49.f, 28.f, status ? Yellow : Label, 2);
}

int itemIndex(const Screen& s, Item item, int nth = 0)
{
	for (int i = 0; i < (int)s.items.size(); i++)
		if (s.items[i] == item && nth-- == 0)
			return i;
	return -1;
}

bool picked(const Screen& s, int index) { return index >= 0 && index == s.selected; }

// a value with arrows (Mode, Arena, Points): label above the value, arrows on the right
void valueButton(Painter& p, const Rect& r, bool sel, const std::string& label, const std::string& value)
{
	p.frame(r, sel);
	const float my = (r.y0 + r.y1) * 0.5f;
	p.text(upper(label), r.x0 + 36.f, my - 12.f, 24.f, Label);
	p.text(p.fit(value, 40.f, r.x1 - r.x0 - 200.f), r.x0 + 36.f, my + 34.f, 40.f, Text);
	const glm::vec3 c = sel ? Yellow : Faint;
	p.arrow(r.x1 - 110.f, my, 34.f, -1, c);
	p.arrow(r.x1 - 50.f, my, 34.f, 1, c);
}

void topPage(Painter& p, const Screen& s, std::vector<Rect>& rects)
{
	const int host = itemIndex(s, Item::Host), join = itemIndex(s, Item::Join), name = itemIndex(s, Item::Name);
	const float top = 150.f, bottom = 630.f, mid = W * 0.5f;
	struct TileText { int index; float x0, x1; const char *title, *text, *hint; bool house; };
	const TileText tiles[] {
		{ host, Margin, mid - 12.f, "Host a game", "Pick the mode, the arena and the score. Friends on the same Wi-Fi join you.", "Up to 4 players", true },
		{ join, mid + 12.f, W - Margin, "Join a game", "See the games hosted on this Wi-Fi and join one in a press.", "No internet needed", false },
	};
	for (const TileText& t : tiles)
	{
		if (t.index < 0)
			continue;
		const Rect r { t.x0, top, t.x1, bottom };
		rects[t.index] = r;
		const bool sel = picked(s, t.index);
		p.frame(r, sel, false, false, 26.f);
		// the icon: a house, or Wi-Fi bars
		const float cx = r.x0 + 40.f + 55.f, cy = top + 40.f + 55.f;
		p.box(cx - 55.f, cy - 55.f, cx + 55.f, cy + 55.f, 55.f, rgb(0x26303c));
		const glm::vec3 ic = sel ? Yellow : Text;
		if (t.house)
		{
			p.triangle(cx, cy - 34.f, cx - 36.f, cy - 2.f, cx + 36.f, cy - 2.f, ic);
			p.box(cx - 24.f, cy - 4.f, cx + 24.f, cy + 30.f, 3.f, glm::vec4(ic, 1.f));
			p.box(cx - 8.f, cy + 8.f, cx + 8.f, cy + 30.f, 2.f, rgb(0x26303c));
		}
		else
			for (int k = 0; k < 4; k++)
			{
				const float bx = cx - 34.f + k * 18.f, bh = 14.f + k * 14.f;
				p.box(bx, cy + 28.f - bh, bx + 12.f, cy + 28.f, 3.f, glm::vec4(ic, 1.f));
			}
		p.text(t.title, r.x0 + 40.f, top + 230.f, 50.f, White);
		p.text(t.text, r.x0 + 40.f, top + 290.f, 30.f, Soft, 0, r.x1 - r.x0 - 80.f);
		p.text(t.hint, r.x0 + 40.f, bottom - 40.f, 28.f, rgb3(0xbff2cf));
		if (sel)
		{
			p.text("A", r.x1 - 90.f, bottom - 36.f, 40.f, Yellow, 1);
			p.arrow(r.x1 - 52.f, bottom - 50.f, 26.f, 1, Yellow);
		}
	}
	if (name >= 0)
	{
		const Rect r { Margin, 666.f, 700.f, 786.f };
		rects[name] = r;
		const bool sel = picked(s, name);
		p.frame(r, sel);
		const float my = (r.y0 + r.y1) * 0.5f;
		p.text("YOUR NAME", r.x0 + 36.f, my + 10.f, 26.f, Label);
		p.text(s.name, r.x0 + 290.f, my + 16.f, 44.f, s.nameEditing ? Yellow : Text);
		if (!s.nameEditing)
			p.text("Edit", r.x1 - 36.f, my + 10.f, 26.f, sel ? Yellow : Faint, 2);
		p.text("The other players see this name.", 740.f, my - 8.f, 28.f, Soft);
		p.text("Up to 3 letters, like the game's own.", 740.f, my + 32.f, 28.f, Faint);
	}
}

void createPage(Painter& p, const Screen& s, std::vector<Rect>& rects)
{
	const float x0 = Margin, x1 = 840.f;
	const Item rows[] { Item::Mode, Item::Room, Item::Points };
	float y = 140.f;
	for (Item it : rows)
	{
		const int i = itemIndex(s, it);
		if (i < 0)
			continue;
		const Rect r { x0, y, x1, y + 120.f };
		rects[i] = r;
		valueButton(p, r, picked(s, i), it == Item::Mode ? "Mode" : it == Item::Room ? "Arena" : s.limitLabel,
				it == Item::Mode ? s.mode : it == Item::Room ? s.room : s.limit);
		y += 140.f;
	}
	const int go = itemIndex(s, Item::StartHosting);
	if (go >= 0)
	{
		const Rect r { x0, 600.f, x1, 750.f };
		rects[go] = r;
		const bool sel = picked(s, go);
		p.frame(r, sel, true);
		const float my = (r.y0 + r.y1) * 0.5f;
		const float tw = panelTextWidth("Host this game", 48.f);
		p.arrow((r.x0 + r.x1) * 0.5f - tw * 0.5f - 30.f, my, 34.f, 1, White);
		p.text("Host this game", (r.x0 + r.x1) * 0.5f + 20.f, my + 17.f, 48.f, White, 1);
	}
	// the arena's card
	const Rect c { 880.f, 140.f, W - Margin, 540.f };
	p.box(c.x0, c.y0, c.x1, c.y1, 22.f, rgb(0x6b4b2a));
	for (int k = 0; k < 8; k++)	// (planks, a floor of the toy house)
		if (k % 2 == 0)
			p.box(c.x0 + k * (c.x1 - c.x0) / 8.f, c.y0, c.x0 + (k + 1) * (c.x1 - c.x0) / 8.f, c.y1, k == 0 ? 22.f : 0.f, rgb(0x8a6a44, 0.35f));
	p.box(c.x0, c.y1 - 110.f, c.x1, c.y1, 22.f, rgb(0x000000, 0.45f));
	p.text(p.fit(s.room, 52.f, c.x1 - c.x0 - 60.f), c.x0 + 30.f, c.y1 - 38.f, 52.f, White);
	p.chip("Arena " + std::to_string(s.roomIndex + 1) + " / " + std::to_string(s.roomCount), c.x1 - 20.f, c.y0 + 40.f,
			rgb(0x5a1f1a), LeaveText, 26.f, 2);
	p.text("Friends on the same Wi-Fi find your game in Join a game. You start the match from the lobby when everyone is ready.",
			c.x0, 600.f, 28.f, Soft, 0, c.x1 - c.x0);
}

void joinPage(Painter& p, const Screen& s, std::vector<Rect>& rects)
{
	p.text("Games hosted on this Wi-Fi, updated every second", Margin, 160.f, 28.f, Soft);
	const float cardH = 150.f, gap = 18.f, top = 190.f;
	// at most 3 cards shown: the ones around the picked game
	const int count = (int)s.games.size();
	int first = 0;
	int pickedGame = -1;
	for (int k = 0; k < count; k++)
		if (picked(s, itemIndex(s, Item::Game, k)))
			pickedGame = k;
	if (count > 3 && pickedGame >= 0)
		first = std::clamp(pickedGame - 1, 0, count - 3);
	float y = top;
	for (int k = first; k < count && k < first + 3; k++)
	{
		const GameCard& g = s.games[k];
		const int i = itemIndex(s, Item::Game, k);
		const Rect r { Margin, y, W - Margin, y + cardH };
		if (i >= 0)
			rects[i] = r;
		const bool sel = picked(s, i);
		p.frame(r, sel);
		const float my = (r.y0 + r.y1) * 0.5f;
		p.avatar(g.host, r.x0 + 80.f, my, 45.f, AvatarColours[k % 4]);
		p.text(p.fit(g.host + "'s game", 42.f, 640.f), r.x0 + 150.f, my - 6.f, 42.f, White);
		p.text(p.fit(g.mode + "  \xC2\xB7  " + g.room + "  \xC2\xB7  " + g.limit, 26.f, 700.f), r.x0 + 150.f, my + 38.f, 26.f, Soft);
		// a dot per place, green when taken
		for (int d = 0; d < g.maxPlayers && d < 4; d++)
		{
			const float dx = r.x1 - 420.f + d * 32.f;
			p.box(dx, my - 11.f, dx + 22.f, my + 11.f, 11.f, d < g.players ? rgb(0x2f8f4e) : rgb(0x333a45));
		}
		const bool full = g.players >= g.maxPlayers;
		if (g.started || full)
			p.chip(g.started ? "Playing" : "Full", r.x1 - 40.f, my, rgb(0x3a3220), rgb3(0xffd98a), 28.f, 2);
		else
		{
			const Rect b { r.x1 - 250.f, my - 45.f, r.x1 - 30.f, my + 45.f };
			p.box(b.x0, b.y0, b.x1, b.y1, 20.f, sel ? GoPicked : Go, GoEdge, 4.f);
			p.centred(b, "Join", 36.f, White);
		}
		y += cardH + gap;
	}
	if (count == 0)
	{
		const Rect r { Margin, top, W - Margin, top + cardH };
		p.box(r.x0, r.y0, r.x1, r.y1, 22.f, glm::vec4(0.f), CardEdge, 4.f, true);
		p.centred(r, "Searching for games on this Wi-Fi...", 32.f, Faint, -18.f);
		p.centred(r, "The host must be on the same Wi-Fi as you.", 26.f, Faint, 26.f);
	}
	else if (count > 3)
		p.text(std::to_string(count) + " games: move down to see more", W * 0.5f, y + 30.f, 26.f, Faint, 1);
	const int addr = itemIndex(s, Item::Address), refresh = itemIndex(s, Item::Refresh);
	if (addr >= 0)
	{
		const Rect r { Margin, 780.f, 1010.f, 880.f };
		rects[addr] = r;
		p.frame(r, picked(s, addr));
		const float my = (r.y0 + r.y1) * 0.5f;
		p.text("JOIN BY ADDRESS", r.x0 + 34.f, my + 9.f, 24.f, Label);
		p.text(p.fit(s.address, 34.f, r.x1 - r.x0 - 340.f), r.x0 + 300.f, my + 12.f, 34.f,
				s.addressEditing ? Yellow : Text);
	}
	if (refresh >= 0)
	{
		const Rect r { 1040.f, 780.f, W - Margin, 880.f };
		rects[refresh] = r;
		p.frame(r, picked(s, refresh));
		p.centred(r, "Search again", 32.f, Text);
	}
}

void lobbyPage(Painter& p, const Screen& s, std::vector<Rect>& rects)
{
	const float row = 150.f;
	const float w = p.chip(s.isHost ? "HOST" : "JOINED", Margin, 150.f, rgb(0x5a1f1a), LeaveText, 26.f);
	p.text(p.fit(s.summary, 36.f, W - 2 * Margin - w - 30.f), Margin + w + 24.f, 163.f, 36.f, White);
	const float gridTop = 200.f, colW = (W - 2 * Margin - 24.f) * 0.5f;
	for (int k = 0; k < (int)s.slots.size() && k < 4; k++)
	{
		const Slot& sl = s.slots[k];
		const float x0 = Margin + (k % 2) * (colW + 24.f), y0 = gridTop + (k / 2) * (row + 20.f);
		const Rect r { x0, y0, x0 + colW, y0 + row };
		const float my = (r.y0 + r.y1) * 0.5f;
		if (!sl.present)
		{
			p.box(r.x0, r.y0, r.x1, r.y1, 22.f, glm::vec4(0.f), CardEdge, 4.f, true);
			p.text("+  Waiting for a player", r.x0 + 40.f, my + 11.f, 32.f, Faint);
			continue;
		}
		p.box(r.x0, r.y0, r.x1, r.y1, 22.f, Card, sl.you ? rgb(0x4a5565) : CardEdge, 4.f);
		p.avatar(sl.name, r.x0 + 75.f, my, 45.f, AvatarColours[k % 4]);
		p.text(sl.name, r.x0 + 145.f, my + 2.f, 42.f, White);
		if (sl.you)
			p.text("(you)", r.x0 + 145.f + panelTextWidth(sl.name, 42.f) + 14.f, my + 2.f, 26.f, Soft);
		p.text("Player " + std::to_string(k + 1), r.x0 + 145.f, my + 40.f, 26.f, Soft);
		if (sl.host)
			p.chip("Host", r.x1 - 30.f, my, rgb(0x5a1f1a), LeaveText, 28.f, 2);
		else if (sl.ready)
			p.chip("Ready", r.x1 - 30.f, my, rgb(0x1f6b3a), rgb3(0xbff2cf), 28.f, 2);
		else
			p.chip("Not ready", r.x1 - 30.f, my, rgb(0x3a3220), rgb3(0xffd98a), 28.f, 2);
	}
	float y = gridTop + 2 * (row + 20.f) + 8.f;
	if (!s.results.empty())
	{
		// the last match's ranking
		p.box(Margin, y, W - Margin, y + 84.f, 20.f, rgb(0x161b22));
		p.text("LAST MATCH", Margin + 30.f, y + 52.f, 24.f, Label);
		float x = Margin + 230.f;
		static const char *const places[] { "1st", "2nd", "3rd", "4th" };
		static const unsigned medal[] { 0xb8902f, 0x5d6470, 0x7a5536, 0x333a45 };
		for (size_t k = 0; k < s.results.size() && k < 4; k++)
		{
			const Ranking& rk = s.results[k];
			x += p.chip(places[k], x, y + 42.f, rgb(medal[k]), White, 24.f) + 12.f;
			std::string t = rk.name + "  " + std::to_string(rk.points) + " pt";
			if (rk.kills > 0)
				t += ", " + std::to_string(rk.kills) + " kills";
			p.text(t, x, y + 51.f, 28.f, Text);
			x += panelTextWidth(t, 28.f) + 36.f;
		}
		y += 100.f;
	}
	else if (s.isHost && !s.hostAddress.empty())
	{
		p.text("To join by hand, the address is " + s.hostAddress, Margin, y + 40.f, 26.f, Faint);
		y += 60.f;
	}
	if (!s.message.empty())
		p.text(p.fit(s.message, 28.f, W - 2 * Margin), Margin, y + 30.f, 28.f, Yellow);
	// the buttons
	const float by0 = 770.f, by1 = 890.f;
	const int start = itemIndex(s, Item::StartMatch), ready = itemIndex(s, Item::Ready), leave = itemIndex(s, Item::Leave);
	const float split = W - Margin - 360.f;
	if (start >= 0)
	{
		const Rect r { Margin, by0, split - 24.f, by1 };
		rects[start] = r;
		const bool sel = picked(s, start);
		p.frame(r, sel, true, !s.canStart);
		const glm::vec3 c = s.canStart ? White : rgb3(0xb0b4bc);
		if (s.canStart)
			p.centred(r, "Start match", 44.f, c);
		else
		{
			p.centred(r, "Start match", 40.f, c, -16.f);
			p.centred(r, s.whyNot, 24.f, c, 26.f);
		}
	}
	if (ready >= 0)
	{
		const Rect r { Margin, by0, split - 24.f, by1 };
		rects[ready] = r;
		const bool sel = picked(s, ready);
		p.frame(r, sel, s.ready);
		p.centred(r, s.ready ? "I'm ready" : "Press A when you're ready", 40.f, s.ready ? White : Text, -14.f);
		p.centred(r, s.ready ? "The host starts the match. A again: not ready" : "The host can start once everyone is ready", 24.f, s.ready ? rgb3(0xd8f5e0) : Soft, 28.f);
	}
	if (leave >= 0)
	{
		const Rect r { split, by0, W - Margin, by1 };
		rects[leave] = r;
		p.frame(r, picked(s, leave));
		p.centred(r, "Leave", 38.f, LeaveText);
	}
}

void messagePage(Painter& p, const Screen& s, std::vector<Rect>& rects)
{
	p.text(s.message, W * 0.5f, 420.f, 44.f, White, 1, 1100.f);
	for (int i = 0; i < (int)s.items.size(); i++)
	{
		const Item it = s.items[i];
		if (it != Item::Leave && it != Item::Back)
			continue;
		const Rect r { W * 0.5f - 220.f, 620.f, W * 0.5f + 220.f, 740.f };
		rects[i] = r;
		p.frame(r, picked(s, i));
		p.centred(r, it == Item::Leave ? "Leave" : "Back", 38.f, it == Item::Leave ? LeaveText : Text);
	}
}

}	// namespace

std::vector<Rect> build(const Screen& s, Panel& panel)
{
	panel.imageWidth = (int)W;
	panel.imageHeight = (int)H;
	Painter p { panel };
	std::vector<Rect> rects(s.items.size());
	header(p, s);
	switch (s.page)
	{
	case Page::Top: topPage(p, s, rects); break;
	case Page::Create: createPage(p, s, rects); break;
	case Page::Join: joinPage(p, s, rects); break;
	case Page::Lobby: lobbyPage(p, s, rects); break;
	case Page::Message: messagePage(p, s, rects); break;
	}
	footer(p, s);
	return rects;
}

int move(const std::vector<Rect>& rects, int from, int dir)
{
	if (from < 0 || from >= (int)rects.size() || !rects[from].valid())
		return -1;
	const Rect& a = rects[from];
	const float ax = (a.x0 + a.x1) * 0.5f, ay = (a.y0 + a.y1) * 0.5f;
	int best = -1;
	float bestScore = 1e9f;
	for (int i = 0; i < (int)rects.size(); i++)
	{
		const Rect& b = rects[i];
		if (i == from || !b.valid())
			continue;
		const float bx = (b.x0 + b.x1) * 0.5f, by = (b.y0 + b.y1) * 0.5f;
		float along, across;
		bool overlap;
		switch (dir)
		{
		case 1: along = a.y0 - b.y1; across = std::abs(bx - ax); overlap = b.x0 < a.x1 && b.x1 > a.x0; break;
		case 2: along = b.y0 - a.y1; across = std::abs(bx - ax); overlap = b.x0 < a.x1 && b.x1 > a.x0; break;
		case 3: along = a.x0 - b.x1; across = std::abs(by - ay); overlap = b.y0 < a.y1 && b.y1 > a.y0; break;
		case 4: along = b.x0 - a.x1; across = std::abs(by - ay); overlap = b.y0 < a.y1 && b.y1 > a.y0; break;
		default: return -1;
		}
		// only a button wholly that way (left on Host a game went to Your name, below it, whose
		// centre is a little to the left)
		if (along < -1.f)
			continue;
		// the nearest that way, the ones facing it first
		const float score = std::max(along, 0.f) + (overlap ? 0.f : 2000.f) + across * 0.5f;
		if (score < bestScore)
		{
			bestScore = score;
			best = i;
		}
	}
	return best;
}

}
