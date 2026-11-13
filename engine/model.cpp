#include "model.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>

#include "pool.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace model {
namespace {

struct Part {
    gfx::Handle mesh = 0, tex = 0, normalTex = 0;
    float color[4] = {1, 1, 1, 1};
    int blend = gfx::BLEND_OPAQUE;
    float cutoff = 0;
    bool doubleSided = false;
    std::vector<uint16_t> joints;   // four per vertex, for skinned parts
    std::vector<float> weights;     // four per vertex
};

struct Skin {
    std::vector<int> joints;
    std::vector<float> inverseBind;   // sixteen per joint
};

struct Channel {
    int node = 0, path = 0, width = 3;   // path: 0 move, 1 turn, 2 scale
    bool step = false, cubic = false;
    std::vector<float> times, values;
};

struct Clip {
    ClipInfo info;
    std::vector<Channel> channels;
};

struct Model {
    std::vector<Node> nodes;
    std::vector<int> order;   // parents before children
    std::vector<std::vector<Part>> groups;
    std::vector<Skin> skins;
    std::vector<Clip> clips;
    std::vector<gfx::Handle> textures;
    std::string key;
    Sint64 mtime = 0;
};

struct Animator {
    Handle model = 0;
    std::vector<float> t, r, s, world;   // per node: 3, 4, 3 and 16 numbers
    bool dirty = true;
};

Pool<Model> models;
Pool<Animator> animators;
std::vector<gfx::Vertex> skinned;   // reused every draw

Sint64 file_time(const std::string& path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path.c_str(), &info) ? (Sint64)info.modify_time : 0;
}

void destroy(Model& m) {
    for (auto& group : m.groups)
        for (Part& p : group) gfx::free_mesh(p.mesh);
    for (gfx::Handle t : m.textures) gfx::free_texture(t);
}

// Colours in the file are in linear light. The renderer expects them the way a colour picker shows them.
float to_srgb(float c) { return powf(SDL_clamp(c, 0.0f, 1.0f), 1.0f / 2.2f); }

gfx::Handle load_image(Model& m, const cgltf_texture* texture, const std::string& folder, bool data, std::vector<gfx::Handle>& cache, const cgltf_data* file) {
    if (!texture || !texture->image) return 0;
    const cgltf_image* image = texture->image;
    size_t index = (size_t)(image - file->images) * 2 + (data ? 1 : 0);
    if (cache[index]) return cache[index];
    int flags = data ? gfx::TEX_DATA : 0;
    gfx::Handle made = 0;
    if (image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data) {
        const cgltf_buffer_view* view = image->buffer_view;
        made = gfx::texture_from_memory((const uint8_t*)view->buffer->data + view->offset, view->size, flags, false);
    } else if (image->uri && strncmp(image->uri, "data:", 5) != 0) {
        size_t size = 0;
        void* bytes = SDL_LoadFile((folder + image->uri).c_str(), &size);
        if (bytes) made = gfx::texture_from_memory(bytes, size, flags, false);
        SDL_free(bytes);
    }
    if (made) m.textures.push_back(made);
    cache[index] = made;
    return made;
}

void read_floats(const cgltf_accessor* a, std::vector<float>& out, int width) {
    out.assign(a->count * (size_t)width, 0.0f);
    for (size_t i = 0; i < a->count; i++) cgltf_accessor_read_float(a, i, &out[i * (size_t)width], (cgltf_size)width);
}

