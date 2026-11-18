/*
	tcvr: the VR menu (Toy Commander VR, backlog P0-3), drawn by the emulator over the game.

	The left controller's menu button held 0.6 s or pressed twice opens it (a short press is the game's Start) and
	pauses the game (the last picture stays, head-tracked); it closes with a press of the same
	button, B, or Resume. Either stick moves through it
	(up/down: the line, left/right: the value), A picks. It holds what the game's own menus
	can't: recenter, world size, comfort, the image sharpness settings one by one (to compare
	quality and frame rate in the headset), save export/import, unlock all levels and quit. Settings are kept in
	emu.cfg as soon as they change.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "xr_panel.h"
#include <glm/glm.hpp>
#include <string>

namespace vr::xr
{

struct MenuInput
{
	bool toggle = false;		// open (left menu button held) or close (pressed) now
	glm::vec2 stick { 0.f };	// the stronger of the two sticks
	bool accept = false;		// A, pressed now
	bool back = false;			// B, pressed now
};

enum class MenuAction { None, Recenter };

bool menuOpen();
// Once per headset frame with the controllers' state (gamepad mode). headYaw: where the head
// looks (radians, relative to the game camera's direction), for placing the menu when it opens.
MenuAction updateMenu(const MenuInput& input, float headYaw);
// Over an eye's picture (its framebuffer bound); viewProj as for drawPanel. (Only where the
// menu's own compositor layer can't be made: see menuPanel.)
void drawMenu(const glm::mat4& viewProj);
// tcvr: what the open menu shows, and the yaw it is placed at (radians, relative to the game
// camera's direction), MenuDistanceMetres in front at eye height. xr_host gives it to the
// headset as its own quad layer: the compositor samples the picture once, straight to the
// display, where drawn into the eye's picture it was resampled twice (soft, shimmering letters).
// False: the menu is closed.
constexpr float MenuDistanceMetres = 1.6f;
bool menuPanel(Panel& panel, float& yaw);
// Counts headset frames, for the frame rate the menu shows.
void noteHeadsetFrame();
// The headset's and the game's frames per second over the last second (the frame-rate counter).
void frameRates(float& headset, float& game);
// The menu goes (session ending): the game runs again if the menu paused it.
void closeMenu();

// What the platform does for the menu (Android.cpp): export the saves to Download, import the
// newest exported ones and start the game again, quit, open every level of the saved games
// (Toy Commander only) and start the game again, the first saved game's name ("" if none; the
// default player name of the multiplayer), start the game again (a multiplayer match starts and ends
// that way). Returns a line for the menu to show.
// The values are BaseGLActivity.onVrMenuRequest's request numbers.
enum class MenuRequest { ExportSaves, ImportSaves, Quit, UnlockAll, SaveName, Restart };
extern std::string (*menuRequestHook)(MenuRequest request);

}
