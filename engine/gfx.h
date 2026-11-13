// The renderer: Vulkan on every platform. On the iPhone, MoltenVK runs it on Metal.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace gfx {

typedef uint32_t Handle;   // 0 means "none". See pool.h

enum Mesh { MESH_CUBE = 0, MESH_SPHERE = 1, MESH_PLANE = 2, MESH_COUNT };
enum Blend { BLEND_OPAQUE = 0, BLEND_ALPHA = 1, BLEND_ADD = 2 };
enum TextureFlags { TEX_NEAREST = 1, TEX_CLAMP = 2, TEX_NOMIP = 4, TEX_DATA = 8 };   // TEX_DATA: not a colour picture (a normal map)

// The one vertex layout every mesh and shader uses.
struct Vertex {
    float pos[3];
    float normal[3];
    float color[4];
    float uv[2];
    float tangent[4];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float center[3] = {0, 0, 0}, radius = 0;
};

struct Stats {
    int draws = 0, vertices = 0, textures = 0, meshes = 0;
};

bool init(SDL_Window* window, const std::string& engineShaderDir, std::string& error);
void shutdown();

// Files are looked up in <gameDir>. Calling this drops the game's shaders and everything it made itself.
// Files loaded from the same folder are kept unless they changed, so a reload after an edit is quick.
void set_game_dir(const std::string& gameDir);

// Everything below is called between begin_frame and end_frame.
bool begin_frame();
void end_frame(float timeSeconds);

// ---- the scene: these last for one frame ----
void clear(float r, float g, float b);
// fovDegrees below zero gives a flat (orthographic) camera that many units tall.
void camera(const float eye[3], const float target[3], float fovDegrees, float zNear = 0.1f, float zFar = 500.0f, const float* up = nullptr);
void ambient(float r, float g, float b);
void light(float x, float y, float z, float r = 0.65f, float g = 0.65f, float b = 0.65f);
void point_light(const float pos[3], float radius, const float rgb[3]);
void spot_light(const float pos[3], const float dir[3], float radius, float angleDegrees, const float rgb[3]);
void fog(const float rgb[3], float start, float end);
void shadows(const float center[3], float radius, float strength);
void frame_param(int index, const float v[4]);
// Draw into a texture made by new_target instead of the screen. 0 goes back to the screen.
void target(Handle renderTarget);

// ---- how the next things are drawn: these stay until changed ----
void shader(const std::string& name);   // "" selects the engine's standard shader
void texture(int slot, Handle tex);     // slot 0 to 3
void blend(int mode);
void cull(bool backFaces);
void unlit(bool on);
void depth(bool test, bool write);
void cutoff(float alpha);
void cast_shadows(bool on);
void param(int index, const float v[4]);
void font(Handle f);                    // 0 selects the built-in 8x8 font
void reset_state();

// ---- things that are loaded or made once and then used by handle ----
Handle load_texture(const std::string& name, int flags);
Handle texture_from_memory(const void* bytes, size_t size, int flags, bool scriptOwned);
Handle new_texture(int w, int h, const uint8_t* rgba, int flags, bool scriptOwned);
Handle new_target(int w, int h);
bool texture_size(Handle tex, int& w, int& h);
Handle missing_texture();   // the loud checker that load_texture hands back for a file it can't find
void free_texture(Handle tex);
Handle new_mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool scriptOwned);
const MeshData* mesh_data(Handle mesh);
void free_mesh(Handle mesh);
Handle builtin_mesh(Mesh which);
Handle load_font(const std::string& name, float pixelHeight);
// Fills in tangents from the texture coordinates, and normals where a mesh has none.
void finish_vertices(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool makeNormals);
std::string game_path(const std::string& name);   // "" if the name tries to leave the game's folder

// ---- 3D drawing ----
void draw(Handle mesh, const float model[16], const float rgba[4]);
// The same mesh with its vertices moved, for skinned characters.
void draw_vertices(Handle mesh, const Vertex* vertices, uint32_t count, const float model[16], const float rgba[4]);
void mesh(Mesh which, const float pos[3], const float size[3], float yawDegrees, const float rgba[4]);
void sky(Handle tex, const float rgba[4]);
// count squares that face the camera. Eight numbers each: x, y, z, size, r, g, b, a.
void particles(const float* data, int count);
void line(const float a[3], const float b[3], const float rgba[4], float width);
bool project(const float world[3], float& x, float& y);              // false when behind the camera
void unproject(float x, float y, float origin[3], float dir[3]);
const float* camera_position();
const float* camera_right();

// 2D drawing uses a screen whose shorter side is always 720 units, whichever way the phone is held.
float view_width();
float view_height();
void rect(float x, float y, float w, float h, const float rgba[4]);
void circle(float cx, float cy, float radius, const float rgba[4]);
void image(Handle tex, float x, float y, float w, float h, const float rgba[4], const float uv[4]);
void text(const std::string& s, float x, float y, float size, const float rgba[4]);
float text_width(const std::string& s, float size);

// The strips along each edge that a phone keeps for its notch, island, rounded corners and home bar,
// in 2D units: left, top, right, bottom. The app sets them every frame; games read them with gfx.safe().
void set_safe_area(const float insets[4]);
const float* safe_area();

// PC only: leaves a black border this many pixels wide around the picture, which stands in for a phone's body.
void set_bezel(int pixels);
// Turns a position in the window (0 to 1 across and down) into a position in the picture inside that border.
void window_to_view(float& x, float& y);

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
Stats stats();   // of the frame before this one

// Small maths shared with the model loader. Matrices are column-major.
void mat_identity(float m[16]);
void mat_multiply(const float a[16], const float b[16], float out[16]);
void mat_from_trs(const float pos[3], const float quat[4], const float scale[3], float out[16]);

#if defined(GFX_SCREENSHOT)
bool screenshot(const std::string& path);
#endif

}  // namespace gfx
