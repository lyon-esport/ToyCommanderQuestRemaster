/*
	A panel with text in the headset (hotd2-vr). See xr_panel.h.

	The text is drawn on the CPU with stb_truetype (the copy that comes with Dear ImGui) in
	Flycast's own Roboto, into a picture that goes on a quad.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_panel.h"
#ifndef XR_PANEL_PAINT_ONLY		// (tcvr: the offline menu preview builds the painting alone)
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#endif
#include "oslib/resources.h"
#include "log/Log.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "imgui/imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstring>
#include <cmath>
#include <memory>

namespace vr::xr
{
namespace
{

constexpr int Width = 1024, Height = 448;
constexpr float Distance = 1.6f;

std::unique_ptr<u8[]> fontData;
stbtt_fontinfo font;
bool fontTried, fontLoaded;

std::vector<u8> pixels;
std::string shown;			// what the picture says now
// tcvr: the picture blend/fill/text draw on: the panel's (Width x Height), or the label's
std::vector<u8> *canvas = &pixels;
int canvasW = Width, canvasH = Height;
// tcvr: the small head-locked label (drawLabel)
constexpr int LabelW = 512, LabelH = 96;
std::vector<u8> labelPixels;
std::string labelShown;

#ifndef XR_PANEL_PAINT_ONLY
GLuint program, texture;
GLuint quadVao, quadVbo;
GLint uMvp = -1;
GLuint labelTexture;

void logGlErrors()
{
	// (hotd2-vr: the first few seconds only, at most 10 lines: checked after every draw it
	// costs a GPU round trip and filled the log at headset rate)
	static int checks, logged;
	if (checks >= 1000 || logged >= 10)
		return;
	checks++;
	for (GLenum error; (error = glGetError()) != GL_NO_ERROR && logged < 10; logged++)
		ERROR_LOG(RENDERER, "VR panel OpenGL error 0x%04x", error);
}
#endif

bool loadFont()
{
	if (fontTried)
		return fontLoaded;
	fontTried = true;
	size_t size = 0;
	fontData = resource::load("fonts/Roboto-Bold.ttf", size);
	fontLoaded = fontData != nullptr && stbtt_InitFont(&font, fontData.get(), stbtt_GetFontOffsetForIndex(fontData.get(), 0)) != 0;
	if (!fontLoaded)
		WARN_LOG(RENDERER, "XR: no font for the panel");
	return fontLoaded;
}

void blend(int x, int y, const glm::vec3& colour, float alpha)
{
	if (x < 0 || y < 0 || x >= canvasW || y >= canvasH || alpha <= 0.f)
		return;
	u8 *p = &(*canvas)[((size_t)y * canvasW + x) * 4];
	for (int c = 0; c < 3; c++)
		p[c] = (u8)std::lround(p[c] * (1.f - alpha) + colour[c] * 255.f * alpha);
}

void fill(int x0, int y0, int x1, int y1, const glm::vec3& colour, float alpha = 1.f)
{
	for (int y = std::max(y0, 0); y < std::min(y1, canvasH); y++)
		for (int x = std::max(x0, 0); x < std::min(x1, canvasW); x++)
			blend(x, y, colour, alpha);
}

// tcvr: the code points of a UTF-8 text (a broken byte counts as itself)
std::vector<int> codepoints(const std::string& s)
{
	std::vector<int> out;
	for (size_t i = 0; i < s.size();)
	{
		const u8 c = (u8)s[i];
		int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		int cp = n == 0 ? c : c & (0x3F >> n);
		bool ok = true;
		for (int k = 1; k <= n; k++)
		{
			if (i + k >= s.size() || ((u8)s[i + k] & 0xC0) != 0x80)
			{
				ok = false;
				break;
			}
			cp = (cp << 6) | ((u8)s[i + k] & 0x3F);
		}
		if (!ok)
		{
			cp = c;
			n = 0;
		}
		out.push_back(cp);
		i += n + 1;
	}
	return out;
}

float widthOf(const std::vector<int>& cps, float px)
{
	const float scale = stbtt_ScaleForPixelHeight(&font, px);
	float width = 0.f;
	for (size_t i = 0; i < cps.size(); i++)
	{
		int advance, bearing;
		stbtt_GetCodepointHMetrics(&font, cps[i], &advance, &bearing);
		width += advance * scale;
		if (i + 1 < cps.size())
			width += scale * stbtt_GetCodepointKernAdvance(&font, cps[i], cps[i + 1]);
	}
	return width;
}

// One line from x (its left edge), its baseline at y.
void textFrom(const std::vector<int>& cps, float px, float x, int y, const glm::vec3& colour)
{
	const float scale = stbtt_ScaleForPixelHeight(&font, px);
	std::vector<u8> glyph;
	for (size_t i = 0; i < cps.size(); i++)
	{
		const int c = cps[i];
		int advance, bearing, x0, y0, x1, y1;
		stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
		stbtt_GetCodepointBitmapBoxSubpixel(&font, c, scale, scale, x - std::floor(x), 0.f, &x0, &y0, &x1, &y1);
		const int w = x1 - x0, h = y1 - y0;
		if (w > 0 && h > 0)
		{
			glyph.assign((size_t)w * h, 0);
			stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), w, h, w, scale, scale, x - std::floor(x), 0.f, c);
			for (int gy = 0; gy < h; gy++)
				for (int gx = 0; gx < w; gx++)
					blend((int)std::floor(x) + x0 + gx, y + y0 + gy, colour, glyph[(size_t)gy * w + gx] / 255.f);
		}
		x += advance * scale;
		if (i + 1 < cps.size())
			x += scale * stbtt_GetCodepointKernAdvance(&font, c, cps[i + 1]);
	}
}

// One line, centred, its baseline at y.
void text(const std::string& s, float px, int y, const glm::vec3& colour)
{
	const std::vector<int> cps = codepoints(s);
	textFrom(cps, px, (canvasW - widthOf(cps, px)) * 0.5f, y, colour);
}

// tcvr: a placed text, wrapped if it has a width
void placedText(const PanelText& t)
{
	std::vector<std::string> lines;
	if (t.maxWidth <= 0.f)
		lines.push_back(t.text);
	else
	{
		std::string line, word;
		auto flush = [&]() {
			if (word.empty())
				return;
			const std::string tryLine = line.empty() ? word : line + ' ' + word;
			if (!line.empty() && widthOf(codepoints(tryLine), t.px) > t.maxWidth)
			{
				lines.push_back(line);
				line = word;
			}
			else
				line = tryLine;
			word.clear();
		};
		for (char c : t.text)
		{
			if (c == ' ')
				flush();
			else if (c == '\n')
			{
				flush();
				lines.push_back(line);
				line.clear();
			}
			else
				word += c;
		}
		flush();
		if (!line.empty())
			lines.push_back(line);
	}
	float y = t.y;
	for (const std::string& l : lines)
	{
		const std::vector<int> cps = codepoints(l);
		const float w = widthOf(cps, t.px);
		const float x = t.align == 1 ? t.x - w * 0.5f : t.align == 2 ? t.x - w : t.x;
		textFrom(cps, t.px, x, (int)std::lround(y), t.colour);
		y += t.px * 1.3f;
	}
}

// tcvr: how much of the pixel at (px, py) a rounded box covers (its signed distance, smoothed over a pixel)
float boxDistance(float px, float py, float x0, float y0, float x1, float y1, float r)
{
	const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
	const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
	r = std::min(r, std::min(hx, hy));
	const float qx = std::abs(px - cx) - hx + r, qy = std::abs(py - cy) - hy + r;
	const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
	return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

void shape(const PanelShape& s)
{
	if (s.kind == PanelShape::Triangle)
	{
		if (s.fill.a <= 0.f)
			return;
		const float xs[3] { s.x0, s.x1, s.x2 }, ys[3] { s.y0, s.y1, s.y2 };
		const int bx0 = (int)std::floor(std::min({ xs[0], xs[1], xs[2] })) - 1, bx1 = (int)std::ceil(std::max({ xs[0], xs[1], xs[2] })) + 1;
		const int by0 = (int)std::floor(std::min({ ys[0], ys[1], ys[2] })) - 1, by1 = (int)std::ceil(std::max({ ys[0], ys[1], ys[2] })) + 1;
		const float area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0]);
		const float sign = area < 0.f ? -1.f : 1.f;
		for (int y = by0; y <= by1; y++)
			for (int x = bx0; x <= bx1; x++)
			{
				const float px = x + 0.5f, py = y + 0.5f;
				float inside = 1e9f;
				for (int e = 0; e < 3; e++)
				{
					const int f = (e + 1) % 3;
					const float ex = xs[f] - xs[e], ey = ys[f] - ys[e];
					const float len = std::sqrt(ex * ex + ey * ey);
					if (len <= 0.f)
						continue;
					inside = std::min(inside, sign * (ex * (py - ys[e]) - ey * (px - xs[e])) / len);
				}
				const float cover = std::clamp(inside + 0.5f, 0.f, 1.f);
				blend(x, y, glm::vec3(s.fill), s.fill.a * cover);
			}
		return;
	}
	const int bx0 = (int)std::floor(s.x0) - 1, bx1 = (int)std::ceil(s.x1) + 1;
	const int by0 = (int)std::floor(s.y0) - 1, by1 = (int)std::ceil(s.y1) + 1;
	for (int y = by0; y <= by1; y++)
		for (int x = bx0; x <= bx1; x++)
		{
			const float px = x + 0.5f, py = y + 0.5f;
			const float d = boxDistance(px, py, s.x0, s.y0, s.x1, s.y1, s.radius);
			const float outer = std::clamp(0.5f - d, 0.f, 1.f);
			if (outer <= 0.f)
				continue;
			if (s.fill.a > 0.f)
				blend(x, y, glm::vec3(s.fill), s.fill.a * outer);
			if (s.borderPx > 0.f && s.border.a > 0.f)
			{
				const float inner = std::clamp(0.5f - (d + s.borderPx), 0.f, 1.f);
				float cover = outer - inner;
				if (s.dashed)
				{
					// dashes along the edge nearest to the pixel
					const bool horizontal = std::min(py - s.y0, s.y1 - py) < std::min(px - s.x0, s.x1 - px);
					if (((int)((horizontal ? px : py) / 18.f)) % 2 != 0)
						cover = 0.f;
				}
				blend(x, y, glm::vec3(s.border), s.border.a * cover);
			}
		}
}

// The panel's picture into pixels (panel.imageWidth x panel.imageHeight, top row first).
void paint(const Panel& panel, std::vector<u8>& out)
{
	const int w = panel.imageWidth, h = panel.imageHeight;
	canvas = &out;
	canvasW = w;
	canvasH = h;
	out.resize((size_t)w * h * 4);
	// tcvr: nearly opaque, for contrast over a bright game (it was 235: the game showed through)
	const u8 background[4] { 16, 15, 17, 246 };
	for (size_t i = 0; i < out.size(); i += 4)
		memcpy(&out[i], background, 4);
	// a thin red frame, like the game's own
	const glm::vec3 red(0.62f, 0.06f, 0.06f);
	const int edge = std::max(5, h / 90);
	fill(0, 0, w, edge, red);
	fill(0, h - edge, w, h, red);
	fill(0, 0, edge, h, red);
	fill(w - edge, 0, w, h, red);

	float total = 0.f;
	for (const PanelLine& line : panel.lines)
		total += line.size * h * 1.45f;
	const int bar = panel.progress >= 0.f ? h * 46 / 448 : 0;
	float y = (h - total - bar) * 0.5f;
	for (size_t i = 0; i < panel.lines.size(); i++)
	{
		const PanelLine& line = panel.lines[i];
		const float px = line.size * h;
		if ((int)i == panel.highlight)
		{
			// tcvr: the picked line on a lighter bar, across the middle of the panel
			const int top = (int)(y + px * 0.2f), bottom = (int)(y + px * 1.45f + px * 0.05f);
			fill(w * 22 / 100, top, w - w * 22 / 100, bottom, glm::vec3(0.36f, 0.08f, 0.07f), 0.85f);
		}
		y += px * 1.45f;
		if (!line.text.empty())
			text(line.text, px, (int)(y - px * 0.38f), line.colour);
	}
	if (panel.progress >= 0.f)
	{
		const int x0 = w * 18 / 100, x1 = w - x0, top = (int)y + h * 24 / 448, bottom = top + h * 14 / 448;
		fill(x0, top, x1, bottom, glm::vec3(0.24f, 0.24f, 0.26f));
		fill(x0, top, x0 + (int)((x1 - x0) * std::clamp(panel.progress, 0.f, 1.f)), bottom, glm::vec3(0.8f, 0.1f, 0.1f));
	}
	if (!panel.footer.empty())
		text(panel.footer, h * 0.042f, h - h * 28 / 448, glm::vec3(0.55f));
	for (const PanelShape& s : panel.shapes)
		shape(s);
	for (const PanelText& t : panel.texts)
		placedText(t);
	canvas = &pixels;
	canvasW = Width;
	canvasH = Height;
}

#ifndef XR_PANEL_PAINT_ONLY
bool initGl()
{
	if (program != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec4 in_pos;
in highp vec2 in_uv;
uniform highp mat4 mvp;
out highp vec2 vtx_uv;
void main()
{
	vtx_uv = in_uv;
	gl_Position = mvp * vec4(in_pos.xyz, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform sampler2D tex;
in highp vec2 vtx_uv;
void main()
{
	gl_FragColor = texture(tex, vtx_uv);
}
)");
	program = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (program == 0)
		return false;
	uMvp = glGetUniformLocation(program, "mvp");
	glcache.UseProgram(program);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	glGenTextures(1, &texture);
	glcache.BindTexture(GL_TEXTURE_2D, texture);
	// tcvr: mipmapped (the menu's picture is finer than the eye's pixels: without, its letters shimmered)
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	quadVao = createVertexArray();
	bindVertexArray(quadVao);
	glGenBuffers(1, &quadVbo);
	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	glBufferData(GL_ARRAY_BUFFER, 20 * sizeof(float), nullptr, GL_STREAM_DRAW);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)0);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	bindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	shown.clear();
	return true;
}

#endif

}	// namespace

#ifndef XR_PANEL_PAINT_ONLY
void drawPanel(const glm::mat4& viewProj, const Panel& panel)
{
	if (!loadFont() || !initGl())
		return;
	const std::string says = panelKey(panel);
	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, texture);
	if (says != shown)
	{
		shown = says;
		paint(panel, pixels);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, panel.imageWidth, panel.imageHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	const float hx = panel.widthMetres * 0.5f, hy = hx * panel.imageHeight / panel.imageWidth;
	const float quad[] {
		-hx,  hy, -Distance,  0.f, 0.f,
		 hx,  hy, -Distance,  1.f, 0.f,
		-hx, -hy, -Distance,  0.f, 1.f,
		 hx, -hy, -Distance,  1.f, 1.f,
	};
	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
	glcache.UseProgram(program);
	glUniformMatrix4fv(uMvp, 1, GL_FALSE, &viewProj[0][0]);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	// (through the cache: the game's next draw relies on it)
	bindVertexArray(quadVao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	bindVertexArray(0);
	glcache.Disable(GL_BLEND);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	logGlErrors();
}

void drawLabel(const glm::mat4& mvp, const std::string& s, float widthMetres)
{
	if (!loadFont() || !initGl())
		return;
	glActiveTexture(GL_TEXTURE0);
	if (labelTexture == 0)
	{
		glGenTextures(1, &labelTexture);
		glcache.BindTexture(GL_TEXTURE_2D, labelTexture);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		labelShown.clear();
	}
	glcache.BindTexture(GL_TEXTURE_2D, labelTexture);
	if (s != labelShown)
	{
		labelShown = s;
		labelPixels.assign((size_t)LabelW * LabelH * 4, 0);
		for (size_t i = 0; i < labelPixels.size(); i += 4)
		{
			labelPixels[i] = labelPixels[i + 1] = labelPixels[i + 2] = 10;
			labelPixels[i + 3] = 170;
		}
		canvas = &labelPixels;
		canvasW = LabelW;
		canvasH = LabelH;
		text(s, LabelH * 0.62f, (int)(LabelH * 0.72f), glm::vec3(0.95f, 0.95f, 0.6f));
		canvas = &pixels;
		canvasW = Width;
		canvasH = Height;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LabelW, LabelH, 0, GL_RGBA, GL_UNSIGNED_BYTE, labelPixels.data());
	}
	const float hx = widthMetres * 0.5f, hy = hx * LabelH / LabelW;
	const float quad[] {
		-hx,  hy, 0.f,  0.f, 0.f,
		 hx,  hy, 0.f,  1.f, 0.f,
		-hx, -hy, 0.f,  0.f, 1.f,
		 hx, -hy, 0.f,  1.f, 1.f,
	};
	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
	glcache.UseProgram(program);
	glUniformMatrix4fv(uMvp, 1, GL_FALSE, &mvp[0][0]);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	bindVertexArray(quadVao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	bindVertexArray(0);
	glcache.Disable(GL_BLEND);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
}

#endif

std::string panelKey(const Panel& panel)
{
	std::string key;
	char buf[96];
	for (const PanelLine& line : panel.lines)
	{
		snprintf(buf, sizeof(buf), "|%.3f %d,%d,%d ", line.size, (int)(line.colour.r * 255.f), (int)(line.colour.g * 255.f), (int)(line.colour.b * 255.f));
		key += buf + line.text + '\n';
	}
	key += panel.footer + '\n' + std::to_string(panel.progress < 0.f ? -1 : (int)(panel.progress * 200.f))
			+ '\n' + std::to_string(panel.imageWidth) + 'x' + std::to_string(panel.imageHeight) + ' ' + std::to_string(panel.highlight);
	for (const PanelShape& s : panel.shapes)
	{
		snprintf(buf, sizeof(buf), "\nS%d %.0f %.0f %.0f %.0f %.0f %.0f %.0f ", (int)s.kind, s.x0, s.y0, s.x1, s.y1, s.x2, s.y2, s.radius);
		key += buf;
		snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.1f %d", s.fill.r, s.fill.g, s.fill.b, s.fill.a,
				s.border.r, s.border.g, s.border.b, s.border.a, s.borderPx, (int)s.dashed);
		key += buf;
	}
	for (const PanelText& t : panel.texts)
	{
		snprintf(buf, sizeof(buf), "\nT%.0f %.0f %.1f %.2f %.2f %.2f %d %.0f ", t.x, t.y, t.px, t.colour.r, t.colour.g, t.colour.b, t.align, t.maxWidth);
		key += buf + t.text;
	}
	return key;
}

float panelTextWidth(const std::string& text, float px)
{
	if (!loadFont())
		return text.size() * px * 0.5f;
	return widthOf(codepoints(text), px);
}

void paintPanelImage(const Panel& panel, std::vector<u8>& out)
{
	if (!loadFont())
		return;
	std::vector<u8> image;
	paint(panel, image);
	// premultiplied alpha, and the rows bottom first
	const size_t row = (size_t)panel.imageWidth * 4;
	out.resize(image.size());
	for (int y = 0; y < panel.imageHeight; y++)
	{
		const u8 *src = &image[(size_t)y * row];
		u8 *dst = &out[(size_t)(panel.imageHeight - 1 - y) * row];
		for (size_t x = 0; x < row; x += 4)
		{
			const unsigned a = src[x + 3];
			dst[x] = (u8)((src[x] * a + 127) / 255);
			dst[x + 1] = (u8)((src[x + 1] * a + 127) / 255);
			dst[x + 2] = (u8)((src[x + 2] * a + 127) / 255);
			dst[x + 3] = (u8)a;
		}
	}
}

#ifndef XR_PANEL_PAINT_ONLY
void termPanel()
{
	if (labelTexture != 0)
		glcache.DeleteTextures(1, &labelTexture);
	labelTexture = 0;
	labelShown.clear();
	if (program != 0)
		glcache.DeleteProgram(program);
	if (texture != 0)
		glcache.DeleteTextures(1, &texture);
	if (quadVbo != 0) glDeleteBuffers(1, &quadVbo);
	deleteVertexArray(quadVao);
	program = texture = 0;
	quadVao = quadVbo = 0;
	uMvp = -1;
	shown.clear();
}

#endif

}
