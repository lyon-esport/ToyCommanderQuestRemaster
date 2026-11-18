/*
	tcvr: own view in split screen (Toy Commander VR multiplayer, task 2.1).

	In a split-screen battle the game draws one player's view per picture, taking turns
	(Phase 0 report: each polygon list holds a single player's view, in its own part of the
	screen). A headset belongs to one player: it keeps only that player's pictures, shows them
	full size (the 3D rebuilt with that player's own focal length, the HUD scaled up from its
	quarter onto the whole 2D plane) and drops the others' pictures, so the headset goes on
	showing its own player's last one.

	Emulator side only: the game's memory is read (player count, the players' view records),
	never written, so netplay stays in sync whatever player each side shows.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

struct rend_context;

namespace vr
{

// The local player (0..3) whose view this headset shows in split screen, or -1: none (not
// Toy Commander, one player, or switched off). From the netplay session's local player, else
// the test option vr.MpLocalPlayer (1..4; 0 auto, -1 off).
int ownViewPlayer();

// Decides, for a picture about to be rendered to the screen, whether it is kept: sets
// ctx.tcvrView / tcvrViewFocal for the local player's picture (width 0 otherwise). Returns
// false for another player's picture in split screen: not to be shown.
bool ownViewKeep(rend_context& ctx);

}
