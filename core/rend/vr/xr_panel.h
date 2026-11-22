/*
	A panel with a few lines of text in the headset (hotd2-vr), for what the app has to say
	when there's no game to show: making the agent's hands at the first start (hands_rip.h).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "types.h"
#include <glm/glm.hpp>
#include <memory>
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

// tcvr: a drawn shape (the multiplayer pages' cards and buttons), in the picture's pixels, top row 0
struct PanelShape
{
	enum Kind : u8 { Box, Triangle, Image } kind = Box;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;	// Box, Image: its corners; Triangle: two of its points
	float x2 = 0, y2 = 0;					// Triangle: the third
	float radius = 0;						// Box: rounded corners
	glm::vec4 fill { 0.f };					// alpha 0: not filled
	glm::vec4 border { 0.f };
	float borderPx = 0;
	bool dashed = false;
	// Image: a picture (RGBA, top row first) scaled into the box, rounded like it; pictureId names the
	// picture (the panel is painted again when it changes), keep holds it while the panel is painted
	const u8 *pixels = nullptr;
	int pixelsW = 0, pixelsH = 0;
	u32 pictureId = 0;
	std::shared_ptr<const void> keep;
};

// tcvr: text placed anywhere (UTF-8): x is its left edge, centre or right edge (align 0, 1, 2), y its
// baseline; with maxWidth > 0 it wraps into lines 1.3 of its height apart
struct PanelText
{
	std::string text;
	float x = 0, y = 0, px = 30;
	glm::vec3 colour { 1.f };
	int align = 0;
	float maxWidth = 0;
};

struct Panel
{
	std::vector<PanelLine> lines;
	// tcvr: shapes, then texts, drawn over the background (after the lines, if any)
	std::vector<PanelShape> shapes;
	std::vector<PanelText> texts;
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
// tcvr: everything the panel's picture depends on, as a text (the picture is made again when it changes)
std::string panelKey(const Panel& panel);
// tcvr: a text's width in pixels at that letter height (UTF-8)
float panelTextWidth(const std::string& text, float px);
void termPanel();
// tcvr: the panel's picture alone, for a compositor quad layer (the VR menu): imageWidth x
// imageHeight RGBA, alpha premultiplied, the bottom row first (OpenGL's image origin, which
// OpenXR keeps for OpenGL swapchains).
void paintPanelImage(const Panel& panel, std::vector<u8>& out);
// tcvr: a one-line label (the frame-rate counter), widthMetres wide, centred on mvp's origin in
// its x/y plane. Its picture is made again only when the text changes.
void drawLabel(const glm::mat4& mvp, const std::string& text, float widthMetres);

}