void load_primitive(Model& m, const cgltf_primitive& prim, const std::string& folder, std::vector<gfx::Handle>& imageCache, const cgltf_data* file, std::vector<Part>& group) {
    if (prim.type != cgltf_primitive_type_triangles) return;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr, *tan = nullptr, *jnt = nullptr, *wgt = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; i++) {
        const cgltf_attribute& a = prim.attributes[i];
        if (a.type == cgltf_attribute_type_position) pos = a.data;
        else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
        else if (a.type == cgltf_attribute_type_tangent) tan = a.data;
        else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
        else if (a.type == cgltf_attribute_type_color && a.index == 0) col = a.data;
        else if (a.type == cgltf_attribute_type_joints && a.index == 0) jnt = a.data;
        else if (a.type == cgltf_attribute_type_weights && a.index == 0) wgt = a.data;
    }
    if (!pos || pos->count == 0) return;

    std::vector<gfx::Vertex> vertices(pos->count);
    for (size_t i = 0; i < pos->count; i++) {
        gfx::Vertex& v = vertices[i];
        memset(&v, 0, sizeof(v));
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1;
        cgltf_accessor_read_float(pos, i, v.pos, 3);
        if (nrm) cgltf_accessor_read_float(nrm, i, v.normal, 3);
        if (uv) cgltf_accessor_read_float(uv, i, v.uv, 2);
        if (tan) cgltf_accessor_read_float(tan, i, v.tangent, 4);
        if (col) cgltf_accessor_read_float(col, i, v.color, col->type == cgltf_type_vec3 ? 3 : 4);
    }
    std::vector<uint32_t> indices;
    if (prim.indices) {
        indices.resize(prim.indices->count);
        for (size_t i = 0; i < indices.size(); i++) indices[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);
    } else {
        indices.resize(pos->count);
        for (size_t i = 0; i < indices.size(); i++) indices[i] = (uint32_t)i;
    }
    gfx::finish_vertices(vertices, indices, !nrm);

    Part part;
    part.mesh = gfx::new_mesh(vertices, indices, false);
    if (!part.mesh) return;
    if (jnt && wgt) {
        part.joints.resize(pos->count * 4);
        part.weights.resize(pos->count * 4);
        for (size_t i = 0; i < pos->count; i++) {
            cgltf_uint j[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(jnt, i, j, 4);
            cgltf_accessor_read_float(wgt, i, &part.weights[i * 4], 4);
            for (int k = 0; k < 4; k++) part.joints[i * 4 + k] = (uint16_t)j[k];
        }
    }
    if (const cgltf_material* mat = prim.material) {
        if (mat->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
            for (int c = 0; c < 3; c++) part.color[c] = to_srgb(pbr.base_color_factor[c]);
            part.color[3] = pbr.base_color_factor[3];
            part.tex = load_image(m, pbr.base_color_texture.texture, folder, false, imageCache, file);
        }
        part.normalTex = load_image(m, mat->normal_texture.texture, folder, true, imageCache, file);
        if (mat->alpha_mode == cgltf_alpha_mode_mask) part.cutoff = mat->alpha_cutoff;
        if (mat->alpha_mode == cgltf_alpha_mode_blend) part.blend = gfx::BLEND_ALPHA;
        part.doubleSided = mat->double_sided != 0;
    }
    group.push_back(std::move(part));
}

void local_matrix(const float t[3], const float r[4], const float s[3], float out[16]) { gfx::mat_from_trs(t, r, s, out); }

// Splits a matrix back into move, turn and scale. Good enough for matrices without skew.
void decompose(const float m[16], float t[3], float r[4], float s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    float c[3][3];
    for (int i = 0; i < 3; i++) {
        s[i] = sqrtf(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]);
        for (int k = 0; k < 3; k++) c[i][k] = s[i] > 1e-8f ? m[i * 4 + k] / s[i] : (i == k ? 1.0f : 0.0f);
    }
    float trace = c[0][0] + c[1][1] + c[2][2];
    if (trace > 0) {
        float k = sqrtf(trace + 1) * 2;
        r[3] = k / 4; r[0] = (c[1][2] - c[2][1]) / k; r[1] = (c[2][0] - c[0][2]) / k; r[2] = (c[0][1] - c[1][0]) / k;
    } else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
        float k = sqrtf(1 + c[0][0] - c[1][1] - c[2][2]) * 2;
        r[3] = (c[1][2] - c[2][1]) / k; r[0] = k / 4; r[1] = (c[1][0] + c[0][1]) / k; r[2] = (c[2][0] + c[0][2]) / k;
    } else if (c[1][1] > c[2][2]) {
        float k = sqrtf(1 + c[1][1] - c[0][0] - c[2][2]) * 2;
        r[3] = (c[2][0] - c[0][2]) / k; r[0] = (c[1][0] + c[0][1]) / k; r[1] = k / 4; r[2] = (c[2][1] + c[1][2]) / k;
    } else {
        float k = sqrtf(1 + c[2][2] - c[0][0] - c[1][1]) * 2;
        r[3] = (c[0][1] - c[1][0]) / k; r[0] = (c[2][0] + c[0][2]) / k; r[1] = (c[2][1] + c[1][2]) / k; r[2] = k / 4;
    }
}

