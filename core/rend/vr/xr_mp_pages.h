/*
	tcvr: the Multiplayer pages of the VR menu, drawn as cards and buttons (Manu chose layout A,
	2026-11-21): the start page (Host a game / Join a game tiles, player name), the host page, the
	games found on the Wi-Fi, the lobby (player slots, Start or Ready, Leave) and a message page
	(connecting, closed, starting).

	Only the drawing and the stick's moves between buttons: what the pages say and do is xr_menu.cpp's.
	No OpenGL nor game state here, so a PC tool can draw the pages from made-up data.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "xr_panel.h"
#include <string>
#include <vector>

namespace vr::xr::mp
{

struct Rect
{
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool valid() const { return x1 > x0 && y1 > y0; }
};

// what can be picked on a page
enum class Item { Host, Join, Name, Mode, Room, Points, StartHosting, Game, Address, Refresh, Ready, StartMatch, Leave, Back, None };

struct GameCard
{
	std::string host, mode, room, limit;
	int players = 0, maxPlayers = 4;
	bool started = false;
};

struct Slot
{
	bool present = false;
	std::string name;
	bool you = false, host = false, ready = false;
};

struct Ranking
{
	std::string name;
	int points = 0, kills = 0;
};

enum class Page { Top, Create, Join, Lobby, Message };

struct Screen
{
	Page page = Page::Top;
	std::vector<Item> items;		// what can be picked, in the menu's order (Game: the games in order)
	int selected = 0;				// in items
	// the start page
	std::string name;				// the player's name, or the text being edited (with its cursor)
	bool nameEditing = false;
	// the host page
	std::string mode, room, limitLabel, limit;
	int roomIndex = 0, roomCount = 1;
	// the arena's picture (the game's own room select page, tcvr_arena_pics.h): RGBA, top row first;
	// none: the drawn card. pictureId names it, pictureKeep holds it while the page is painted.
	const u8 *picture = nullptr;
	int pictureW = 0, pictureH = 0;
	u32 pictureId = 0;
	std::shared_ptr<const void> pictureKeep;
	// the games found
	std::vector<GameCard> games;
	std::string address;			// "Join by address", or the text being edited
	bool addressEditing = false;
	// the lobby
	bool isHost = false;
	std::string summary;			// mode, room, limit
	std::vector<Slot> slots;
	std::string hostAddress;		// (host) to join by hand
	bool canStart = false, ready = false;
	std::string whyNot;				// why Start match can't be used yet
	std::vector<Ranking> results;	// the last match, best first
	std::string message;			// a lobby notice, or the message page's text
	// all pages
	std::string crumb;				// "Multiplayer › Host a game"
	std::string status;				// the last action's result (yellow), else rates (grey)
	std::string rates;
	bool editing = false;			// the footer shows the text-edit keys
};

// The page into panel (1536 x 1000); the place of each item, in the order of screen.items.
std::vector<Rect> build(const Screen& screen, Panel& panel);

// The stick's move from item `from` in dir (1 up, 2 down, 3 left, 4 right): the nearest item that way,
// or -1.
int move(const std::vector<Rect>& rects, int from, int dir);

}
