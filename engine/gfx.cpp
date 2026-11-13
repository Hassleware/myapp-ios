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

#include "font8x8_basic.h"
#include "pool.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace gfx {
namespace {

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

SDL_Window* window = nullptr;
VkInstance instance = VK_NULL_HANDLE;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
uint32_t queueFamily = 0;
VkQueue queue = VK_NULL_HANDLE;
bool anisotropy = false;

VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkFormat colorFormat = VK_FORMAT_UNDEFINED;
VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
const VkFormat targetFormat = VK_FORMAT_R8G8B8A8_SRGB;
VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
VkExtent2D extent = {0, 0};
std::vector<VkImageView> colorViews;
std::vector<VkFramebuffer> framebuffers;
VkImage depthImage = VK_NULL_HANDLE, msaaImage = VK_NULL_HANDLE;
VkDeviceMemory depthMemory = VK_NULL_HANDLE, msaaMemory = VK_NULL_HANDLE;
VkImageView depthView = VK_NULL_HANDLE, msaaView = VK_NULL_HANDLE;
bool swapchainStale = false;

VkRenderPass screenPass = VK_NULL_HANDLE, targetPass = VK_NULL_HANDLE, shadowPass = VK_NULL_HANDLE;
VkCommandPool commandPool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE, onceCmd = VK_NULL_HANDLE;
VkSemaphore imageReady = VK_NULL_HANDLE;
std::vector<VkSemaphore> renderDone;   // one per swapchain image
VkFence frameFence = VK_NULL_HANDLE;

VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE, materialLayout = VK_NULL_HANDLE;
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet frameSet = VK_NULL_HANDLE;
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkSampler samplers[8] = {};
VkSampler shadowSampler = VK_NULL_HANDLE;

Buffer frameBuffer, dynBuffer;
VkDeviceSize frameStride = 0;
uint32_t dynCount = 0;

Pool<Texture> textures;
Pool<MeshRes> meshes;
Pool<Font> fonts;
std::map<std::array<Handle, 4>, VkDescriptorSet> materialSets;
std::map<std::string, ShaderEntry> shaders;
Texture shadowTex;
Handle whiteTex = 0, normalTex = 0, missingTex = 0;
Handle builtin[MESH_COUNT] = {};

std::string shaderDir, gameDir, shaderProblem;

// Per-frame state
uint32_t imageIndex = 0;
FrameState fs, savedScreen;
DrawState ds;
std::vector<Pass> passes;
size_t cur = 0;
VkDescriptorSet currentSet = VK_NULL_HANDLE;
std::vector<UiBox> uiBoxes;
float safeInsets[4] = {0, 0, 0, 0};
bool uiLogging = false;
Stats lastStats, nowStats;

// ---- small maths (column-major matrices, the same layout Vulkan and GLSL use) ----

void normalize3(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Builds a view matrix and hands back the camera's own axes.
void look_at(const float eye[3], const float target[3], const float upHint[3], float view[16], float s[3], float u[3], float f[3]) {
    f[0] = target[0] - eye[0]; f[1] = target[1] - eye[1]; f[2] = target[2] - eye[2];
    normalize3(f);
    cross3(f, upHint, s);
    if (dot3(s, s) < 1e-8f) {   // looking straight along the up direction
        const float other[3] = {0, 0, -1};
        cross3(f, other, s);
    }
    normalize3(s);
    cross3(s, f, u);
    mat_identity(view);
    view[0] = s[0]; view[4] = s[1]; view[8] = s[2];
    view[1] = u[0]; view[5] = u[1]; view[9] = u[2];
    view[2] = -f[0]; view[6] = -f[1]; view[10] = -f[2];
    view[12] = -dot3(s, eye); view[13] = -dot3(u, eye); view[14] = dot3(f, eye);
}

// Colours arrive the way people pick them (sRGB). Lighting is worked out in linear light,
// and the screen converts back. This is a close, cheap version of the exact curve.
float lin(float c) { return c <= 0 ? 0 : c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f); }

void lin4(const float in[4], float out[4]) {
    out[0] = lin(in[0]); out[1] = lin(in[1]); out[2] = lin(in[2]); out[3] = in[3];
}

// The part of the window the game is drawn in: all of it, unless a border has been asked for.
int bezel = 0;
int inset() { return (uint32_t)bezel * 4 < SDL_min(extent.width, extent.height) ? bezel : 0; }
float inner_width() { return (float)extent.width - 2 * inset(); }
float inner_height() { return (float)extent.height - 2 * inset(); }
float screen_aspect() { return extent.height ? inner_width() / inner_height() : 1.0f; }

float aspect() {
    if (cur < passes.size() && passes[cur].target)
        if (Texture* t = textures.get(passes[cur].target)) return (float)t->w / (float)t->h;
    return screen_aspect();
}

// ---- Vulkan helpers ----

bool fail(std::string& error, const char* what) {
    error = what;
    SDL_Log("gfx: %s", what);
    return false;
}

uint32_t find_memory(uint32_t typeBits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return 0;
}

bool create_buffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    return vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

void destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer();
}

bool create_image(VkImage& image, VkDeviceMemory& memory, int w, int h, uint32_t mips, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits count) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {(uint32_t)w, (uint32_t)h, 1};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.samples = count;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);
    return true;
}

VkImageView create_view(VkImage image, VkFormat format, VkImageAspectFlags aspectBits, uint32_t mips) {
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.image = image;
    info.format = format;
    info.subresourceRange.aspectMask = aspectBits;
    info.subresourceRange.levelCount = mips;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &info, nullptr, &view);
    return view;
}

// A short job for the GPU that is finished before the call returns: uploads and the like.
VkCommandBuffer once_begin() {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(onceCmd, 0);
    vkBeginCommandBuffer(onceCmd, &begin);
    return onceCmd;
}

void once_end() {
    vkEndCommandBuffer(onceCmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &onceCmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

VkAccessFlags access_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    default: return 0;
    }
}

void transition(VkCommandBuffer c, VkImage image, VkImageAspectFlags aspectBits, uint32_t mips, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspectBits, 0, mips, 0, 1};
    barrier.srcAccessMask = access_for(from);
    barrier.dstAccessMask = access_for(to);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VkSampler sampler_for(int flags) {
    int index = flags & 7;
    if (samplers[index]) return samplers[index];
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkFilter filter = (flags & TEX_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.magFilter = info.minFilter = filter;
    info.mipmapMode = (flags & TEX_NEAREST) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = info.addressModeV = info.addressModeW = (flags & TEX_CLAMP) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxLod = (flags & TEX_NOMIP) ? 0.0f : 16.0f;
    if (anisotropy && !(flags & (TEX_NEAREST | TEX_NOMIP))) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = 4.0f;
    }
    vkCreateSampler(device, &info, nullptr, &samplers[index]);
    return samplers[index];
}

// ---- textures ----

void drop_sets_using(Handle tex) {
    for (auto it = materialSets.begin(); it != materialSets.end();) {
        const auto& k = it->first;
        if (k[0] == tex || k[1] == tex || k[2] == tex || k[3] == tex) {
            vkFreeDescriptorSets(device, descriptorPool, 1, &it->second);
            it = materialSets.erase(it);
        } else {
            ++it;
        }
    }
    currentSet = VK_NULL_HANDLE;
}

void destroy_texture(Texture& t) {
    if (t.framebuffer) vkDestroyFramebuffer(device, t.framebuffer, nullptr);
    if (t.depthView) vkDestroyImageView(device, t.depthView, nullptr);
    if (t.depthImage) vkDestroyImage(device, t.depthImage, nullptr);
    if (t.depthMemory) vkFreeMemory(device, t.depthMemory, nullptr);
    if (t.view) vkDestroyImageView(device, t.view, nullptr);
    if (t.image) vkDestroyImage(device, t.image, nullptr);
    if (t.memory) vkFreeMemory(device, t.memory, nullptr);
    t = Texture();
}

// Copies pixels into a texture, making the smaller copies (mipmaps) on the way.
bool upload_pixels(Texture& t, const uint8_t* rgba) {
    std::vector<std::vector<uint8_t>> levels(t.mips);
    VkDeviceSize total = (VkDeviceSize)t.w * t.h * 4;
    int w = t.w, h = t.h;
    const uint8_t* src = rgba;
    for (uint32_t level = 1; level < t.mips; level++) {
        int nw = SDL_max(1, w / 2), nh = SDL_max(1, h / 2);
        levels[level].resize((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                int x0 = SDL_min(x * 2, w - 1), x1 = SDL_min(x * 2 + 1, w - 1), y0 = SDL_min(y * 2, h - 1), y1 = SDL_min(y * 2 + 1, h - 1);
                for (int c = 0; c < 4; c++)
                    levels[level][((size_t)y * nw + x) * 4 + c] = (uint8_t)((src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                                                                             src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c] + 2) / 4);
            }
        src = levels[level].data();
        w = nw; h = nh;
        total += (VkDeviceSize)nw * nh * 4;
    }

    Buffer staging;
    if (!create_buffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    w = t.w; h = t.h;
    for (uint32_t level = 0; level < t.mips; level++) {
        size_t bytes = (size_t)w * h * 4;
        memcpy((uint8_t*)staging.mapped + offset, level == 0 ? rgba : levels[level].data(), bytes);
        VkBufferImageCopy region = {};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        regions.push_back(region);
        offset += bytes;
        w = SDL_max(1, w / 2); h = SDL_max(1, h / 2);
    }
    VkCommandBuffer c = once_begin();
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(c, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    once_end();
    destroy_buffer(staging);
    return true;
}

Handle make_texture(int w, int h, const uint8_t* rgba, int flags, int owner) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !rgba) return 0;
    Texture t;
    t.w = w; t.h = h; t.flags = flags; t.owner = owner;
    if (!(flags & TEX_NOMIP))
        for (int size = SDL_max(w, h); size > 1; size /= 2) t.mips++;
    VkFormat format = (flags & TEX_DATA) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
    if (!create_image(t.image, t.memory, w, h, t.mips, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT)) {
        destroy_texture(t);
        return 0;
    }
    t.view = create_view(t.image, format, VK_IMAGE_ASPECT_COLOR_BIT, t.mips);
    t.sampler = sampler_for(flags);
    if (!upload_pixels(t, rgba)) {
        destroy_texture(t);
        return 0;
    }
    return textures.add(t);
}

