// The renderer: Vulkan on every platform. On the iPhone, MoltenVK runs it on Metal.
#pragma once
#include <string>
#include <vector>

struct SDL_Window;

namespace gfx {

enum Mesh { MESH_CUBE = 0, MESH_SPHERE = 1, MESH_PLANE = 2, MESH_COUNT };

bool init(SDL_Window* window, const std::string& engineShaderDir, std::string& error);
void shutdown();

// Custom shaders are looked up in <gameDir>/shaders. Calling this drops the ones loaded so far.
void set_game_dir(const std::string& gameDir);

// Everything below is called between begin_frame and end_frame.
bool begin_frame();
void end_frame(float timeSeconds);

void clear(float r, float g, float b);
void camera(const float eye[3], const float target[3], float fovDegrees);
void light(float x, float y, float z);
void shader(const std::string& name);   // "" selects the engine's standard shader
void mesh(Mesh which, const float pos[3], const float size[3], float yawDegrees, const float rgba[4]);

// 2D drawing uses a screen whose shorter side is always 720 units, whichever way the phone is held.
float view_width();
float view_height();
void rect(float x, float y, float w, float h, const float rgba[4]);
void circle(float cx, float cy, float radius, const float rgba[4]);
void text(const std::string& s, float x, float y, float size, const float rgba[4]);
float text_width(const std::string& s, float size);

// The strips along each edge that a phone keeps for its notch, island, rounded corners and home bar,
// in 2D units: left, top, right, bottom. The app sets them every frame; games read them with gfx.safe().
void set_safe_area(const float insets[4]);
const float* safe_area();

// For the PC's layout check: while logging is on, every rect, circle and text drawn is remembered
// as one box until the next begin_frame.
struct UiBox {
    float x, y, w, h;
    bool round;          // a circle
    std::string text;    // what was written, or empty for a shape
};
void ui_log(bool on);
const std::vector<UiBox>& ui_boxes();

// The last shader problem, shown on screen by the engine. Empty when there is none.
const std::string& shader_error();

#if defined(GFX_SCREENSHOT)
bool screenshot(const std::string& path);
#endif

}  // namespace gfx
