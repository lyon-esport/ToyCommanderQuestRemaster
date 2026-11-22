/*
	tcvr: the arena pictures of the VR menu's Host a game page (Toy Commander VR, Manu's choice 2026-11-22).

	The game has no 2D picture of its 8 multiplayer arenas (the 8 rooms of the house). Its one-player room
	select page shows each room on its 3D house with the room's name. The first time the Host a game page
	opens, a short "picture tour" makes the 8 pictures from the player's own disc:

	- the player's own game is kept (as a match keeps it, tcvr_match.h) and the game is loaded again with
	  both memory card slots empty (the game then never writes a save) and no savestate;
	- START presses go through the start screens to the room select page; there, every frame, the rooms
	  unlocked mask, the page's cursor and the house's turn are written (each room at its own fixed angle,
	  its name shown); 0.6 s per room, then the frame is grabbed (PC: the last frame; headset: the kept frame
	  drawn flat into an offscreen picture);
	- the player's own game comes back (memory cards plugged in again, its state loaded).

	About 17 s of game time. The pictures (480 x 360) are kept in the app's data folder
	(arena/<game id>_room<n>.png), not tied to the app version, and never made again. The tour gives up and
	puts everything back after 45 s, or when the player leaves the page or closes the menu. Never during a
	lobby or a match. Toy Commander (PAL, MK-5102050) only: the memory addresses are that disc's.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "types.h"
#include <memory>
#include <string>
#include <vector>

namespace tcvr::arena
{

constexpr int Rooms = 8;
constexpr int PicWidth = 480, PicHeight = 360;

struct Picture
{
	int width = 0, height = 0;
	std::vector<u8> rgba;	// top row first
	u32 id = 0;				// changes with each new picture
};

// The Host a game page opened: the tour starts if a room's picture is missing (and it can run now).
// True: the tour runs (or already ran this way).
bool startIfMissing();
// Test aid (the UDP "TOUR" command): a tour now, even with the pictures there.
bool forceStart();
// A tour is running (the menu shows progressText() instead of the page).
bool busy();
std::string progressText();
// The player left the page or closed the menu: the tour stops and everything is put back.
void cancel();
// True once after a tour ended and the player's own game runs again (the menu pauses it again).
bool takeFinished();

// The render thread, once per loop (mainui.cpp): the tour's reloads, the frame grabs, the pictures' loading.
void poll();
// At each game load (emulator.cpp), after the multiplayer gate: a tour's load gets empty memory card slots.
void gate();
// At the game load, where a savestate would load: true when the tour took care of it (the tour's game
// starts from its power-on; after the tour, the player's own game is loaded).
bool loadHandled();

// A room's picture (0..7) for the menu, or nullptr.
std::shared_ptr<const Picture> picture(int room);

}
