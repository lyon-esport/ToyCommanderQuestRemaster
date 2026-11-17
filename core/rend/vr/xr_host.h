/*
	OpenXR host (hotd2-vr): turns the Android build into an immersive Quest app.

	The emulator keeps producing frames at the game's own rate. The render thread paces
	itself on the headset instead (xrWaitFrame), lets those frames through, and draws the
	latest one once per eye with the reprojection from rend/vr_reproject.h, so head
	movement is tracked at the display rate. The right controller is the light gun.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

namespace vr::xr
{

struct Eye
{
	unsigned fbo;
	int width;
	int height;
	// Rebuilt game eye space (renderer y convention, game units) -> this eye's clip space.
	glm::mat4 viewProj;
	// tcvr: the same without the head-driven camera's turn, for the HUD plane
	glm::mat4 hudViewProj;
};

#ifdef USE_OPENXR
// Built with OpenXR and switched on (vr.Xr).
bool enabled();
// One headset frame, called by the render thread in place of the usual wait-and-present.
// Returns false when nothing was shown (session not running).
bool frame();
// The eye being drawn right now, or nullptr outside an eye pass.
const Eye *currentEye();
// The render context is going away (app in the background, renderer restart): drop the
// session bound to it. The next frame() starts a new one on the new context.
void term();
// tcvr: where the head looks, relative to the recenter direction (the game camera's forward):
// yaw (radians, positive to the left) and pitch (radians, positive up), at the last headset
// frame. Any thread. For the head-driven game camera (backlog P0-8).
glm::vec2 headYawPitch();
// tcvr: test aid (files/head.request): which "set camera" calls the head camera turns
// (0 player 1's camera, the default; see xr_headcam.cpp).
int headCameraTarget();
int headCameraAxes();	// (test aid, see head.request)
// tcvr: the head camera's turn of a picture (rend_context::tcvrCamYaw/Pitch) as a matrix in the
// rebuilt game eye space: Eye::viewProj * gameTurn(turn) puts that picture where it belongs.
glm::mat4 gameTurn(glm::vec2 turn);
#else
inline bool enabled() { return false; }
inline bool frame() { return false; }
inline const Eye *currentEye() { return nullptr; }
inline void term() {}
inline glm::vec2 headYawPitch() { return glm::vec2(0.f); }
inline int headCameraTarget() { return 0; }
inline int headCameraAxes() { return 0; }
inline glm::mat4 gameTurn(glm::vec2) { return glm::mat4(1.f); }
#endif

}
