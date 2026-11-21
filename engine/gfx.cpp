// The renderer: textures, meshes, materials, the frame's record and replay, and the drawing games ask for.
// Vulkan set-up is in gfx_vulkan.cpp and text in gfx_text.cpp; gfx_internal.h is what the three share.
#include "gfx_internal.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

namespace gfx {
namespace internal {

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
float srgb_to_linear(float c) { return c <= 0 ? 0 : c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f); }

void srgb_to_linear4(const float in[4], float out[4]) {
    out[0] = srgb_to_linear(in[0]); out[1] = srgb_to_linear(in[1]); out[2] = srgb_to_linear(in[2]); out[3] = in[3];
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
    VkCommandBuffer c = begin_one_time_commands();
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(c, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_one_time_commands();
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

int pass_kind() { return passes[cur].target ? KIND_TARGET : KIND_SCREEN; }

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

Vertex* frame_alloc(uint32_t count, uint32_t& first) {
    if (dynCount + count > MAX_DYNAMIC_VERTS) return nullptr;
    first = dynCount;
    dynCount += count;
    return (Vertex*)dynBuffer.mapped + first;
}

void fill_push(PushData& push, const float model[16], const float rgba[4], bool flat, bool noLight) {
    memcpy(push.model, model, sizeof(float) * 16);
    srgb_to_linear4(rgba, push.color);
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
    Vertex* out = frame_alloc(count, first);
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
        nowStats.triangles += (int)d.count / 3;
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
        if (!f.engine && (everything || file_time(f.key) != f.mtime)) gone.push_back(h);
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

}  // namespace internal

using namespace internal;

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
        for (int c = 0; c < 3; c++) clears[0].color.float32[c] = srgb_to_linear(passes[i].state.clear[c]);
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
    for (int c = 0; c < 3; c++) clearColor[c] = srgb_to_linear(passes[0].state.clear[c]);
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
    fs.fogColor[0] = srgb_to_linear(rgb[0]); fs.fogColor[1] = srgb_to_linear(rgb[1]); fs.fogColor[2] = srgb_to_linear(rgb[2]);
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

Handle engine_texture(int w, int h, const uint8_t* rgba, int flags) { return make_texture(w, h, rgba, flags, OWNER_ENGINE); }

bool update_texture(Handle tex, const uint8_t* rgba) {
    Texture* t = textures.get(tex);
    if (!t || !rgba || t->mips != 1 || t->framebuffer) return false;
    vkQueueWaitIdle(queue);   // ponytail: waits for the last frame to finish with it. Fine for one small picture a frame
    return upload_pixels(*t, rgba);
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
    VkCommandBuffer c = begin_one_time_commands();
    transition(c, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    end_one_time_commands();
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
        Vertex* out = frame_alloc(count, first);
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
    Vertex* out = frame_alloc(count, first);
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
        srgb_to_linear4(p + 4, color);
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
    srgb_to_linear4(rgba, color);
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
    srgb_to_linear4(rgba, color);
    quad_2d(whiteTex, x, y, w, h, color, 0, 0, 1, 1);
}

void image(Handle tex, float x, float y, float w, float h, const float rgba[4], const float uv[4]) {
    if (uiLogging) uiBoxes.push_back({x, y, w, h, false, ""});
    float color[4];
    srgb_to_linear4(rgba, color);
    quad_2d(tex, x, y, w, h, color, uv[0], uv[1], uv[2], uv[3]);
}

void triangle(Handle tex, const float xy[6], const float uv[6], const float rgba[4]) {
    float color[4];
    srgb_to_linear4(rgba, color);
    Vertex* v = flat_verts(3, flat_material(tex));
    if (!v) return;
    for (int i = 0; i < 3; i++) set_vertex(v[i], xy[i * 2], xy[i * 2 + 1], 0, uv[i * 2], uv[i * 2 + 1], color);
}

void circle(float cx, float cy, float radius, const float rgba[4]) {
    if (uiLogging) uiBoxes.push_back({cx - radius, cy - radius, radius * 2, radius * 2, true, ""});
    const int segments = 28;
    Vertex* v = flat_verts(segments * 3, flat_material(whiteTex));
    if (!v) return;
    float color[4];
    srgb_to_linear4(rgba, color);
    for (int i = 0; i < segments; i++) {
        float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
        set_vertex(v[i * 3], cx, cy, 0, 0, 0, color);
        set_vertex(v[i * 3 + 1], cx + cosf(a0) * radius, cy + sinf(a0) * radius, 0, 0, 0, color);
        set_vertex(v[i * 3 + 2], cx + cosf(a1) * radius, cy + sinf(a1) * radius, 0, 0, 0, color);
    }
}

const std::string& shader_error() { return shaderProblem; }

Stats stats() { return lastStats; }

Usage usage() {
    Usage u;
    std::vector<Handle> atlases;
    fonts.each([&](Handle, Font& f) {
        if (f.engine) return;
        atlases.push_back(f.atlas);
        u.texels += f.penY * ATLAS_SIZE + f.rowH * f.penX;   // the rows glyphs have filled
    });
    textures.each([&](Handle h, Texture& t) {
        if (t.owner == OWNER_ENGINE || std::find(atlases.begin(), atlases.end(), h) != atlases.end()) return;
        u.texels += t.w * t.h;
        u.largestTexture = SDL_max(u.largestTexture, SDL_max(t.w, t.h));
    });
    meshes.each([&](Handle, MeshRes& m) {
        if (m.owner == OWNER_ENGINE) return;
        u.meshVertices += (int)m.data.vertices.size();
        u.meshTriangles += (int)m.data.indices.size() / 3;
    });
    return u;
}

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

    VkCommandBuffer c = begin_one_time_commands();
    transition(c, images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(c, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.buffer, 1, &region);
    end_one_time_commands();

    bool bgr = colorFormat == VK_FORMAT_B8G8R8A8_UNORM || colorFormat == VK_FORMAT_B8G8R8A8_SRGB;
    SDL_Surface* shot = SDL_CreateSurfaceFrom((int)extent.width, (int)extent.height, bgr ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32, pixels.mapped, (int)extent.width * 4);
    bool ok = shot && SDL_SaveBMP(shot, path.c_str());
    SDL_DestroySurface(shot);
    destroy_buffer(pixels);
    return ok;
}

#endif

}  // namespace gfx