VkDescriptorSet material_set(const Handle in[4]) {
    std::array<Handle, 4> key;
    Texture* tx[4];
    for (int i = 0; i < 4; i++) {
        key[i] = in[i];
        tx[i] = textures.get(in[i]);
        if (!tx[i]) {
            key[i] = in[i] ? missingTex : (i == 1 ? normalTex : whiteTex);
            tx[i] = textures.get(key[i]);
        }
    }
    auto found = materialSets.find(key);
    if (found != materialSets.end()) return found->second;

    VkDescriptorSetAllocateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = descriptorPool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &materialLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &info, &set) != VK_SUCCESS) return materialSets.empty() ? VK_NULL_HANDLE : materialSets.begin()->second;
    VkDescriptorImageInfo images[4];
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; i++) {
        images[i] = {tx[i]->sampler, tx[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    materialSets[key] = set;
    return set;
}

VkDescriptorSet current_material() {
    if (!currentSet) currentSet = material_set(ds.tex);
    return currentSet;
}

Sint64 file_time(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? (Sint64)info.modify_time : 0;
}

// ---- render passes and the swapchain ----

void destroy_swapchain_views() {
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (auto view : colorViews) vkDestroyImageView(device, view, nullptr);
    for (auto done : renderDone) vkDestroySemaphore(device, done, nullptr);
    renderDone.clear();
    framebuffers.clear();
    colorViews.clear();
    if (depthView) vkDestroyImageView(device, depthView, nullptr);
    if (depthImage) vkDestroyImage(device, depthImage, nullptr);
    if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);
    if (msaaView) vkDestroyImageView(device, msaaView, nullptr);
    if (msaaImage) vkDestroyImage(device, msaaImage, nullptr);
    if (msaaMemory) vkFreeMemory(device, msaaMemory, nullptr);
    depthView = msaaView = VK_NULL_HANDLE;
    depthImage = msaaImage = VK_NULL_HANDLE;
    depthMemory = msaaMemory = VK_NULL_HANDLE;
}

VkAttachmentDescription attachment(VkFormat format, VkSampleCountFlagBits count, bool clearIt, bool store, VkImageLayout finalLayout) {
    VkAttachmentDescription a = {};
    a.format = format;
    a.samples = count;
    a.loadOp = clearIt ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = finalLayout;
    return a;
}

// kind: the screen (smoothed edges, resolved into the window), a texture target, or the shadow map (depth only).
bool create_render_pass(int kind, VkRenderPass& out) {
    VkAttachmentDescription attachments[3];
    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t count = 2;
    if (kind == KIND_SCREEN) {
        bool multi = samples != VK_SAMPLE_COUNT_1_BIT;
        attachments[0] = attachment(colorFormat, samples, true, !multi, multi ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        attachments[1] = attachment(depthFormat, samples, true, false, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        attachments[2] = attachment(colorFormat, VK_SAMPLE_COUNT_1_BIT, false, true, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        if (multi) {
            subpass.pResolveAttachments = &resolveRef;
            count = 3;
        }
    } else if (kind == KIND_TARGET) {
        attachments[0] = attachment(targetFormat, VK_SAMPLE_COUNT_1_BIT, true, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        attachments[1] = attachment(depthFormat, VK_SAMPLE_COUNT_1_BIT, true, false, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
    } else {
        attachments[0] = attachment(depthFormat, VK_SAMPLE_COUNT_1_BIT, true, true, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        depthRef.attachment = 0;
        subpass.pDepthStencilAttachment = &depthRef;
        count = 1;
    }

    // What was drawn into a texture is read by later passes, so each pass waits for the one before.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = deps[0].srcStageMask;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = deps[0].srcStageMask;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = deps[0].dstAccessMask;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    return vkCreateRenderPass(device, &info, nullptr, &out) == VK_SUCCESS;
}

bool is_srgb(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB; }

// Builds (or rebuilds) the swapchain to match the window. Leaves extent at 0x0 if the window is hidden.
bool create_swapchain() {
    vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) return false;

    VkExtent2D size = caps.currentExtent;
    if (size.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        size.width = SDL_clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        size.height = SDL_clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (size.width == 0 || size.height == 0) {
        extent = {0, 0};
        return true;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());
    if (formats.empty()) return false;
    // An sRGB format makes the screen do the conversion back from linear light.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
    for (const auto& f : formats)
        if (is_srgb(f.format)) { chosen = f; break; }
    if (colorFormat != VK_FORMAT_UNDEFINED)
        for (const auto& f : formats)
            if (f.format == colorFormat) { chosen = f; break; }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaChoices[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (auto choice : alphaChoices)
        if (caps.supportedCompositeAlpha & choice) { alpha = choice; break; }

    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = size;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
#if defined(GFX_SCREENSHOT)
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &info, nullptr, &created) != VK_SUCCESS) return false;
    destroy_swapchain_views();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = created;
    extent = size;
    colorFormat = chosen.format;

    if (!screenPass && !create_render_pass(KIND_SCREEN, screenPass)) return false;

    if (!create_image(depthImage, depthMemory, (int)extent.width, (int)extent.height, 1, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, samples)) return false;
    depthView = create_view(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    bool multi = samples != VK_SAMPLE_COUNT_1_BIT;
    if (multi) {
        if (!create_image(msaaImage, msaaMemory, (int)extent.width, (int)extent.height, 1, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples)) return false;
        msaaView = create_view(msaaImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    for (VkImage image : images) {
        VkImageView view = create_view(image, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        if (!view) return false;
        colorViews.push_back(view);

        VkImageView attachments[3] = {multi ? msaaView : view, depthView, view};
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = screenPass;
        fbInfo.attachmentCount = multi ? 3 : 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fb) != VK_SUCCESS) return false;
        framebuffers.push_back(fb);
        VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore done = VK_NULL_HANDLE;
        vkCreateSemaphore(device, &semInfo, nullptr, &done);
        renderDone.push_back(done);
    }
    swapchainStale = false;
    return true;
}

// ---- shaders and pipelines ----

VkShaderModule load_shader(const std::string& path) {
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data || size == 0 || size % 4 != 0) {
        SDL_free(data);
        shaderProblem = "Missing compiled shader: " + path;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = (const uint32_t*)data;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        shaderProblem = "The GPU rejected shader: " + path;
        module = VK_NULL_HANDLE;
    }
    SDL_free(data);
    return module;
}

// Shaders are stored as <dir>/<name>.vert.spv and <dir>/<name>.frag.spv.
VkPipeline create_pipeline(const std::string& dir, const std::string& name, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack) {
    VkShaderModule vert = load_shader(dir + "/" + name + ".vert.spv");
    VkShaderModule frag = load_shader(dir + "/" + name + ".frag.spv");
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        // Meshes wind their triangles counter-clockwise seen from outside, as glTF does.
        VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = (cullBack && kind != KIND_SHADOW) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        if (kind == KIND_SHADOW) {
            raster.depthBiasEnable = VK_TRUE;
            raster.depthBiasConstantFactor = 1.25f;
            raster.depthBiasSlopeFactor = 1.75f;
        }

        VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = kind == KIND_SCREEN ? samples : VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthState = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
        depthState.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment = {};
        blendAttachment.blendEnable = blendMode != BLEND_OPAQUE ? VK_TRUE : VK_FALSE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = blendMode == BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = blendMode == BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blendState = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendState.attachmentCount = kind == KIND_SHADOW ? 0 : 1;
        blendState.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthState;
        info.pColorBlendState = &blendState;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        info.renderPass = kind == KIND_SCREEN ? screenPass : kind == KIND_TARGET ? targetPass : shadowPass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            shaderProblem = "Couldn't build a pipeline from shader '" + name + "'";
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
    return pipeline;
}

// A game's own shader wins over an engine shader of the same name.
ShaderEntry* find_shader(const std::string& name) {
    auto found = shaders.find(name);
    if (found != shaders.end()) return &found->second;
    ShaderEntry entry;
    entry.name = name;
    entry.dir = shaderDir;
    if (!gameDir.empty() && SDL_GetPathInfo((gameDir + "/shaders/" + name + ".vert.spv").c_str(), nullptr)) entry.dir = gameDir + "/shaders";
    return &shaders.emplace(name, entry).first->second;
}

VkPipeline pipeline_for(ShaderEntry* entry, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack) {
    ShaderEntry* standard = find_shader("mesh");
    if (!entry || entry->broken) entry = standard;
    uint32_t variant = (uint32_t)kind | ((uint32_t)blendMode << 2) | (depthTest ? 16u : 0u) | (depthWrite ? 32u : 0u) | (cullBack ? 64u : 0u);
    auto found = entry->variants.find(variant);
    if (found != entry->variants.end()) return found->second;
    VkPipeline created = create_pipeline(entry->dir, entry->name, kind, blendMode, depthTest, depthWrite, cullBack);
    if (!created && entry != standard) {
        // Keep drawing with the standard shader. The problem is shown on screen.
        entry->broken = true;
        return pipeline_for(standard, kind, blendMode, depthTest, depthWrite, cullBack);
    }
    entry->variants[variant] = created;
    return created;
}

int pass_kind() { return passes[cur].target ? KIND_TARGET : KIND_SCREEN; }

// ---- meshes ----

void destroy_mesh(MeshRes& m) {
    destroy_buffer(m.vb);
    destroy_buffer(m.ib);
    m = MeshRes();
}

Handle make_mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, int owner) {
    if (vertices.empty() || indices.empty()) return 0;
    for (uint32_t i : indices)
        if (i >= vertices.size()) return 0;
    MeshRes m;
    m.owner = owner;
    if (!create_buffer(m.vb, vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !create_buffer(m.ib, indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        destroy_mesh(m);
        return 0;
    }
    memcpy(m.vb.mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    memcpy(m.ib.mapped, indices.data(), indices.size() * sizeof(uint32_t));
    m.data.vertices = vertices;
    m.data.indices = indices;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : vertices)
        for (int a = 0; a < 3; a++) {
            lo[a] = SDL_min(lo[a], v.pos[a]);
            hi[a] = SDL_max(hi[a], v.pos[a]);
        }
    for (int a = 0; a < 3; a++) m.data.center[a] = (lo[a] + hi[a]) / 2;
    for (const Vertex& v : vertices) {
        float d[3] = {v.pos[0] - m.data.center[0], v.pos[1] - m.data.center[1], v.pos[2] - m.data.center[2]};
        m.data.radius = SDL_max(m.data.radius, sqrtf(dot3(d, d)));
    }
    return meshes.add(std::move(m));
}

// Adds a four-cornered face, turned so that it faces along n.
void add_quad(std::vector<Vertex>& v, std::vector<uint32_t>& idx, const float p[4][3], const float n[3], const float uv[4][2]) {
    float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]}, c[3];
    cross3(e1, e2, c);
    bool flip = dot3(c, n) < 0;
    uint32_t base = (uint32_t)v.size();
    for (int i = 0; i < 4; i++) {
        Vertex vert = {{p[i][0], p[i][1], p[i][2]}, {n[0], n[1], n[2]}, {1, 1, 1, 1}, {uv[i][0], uv[i][1]}, {0, 0, 0, 0}};
        v.push_back(vert);
    }
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3}, flipped[6] = {0, 2, 1, 0, 3, 2};
    for (int i = 0; i < 6; i++) idx.push_back(base + (flip ? flipped[i] : order[i]));
}

void build_meshes() {
    const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    {   // Cube, one unit wide, centred on the origin.
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        for (int axis = 0; axis < 3; axis++)
            for (int sign = -1; sign <= 1; sign += 2) {
                int u = (axis + 1) % 3, w = (axis + 2) % 3;
                float n[3] = {0, 0, 0}, p[4][3];
                n[axis] = (float)sign;
                const float su[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sw[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
                for (int i = 0; i < 4; i++) {
                    p[i][axis] = 0.5f * sign;
                    p[i][u] = su[i];
                    p[i][w] = sw[i];
                }
                add_quad(v, idx, p, n, uv);
            }
        finish_vertices(v, idx, false);
        builtin[MESH_CUBE] = make_mesh(v, idx, OWNER_ENGINE);
    }
    {   // Sphere, one unit across. Its texture wraps once around and runs top to bottom.
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        const int rings = 16, segments = 28;
        const float pi = 3.14159265f;
        for (int ring = 0; ring <= rings; ring++)
            for (int seg = 0; seg <= segments; seg++) {
                float phi = pi * ring / rings, theta = 2 * pi * seg / segments;
                float x = sinf(phi) * cosf(theta), y = cosf(phi), z = sinf(phi) * sinf(theta);
                Vertex vert = {{x * 0.5f, y * 0.5f, z * 0.5f}, {x, y, z}, {1, 1, 1, 1}, {1.0f - (float)seg / segments, (float)ring / rings}, {0, 0, 0, 0}};
                v.push_back(vert);
            }
        for (int ring = 0; ring < rings; ring++)
            for (int seg = 0; seg < segments; seg++) {
                uint32_t a = (uint32_t)(ring * (segments + 1) + seg), b = a + (uint32_t)segments + 1;
                const uint32_t tri[6] = {a, a + 1, b, a + 1, b + 1, b};
                idx.insert(idx.end(), tri, tri + 6);
            }
        finish_vertices(v, idx, false);
        builtin[MESH_SPHERE] = make_mesh(v, idx, OWNER_ENGINE);
    }
    {   // Flat square on the ground, one unit wide, facing up.
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        const float p[4][3] = {{-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f}, {-0.5f, 0, -0.5f}}, up[3] = {0, 1, 0};
        add_quad(v, idx, p, up, uv);
        finish_vertices(v, idx, false);
        builtin[MESH_PLANE] = make_mesh(v, idx, OWNER_ENGINE);
    }
}

// ---- this frame's vertices: 2D shapes, text, particles, lines, skinned meshes ----

Vertex* dyn_alloc(uint32_t count, uint32_t& first) {
    if (dynCount + count > MAX_DYNAMIC_VERTS) return nullptr;
    first = dynCount;
    dynCount += count;
    return (Vertex*)dynBuffer.mapped + first;
}

void fill_push(PushData& push, const float model[16], const float rgba[4], bool flat, bool noLight) {
    memcpy(push.model, model, sizeof(float) * 16);
    lin4(rgba, push.color);
    push.flags[0] = flat ? 1.0f : 0.0f;
    push.flags[1] = noLight ? 1.0f : 0.0f;
    push.flags[2] = ds.cutoff;
    push.flags[3] = 0;
    memcpy(push.params, ds.params, sizeof(push.params));
}

// Starts or extends the run of 2D triangles that share a texture and shader.
Vertex* flat_verts(uint32_t count, VkDescriptorSet material) {
    Pass& pass = passes[cur];
    VkPipeline pipeline = pipeline_for(ds.shader, pass_kind(), ds.blend == BLEND_ADD ? BLEND_ADD : BLEND_ALPHA, false, false, false);
    uint32_t first = 0;
    Vertex* out = dyn_alloc(count, first);
    if (!out || !pipeline || !material) return nullptr;
    if (!pass.flat.empty()) {
        Draw& last = pass.flat.back();
        if (last.pipeline == pipeline && last.material == material && last.first + last.count == first &&
            memcmp(last.push.params, ds.params, sizeof(ds.params)) == 0) {
            last.count += count;
            return out;
        }
    }
    Draw d;
    d.pipeline = pipeline;
    d.material = material;
    d.vb = dynBuffer.buffer;
    d.first = first;
    d.count = count;
    float identity[16];
    const float white[4] = {1, 1, 1, 1};
    mat_identity(identity);
    fill_push(d.push, identity, white, true, true);
    d.push.flags[2] = 0;
    pass.flat.push_back(d);
    return out;
}

VkDescriptorSet flat_material(Handle first) {
    Handle t[4] = {first, ds.tex[1], ds.tex[2], ds.tex[3]};
    return material_set(t);
}

void set_vertex(Vertex& v, float x, float y, float z, float u, float w, const float rgba[4]) {
    v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
    v.normal[0] = 0; v.normal[1] = 0; v.normal[2] = 1;
    memcpy(v.color, rgba, sizeof(float) * 4);
    v.uv[0] = u; v.uv[1] = w;
    v.tangent[0] = v.tangent[1] = v.tangent[2] = v.tangent[3] = 0;
}

void quad_2d(Handle tex, float x, float y, float w, float h, const float linear[4], float u0, float v0, float u1, float v1) {
    Vertex* v = flat_verts(6, flat_material(tex));
    if (!v) return;
    set_vertex(v[0], x, y, 0, u0, v0, linear);
    set_vertex(v[1], x + w, y, 0, u1, v0, linear);
    set_vertex(v[2], x + w, y + h, 0, u1, v1, linear);
    set_vertex(v[3], x, y, 0, u0, v0, linear);
    set_vertex(v[4], x + w, y + h, 0, u1, v1, linear);
    set_vertex(v[5], x, y + h, 0, u0, v1, linear);
}

// ---- fonts ----

uint32_t next_codepoint(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

// Finds a letter in the font's atlas, drawing it in the first time it is needed.
const Glyph* glyph_for(Font& f, uint32_t cp) {
    auto found = f.glyphs.find(cp);
    if (found != f.glyphs.end()) return &found->second;
    Glyph g = {};
    int advance = 0, bearing = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointHMetrics(&f.info, (int)cp, &advance, &bearing);
    stbtt_GetCodepointBitmapBox(&f.info, (int)cp, f.scale, f.scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    g.advance = advance * f.scale;
    if (w > 0 && h > 0 && !f.full) {
        if (f.penX + w + 1 > ATLAS_SIZE) {
            f.penX = 1;
            f.penY += f.rowH + 1;
            f.rowH = 0;
        }
        if (f.penY + h + 1 > ATLAS_SIZE) {
            f.full = true;
            SDL_Log("gfx: the font atlas is full, so some letters will be missing");
        } else {
            stbtt_MakeCodepointBitmap(&f.info, f.alpha.data() + (size_t)f.penY * ATLAS_SIZE + f.penX, w, h, ATLAS_SIZE, f.scale, f.scale, (int)cp);
            g.u0 = (float)f.penX / ATLAS_SIZE; g.v0 = (float)f.penY / ATLAS_SIZE;
            g.u1 = (float)(f.penX + w) / ATLAS_SIZE; g.v1 = (float)(f.penY + h) / ATLAS_SIZE;
            g.x0 = (float)x0; g.y0 = (float)y0; g.w = (float)w; g.h = (float)h;
            f.penX += w + 1;
            f.rowH = SDL_max(f.rowH, h);
            f.dirty = true;
        }
    }
    return &f.glyphs.emplace(cp, g).first->second;
}

void upload_fonts() {
    fonts.each([](Handle, Font& f) {
        Texture* atlas = textures.get(f.atlas);
        if (!f.dirty || !atlas) return;
        std::vector<uint8_t> rgba((size_t)ATLAS_SIZE * ATLAS_SIZE * 4, 255);
        for (size_t i = 0; i < f.alpha.size(); i++) rgba[i * 4 + 3] = f.alpha[i];
        upload_pixels(*atlas, rgba.data());
        f.dirty = false;
    });
}

void free_font(Handle h) {
    Font* f = fonts.get(h);
    if (!f) return;
    free_texture(f->atlas);
    fonts.remove(h);
}

// ---- the frame ----

void reset_frame_state() {
    fs = FrameState();
    const float eye[3] = {0, 3, 8}, target[3] = {0, 0, 0};
    fs.clear[0] = 0.06f; fs.clear[1] = 0.07f; fs.clear[2] = 0.10f;
    fs.sunDir[0] = -0.4f; fs.sunDir[1] = -1.0f; fs.sunDir[2] = -0.3f;
    fs.sunColor[0] = fs.sunColor[1] = fs.sunColor[2] = 0.65f;
    fs.ambient[0] = fs.ambient[1] = fs.ambient[2] = 0.35f;
    memset(fs.fogColor, 0, sizeof(fs.fogColor));
    memset(fs.user, 0, sizeof(fs.user));
    fs.fogRange[0] = fs.fogRange[1] = 0;
    fs.shadowCenter[0] = fs.shadowCenter[1] = fs.shadowCenter[2] = 0;
    camera(eye, target, 60.0f);
}

void shadow_matrix(const FrameState& s, float out[16]) {
    float dir[3] = {s.sunDir[0], s.sunDir[1], s.sunDir[2]};
    normalize3(dir);
    float r = SDL_max(s.shadowRadius, 0.01f);
    float eye[3] = {s.shadowCenter[0] - dir[0] * r * 2, s.shadowCenter[1] - dir[1] * r * 2, s.shadowCenter[2] - dir[2] * r * 2};
    const float up[3] = {0, 1, 0};
    float view[16], a[3], b[3], c[3], proj[16];
    look_at(eye, s.shadowCenter, up, view, a, b, c);
    mat_identity(proj);
    proj[0] = 1.0f / r;
    proj[5] = -1.0f / r;
    proj[10] = -1.0f / (4 * r);
    mat_multiply(proj, view, out);
}

void build_frame(const FrameState& s, float timeSeconds, float width, float height, FrameData& f) {
    memset(&f, 0, sizeof(f));
    memcpy(f.viewProj, s.viewProj, sizeof(f.viewProj));
    mat_identity(f.ortho);
    f.ortho[0] = 2.0f / view_width();
    f.ortho[5] = 2.0f / view_height();
    f.ortho[12] = -1.0f;
    f.ortho[13] = -1.0f;
    shadow_matrix(s, f.shadowMat);
    memcpy(f.lightDir, s.sunDir, sizeof(float) * 3);
    f.lightDir[3] = s.shadowStrength;
    memcpy(f.lightColor, s.sunColor, sizeof(float) * 3);
    memcpy(f.ambient, s.ambient, sizeof(float) * 3);
    memcpy(f.cameraPos, s.eye, sizeof(float) * 3);
    memcpy(f.fogColor, s.fogColor, sizeof(f.fogColor));
    f.fogRange[0] = s.fogRange[0];
    f.fogRange[1] = s.fogRange[1];
    f.shadowInfo[0] = 1.0f / SHADOW_SIZE;
    f.shadowInfo[1] = 0.0015f;
    f.shadowInfo[2] = s.shadowRadius * 3.0f / SHADOW_SIZE;
    f.params[0] = timeSeconds;
    f.params[1] = width;
    f.params[2] = height;
    f.params[3] = (float)s.lightCount;
    memcpy(f.user, s.user, sizeof(f.user));
    memcpy(f.lightPos, s.lightPos, sizeof(f.lightPos));
    memcpy(f.lightCol, s.lightCol, sizeof(f.lightCol));
    memcpy(f.lightAim, s.lightAim, sizeof(f.lightAim));
}

void run_draws(const std::vector<Draw>& list, bool shadow, VkPipeline& boundPipeline, VkDescriptorSet& boundSet) {
    const VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkBuffer boundVb = VK_NULL_HANDLE, boundIb = VK_NULL_HANDLE;
    for (const Draw& d : list) {
        VkPipeline pipeline = shadow ? d.shadowPipeline : d.pipeline;
        if (!pipeline || !d.material) continue;
        if (pipeline != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            boundPipeline = pipeline;
        }
        if (d.material != boundSet) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &d.material, 0, nullptr);
            boundSet = d.material;
        }
        if (d.vb != boundVb) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, &offset);
            boundVb = d.vb;
        }
        vkCmdPushConstants(cmd, pipelineLayout, pushStages, 0, sizeof(PushData), &d.push);
        if (d.ib) {
            if (d.ib != boundIb) {
                vkCmdBindIndexBuffer(cmd, d.ib, 0, VK_INDEX_TYPE_UINT32);
                boundIb = d.ib;
            }
            vkCmdDrawIndexed(cmd, d.count, 1, d.first, d.vertexOffset, 0);
        } else {
            vkCmdDraw(cmd, d.count, 1, d.first, 0);
        }
        nowStats.draws++;
        nowStats.vertices += (int)d.count;
    }
}

void bind_frame(uint32_t slot) {
    uint32_t offset = (uint32_t)(frameStride * slot);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &frameSet, 1, &offset);
}

void run_pass(Pass& pass) {
    std::stable_sort(pass.transparent.begin(), pass.transparent.end(), [](const Draw& a, const Draw& b) { return a.depth > b.depth; });
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    run_draws(pass.sky, false, boundPipeline, boundSet);
    run_draws(pass.opaque, false, boundPipeline, boundSet);
    run_draws(pass.transparent, false, boundPipeline, boundSet);
    run_draws(pass.flat, false, boundPipeline, boundSet);
}

void free_game_resources(bool everything) {
    std::vector<Handle> gone;
    fonts.each([&](Handle h, Font& f) {
        if (everything || file_time(f.key) != f.mtime) gone.push_back(h);
    });
    for (Handle h : gone) free_font(h);
    gone.clear();
    textures.each([&](Handle h, Texture& t) {
        if (t.owner == OWNER_ENGINE) return;
        if (everything || t.owner == OWNER_SCRIPT || (!t.key.empty() && file_time(t.key) != t.mtime)) gone.push_back(h);
    });
    for (Handle h : gone) free_texture(h);
    gone.clear();
    meshes.each([&](Handle h, MeshRes& m) {
        if (m.owner != OWNER_ENGINE && (everything || m.owner == OWNER_SCRIPT)) gone.push_back(h);
    });
    for (Handle h : gone) free_mesh(h);
}

}  // namespace

// ---- public API ----

void mat_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat_multiply(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

void mat_from_trs(const float p[3], const float q[4], const float s[3], float m[16]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (1 - 2 * (y * y + z * z)) * s[0]; m[1] = (2 * (x * y + z * w)) * s[0]; m[2] = (2 * (x * z - y * w)) * s[0]; m[3] = 0;
    m[4] = (2 * (x * y - z * w)) * s[1]; m[5] = (1 - 2 * (x * x + z * z)) * s[1]; m[6] = (2 * (y * z + x * w)) * s[1]; m[7] = 0;
    m[8] = (2 * (x * z + y * w)) * s[2]; m[9] = (2 * (y * z - x * w)) * s[2]; m[10] = (1 - 2 * (x * x + y * y)) * s[2]; m[11] = 0;
    m[12] = p[0]; m[13] = p[1]; m[14] = p[2]; m[15] = 1;
}

bool init(SDL_Window* win, const std::string& engineShaderDir, std::string& error) {
    window = win;
    shaderDir = engineShaderDir;

    if (!SDL_Vulkan_LoadLibrary(nullptr)) return fail(error, "This device has no Vulkan driver.");
#if defined(USE_VOLK)
    volkInitializeCustom((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr());
#else
    // MoltenVK is linked into the app. SDL finds it by this name, so make sure the linker keeps it.
    static volatile PFN_vkGetInstanceProcAddr keepLinked = &vkGetInstanceProcAddr;
    (void)keepLinked;
#endif

    // Instance
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    Uint32 sdlCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlCount);
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(available, "VK_KHR_portability_enumeration")) {
        extensions.push_back("VK_KHR_portability_enumeration");
        instanceInfo.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gamelab";
    app.apiVersion = VK_API_VERSION_1_2;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return fail(error, "Couldn't start Vulkan (version 1.2 is needed).");
#if defined(USE_VOLK)
    volkLoadInstance(instance);
#endif

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) return fail(error, SDL_GetError());

    // Pick a GPU that can draw to the window, preferring a dedicated one.
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    int bestScore = -1;
    for (VkPhysicalDevice candidate : gpus) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; i++) {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &canPresent);
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !canPresent) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(candidate, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > bestScore) {
                bestScore = score;
                gpu = candidate;
                queueFamily = i;
            }
            break;
        }
    }
    if (!gpu) return fail(error, "No graphics card here can draw to the window with Vulkan.");

    // Device
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceAvailable(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, deviceAvailable.data());
    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (has_extension(deviceAvailable, "VK_KHR_portability_subset")) deviceExtensions.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceFeatures supported, wanted = {};
    vkGetPhysicalDeviceFeatures(gpu, &supported);
    anisotropy = supported.samplerAnisotropy == VK_TRUE;
    wanted.samplerAnisotropy = supported.samplerAnisotropy;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &wanted;
    if (vkCreateDevice(gpu, &deviceInfo, nullptr, &device) != VK_SUCCESS) return fail(error, "Couldn't open the graphics card.");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkFormatProperties depthProps;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depthProps);
    depthFormat = (depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;

    // Smooth edges (4x MSAA) where the GPU offers it, which every iPhone does.
    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(gpu, &gpuProps);
    const char* noSmooth = SDL_getenv("GAMELAB_NO_MSAA");
    if (!noSmooth && (gpuProps.limits.framebufferColorSampleCounts & gpuProps.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT)) samples = VK_SAMPLE_COUNT_4_BIT;

    if (!create_swapchain() || !screenPass) return fail(error, "Couldn't set up drawing to the window.");
    if (!create_render_pass(KIND_TARGET, targetPass) || !create_render_pass(KIND_SHADOW, shadowPass)) return fail(error, "Couldn't set up drawing to textures.");

    // Commands and frame pacing
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 2;
    VkCommandBuffer both[2];
    vkAllocateCommandBuffers(device, &cmdInfo, both);
    cmd = both[0];
    onceCmd = both[1];
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device, &semInfo, nullptr, &imageReady);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fenceInfo, nullptr, &frameFence);

    // Set 0: the per-frame data block (camera, lights, time) and the shadow map. Set 1: a material's four textures.
    const VkShaderStageFlags bothStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding frameBindings[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, bothStages, nullptr},
                                                     {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = frameBindings;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &frameLayout);
    VkDescriptorSetLayoutBinding materialBindings[4];
    for (uint32_t i = 0; i < 4; i++) materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, bothStages, nullptr};
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = materialBindings;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &materialLayout);

    const uint32_t maxMaterials = 2048;
    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxMaterials * 4 + 1}};
    VkDescriptorPoolCreateInfo descPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = maxMaterials + 1;
    descPoolInfo.poolSizeCount = 2;
    descPoolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(device, &descPoolInfo, nullptr, &descriptorPool);
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &frameLayout;
    vkAllocateDescriptorSets(device, &setInfo, &frameSet);

    VkDeviceSize align = SDL_max(gpuProps.limits.minUniformBufferOffsetAlignment, (VkDeviceSize)16);
    frameStride = (sizeof(FrameData) + align - 1) / align * align;
    if (!create_buffer(frameBuffer, frameStride * FRAME_SLOTS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer(dynBuffer, (VkDeviceSize)MAX_DYNAMIC_VERTS * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return fail(error, "Out of graphics memory.");

    // The shadow map: a depth picture of the scene as the sun sees it.
    shadowTex.w = shadowTex.h = SHADOW_SIZE;
    if (!create_image(shadowTex.image, shadowTex.memory, SHADOW_SIZE, SHADOW_SIZE, 1, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT))
        return fail(error, "Out of graphics memory.");
    shadowTex.view = create_view(shadowTex.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = shadowPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &shadowTex.view;
    fbInfo.width = fbInfo.height = SHADOW_SIZE;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &shadowTex.framebuffer);
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &samplerInfo, nullptr, &shadowSampler);
    VkCommandBuffer c = once_begin();
    transition(c, shadowTex.image, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    once_end();

    VkDescriptorBufferInfo bufferInfo = {frameBuffer.buffer, 0, sizeof(FrameData)};
    VkDescriptorImageInfo shadowInfo = {shadowSampler, shadowTex.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = writes[1].dstSet = frameSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &shadowInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkPushConstantRange pushRange = {bothStages, 0, sizeof(PushData)};
    VkDescriptorSetLayout setLayouts[2] = {frameLayout, materialLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

    // Stand-ins: plain white where no texture is set, a flat normal map, and a loud checker for a missing file.
    const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    uint8_t checker[8 * 8 * 4];
    for (int i = 0; i < 64; i++) {
        bool on = ((i % 8) / 4 + (i / 8) / 4) % 2 == 0;
        const uint8_t pixel[4] = {(uint8_t)(on ? 255 : 0), 0, (uint8_t)(on ? 255 : 0), 255};
        memcpy(checker + i * 4, pixel, 4);
    }
    whiteTex = make_texture(1, 1, white, TEX_NOMIP, OWNER_ENGINE);
    normalTex = make_texture(1, 1, flat, TEX_NOMIP | TEX_DATA, OWNER_ENGINE);
    missingTex = make_texture(8, 8, checker, TEX_NOMIP | TEX_NEAREST, OWNER_ENGINE);
    build_meshes();
    if (!whiteTex || !normalTex || !missingTex || !builtin[MESH_CUBE]) return fail(error, "Out of graphics memory.");

    passes.resize(1);
    ShaderEntry* standard = find_shader("mesh");
    if (!pipeline_for(standard, KIND_SCREEN, BLEND_OPAQUE, true, true, true) || !pipeline_for(standard, KIND_SCREEN, BLEND_ALPHA, false, false, false))
        return fail(error, shaderProblem.c_str());
    shaderProblem.clear();
    return true;
}

void shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    set_game_dir("");
    for (auto& entry : shaders)
        for (auto& variant : entry.second.variants)
            if (variant.second) vkDestroyPipeline(device, variant.second, nullptr);
    shaders.clear();
    textures.each([](Handle, Texture& t) { destroy_texture(t); });
    meshes.each([](Handle, MeshRes& m) { destroy_mesh(m); });
    destroy_texture(shadowTex);
    for (VkSampler s : samplers)
        if (s) vkDestroySampler(device, s, nullptr);
    if (shadowSampler) vkDestroySampler(device, shadowSampler, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    destroy_buffer(frameBuffer);
    destroy_buffer(dynBuffer);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (frameLayout) vkDestroyDescriptorSetLayout(device, frameLayout, nullptr);
    if (materialLayout) vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
    if (frameFence) vkDestroyFence(device, frameFence, nullptr);
    if (imageReady) vkDestroySemaphore(device, imageReady, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    destroy_swapchain_views();
    if (screenPass) vkDestroyRenderPass(device, screenPass, nullptr);
    if (targetPass) vkDestroyRenderPass(device, targetPass, nullptr);
    if (shadowPass) vkDestroyRenderPass(device, shadowPass, nullptr);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    if (surface) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}

void set_game_dir(const std::string& dir) {
    if (!device) return;
    vkDeviceWaitIdle(device);
    // Shaders may have been recompiled, so the game's are dropped and built again when next used.
    for (auto it = shaders.begin(); it != shaders.end();) {
        if (it->second.dir == shaderDir) {
            ++it;
            continue;
        }
        for (auto& variant : it->second.variants)
            if (variant.second) vkDestroyPipeline(device, variant.second, nullptr);
        it = shaders.erase(it);
    }
    free_game_resources(dir != gameDir);
    gameDir = dir;
    shaderProblem.clear();
    for (Pass& pass : passes) {
        pass.sky.clear(); pass.opaque.clear(); pass.transparent.clear(); pass.flat.clear();
    }
    reset_state();
}

std::string game_path(const std::string& name) {
    if (gameDir.empty() || name.empty() || name.find("..") != std::string::npos || name[0] == '/' || name.find(':') != std::string::npos) return "";
    return gameDir + "/" + name;
}

bool begin_frame() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (swapchainStale || extent.width == 0 || (uint32_t)w != extent.width || (uint32_t)h != extent.height) {
        if (!create_swapchain() || extent.width == 0) return false;
    }
    vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageReady, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainStale = true;
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return false;

    passes.resize(1);
    passes[0] = Pass();
    cur = 0;
    dynCount = 0;
    uiBoxes.clear();
    lastStats = nowStats;
    nowStats = Stats();
    textures.each([](Handle, Texture&) { lastStats.textures++; });
    meshes.each([](Handle, MeshRes&) { lastStats.meshes++; });
    reset_frame_state();
    return true;
}

void end_frame(float timeSeconds) {
    if (cur != 0) target(0);
    passes[0].state = fs;
    upload_fonts();

    for (size_t i = 0; i < passes.size(); i++) {
        Texture* t = textures.get(passes[i].target);
        FrameData frame;
        build_frame(passes[i].state, timeSeconds, t ? (float)t->w : inner_width(), t ? (float)t->h : inner_height(), frame);
        memcpy((uint8_t*)frameBuffer.mapped + frameStride * i, &frame, sizeof(frame));
        if (i == 0) {
            // The shadow pass sees the scene through the sun's matrix.
            memcpy(frame.viewProj, frame.shadowMat, sizeof(frame.viewProj));
            memcpy((uint8_t*)frameBuffer.mapped + frameStride * SHADOW_SLOT, &frame, sizeof(frame));
        }
    }

    vkResetFences(device, 1, &frameFence);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    if (passes[0].state.shadowStrength > 0) {
        VkClearValue clearDepth = {};
        clearDepth.depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = shadowPass;
        pass.framebuffer = shadowTex.framebuffer;
        pass.renderArea.extent = {(uint32_t)SHADOW_SIZE, (uint32_t)SHADOW_SIZE};
        pass.clearValueCount = 1;
        pass.pClearValues = &clearDepth;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport = {0, 0, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
        bind_frame(SHADOW_SLOT);
        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundSet = VK_NULL_HANDLE;
        run_draws(passes[0].opaque, true, boundPipeline, boundSet);
        vkCmdEndRenderPass(cmd);
    }

    // Pictures drawn into textures come first, so the screen can use them.
    for (size_t i = 1; i < passes.size(); i++) {
        Texture* t = textures.get(passes[i].target);
        if (!t || !t->framebuffer) continue;
        VkClearValue clears[2] = {};
        for (int c = 0; c < 3; c++) clears[0].color.float32[c] = lin(passes[i].state.clear[c]);
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = targetPass;
        pass.framebuffer = t->framebuffer;
        pass.renderArea.extent = {(uint32_t)t->w, (uint32_t)t->h};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport = {0, 0, (float)t->w, (float)t->h, 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
        bind_frame((uint32_t)i);
        run_pass(passes[i]);
        vkCmdEndRenderPass(cmd);
    }

    VkClearValue clears[3] = {};
    float clearColor[3];
    for (int c = 0; c < 3; c++) clearColor[c] = lin(passes[0].state.clear[c]);
    if (inset() == 0) memcpy(clears[0].color.float32, clearColor, sizeof(clearColor));
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = screenPass;
    pass.framebuffer = framebuffers[imageIndex];
    pass.renderArea.extent = extent;
    pass.clearValueCount = 3;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    int edge = inset();
    VkViewport viewport = {(float)edge, (float)edge, inner_width(), inner_height(), 0.0f, 1.0f};
    VkRect2D scissor = {{edge, edge}, {(uint32_t)inner_width(), (uint32_t)inner_height()}};
    if (edge > 0) {
        // The whole window was cleared to black for the border. Fill the picture inside it with the game's colour.
        VkClearAttachment fill = {VK_IMAGE_ASPECT_COLOR_BIT, 0, clears[0]};
        memcpy(fill.clearValue.color.float32, clearColor, sizeof(clearColor));
        VkClearRect area = {scissor, 0, 1};
        vkCmdClearAttachments(cmd, 1, &fill, 1, &area);
    }
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    bind_frame(0);
    run_pass(passes[0]);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageReady;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone[imageIndex];
    vkQueueSubmit(queue, 1, &submit, frameFence);

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    VkResult result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) swapchainStale = true;
}

// ---- the scene ----

void clear(float r, float g, float b) {
    fs.clear[0] = r; fs.clear[1] = g; fs.clear[2] = b;
}

void camera(const float eye[3], const float target[3], float fovDegrees, float zNear, float zFar, const float* up) {
    const float worldUp[3] = {0, 1, 0};
    float view[16];
    look_at(eye, target, up ? up : worldUp, view, fs.right, fs.up, fs.fwd);
    memcpy(fs.eye, eye, sizeof(fs.eye));
    if (zNear <= 0) zNear = 0.1f;
    if (zFar <= zNear) zFar = zNear + 500.0f;
    fs.zNear = zNear;
    fs.zFar = zFar;
    fs.aspect = aspect();

    // Vulkan's screen has Y pointing down and depth running 0 to 1.
    float proj[16] = {};
    if (fovDegrees < 0) {
        fs.orthoHeight = -fovDegrees;
        proj[0] = 2.0f / (fs.orthoHeight * fs.aspect);
        proj[5] = -2.0f / fs.orthoHeight;
        proj[10] = 1.0f / (zNear - zFar);
        proj[14] = zNear / (zNear - zFar);
        proj[15] = 1.0f;
    } else {
        fs.orthoHeight = 0;
        fs.focal = 1.0f / tanf(SDL_clamp(fovDegrees, 1.0f, 170.0f) * 3.14159265f / 360.0f);
        proj[0] = fs.focal / fs.aspect;
        proj[5] = -fs.focal;
        proj[10] = zFar / (zNear - zFar);
        proj[11] = -1.0f;
        proj[14] = (zNear * zFar) / (zNear - zFar);
    }
    mat_multiply(proj, view, fs.viewProj);
}

void ambient(float r, float g, float b) {
    fs.ambient[0] = r; fs.ambient[1] = g; fs.ambient[2] = b;
}

void light(float x, float y, float z, float r, float g, float b) {
    fs.sunDir[0] = x; fs.sunDir[1] = y; fs.sunDir[2] = z;
    fs.sunColor[0] = r; fs.sunColor[1] = g; fs.sunColor[2] = b;
}

void point_light(const float pos[3], float radius, const float rgb[3]) {
    if (fs.lightCount >= 8) return;
    int i = fs.lightCount++;
    memcpy(fs.lightPos[i], pos, sizeof(float) * 3);
    fs.lightPos[i][3] = SDL_max(radius, 0.001f);
    memcpy(fs.lightCol[i], rgb, sizeof(float) * 3);
    fs.lightCol[i][3] = 0;
    memset(fs.lightAim[i], 0, sizeof(fs.lightAim[i]));
}

void spot_light(const float pos[3], const float dir[3], float radius, float angleDegrees, const float rgb[3]) {
    if (fs.lightCount >= 8) return;
    int i = fs.lightCount;
    point_light(pos, radius, rgb);
    fs.lightCol[i][3] = 1;
    memcpy(fs.lightAim[i], dir, sizeof(float) * 3);
    fs.lightAim[i][3] = cosf(angleDegrees * 3.14159265f / 360.0f);
}

void fog(const float rgb[3], float start, float end) {
    fs.fogColor[0] = lin(rgb[0]); fs.fogColor[1] = lin(rgb[1]); fs.fogColor[2] = lin(rgb[2]);
    fs.fogColor[3] = end > start ? 1.0f : 0.0f;
    fs.fogRange[0] = start;
    fs.fogRange[1] = end;
}

void shadows(const float center[3], float radius, float strength) {
    memcpy(fs.shadowCenter, center, sizeof(fs.shadowCenter));
    fs.shadowRadius = radius;
    fs.shadowStrength = radius > 0 ? SDL_clamp(strength, 0.0f, 1.0f) : 0.0f;
}

void frame_param(int index, const float v[4]) {
    if (index >= 0 && index < 4) memcpy(fs.user[index], v, sizeof(float) * 4);
}

void target(Handle renderTarget) {
    Texture* t = textures.get(renderTarget);
    if (cur == 0) savedScreen = fs;
    else passes[cur].state = fs;
    if (t && t->framebuffer && passes.size() < MAX_PASSES) {
        passes.emplace_back();
        cur = passes.size() - 1;
        passes[cur].target = renderTarget;
    } else if (cur != 0) {
        cur = 0;
        fs = savedScreen;
    }
}

// ---- how things are drawn ----

void shader(const std::string& name) { ds.shader = name.empty() ? nullptr : find_shader(name); }

void texture(int slot, Handle tex) {
    if (slot < 0 || slot > 3) return;
    ds.tex[slot] = tex;
    currentSet = VK_NULL_HANDLE;
}

void blend(int mode) { ds.blend = SDL_clamp(mode, 0, 2); }
void cull(bool backFaces) { ds.cull = backFaces; }
void unlit(bool on) { ds.unlit = on; }
void depth(bool test, bool write) { ds.depthTest = test; ds.depthWrite = write; }
void cutoff(float alpha) { ds.cutoff = alpha; }
void cast_shadows(bool on) { ds.castShadow = on; }
void param(int index, const float v[4]) {
    if (index >= 0 && index < 2) memcpy(ds.params + index * 4, v, sizeof(float) * 4);
}
void font(Handle f) { ds.font = f; }

void reset_state() {
    ds = DrawState();
    currentSet = VK_NULL_HANDLE;
}

// ---- textures, meshes and fonts ----

Handle texture_from_memory(const void* bytes, size_t size, int flags, bool scriptOwned) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)bytes, (int)size, &w, &h, &channels, 4);
    if (!pixels) return 0;
    Handle made = make_texture(w, h, pixels, flags, scriptOwned ? OWNER_SCRIPT : OWNER_FILE);
    stbi_image_free(pixels);
    return made;
}

Handle load_texture(const std::string& name, int flags) {
    std::string path = game_path(name);
    Handle found = 0;
    textures.each([&](Handle h, Texture& t) {
        if (t.key == path && t.flags == flags && !path.empty()) found = h;
    });
    if (found) return found;
    size_t size = 0;
    void* data = path.empty() ? nullptr : SDL_LoadFile(path.c_str(), &size);
    Handle made = data ? texture_from_memory(data, size, flags, false) : 0;
    SDL_free(data);
    if (Texture* t = textures.get(made)) {
        t->key = path;
        t->mtime = file_time(path);
        return made;
    }
    return missingTex;
}

Handle new_texture(int w, int h, const uint8_t* rgba, int flags, bool scriptOwned) {
    return make_texture(w, h, rgba, flags, scriptOwned ? OWNER_SCRIPT : OWNER_FILE);
}

Handle new_target(int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    Texture t;
    t.w = w; t.h = h; t.flags = TEX_NOMIP | TEX_CLAMP;
    bool ok = create_image(t.image, t.memory, w, h, 1, targetFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT) &&
              create_image(t.depthImage, t.depthMemory, w, h, 1, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT);
    if (ok) {
        t.view = create_view(t.image, targetFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        t.depthView = create_view(t.depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
        t.sampler = sampler_for(t.flags);
        VkImageView attachments[2] = {t.view, t.depthView};
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = targetPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = (uint32_t)w;
        fbInfo.height = (uint32_t)h;
        fbInfo.layers = 1;
        ok = vkCreateFramebuffer(device, &fbInfo, nullptr, &t.framebuffer) == VK_SUCCESS;
    }
    if (!ok) {
        destroy_texture(t);
        return 0;
    }
    // Readable straight away, in case it is used before anything is drawn into it.
    VkCommandBuffer c = once_begin();
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    once_end();
    return textures.add(t);
}

bool texture_size(Handle tex, int& w, int& h) {
    Texture* t = textures.get(tex);
    if (!t) return false;
    w = t->w;
    h = t->h;
    return true;
}

Handle missing_texture() { return missingTex; }

void free_texture(Handle tex) {
    Texture* t = textures.get(tex);
    if (!t || t->owner == OWNER_ENGINE) return;
    // ponytail: waits for the GPU on every free. Queue the frees if games start freeing every frame.
    vkDeviceWaitIdle(device);
    drop_sets_using(tex);
    destroy_texture(*t);
    textures.remove(tex);
}

Handle new_mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool scriptOwned) {
    return make_mesh(vertices, indices, scriptOwned ? OWNER_SCRIPT : OWNER_FILE);
}

const MeshData* mesh_data(Handle h) {
    MeshRes* m = meshes.get(h);
    return m ? &m->data : nullptr;
}

void free_mesh(Handle h) {
    MeshRes* m = meshes.get(h);
    if (!m || m->owner == OWNER_ENGINE) return;
    vkDeviceWaitIdle(device);
    destroy_mesh(*m);
    meshes.remove(h);
}

Handle builtin_mesh(Mesh which) { return which >= 0 && which < MESH_COUNT ? builtin[which] : 0; }

void finish_vertices(std::vector<Vertex>& v, const std::vector<uint32_t>& idx, bool makeNormals) {
    if (makeNormals)
        for (Vertex& vert : v) vert.normal[0] = vert.normal[1] = vert.normal[2] = 0;
    std::vector<float> tan(v.size() * 3, 0.0f), bit(v.size() * 3, 0.0f);
    bool anyUv = false;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
        if (ia >= v.size() || ib >= v.size() || ic >= v.size()) continue;
        const Vertex &a = v[ia], &b = v[ib], &c = v[ic];
        float e1[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]}, e2[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
        if (makeNormals) {
            float n[3];
            cross3(e1, e2, n);   // longer for bigger triangles, so they count for more
            for (uint32_t k : {ia, ib, ic})
                for (int axis = 0; axis < 3; axis++) v[k].normal[axis] += n[axis];
        }
        float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1], du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-12f) continue;
        anyUv = true;
        for (int axis = 0; axis < 3; axis++) {
            float t = (e1[axis] * dv2 - e2[axis] * dv1) / det, s = (e2[axis] * du1 - e1[axis] * du2) / det;
            for (uint32_t k : {ia, ib, ic}) {
                tan[k * 3 + axis] += t;
                bit[k * 3 + axis] += s;
            }
        }
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (makeNormals) normalize3(v[i].normal);
        if (!anyUv || dot3(v[i].tangent, v[i].tangent) > 0.25f) continue;   // no texture coordinates, or the file brought its own
        float* n = v[i].normal;
        float t[3] = {tan[i * 3], tan[i * 3 + 1], tan[i * 3 + 2]};
        float along = dot3(n, t);
        for (int axis = 0; axis < 3; axis++) t[axis] -= n[axis] * along;
        if (dot3(t, t) < 1e-12f) continue;
        normalize3(t);
        float c[3];
        cross3(n, t, c);
        memcpy(v[i].tangent, t, sizeof(t));
        v[i].tangent[3] = dot3(c, &bit[i * 3]) < 0 ? -1.0f : 1.0f;
    }
}

