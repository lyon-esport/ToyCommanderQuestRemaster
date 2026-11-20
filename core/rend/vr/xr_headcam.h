/*
	tcvr: the head-driven game camera of Toy Commander VR (backlog P0-8).

	The player turns their head and the game's own camera turns with it, so the game draws what
	the head looks at (all around the vehicle), not only the stock view ahead. The vehicle keeps
	its heading, steering and aim: only the picture's camera turns.

	How: a hook in the game's "set camera" routine (see xr_headcam.cpp and
	tcvr/docs/RE/Camera360.md) hands it a turned copy of the camera, never writing the game's
	own camera records. The turn used for each picture goes with it (rend_context::tcvrCamYaw/
	Pitch, latched at the game's scene end) and the VR view turns that picture back by as much,
	so the world stays where it is and the head sees it from the right direction.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

namespace vr::xr
{

// Each frame (VBlank): puts the hooks in the game's code when it is wanted (Toy Commander
// running, vr.HeadCamera on, no netplay), takes it out otherwise.
void updateHeadCamera();
// The turn (yaw, pitch; radians, positive left / up) the game camera of the picture on show
// was given: the VR view turns the picture back by this much.
glm::vec2 shownCameraTurn();

}
