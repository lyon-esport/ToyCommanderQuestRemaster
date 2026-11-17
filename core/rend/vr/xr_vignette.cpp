/*
	tcvr: the comfort vignette. See xr_vignette.h.

	A full-screen triangle per eye; the fragment shader works out each pixel's angle from the
	eye's forward direction (from the eye's own, asymmetric projection) and darkens beyond an
	angle that closes in as the strength grows.

	This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_vignette.h"
#include "cfg/option.h"
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#include "log/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace vr::xr
{
namespace
{

std::atomic<float> steering;	// 0..1
std::atomic<float> turnRate;	// rad/s
float shown;					// the eased strength, 0..1

GLuint program, vao, vbo;
GLint uTanScale = -1, uTanOffset = -1, uInner = -1, uOuter = -1, uAlpha = -1;

// The view turning at this rate or more: full strength. 90 degrees a second is a brisk turn
// of a toy car; slower turns get a lighter vignette.
constexpr float FullRate = 1.57f;
// Eased in fast (it must be there as the turn starts) and out slower (no flicker between
// small steering corrections). Time constants in seconds.
constexpr float RiseTime = 0.08f, FallTime = 0.35f;
// The clear area, as a half angle from the eye's forward direction: from (barely there) to
// (at full strength), and the soft edge's width. Degrees.
constexpr float ClearAtLow = 62.f, ClearAtFull = 30.f, Edge = 22.f;

bool initGl()
{
	if (program != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec4 in_pos;
out highp vec2 vtx_ndc;
void main()
{
	vtx_ndc = in_pos.xy;
	gl_Position = vec4(in_pos.xy, 0.0, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform highp vec2 tanScale;	// 1 / (projection[0][0], projection[1][1])
uniform highp vec2 tanOffset;	// (projection[2][0], projection[2][1])
uniform highp float inner;		// radians: clear up to here
uniform highp float outer;		// radians: black from here
uniform highp float alpha;
in highp vec2 vtx_ndc;
void main()
{
	highp vec2 t = (vtx_ndc + tanOffset) * tanScale;	// tangents of the angles from forward
	highp float angle = atan(length(t));
	gl_FragColor = vec4(0.0, 0.0, 0.0, alpha * smoothstep(inner, outer, angle));
}
)");
	program = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (program == 0)
	{
		ERROR_LOG(RENDERER, "XR: the comfort vignette's shader didn't build");
		return false;
	}
	uTanScale = glGetUniformLocation(program, "tanScale");
	uTanOffset = glGetUniformLocation(program, "tanOffset");
	uInner = glGetUniformLocation(program, "inner");
	uOuter = glGetUniformLocation(program, "outer");
	uAlpha = glGetUniformLocation(program, "alpha");
	// one triangle over the whole screen
	const float tri[] { -1.f, -1.f, 0.f,  3.f, -1.f, 0.f,  -1.f, 3.f, 0.f };
	vao = createVertexArray();
	bindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (const void *)0);
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	bindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	return true;
}

}	// namespace

void noteSteering(float amount)
{
	steering = std::clamp(std::abs(amount), 0.f, 1.f);
}

void setArtificialTurnRate(float radiansPerSecond)
{
	turnRate = std::abs(radiansPerSecond);
}

float updateVignette(double seconds)
{
	const float setting = std::clamp((float)config::VrComfortVignette, 0.f, 1.f);
	const float target = setting * std::max((float)steering, std::min((float)turnRate / FullRate, 1.f));
	const float dt = (float)std::clamp(seconds, 0.0, 0.1);
	const float tau = target > shown ? RiseTime : FallTime;
	shown += (target - shown) * (1.f - std::exp(-dt / tau));
	if (target == 0.f && shown < 0.002f)
		shown = 0.f;
	return shown;
}

void drawVignette(const glm::mat4& projection, float strength)
{
	if (strength < 0.01f || !initGl())
		return;
	const float deg = 3.14159265f / 180.f;
	const float inner = (ClearAtLow + (ClearAtFull - ClearAtLow) * strength) * deg;
	glcache.UseProgram(program);
	glUniform2f(uTanScale, 1.f / projection[0][0], 1.f / projection[1][1]);
	glUniform2f(uTanOffset, projection[2][0], projection[2][1]);
	glUniform1f(uInner, inner);
	glUniform1f(uOuter, inner + Edge * deg);
	// never fully black at the very edge: a hint of the world stays (less closed in)
	glUniform1f(uAlpha, std::min(1.f, 0.35f + strength) * 0.95f);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	// (through the cache: the game's next draw relies on it)
	bindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	bindVertexArray(0);
	glcache.Disable(GL_BLEND);
}

void termVignette()
{
	if (program != 0)
		glcache.DeleteProgram(program);
	if (vbo != 0)
		glDeleteBuffers(1, &vbo);
	deleteVertexArray(vao);
	program = vao = vbo = 0;
	uTanScale = uTanOffset = uInner = uOuter = uAlpha = -1;
	shown = 0.f;
}

}