Handle load_font(const std::string& name, float pixelHeight) {
    std::string path = game_path(name);
    pixelHeight = SDL_clamp(pixelHeight, 8.0f, 200.0f);
    std::string key = path;
    Handle found = 0;
    fonts.each([&](Handle h, Font& f) {
        if (f.key == key && f.px == pixelHeight) found = h;
    });
    if (found) return found;
    size_t size = 0;
    void* data = path.empty() ? nullptr : SDL_LoadFile(path.c_str(), &size);
    if (!data) return 0;
    Handle h = fonts.add(Font());
    Font* f = fonts.get(h);
    f->file.assign((unsigned char*)data, (unsigned char*)data + size);
    SDL_free(data);
    if (!stbtt_InitFont(&f->info, f->file.data(), stbtt_GetFontOffsetForIndex(f->file.data(), 0))) {
        fonts.remove(h);
        return 0;
    }
    f->px = pixelHeight;
    f->scale = stbtt_ScaleForPixelHeight(&f->info, pixelHeight);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    f->ascent = ascent * f->scale;
    f->alpha.assign((size_t)ATLAS_SIZE * ATLAS_SIZE, 0);
    std::vector<uint8_t> blank((size_t)ATLAS_SIZE * ATLAS_SIZE * 4, 0);
    f->atlas = make_texture(ATLAS_SIZE, ATLAS_SIZE, blank.data(), TEX_NOMIP | TEX_CLAMP | TEX_DATA, OWNER_FILE);
    f->key = key;
    f->mtime = file_time(path);
    return h;
}

