// What gfx.cpp, gfx_vulkan.cpp and gfx_text.cpp share: the renderer's types, its state and its internal
// functions. Internal to the renderer; the rest of the engine uses gfx.h.
#pragma once

#include "gfx.h"

// Desktop builds find Vulkan at run time through volk. The iPhone build links
// MoltenVK directly, so it uses the ordinary Vulkan function names.
#if defined(USE_VOLK)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
#else
#include <vulkan/vulkan.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "handles.h"

#define STBTT_STATIC
#include "stb_truetype.h"

namespace gfx {
namespace internal {

// Must match struct Frame in shaders/common.slang.
struct FrameData {
    float viewProj[16];
    float ortho[16];
    float shadowMat[16];
    float lightDir[4];
    float lightColor[4];
    float ambient[4];
    float cameraPos[4];
    float fogColor[4];
    float fogRange[4];
    float shadowInfo[4];
    float params[4];
    float user[4][4];
    float lightPos[8][4];
    float lightCol[8][4];
    float lightAim[8][4];
};

struct PushData {
    float model[16];
    float color[4];
    float flags[4];
    float params[8];
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

enum Owner { OWNER_ENGINE = 0, OWNER_FILE = 1, OWNER_SCRIPT = 2 };

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    int w = 0, h = 0, flags = 0;
    uint32_t mips = 1;
    // Only for render targets:
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    int owner = OWNER_SCRIPT;
    std::string key;       // the file it came from
    Sint64 mtime = 0;
};

struct MeshRes {
    Buffer vb, ib;
    MeshData data;
    int owner = OWNER_SCRIPT;
};

struct Glyph {
    float u0, v0, u1, v1;      // where it sits in the atlas
    float x0, y0, w, h, advance;   // in baked pixels
};

struct Font {
    std::vector<unsigned char> file;
    stbtt_fontinfo info;
    float px = 48, scale = 1, ascent = 0;
    Handle atlas = 0;
    std::vector<uint8_t> alpha;
    int penX = 1, penY = 1, rowH = 0;
    bool dirty = false, full = false;
    std::map<uint32_t, Glyph> glyphs;
    std::string key;
    Sint64 mtime = 0;
    bool engine = false;    // the engine's own: kept when games change
    float boost = 1;        // the engine's font is scaled so its capitals are as tall as the 8x8 font's
    float lift = 0;         // and raised so their tops sit where the 8x8 font's do
};

struct Draw {
    VkPipeline pipeline = VK_NULL_HANDLE, shadowPipeline = VK_NULL_HANDLE;
    VkDescriptorSet material = VK_NULL_HANDLE;
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;   // no ib: plain triangles
    uint32_t first = 0, count = 0;
    int32_t vertexOffset = 0;
    float depth = 0;
    PushData push;
};

// Everything gfx.camera, gfx.light and friends set. It starts fresh every frame.
struct FrameState {
    float viewProj[16];
    float eye[3], right[3], up[3], fwd[3];
    float focal = 1, orthoHeight = 0, aspect = 1, zNear = 0.1f, zFar = 500;
    float sunDir[3], sunColor[3], ambient[3];
    int lightCount = 0;
    float lightPos[8][4], lightCol[8][4], lightAim[8][4];
    float fogColor[4], fogRange[2];
    float shadowCenter[3], shadowRadius = 0, shadowStrength = 0;
    float user[4][4];
    float clear[3];
};

struct Pass {
    Handle target = 0;
    FrameState state;
    std::vector<Draw> sky, opaque, transparent, flat;
};

struct ShaderEntry {
    std::string name, dir;
    bool broken = false;
    std::map<uint32_t, VkPipeline> variants;
};

struct DrawState {
    ShaderEntry* shader = nullptr;
    Handle tex[4] = {0, 0, 0, 0};
    int blend = BLEND_OPAQUE;
    bool cull = true, unlit = false, depthTest = true, depthWrite = true, castShadow = true;
    float cutoff = 0;
    float params[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    Handle font = 0;
};

enum PassKind { KIND_SCREEN = 0, KIND_TARGET = 1, KIND_SHADOW = 2 };

const float VIEW_HEIGHT = 720.0f;

const uint32_t MAX_DYNAMIC_VERTS = 300000;   // 2D shapes, text, particles, lines and skinned meshes, per frame

const size_t MAX_PASSES = 8;

const uint32_t SHADOW_SLOT = 8, FRAME_SLOTS = 9;

const int SHADOW_SIZE = 2048;

const int ATLAS_SIZE = 1024;

const VkFormat targetFormat = VK_FORMAT_R8G8B8A8_SRGB;

// ---- the renderer's state (defined in gfx.cpp) ----

extern SDL_Window* window;
extern VkInstance instance;
extern VkSurfaceKHR surface;
extern VkPhysicalDevice gpu;
extern VkDevice device;
extern uint32_t queueFamily;
extern VkQueue queue;
extern bool anisotropy;
extern VkSwapchainKHR swapchain;
extern VkFormat colorFormat;
extern VkFormat depthFormat;
extern VkSampleCountFlagBits samples;
extern VkExtent2D extent;
extern std::vector<VkImageView> colorViews;
extern std::vector<VkFramebuffer> framebuffers;
extern VkImage depthImage, msaaImage;
extern VkDeviceMemory depthMemory, msaaMemory;
extern VkImageView depthView, msaaView;
extern bool swapchainStale;
extern VkRenderPass screenPass, targetPass, shadowPass;
extern VkCommandPool commandPool;
extern VkCommandBuffer cmd, onceCmd;
extern VkSemaphore imageReady;
extern std::vector<VkSemaphore> renderDone;
extern VkFence frameFence;
extern VkDescriptorSetLayout frameLayout, materialLayout;
extern VkDescriptorPool descriptorPool;
extern VkDescriptorSet frameSet;
extern VkPipelineLayout pipelineLayout;
extern VkSampler samplers[8];
extern VkSampler shadowSampler;
extern Buffer frameBuffer, dynBuffer;
extern VkDeviceSize frameStride;
extern uint32_t dynCount;
extern Pool<Texture> textures;
extern Pool<MeshRes> meshes;
extern Pool<Font> fonts;
extern std::map<std::array<Handle, 4>, VkDescriptorSet> materialSets;
extern std::map<std::string, ShaderEntry> shaders;
extern Texture shadowTex;
extern Handle whiteTex, normalTex, missingTex;
extern Handle builtin[MESH_COUNT];
extern std::string shaderDir, gameDir, shaderProblem;
extern uint32_t imageIndex;
extern FrameState fs, savedScreen;
extern DrawState ds;
extern std::vector<Pass> passes;
extern size_t cur;
extern VkDescriptorSet currentSet;
extern std::vector<UiBox> uiBoxes;
extern float safeInsets[4];
extern bool uiLogging;
extern Stats lastStats, nowStats;
extern int bezel;

// ---- internal functions (gfx.cpp, gfx_vulkan.cpp, gfx_text.cpp) ----

void normalize3(float v[3]);
void cross3(const float a[3], const float b[3], float out[3]);
float dot3(const float a[3], const float b[3]);
void look_at(const float eye[3], const float target[3], const float upHint[3], float view[16], float s[3], float u[3], float f[3]);
float srgb_to_linear(float c);
void srgb_to_linear4(const float in[4], float out[4]);
int inset();
float inner_width();
float inner_height();
float screen_aspect();
float aspect();
bool fail(std::string& error, const char* what);
uint32_t find_memory(uint32_t typeBits, VkMemoryPropertyFlags wanted);
bool create_buffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage);
void destroy_buffer(Buffer& b);
bool create_image(VkImage& image, VkDeviceMemory& memory, int w, int h, uint32_t mips, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits count);
VkImageView create_view(VkImage image, VkFormat format, VkImageAspectFlags aspectBits, uint32_t mips);
VkCommandBuffer begin_one_time_commands();
void end_one_time_commands();
VkAccessFlags access_for(VkImageLayout layout);
void transition(VkCommandBuffer c, VkImage image, VkImageAspectFlags aspectBits, uint32_t mips, VkImageLayout from, VkImageLayout to);
bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name);
VkSampler sampler_for(int flags);
void drop_sets_using(Handle tex);
void destroy_texture(Texture& t);
bool upload_pixels(Texture& t, const uint8_t* rgba);
Handle make_texture(int w, int h, const uint8_t* rgba, int flags, int owner);
VkDescriptorSet material_set(const Handle in[4]);
VkDescriptorSet current_material();
Sint64 file_time(const std::string& path);
void destroy_swapchain_views();
VkAttachmentDescription attachment(VkFormat format, VkSampleCountFlagBits count, bool clearIt, bool store, VkImageLayout finalLayout);
bool create_render_pass(int kind, VkRenderPass& out);
bool is_srgb(VkFormat f);
bool create_swapchain();
VkShaderModule load_shader(const std::string& path);
VkPipeline create_pipeline(const std::string& dir, const std::string& name, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack);
ShaderEntry* find_shader(const std::string& name);
VkPipeline pipeline_for(ShaderEntry* entry, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack);
int pass_kind();
void destroy_mesh(MeshRes& m);
Handle make_mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int owner);
void add_quad(std::vector<Vertex>& v, std::vector<uint32_t>& idx, const float p[4][3], const float n[3], const float uv[4][2]);
void build_meshes();
Vertex* frame_alloc(uint32_t count, uint32_t& first);
void fill_push(PushData& push, const float model[16], const float rgba[4], bool flat, bool noLight);
Vertex* flat_verts(uint32_t count, VkDescriptorSet material);
VkDescriptorSet flat_material(Handle first);
void set_vertex(Vertex& v, float x, float y, float z, float u, float w, const float rgba[4]);
void quad_2d(Handle tex, float x, float y, float w, float h, const float linear[4], float u0, float v0, float u1, float v1);
uint32_t next_codepoint(const std::string& s, size_t& i);
const Glyph* glyph_for(Font& f, uint32_t cp);
void upload_fonts();
void free_font(Handle h);
void reset_frame_state();
void shadow_matrix(const FrameState& s, float out[16]);
void build_frame(const FrameState& s, float timeSeconds, float width, float height, FrameData& f);
void run_draws(const std::vector<Draw>& list, bool shadow, VkPipeline& boundPipeline, VkDescriptorSet& boundSet);
void bind_frame(uint32_t slot);
void run_pass(Pass& pass);
void free_game_resources(bool everything);

}  // namespace internal
}  // namespace gfx