void normalize_quat(float q[4]) {
    float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len < 1e-8f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; i++) q[i] /= len;
}

// The value of one animation track at a time.
void sample_channel(const Channel& ch, float time, float out[4]) {
    size_t count = ch.times.size();
    if (count == 0) return;
    size_t stride = (size_t)ch.width * (ch.cubic ? 3 : 1), offset = ch.cubic ? (size_t)ch.width : 0;
    auto value = [&](size_t key) { return &ch.values[key * stride + offset]; };
    size_t hi = 0;
    while (hi < count && ch.times[hi] <= time) hi++;   // ponytail: linear search, binary if clips get thousands of keys
    if (hi == 0 || hi == count || ch.step) {
        memcpy(out, value(hi == 0 ? 0 : hi - 1), sizeof(float) * (size_t)ch.width);
        return;
    }
    size_t lo = hi - 1;
    float span = ch.times[hi] - ch.times[lo], k = span > 1e-8f ? (time - ch.times[lo]) / span : 0;
    const float *a = value(lo), *b = value(hi);
    float sign = 1;
    if (ch.path == 1 && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) sign = -1;   // turn the short way round
    for (int i = 0; i < ch.width; i++) out[i] = a[i] + (b[i] * sign - a[i]) * k;
    if (ch.path == 1) normalize_quat(out);
}

void apply_clip(const Model& m, const Clip& clip, float time, std::vector<float>& t, std::vector<float>& r, std::vector<float>& s) {
    for (const Channel& ch : clip.channels) {
        if (ch.node < 0 || ch.node >= (int)m.nodes.size()) continue;
        float v[4] = {0, 0, 0, 1};
        sample_channel(ch, time, v);
        if (ch.path == 0) memcpy(&t[(size_t)ch.node * 3], v, sizeof(float) * 3);
        else if (ch.path == 1) memcpy(&r[(size_t)ch.node * 4], v, sizeof(float) * 4);
        else memcpy(&s[(size_t)ch.node * 3], v, sizeof(float) * 3);
    }
}

void rest_pose(const Model& m, std::vector<float>& t, std::vector<float>& r, std::vector<float>& s) {
    size_t n = m.nodes.size();
    t.resize(n * 3); r.resize(n * 4); s.resize(n * 3);
    for (size_t i = 0; i < n; i++) {
        memcpy(&t[i * 3], m.nodes[i].t, sizeof(float) * 3);
        memcpy(&r[i * 4], m.nodes[i].r, sizeof(float) * 4);
        memcpy(&s[i * 3], m.nodes[i].s, sizeof(float) * 3);
    }
}

void update_world(const Model& m, Animator& a) {
    if (!a.dirty) return;
    a.world.resize(m.nodes.size() * 16);
    for (int i : m.order) {
        float local[16];
        local_matrix(&a.t[(size_t)i * 3], &a.r[(size_t)i * 4], &a.s[(size_t)i * 3], local);
        int parent = m.nodes[(size_t)i].parent;
        if (parent >= 0) gfx::mat_multiply(&a.world[(size_t)parent * 16], local, &a.world[(size_t)i * 16]);
        else memcpy(&a.world[(size_t)i * 16], local, sizeof(local));
    }
    a.dirty = false;
}