// ---- 3D drawing ----

void draw_vertices(Handle meshHandle, const Vertex* vertices, uint32_t count, const float model[16], const float rgba[4]) {
    MeshRes* m = meshes.get(meshHandle);
    if (!m) return;
    Pass& pass = passes[cur];
    int mode = (ds.blend == BLEND_OPAQUE && rgba[3] < 1.0f) ? BLEND_ALPHA : ds.blend;
    bool solid = mode == BLEND_OPAQUE;
    Draw d;
    d.pipeline = pipeline_for(ds.shader, pass_kind(), mode, ds.depthTest, ds.depthWrite && solid, ds.cull);
    d.material = current_material();
    d.vb = m->vb.buffer;
    d.ib = m->ib.buffer;
    d.count = (uint32_t)m->data.indices.size();
    if (vertices) {
        uint32_t first = 0;
        Vertex* out = dyn_alloc(count, first);
        if (!out || count != m->data.vertices.size()) return;
        memcpy(out, vertices, (size_t)count * sizeof(Vertex));
        d.vb = dynBuffer.buffer;
        d.vertexOffset = (int32_t)first;
    }
    fill_push(d.push, model, rgba, false, ds.unlit);
    if (solid && ds.castShadow && !ds.unlit && cur == 0 && fs.shadowStrength >= 0)
        d.shadowPipeline = pipeline_for(find_shader("shadow"), KIND_SHADOW, BLEND_OPAQUE, true, true, false);
    if (solid) {
        pass.opaque.push_back(d);
    } else {
        const float* vp = fs.viewProj;
        d.depth = vp[3] * model[12] + vp[7] * model[13] + vp[11] * model[14] + vp[15];
        pass.transparent.push_back(d);
    }
}

