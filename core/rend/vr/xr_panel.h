/*
	A panel with a few lines of text in the headset (hotd2-vr), for what the app has to say
	when there's no game to show: making the agent's hands at the first start (hands_rip.h).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "types.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace vr::xr
{

struct PanelLine
{
	std::string text;
	float size;			// letter height, of the panel's height
	glm::vec3 colour;
};

struct Panel
{
	std::vector<PanelLine> lines;
	float progress = -1.f;	// a bar under the text, 0..1 (below 0: none)
	std::string footer;		// small, at the bottom
	// tcvr: its picture's size in pixels and its width in the room (the menu's is bigger and
	// sharper than the first-start panel's), and a line shown picked (a bar behind it; -1 none)
	int imageWidth = 1024, imageHeight = 448;
	float widthMetres = 1.3f;
	int highlight = -1;
};

// The panel, 1.3 m wide, 1.6 m in front of where the game camera is, at eye height. The
// room is dark around it. viewProj is the eye's (room space, relative to the game camera's
// place). Its picture is made again only when what it says changes.
void drawPanel(const glm::mat4& viewProj, const Panel& panel);
void termPanel();
// tcvr: the panel's picture alone, for a compositor quad layer (the VR menu): imageWidth x
// imageHeight RGBA, alpha premultiplied, the bottom row first (OpenGL's image origin, which
// OpenXR keeps for OpenGL swapchains).
void paintPanelImage(const Panel& panel, std::vector<u8>& out);
// tcvr: a one-line label (the frame-rate counter), widthMetres wide, centred on mvp's origin in
// its x/y plane. Its picture is made again only when the text changes.
void drawLabel(const glm::mat4& mvp, const std::string& text, float widthMetres);

}