// Moves a part's vertices to follow the skeleton. Done on the CPU so every shader works on characters unchanged.
// ponytail: fine for a handful of characters; move to the GPU if a game wants crowds.
void skin_part(const Part& part, const Skin& skin, const float* world, const gfx::MeshData& mesh) {
    size_t jointCount = skin.joints.size();
    std::vector<float> palette(jointCount * 16);
    for (size_t j = 0; j < jointCount; j++) gfx::mat_multiply(&world[(size_t)skin.joints[j] * 16], &skin.inverseBind[j * 16], &palette[j * 16]);
    skinned = mesh.vertices;
    for (size_t i = 0; i < skinned.size(); i++) {
        float m[16] = {};
        float total = 0;
        for (int k = 0; k < 4; k++) {
            float w = part.weights[i * 4 + k];
            size_t j = part.joints[i * 4 + k];
            if (w <= 0 || j >= jointCount) continue;
            total += w;
            for (int e = 0; e < 16; e++) m[e] += palette[j * 16 + e] * w;
        }
        if (total < 1e-6f) continue;
        gfx::Vertex& v = skinned[i];
        const gfx::Vertex& src = mesh.vertices[i];
        for (int row = 0; row < 3; row++) {
            v.pos[row] = (m[row] * src.pos[0] + m[4 + row] * src.pos[1] + m[8 + row] * src.pos[2] + m[12 + row]) / total;
            v.normal[row] = m[row] * src.normal[0] + m[4 + row] * src.normal[1] + m[8 + row] * src.normal[2];
            v.tangent[row] = m[row] * src.tangent[0] + m[4 + row] * src.tangent[1] + m[8 + row] * src.tangent[2];
        }
    }
}

void draw_part(const Part& part, const float world[16], const float rgba[4], const gfx::Vertex* moved, uint32_t movedCount) {
    float color[4] = {part.color[0] * rgba[0], part.color[1] * rgba[1], part.color[2] * rgba[2], part.color[3] * rgba[3]};
    gfx::texture(0, part.tex);
    gfx::texture(1, part.normalTex);
    gfx::cull(!part.doubleSided);
    gfx::cutoff(part.cutoff);
    gfx::blend(part.blend);
    gfx::draw_vertices(part.mesh, moved, movedCount, world, color);
}

}  // namespace

void to_trs(const float m[16], float t[3], float r[4], float s[3]) { decompose(m, t, r, s); }