void draw(Handle meshHandle, const float model[16], const float rgba[4]) { draw_vertices(meshHandle, nullptr, 0, model, rgba); }

void mesh(Mesh which, const float pos[3], const float size[3], float yawDegrees, const float rgba[4]) {
    if (which < 0 || which >= MESH_COUNT) return;
    float c = cosf(yawDegrees * 3.14159265f / 180.0f), s = sinf(yawDegrees * 3.14159265f / 180.0f);
    float m[16];
    mat_identity(m);
    m[0] = c * size[0];  m[2] = -s * size[0];
    m[5] = size[1];
    m[8] = s * size[2];  m[10] = c * size[2];
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2];
    draw(builtin[which], m, rgba);
}

// A picture wrapped all the way round the camera (an "equirectangular" panorama), drawn behind everything.
void sky(Handle tex, const float rgba[4]) {
    MeshRes* m = meshes.get(builtin[MESH_SPHERE]);
    if (!m) return;
    Draw d;
    d.pipeline = pipeline_for(ds.shader, pass_kind(), BLEND_OPAQUE, false, false, false);
    Handle t[4] = {tex, ds.tex[1], ds.tex[2], ds.tex[3]};
    d.material = material_set(t);
    d.vb = m->vb.buffer;
    d.ib = m->ib.buffer;
    d.count = (uint32_t)m->data.indices.size();
    float model[16], size = (fs.zNear + fs.zFar) * 0.5f;
    mat_identity(model);
    model[0] = model[5] = model[10] = size;
    model[12] = fs.eye[0]; model[13] = fs.eye[1]; model[14] = fs.eye[2];
    fill_push(d.push, model, rgba, false, true);
    d.push.flags[2] = 0;
    d.push.flags[3] = 1;
    passes[cur].sky.push_back(d);
}

