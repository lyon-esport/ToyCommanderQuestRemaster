/*
	tcvr: the comfort vignette (Toy Commander VR, backlog P0-9).

	When the view turns without the head turning (the vehicle steering, later the game camera
	driven around the player), the edges of the view darken: less motion in the corner of the
	eye, where it makes people sick. It stays put with the head (drawn in eye space), and is
	off in menus and on the setup panels.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

namespace vr::xr
{

// How much the view turns on its own now, from the player's steering: the stick's sideways
// deflection, 0..1 (after its dead zone). Called once per input update.
void noteSteering(float amount);
// The same from a measured turn rate of the game camera (radians per second, either way):
// for the head-driven camera (P0-8). The stronger of the two counts.
void setArtificialTurnRate(float radiansPerSecond);
// Once per headset frame: the vignette's strength (0..1, after vr.ComfortVignette and its
// easing in and out), which drawVignette then uses for both eyes.
float updateVignette(double seconds);
// Over the eye's picture (its framebuffer bound), around the eye's forward direction.
void drawVignette(const glm::mat4& projection, float strength);
void termVignette();

}