Handle load(const std::string& name) {
    std::string path = gfx::game_path(name);
    if (path.empty()) return 0;
    Handle found = 0;
    models.each([&](Handle h, Model& m) {
        if (m.key == path) found = h;
    });
    if (found) return found;

    cgltf_options options = {};
    cgltf_data* file = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &file) != cgltf_result_success) return 0;
    if (cgltf_load_buffers(&options, file, path.c_str()) != cgltf_result_success) {
        cgltf_free(file);
        return 0;
    }
    std::string folder = path.substr(0, path.find_last_of('/') + 1);

    Model m;
    m.key = path;
    m.mtime = file_time(path);
    std::vector<gfx::Handle> imageCache(file->images_count * 2 + 1, 0);
    m.groups.resize(file->meshes_count);
    for (cgltf_size i = 0; i < file->meshes_count; i++)
        for (cgltf_size p = 0; p < file->meshes[i].primitives_count; p++) load_primitive(m, file->meshes[i].primitives[p], folder, imageCache, file, m.groups[i]);

    m.nodes.resize(file->nodes_count);
    for (cgltf_size i = 0; i < file->nodes_count; i++) {
        const cgltf_node& src = file->nodes[i];
        Node& n = m.nodes[i];
        n.name = src.name ? src.name : "";
        n.parent = src.parent ? (int)(src.parent - file->nodes) : -1;
        if (src.has_matrix) {
            decompose(src.matrix, n.t, n.r, n.s);
        } else {
            if (src.has_translation) memcpy(n.t, src.translation, sizeof(n.t));
            if (src.has_rotation) memcpy(n.r, src.rotation, sizeof(n.r));
            if (src.has_scale) memcpy(n.s, src.scale, sizeof(n.s));
        }
        n.group = src.mesh ? (int)(src.mesh - file->meshes) : -1;
        n.skin = src.skin ? (int)(src.skin - file->skins) : -1;
        if (src.extras.data) n.extras = src.extras.data;
        if (src.light) {
            n.light = src.light->type == cgltf_light_type_directional ? 1 : src.light->type == cgltf_light_type_spot ? 3 : 2;
            memcpy(n.lightColor, src.light->color, sizeof(n.lightColor));
            n.lightRange = src.light->range;
            n.lightAngle = src.light->spot_outer_cone_angle * 2 * 180.0f / 3.14159265f;
        }
        if (src.camera && src.camera->type == cgltf_camera_type_perspective) n.cameraFov = src.camera->data.perspective.yfov * 180.0f / 3.14159265f;
    }
    // Work out an order with every parent before its children, then each node's place in the model at rest.
    std::vector<bool> placed(m.nodes.size(), false);
    while (m.order.size() < m.nodes.size()) {
        size_t before = m.order.size();
        for (size_t i = 0; i < m.nodes.size(); i++) {
            int parent = m.nodes[i].parent;
            if (placed[i] || (parent >= 0 && !placed[(size_t)parent])) continue;
            placed[i] = true;
            m.order.push_back((int)i);
        }
        if (m.order.size() == before) break;   // a loop in the file: leave the rest out
    }
    for (int i : m.order) {
        Node& n = m.nodes[(size_t)i];
        float local[16];
        local_matrix(n.t, n.r, n.s, local);
        if (n.parent >= 0) gfx::mat_multiply(m.nodes[(size_t)n.parent].world, local, n.world);
        else memcpy(n.world, local, sizeof(local));
    }

    m.skins.resize(file->skins_count);
    for (cgltf_size i = 0; i < file->skins_count; i++) {
        const cgltf_skin& src = file->skins[i];
        Skin& skin = m.skins[i];
        skin.joints.resize(src.joints_count);
        skin.inverseBind.assign(src.joints_count * 16, 0.0f);
        for (cgltf_size j = 0; j < src.joints_count; j++) {
            skin.joints[j] = (int)(src.joints[j] - file->nodes);
            if (src.inverse_bind_matrices) cgltf_accessor_read_float(src.inverse_bind_matrices, j, &skin.inverseBind[j * 16], 16);
            else gfx::mat_identity(&skin.inverseBind[j * 16]);
        }
    }

    m.clips.resize(file->animations_count);
    for (cgltf_size i = 0; i < file->animations_count; i++) {
        const cgltf_animation& src = file->animations[i];
        Clip& clip = m.clips[i];
        clip.info.name = src.name ? src.name : "";
        for (cgltf_size c = 0; c < src.channels_count; c++) {
            const cgltf_animation_channel& from = src.channels[c];
            if (!from.target_node || !from.sampler || !from.sampler->input || !from.sampler->output) continue;
            Channel ch;
            ch.node = (int)(from.target_node - file->nodes);
            if (from.target_path == cgltf_animation_path_type_translation) ch.path = 0;
            else if (from.target_path == cgltf_animation_path_type_rotation) ch.path = 1;
            else if (from.target_path == cgltf_animation_path_type_scale) ch.path = 2;
            else continue;
            ch.width = ch.path == 1 ? 4 : 3;
            ch.step = from.sampler->interpolation == cgltf_interpolation_type_step;
            ch.cubic = from.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            read_floats(from.sampler->input, ch.times, 1);
            read_floats(from.sampler->output, ch.values, ch.width);
            if (ch.values.size() < ch.times.size() * (size_t)ch.width * (ch.cubic ? 3 : 1)) continue;
            if (!ch.times.empty()) clip.info.duration = SDL_max(clip.info.duration, ch.times.back());
            clip.channels.push_back(std::move(ch));
        }
    }
    cgltf_free(file);
    return models.add(std::move(m));
}