// Triangles made this frame in world space: particles and lines.
Vertex* world_verts(uint32_t count, bool seeThrough, float depthAt[3]) {
    Pass& pass = passes[cur];
    int mode = ds.blend == BLEND_OPAQUE && seeThrough ? BLEND_ALPHA : ds.blend;
    bool solid = mode == BLEND_OPAQUE;
    uint32_t first = 0;
    Vertex* out = dyn_alloc(count, first);
    if (!out) return nullptr;
    Draw d;
    d.pipeline = pipeline_for(ds.shader, pass_kind(), mode, ds.depthTest, ds.depthWrite && solid, false);
    d.material = current_material();
    d.vb = dynBuffer.buffer;
    d.first = first;
    d.count = count;
    float identity[16];
    const float white[4] = {1, 1, 1, 1};
    mat_identity(identity);
    fill_push(d.push, identity, white, false, true);
    // A run of solid lines drawn one after another goes to the GPU as a single draw.
    if (solid && !pass.opaque.empty()) {
        Draw& last = pass.opaque.back();
        if (!last.ib && last.vb == d.vb && last.pipeline == d.pipeline && last.material == d.material && last.first + last.count == first &&
            memcmp(&last.push, &d.push, sizeof(PushData)) == 0) {
            last.count += count;
            return out;
        }
    }
    const float* vp = fs.viewProj;
    d.depth = vp[3] * depthAt[0] + vp[7] * depthAt[1] + vp[11] * depthAt[2] + vp[15];
    (solid ? pass.opaque : pass.transparent).push_back(d);
    return out;
}

void particles(const float* data, int count) {
    if (count <= 0) return;
    float at[3] = {data[0], data[1], data[2]};
    Vertex* v = world_verts((uint32_t)count * 6, true, at);
    if (!v) return;
    const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < count; i++) {
        const float* p = data + i * 8;
        float half = p[3] * 0.5f, color[4];
        lin4(p + 4, color);
        for (int k = 0; k < 6; k++) {
            float cx = corner[k][0] * half, cy = corner[k][1] * half;
            set_vertex(v[i * 6 + k], p[0] + fs.right[0] * cx + fs.up[0] * cy, p[1] + fs.right[1] * cx + fs.up[1] * cy, p[2] + fs.right[2] * cx + fs.up[2] * cy,
                       corner[k][0] * 0.5f + 0.5f, 0.5f - corner[k][1] * 0.5f, color);
        }
    }
}

void line(const float a[3], const float b[3], const float rgba[4], float width) {
    float along[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, toEye[3] = {fs.eye[0] - a[0], fs.eye[1] - a[1], fs.eye[2] - a[2]}, side[3];
    if (fs.orthoHeight > 0) { toEye[0] = -fs.fwd[0]; toEye[1] = -fs.fwd[1]; toEye[2] = -fs.fwd[2]; }
    cross3(along, toEye, side);
    if (dot3(side, side) < 1e-12f) return;
    normalize3(side);
    float mid[3] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
    Vertex* v = world_verts(6, rgba[3] < 1.0f, mid);
    if (!v) return;
    float color[4], h = width * 0.5f;
    lin4(rgba, color);
    const float* ends[6] = {a, b, b, a, b, a};
    const float sign[6] = {-1, -1, 1, -1, 1, 1};
    for (int k = 0; k < 6; k++)
        set_vertex(v[k], ends[k][0] + side[0] * h * sign[k], ends[k][1] + side[1] * h * sign[k], ends[k][2] + side[2] * h * sign[k], 0, 0, color);
}

bool project(const float world[3], float& x, float& y) {
    const float* m = fs.viewProj;
    float cx = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12];
    float cy = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13];
    float cw = m[3] * world[0] + m[7] * world[1] + m[11] * world[2] + m[15];
    if (cw <= 1e-5f) return false;
    x = (cx / cw * 0.5f + 0.5f) * view_width();
    y = (cy / cw * 0.5f + 0.5f) * view_height();
    return true;
}