const std::vector<Node>* nodes(Handle h) {
    Model* m = models.get(h);
    return m ? &m->nodes : nullptr;
}

std::vector<ClipInfo> clips(Handle h) {
    std::vector<ClipInfo> out;
    if (Model* m = models.get(h))
        for (const Clip& c : m->clips) out.push_back(c.info);
    return out;
}

int find_node(Handle h, const std::string& name) {
    if (Model* m = models.get(h))
        for (size_t i = 0; i < m->nodes.size(); i++)
            if (m->nodes[i].name == name) return (int)i;
    return -1;
}

int find_clip(Handle h, const std::string& name) {
    if (Model* m = models.get(h))
        for (size_t i = 0; i < m->clips.size(); i++)
            if (m->clips[i].info.name == name) return (int)i;
    return -1;
}

std::vector<gfx::Handle> node_meshes(Handle h, int node) {
    std::vector<gfx::Handle> out;
    Model* m = models.get(h);
    if (!m || node < 0 || node >= (int)m->nodes.size() || m->nodes[(size_t)node].group < 0) return out;
    for (const Part& p : m->groups[(size_t)m->nodes[(size_t)node].group]) out.push_back(p.mesh);
    return out;
}

bool sample(Handle h, int clip, int node, float time, float t[3], float r[4], float s[3]) {
    Model* m = models.get(h);
    if (!m || node < 0 || node >= (int)m->nodes.size() || clip < 0 || clip >= (int)m->clips.size()) return false;
    const Node& n = m->nodes[(size_t)node];
    memcpy(t, n.t, sizeof(n.t));
    memcpy(r, n.r, sizeof(n.r));
    memcpy(s, n.s, sizeof(n.s));
    for (const Channel& ch : m->clips[(size_t)clip].channels) {
        if (ch.node != node) continue;
        float v[4] = {0, 0, 0, 1};
        sample_channel(ch, time, v);
        memcpy(ch.path == 0 ? t : ch.path == 1 ? r : s, v, sizeof(float) * (size_t)ch.width);
    }
    return true;
}

Handle new_animator(Handle h) {
    Model* m = models.get(h);
    if (!m) return 0;
    Animator a;
    a.model = h;
    rest_pose(*m, a.t, a.r, a.s);
    return animators.add(std::move(a));
}

void free_animator(Handle h) { animators.remove(h); }

void pose(Handle h, const int* clip, const float* time, const float* weight, int count) {
    Animator* a = animators.get(h);
    Model* m = a ? models.get(a->model) : nullptr;
    if (!m) return;
    rest_pose(*m, a->t, a->r, a->s);
    a->dirty = true;
    float total = 0;
    std::vector<float> t, r, s;
    for (int i = 0; i < count; i++) {
        if (clip[i] < 0 || clip[i] >= (int)m->clips.size() || weight[i] <= 0) continue;
        rest_pose(*m, t, r, s);
        apply_clip(*m, m->clips[(size_t)clip[i]], time[i], t, r, s);
        // A running weighted average: each clip is mixed in by its share of the weight so far.
        total += weight[i];
        float k = weight[i] / total;
        for (size_t e = 0; e < t.size(); e++) {
            a->t[e] += (t[e] - a->t[e]) * k;
            a->s[e] += (s[e] - a->s[e]) * k;
        }
        for (size_t n = 0; n < m->nodes.size(); n++) {
            float *q = &a->r[n * 4], *p = &r[n * 4];
            float sign = q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] * p[3] < 0 ? -1.0f : 1.0f;
            for (int e = 0; e < 4; e++) q[e] += (p[e] * sign - q[e]) * k;
            normalize_quat(q);
        }
    }
}