void unproject(float x, float y, float origin[3], float dir[3]) {
    float nx = x / view_width() * 2 - 1, ny = 1 - y / view_height() * 2;
    if (fs.orthoHeight > 0) {
        float ox = nx * fs.orthoHeight * fs.aspect / 2, oy = ny * fs.orthoHeight / 2;
        for (int i = 0; i < 3; i++) {
            origin[i] = fs.eye[i] + fs.right[i] * ox + fs.up[i] * oy;
            dir[i] = fs.fwd[i];
        }
        return;
    }
    for (int i = 0; i < 3; i++) {
        origin[i] = fs.eye[i];
        dir[i] = fs.fwd[i] + fs.right[i] * nx * fs.aspect / fs.focal + fs.up[i] * ny / fs.focal;
    }
    normalize3(dir);
}

const float* camera_position() { return fs.eye; }
const float* camera_right() { return fs.right; }

// ---- 2D drawing ----

float view_width() { return screen_aspect() >= 1.0f ? VIEW_HEIGHT * screen_aspect() : VIEW_HEIGHT; }
float view_height() { return screen_aspect() >= 1.0f ? VIEW_HEIGHT : VIEW_HEIGHT / screen_aspect(); }

void set_bezel(int pixels) { bezel = pixels; }

void window_to_view(float& x, float& y) {
    if (inset() == 0) return;
    x = (x * extent.width - inset()) / inner_width();
    y = (y * extent.height - inset()) / inner_height();
}

void set_safe_area(const float insets[4]) { memcpy(safeInsets, insets, sizeof(safeInsets)); }
const float* safe_area() { return safeInsets; }

void ui_log(bool on) { uiLogging = on; }
const std::vector<UiBox>& ui_boxes() { return uiBoxes; }

void rect(float x, float y, float w, float h, const float rgba[4]) {
    if (uiLogging) uiBoxes.push_back({x, y, w, h, false, ""});
    float color[4];
    lin4(rgba, color);
    quad_2d(whiteTex, x, y, w, h, color, 0, 0, 1, 1);
}

void image(Handle tex, float x, float y, float w, float h, const float rgba[4], const float uv[4]) {
    if (uiLogging) uiBoxes.push_back({x, y, w, h, false, ""});
    float color[4];
    lin4(rgba, color);
    quad_2d(tex, x, y, w, h, color, uv[0], uv[1], uv[2], uv[3]);
}

void circle(float cx, float cy, float radius, const float rgba[4]) {
    if (uiLogging) uiBoxes.push_back({cx - radius, cy - radius, radius * 2, radius * 2, true, ""});
    const int segments = 28;
    Vertex* v = flat_verts(segments * 3, flat_material(whiteTex));
    if (!v) return;
    float color[4];
    lin4(rgba, color);
    for (int i = 0; i < segments; i++) {
        float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
        set_vertex(v[i * 3], cx, cy, 0, 0, 0, color);
        set_vertex(v[i * 3 + 1], cx + cosf(a0) * radius, cy + sinf(a0) * radius, 0, 0, 0, color);
        set_vertex(v[i * 3 + 2], cx + cosf(a1) * radius, cy + sinf(a1) * radius, 0, 0, 0, color);
    }
}

// Text is drawn with the font chosen by gfx.font, or with a built-in 8x8 pixel font that needs no file.
void text(const std::string& s, float x, float y, float size, const float rgba[4]) {
    // Logged as one box for the whole text, not one per letter.
    bool logging = uiLogging;
    uiLogging = false;
    if (logging && s.find_first_not_of(" \n") != std::string::npos) {
        float lines = 1;
        for (char ch : s) lines += ch == '\n';
        uiBoxes.push_back({x, y, text_width(s, size), size * (1 + (lines - 1) * 1.25f), false, s});
    }
    float color[4], penX = x;
    lin4(rgba, color);
    Font* f = fonts.get(ds.font);
    if (f) {
        float k = size / f->px;
        uint32_t previous = 0;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = next_codepoint(s, i);
            if (cp == '\n') { penX = x; y += size * 1.25f; previous = 0; continue; }
            const Glyph* g = glyph_for(*f, cp);
            if (previous) penX += stbtt_GetCodepointKernAdvance(&f->info, (int)previous, (int)cp) * f->scale * k;
            if (g->w > 0) quad_2d(f->atlas, penX + g->x0 * k, y + (f->ascent + g->y0) * k, g->w * k, g->h * k, color, g->u0, g->v0, g->u1, g->v1);
            penX += g->advance * k;
            previous = cp;
        }
    } else {
        float cell = size / 8.0f;
        for (size_t i = 0; i < s.size();) {
            uint32_t ch = next_codepoint(s, i);
            if (ch == '\n') { penX = x; y += size * 1.25f; continue; }
            if (ch >= 128) ch = '?';
            for (int row = 0; row < 8; row++) {
                unsigned char bits = (unsigned char)font8x8_basic[ch][row];
                int col = 0;
                while (col < 8) {
                    if (!(bits & (1 << col))) { col++; continue; }
                    int start = col;
                    while (col < 8 && (bits & (1 << col))) col++;
                    quad_2d(whiteTex, penX + start * cell, y + row * cell, (col - start) * cell, cell, color, 0, 0, 1, 1);
                }
            }
            penX += size;
        }
    }
    uiLogging = logging;
}

float text_width(const std::string& s, float size) {
    float widest = 0, current = 0;
    Font* f = fonts.get(ds.font);
    uint32_t previous = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = next_codepoint(s, i);
        if (cp == '\n') { current = 0; previous = 0; continue; }
        if (f) {
            if (previous) current += stbtt_GetCodepointKernAdvance(&f->info, (int)previous, (int)cp) * f->scale * size / f->px;
            current += glyph_for(*f, cp)->advance * size / f->px;
        } else {
            current += size;
        }
        previous = cp;
        if (current > widest) widest = current;
    }
    return widest;
}

const std::string& shader_error() { return shaderProblem; }
Stats stats() { return lastStats; }

#if defined(GFX_SCREENSHOT)
// Debugging aid, off by default: saves the frame that was just drawn as a BMP file.
bool screenshot(const std::string& path) {
    vkDeviceWaitIdle(device);
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    Buffer pixels;
    VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4;
    if (!create_buffer(pixels, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;

    VkCommandBuffer c = once_begin();
    transition(c, images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(c, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.buffer, 1, &region);
    once_end();

    bool bgr = colorFormat == VK_FORMAT_B8G8R8A8_UNORM || colorFormat == VK_FORMAT_B8G8R8A8_SRGB;
    SDL_Surface* shot = SDL_CreateSurfaceFrom((int)extent.width, (int)extent.height, bgr ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32, pixels.mapped, (int)extent.width * 4);
    bool ok = shot && SDL_SaveBMP(shot, path.c_str());
    SDL_DestroySurface(shot);
    destroy_buffer(pixels);
    return ok;
}
#endif

}  // namespace gfx