bool get_local(Handle h, int node, float t[3], float r[4], float s[3]) {
    Animator* a = animators.get(h);
    if (!a || node < 0 || (size_t)node * 3 >= a->t.size()) return false;
    memcpy(t, &a->t[(size_t)node * 3], sizeof(float) * 3);
    memcpy(r, &a->r[(size_t)node * 4], sizeof(float) * 4);
    memcpy(s, &a->s[(size_t)node * 3], sizeof(float) * 3);
    return true;
}

bool set_local(Handle h, int node, const float t[3], const float r[4], const float s[3]) {
    Animator* a = animators.get(h);
    if (!a || node < 0 || (size_t)node * 3 >= a->t.size()) return false;
    memcpy(&a->t[(size_t)node * 3], t, sizeof(float) * 3);
    memcpy(&a->r[(size_t)node * 4], r, sizeof(float) * 4);
    memcpy(&a->s[(size_t)node * 3], s, sizeof(float) * 3);
    a->dirty = true;
    return true;
}

bool node_world(Handle h, Handle animator, int node, float out[16]) {
    Model* m = models.get(h);
    if (!m || node < 0 || node >= (int)m->nodes.size()) return false;
    Animator* a = animators.get(animator);
    if (a && a->model == h) {
        update_world(*m, *a);
        memcpy(out, &a->world[(size_t)node * 16], sizeof(float) * 16);
    } else {
        memcpy(out, m->nodes[(size_t)node].world, sizeof(float) * 16);
    }
    return true;
}

void draw(Handle h, Handle animator, const float world[16], const float rgba[4], int onlyNode) {
    Model* m = models.get(h);
    if (!m) return;
    Animator* a = animators.get(animator);
    if (a && a->model != h) a = nullptr;
    if (a) update_world(*m, *a);
    for (size_t i = 0; i < m->nodes.size(); i++) {
        const Node& n = m->nodes[i];
        if (n.group < 0 || (onlyNode >= 0 && (int)i != onlyNode)) continue;
        const float* nodeWorld = a ? &a->world[i * 16] : n.world;
        for (const Part& part : m->groups[(size_t)n.group]) {
            const gfx::MeshData* mesh = gfx::mesh_data(part.mesh);
            if (!mesh) continue;
            bool isSkinned = a && n.skin >= 0 && n.skin < (int)m->skins.size() && part.weights.size() == mesh->vertices.size() * 4;
            if (isSkinned) {
                // A skinned mesh follows its joints, not the node it hangs on.
                skin_part(part, m->skins[(size_t)n.skin], a->world.data(), *mesh);
                draw_part(part, world, rgba, skinned.data(), (uint32_t)skinned.size());
            } else if (onlyNode >= 0) {
                draw_part(part, world, rgba, nullptr, 0);
            } else {
                float placed[16];
                gfx::mat_multiply(world, nodeWorld, placed);
                draw_part(part, placed, rgba, nullptr, 0);
            }
        }
    }
    // Leave the drawing settings as a game expects to find them.
    gfx::texture(0, 0);
    gfx::texture(1, 0);
    gfx::cull(true);
    gfx::cutoff(0);
    gfx::blend(gfx::BLEND_OPAQUE);
}

void release(bool everything) {
    std::vector<Handle> gone;
    animators.each([&](Handle h, Animator&) { gone.push_back(h); });
    for (Handle h : gone) animators.remove(h);
    gone.clear();
    models.each([&](Handle h, Model& m) {
        if (everything || file_time(m.key) != m.mtime) gone.push_back(h);
    });
    for (Handle h : gone) {
        destroy(*models.get(h));
        models.remove(h);
    }
}

}  // namespace model
