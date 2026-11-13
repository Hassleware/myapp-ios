#include "collide.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pool.h"

namespace collide {
namespace {

enum Kind { SPHERE, CAPSULE, BOX, MESH };

struct BvhNode {
    float lo[3], hi[3];
    int left = -1, right = -1;   // children, or
    int first = 0, count = 0;    // triangles, in a leaf
};

struct Body {
    int kind = SPHERE;
    uint32_t layer = 1;
    float pos[3] = {0, 0, 0};
    float radius = 0, height = 0;      // capsule: total height, ends included
    float half[3] = {0, 0, 0};         // box
    float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // box: its three directions
    float bound = 0;                   // nothing of the body is further than this from pos
    float groundY = -1e30f;            // a mover's feet the last time it stood on proper ground
    std::vector<float> tris;           // mesh: nine numbers per triangle, in world space
    std::vector<BvhNode> bvh;
};

Pool<Body> bodies;

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void sub(const float a[3], const float b[3], float out[3]) { out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2]; }
void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
float length(const float a[3]) { return sqrtf(dot(a, a)); }

// The nearest point of triangle abc to p (from Ericson, Real-Time Collision Detection).
void closest_on_triangle(const float p[3], const float* a, const float* b, const float* c, float out[3]) {
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, sizeof(float) * 3); return; }
    sub(p, b, bp);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, sizeof(float) * 3); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 / (d1 - d3);
        for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v;
        return;
    }
    sub(p, c, cp);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, sizeof(float) * 3); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 / (d2 - d6);
        for (int i = 0; i < 3; i++) out[i] = a[i] + ac[i] * w;
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; i++) out[i] = b[i] + (c[i] - b[i]) * w;
        return;
    }
    float denom = 1.0f / (va + vb + vc), v = vb * denom, w = vc * denom;
    for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

int build_bvh(Body& b, std::vector<int>& order, int first, int count) {
    BvhNode node;
    for (int a = 0; a < 3; a++) { node.lo[a] = 1e30f; node.hi[a] = -1e30f; }
    for (int i = first; i < first + count; i++)
        for (int v = 0; v < 3; v++)
            for (int a = 0; a < 3; a++) {
                float x = b.tris[(size_t)order[(size_t)i] * 9 + (size_t)v * 3 + (size_t)a];
                node.lo[a] = SDL_min(node.lo[a], x);
                node.hi[a] = SDL_max(node.hi[a], x);
            }
    int index = (int)b.bvh.size();
    b.bvh.push_back(node);
    if (count <= 4) {
        b.bvh[(size_t)index].first = first;
        b.bvh[(size_t)index].count = count;
        return index;
    }
    int axis = 0;
    for (int a = 1; a < 3; a++)
        if (node.hi[a] - node.lo[a] > node.hi[axis] - node.lo[axis]) axis = a;
    auto centre = [&](int tri) { return b.tris[(size_t)tri * 9 + (size_t)axis] + b.tris[(size_t)tri * 9 + 3 + (size_t)axis] + b.tris[(size_t)tri * 9 + 6 + (size_t)axis]; };
    int mid = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count, [&](int x, int y) { return centre(x) < centre(y); });
    int left = build_bvh(b, order, first, mid - first);
    int right = build_bvh(b, order, mid, first + count - mid);
    b.bvh[(size_t)index].left = left;
    b.bvh[(size_t)index].right = right;
    return index;
}

float box_distance_sq(const BvhNode& n, const float p[3]) {
    float d = 0;
    for (int a = 0; a < 3; a++) {
        float v = p[a] < n.lo[a] ? n.lo[a] - p[a] : p[a] > n.hi[a] ? p[a] - n.hi[a] : 0;
        d += v * v;
    }
    return d;
}

// Nearest point of a mesh within reach of p. Returns the distance, or a huge number when nothing is that close.
float mesh_closest(const Body& b, const float p[3], float reach, float closest[3], float normal[3]) {
    float best = reach * reach;
    bool found = false;
    int stack[64], top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const BvhNode& n = b.bvh[(size_t)stack[--top]];
        if (box_distance_sq(n, p) > best) continue;
        if (n.left < 0) {
            for (int i = n.first; i < n.first + n.count; i++) {
                const float* t = &b.tris[(size_t)i * 9];
                float q[3], d[3];
                closest_on_triangle(p, t, t + 3, t + 6, q);
                sub(p, q, d);
                float dist = dot(d, d);
                if (dist > best) continue;
                best = dist;
                found = true;
                memcpy(closest, q, sizeof(q));
                float e1[3], e2[3], faceN[3];
                sub(t + 3, t, e1); sub(t + 6, t, e2);
                cross(e1, e2, faceN);
                float len = sqrtf(dist), faceLen = length(faceN);
                // Away from the surface towards p. On the surface itself, the triangle's own facing.
                for (int a = 0; a < 3; a++) normal[a] = len > 1e-5f ? d[a] / len : (faceLen > 0 ? faceN[a] / faceLen : (a == 1 ? 1.0f : 0.0f));
            }
        } else if (top < 62) {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }
    }
    return found ? sqrtf(best) : 1e30f;
}

void capsule_ends(const Body& b, float lo[3], float hi[3]) {
    float reach = SDL_max(b.height * 0.5f - b.radius, 0.0f);
    for (int i = 0; i < 3; i++) lo[i] = hi[i] = b.pos[i];
    lo[1] -= reach;
    hi[1] += reach;
}

// How far p is from the surface of a body (below zero inside a solid one), with the nearest surface
// point and the direction from it towards p.
float distance_to(const Body& b, const float p[3], float reach, float closest[3], float normal[3]) {
    if (b.kind == MESH) return mesh_closest(b, p, reach, closest, normal);
    float d[3];
    if (b.kind == BOX) {
        float rel[3], local[3], q[3];
        sub(p, b.pos, rel);
        bool inside = true;
        for (int a = 0; a < 3; a++) {
            local[a] = dot(rel, &b.axes[a * 3]);
            q[a] = SDL_clamp(local[a], -b.half[a], b.half[a]);
            if (q[a] != local[a]) inside = false;
        }
        if (inside) {
            int axis = 0;
            for (int a = 1; a < 3; a++)
                if (b.half[a] - fabsf(local[a]) < b.half[axis] - fabsf(local[axis])) axis = a;
            float sign = local[axis] < 0 ? -1.0f : 1.0f, depth = b.half[axis] - fabsf(local[axis]);
            q[axis] = sign * b.half[axis];
            for (int i = 0; i < 3; i++) {
                normal[i] = b.axes[axis * 3 + i] * sign;
                closest[i] = b.pos[i] + b.axes[i] * q[0] + b.axes[3 + i] * q[1] + b.axes[6 + i] * q[2];
            }
            return -depth;
        }
        for (int i = 0; i < 3; i++) closest[i] = b.pos[i] + b.axes[i] * q[0] + b.axes[3 + i] * q[1] + b.axes[6 + i] * q[2];
        sub(p, closest, d);
        float len = length(d);
        for (int i = 0; i < 3; i++) normal[i] = len > 1e-6f ? d[i] / len : (i == 1 ? 1.0f : 0.0f);
        return len;
    }
    float centre[3] = {b.pos[0], b.pos[1], b.pos[2]};
    if (b.kind == CAPSULE) {
        float lo[3], hi[3];
        capsule_ends(b, lo, hi);
        centre[1] = SDL_clamp(p[1], lo[1], hi[1]);
    }
    sub(p, centre, d);
    float len = length(d);
    for (int i = 0; i < 3; i++) {
        normal[i] = len > 1e-6f ? d[i] / len : (i == 1 ? 1.0f : 0.0f);
        closest[i] = centre[i] + normal[i] * b.radius;
    }
    return len - b.radius;
}

bool ray_triangle(const float o[3], const float d[3], const float* a, const float* b, const float* c, float& t) {
    float e1[3], e2[3], h[3], s[3], q[3];
    sub(b, a, e1); sub(c, a, e2);
    cross(d, e2, h);
    float det = dot(e1, h);
    if (fabsf(det) < 1e-9f) return false;
    float inv = 1.0f / det;
    sub(o, a, s);
    float u = dot(s, h) * inv;
    if (u < 0 || u > 1) return false;
    cross(s, e1, q);
    float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t >= 0;
}

bool ray_box(const BvhNode& n, const float o[3], const float d[3], float maxT) {
    float t0 = 0, t1 = maxT;
    for (int a = 0; a < 3; a++) {
        if (fabsf(d[a]) < 1e-9f) {
            if (o[a] < n.lo[a] || o[a] > n.hi[a]) return false;
            continue;
        }
        float x = (n.lo[a] - o[a]) / d[a], y = (n.hi[a] - o[a]) / d[a];
        if (x > y) std::swap(x, y);
        t0 = SDL_max(t0, x);
        t1 = SDL_min(t1, y);
        if (t0 > t1) return false;
    }
    return true;
}

void ray_mesh(const Body& b, Handle handle, const float o[3], const float d[3], Hit& best, float maxT) {
    int stack[64], top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const BvhNode& n = b.bvh[(size_t)stack[--top]];
        if (!ray_box(n, o, d, best.hit ? best.distance : maxT)) continue;
        if (n.left < 0) {
            for (int i = n.first; i < n.first + n.count; i++) {
                const float* t = &b.tris[(size_t)i * 9];
                float at = 0;
                if (!ray_triangle(o, d, t, t + 3, t + 6, at) || at > (best.hit ? best.distance : maxT)) continue;
                best.hit = true;
                best.distance = at;
                best.body = handle;
                float e1[3], e2[3];
                sub(t + 3, t, e1); sub(t + 6, t, e2);
                cross(e1, e2, best.normal);
                float len = length(best.normal), facing = dot(best.normal, d) > 0 ? -1.0f : 1.0f;   // always back towards the ray
                for (int a = 0; a < 3; a++) {
                    best.normal[a] *= len > 0 ? facing / len : 0;
                    best.point[a] = o[a] + d[a] * at;
                }
            }
        } else if (top < 62) {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }
    }
}

// Steps a point (or a ball of the given radius) along a line, each time by the clear distance ahead of it.
// ponytail: grazing along a surface can use up its steps before maxDistance; give it exact sweeps if that bites.
void march(const Body& b, Handle handle, const float o[3], const float d[3], float maxT, float radius, Hit& best) {
    float limit = best.hit ? best.distance : maxT, t = 0;
    float reach = radius + SDL_max(radius * 2, 0.25f);
    for (int step = 0; step < 512 && t <= limit; step++) {
        float p[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t}, closest[3], normal[3];
        float gap = distance_to(b, p, reach, closest, normal) - radius;
        if (gap < 0.001f) {
            best.hit = true;
            best.distance = t;
            best.body = handle;
            memcpy(best.point, closest, sizeof(closest));
            memcpy(best.normal, normal, sizeof(normal));
            return;
        }
        t += SDL_max(SDL_min(gap, reach - radius), 0.01f);
    }
}

Hit cast(const float origin[3], const float dirIn[3], float maxDistance, float radius, uint32_t mask, Handle ignore) {
    Hit best;
    float d[3] = {dirIn[0], dirIn[1], dirIn[2]}, len = length(d);
    if (len < 1e-8f || maxDistance <= 0) return best;
    for (int i = 0; i < 3; i++) d[i] /= len;
    bodies.each([&](Handle h, Body& b) {
        if (h == ignore || !(b.layer & mask)) return;
        // Skip bodies the whole line stays clear of.
        float rel[3];
        sub(b.pos, origin, rel);
        float along = SDL_clamp(dot(rel, d), 0.0f, maxDistance);
        float off[3] = {rel[0] - d[0] * along, rel[1] - d[1] * along, rel[2] - d[2] * along};
        if (length(off) > b.bound + radius) return;
        if (b.kind == MESH && radius <= 0) ray_mesh(b, h, origin, d, best, maxDistance);
        else march(b, h, origin, d, maxDistance, radius, best);
    });
    return best;
}

void finish_mesh(Body& b) {
    int count = (int)(b.tris.size() / 9);
    std::vector<int> order((size_t)count);
    for (int i = 0; i < count; i++) order[(size_t)i] = i;
    b.bvh.reserve((size_t)count);
    build_bvh(b, order, 0, count);
    std::vector<float> sorted(b.tris.size());
    for (int i = 0; i < count; i++) memcpy(&sorted[(size_t)i * 9], &b.tris[(size_t)order[(size_t)i] * 9], sizeof(float) * 9);
    b.tris.swap(sorted);
    const BvhNode& root = b.bvh[0];
    float reach[3];
    for (int a = 0; a < 3; a++) {
        b.pos[a] = (root.lo[a] + root.hi[a]) / 2;
        reach[a] = (root.hi[a] - root.lo[a]) / 2;
    }
    b.bound = length(reach);
}

// The balls that stand in for a moving sphere or capsule: one, or a column of them.
int samples(const Body& b, float out[8][3]) {
    if (b.kind != CAPSULE) {
        memcpy(out[0], b.pos, sizeof(float) * 3);
        return 1;
    }
    float lo[3], hi[3];
    capsule_ends(b, lo, hi);
    int count = SDL_clamp((int)ceilf((hi[1] - lo[1]) / SDL_max(b.radius, 0.01f)) + 1, 2, 8);
    for (int i = 0; i < count; i++) {
        out[i][0] = b.pos[0];
        out[i][1] = lo[1] + (hi[1] - lo[1]) * (float)i / (float)(count - 1);
        out[i][2] = b.pos[2];
    }
    return count;
}

// Pushes a body out of everything it overlaps. Walkable ground pushes straight up, so nothing creeps downhill.
bool push_out(Handle self, Body& me, uint32_t mask, float slopeCos) {
    bool touched = false;
    for (int round = 0; round < 4; round++) {
        bool moved = false;
        bodies.each([&](Handle h, Body& other) {
            if (h == self || !(other.layer & mask)) return;
            float rel[3];
            sub(other.pos, me.pos, rel);
            if (length(rel) > other.bound + me.bound) return;
            float balls[8][3];
            int count = samples(me, balls);
            for (int i = 0; i < count; i++) {
                float closest[3], n[3];
                float depth = me.radius - distance_to(other, balls[i], me.radius + 0.05f, closest, n);
                if (depth <= 0) continue;
                float flat = sqrtf(n[0] * n[0] + n[2] * n[2]);
                float push[3] = {n[0] * depth, n[1] * depth, n[2] * depth};
                if (n[1] >= slopeCos) {
                    push[0] = push[2] = 0;
                    push[1] = depth / n[1];
                } else if (n[1] > 0 && flat > 0.1f) {   // too steep to stand on: a wall
                    push[0] = n[0] / flat * depth / flat;
                    push[1] = 0;
                    push[2] = n[2] / flat * depth / flat;
                }
                for (int a = 0; a < 3; a++) {
                    me.pos[a] += push[a];
                    for (int k = 0; k < count; k++) balls[k][a] += push[a];
                }
                moved = touched = true;
            }
        });
        if (!moved) break;
    }
    return touched;
}

bool travel(Handle self, Body& me, const float delta[3], uint32_t mask, float slopeCos) {
    int steps = SDL_clamp((int)ceilf(length(delta) / SDL_max(me.radius * 0.5f, 0.01f)), 1, 32);
    bool touched = false;
    for (int s = 0; s < steps; s++) {
        for (int a = 0; a < 3; a++) me.pos[a] += delta[a] / (float)steps;
        touched |= push_out(self, me, mask, slopeCos);
    }
    return touched;
}

bool on_ground(Handle self, const Body& me, uint32_t mask, float slopeCos, float normal[3]) {
    float balls[8][3];
    samples(me, balls);
    bool grounded = false;
    bodies.each([&](Handle h, Body& other) {
        if (grounded || h == self || !(other.layer & mask)) return;
        float closest[3], n[3];
        if (distance_to(other, balls[0], me.radius + 0.06f, closest, n) < me.radius + 0.03f && n[1] >= slopeCos) {
            grounded = true;
            memcpy(normal, n, sizeof(float) * 3);
        }
    });
    return grounded;
}

void circle(const float c[3], float r, int axis, const float rgba[4]) {
    const int segments = 16;
    int u = (axis + 1) % 3, w = (axis + 2) % 3;
    for (int i = 0; i < segments; i++) {
        float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
        float p[3] = {c[0], c[1], c[2]}, q[3] = {c[0], c[1], c[2]};
        p[u] += cosf(a0) * r; p[w] += sinf(a0) * r;
        q[u] += cosf(a1) * r; q[w] += sinf(a1) * r;
        gfx::line(p, q, rgba, 0.02f);
    }
}

}  // namespace

Handle add_sphere(const float pos[3], float radius, uint32_t layer) {
    Body b;
    b.kind = SPHERE;
    b.layer = layer;
    memcpy(b.pos, pos, sizeof(b.pos));
    b.radius = b.bound = radius;
    return bodies.add(std::move(b));
}

Handle add_capsule(const float pos[3], float radius, float height, uint32_t layer) {
    Body b;
    b.kind = CAPSULE;
    b.layer = layer;
    memcpy(b.pos, pos, sizeof(b.pos));
    b.radius = radius;
    b.height = SDL_max(height, radius * 2);
    b.bound = b.height * 0.5f;
    return bodies.add(std::move(b));
}

void set_axes(Body& b, const float* q) {
    if (!q) return;
    float m[16];
    const float zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    gfx::mat_from_trs(zero, q, one, m);
    for (int a = 0; a < 3; a++) memcpy(&b.axes[a * 3], &m[a * 4], sizeof(float) * 3);
}

Handle add_box(const float pos[3], const float quat[4], const float halfSize[3], uint32_t layer) {
    Body b;
    b.kind = BOX;
    b.layer = layer;
    memcpy(b.pos, pos, sizeof(b.pos));
    memcpy(b.half, halfSize, sizeof(b.half));
    set_axes(b, quat);
    b.bound = length(b.half);
    return bodies.add(std::move(b));
}

Handle add_mesh(const gfx::MeshData& mesh, const float world[16], uint32_t layer) {
    Body b;
    b.kind = MESH;
    b.layer = layer;
    b.tris.reserve(mesh.indices.size() * 3);
    for (uint32_t index : mesh.indices) {
        const float* p = mesh.vertices[index].pos;
        for (int row = 0; row < 3; row++) b.tris.push_back(world[row] * p[0] + world[4 + row] * p[1] + world[8 + row] * p[2] + world[12 + row]);
    }
    b.tris.resize(b.tris.size() / 9 * 9);
    if (b.tris.empty()) return 0;
    finish_mesh(b);
    return bodies.add(std::move(b));
}

bool place(Handle h, const float pos[3], const float* quat) {
    Body* b = bodies.get(h);
    if (!b || b->kind == MESH) return false;
    memcpy(b->pos, pos, sizeof(b->pos));
    if (b->kind == BOX) set_axes(*b, quat);
    return true;
}

bool position(Handle h, float pos[3]) {
    Body* b = bodies.get(h);
    if (!b) return false;
    memcpy(pos, b->pos, sizeof(b->pos));
    return true;
}

void remove(Handle h) { bodies.remove(h); }
void clear() { bodies = Pool<Body>(); }

Hit raycast(const float origin[3], const float dir[3], float maxDistance, uint32_t mask, Handle ignore) { return cast(origin, dir, maxDistance, 0, mask, ignore); }
Hit spherecast(const float origin[3], const float dir[3], float maxDistance, float radius, uint32_t mask, Handle ignore) {
    return cast(origin, dir, maxDistance, SDL_max(radius, 0.0f), mask, ignore);
}

void overlap(const float center[3], float radius, uint32_t mask, std::vector<Handle>& out) {
    out.clear();
    bodies.each([&](Handle h, Body& b) {
        if (!(b.layer & mask)) return;
        float rel[3], closest[3], normal[3];
        sub(b.pos, center, rel);
        if (length(rel) > b.bound + radius) return;
        if (distance_to(b, center, radius + 0.01f, closest, normal) < radius) out.push_back(h);
    });
}

Moved slide(Handle h, const float delta[3], uint32_t mask, float stepHeight, float slopeDegrees) {
    Moved result;
    Body* found = bodies.get(h);
    if (!found || found->kind == BOX || found->kind == MESH) return result;
    Body& me = *found;
    float slopeCos = cosf(SDL_clamp(slopeDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    float start[3] = {me.pos[0], me.pos[1], me.pos[2]}, unused[3];
    bool wasGrounded = on_ground(h, me, mask, slopeCos, unused);

    travel(h, me, delta, mask, slopeCos);
    float wanted = sqrtf(delta[0] * delta[0] + delta[2] * delta[2]);
    float got = sqrtf((me.pos[0] - start[0]) * (me.pos[0] - start[0]) + (me.pos[2] - start[2]) * (me.pos[2] - start[2]));
    result.blocked = wanted > 1e-5f && got < wanted * 0.7f;

    // Held up by something low while walking: try again from a step higher, then settle back down.
    // It counts only if there is ground to stand on just ahead and above, so walls and steep slopes still stop it.
    const float any = 0.05f;   // while stepping, an edge underfoot is as good as a floor
    bool stepped = false;
    if (result.blocked && stepHeight > 0 && (wasGrounded || on_ground(h, me, mask, any, unused))) {
        float first[3] = {me.pos[0], me.pos[1], me.pos[2]};
        const float up[3] = {0, stepHeight, 0}, down[3] = {0, -stepHeight - 0.02f, 0}, flat[3] = {delta[0], 0, delta[2]};
        memcpy(me.pos, start, sizeof(start));
        travel(h, me, up, mask, slopeCos);
        travel(h, me, flat, mask, slopeCos);
        travel(h, me, down, mask, any);
        float reached = sqrtf((me.pos[0] - start[0]) * (me.pos[0] - start[0]) + (me.pos[2] - start[2]) * (me.pos[2] - start[2]));
        float feet = me.pos[1] - SDL_max(me.height * 0.5f, me.radius);
        float probe[3] = {me.pos[0] + delta[0] / wanted * (me.radius + 0.05f), feet + stepHeight, me.pos[2] + delta[2] / wanted * (me.radius + 0.05f)};
        const float straightDown[3] = {0, -1, 0};
        Hit ahead = cast(probe, straightDown, stepHeight * 2, 0, mask, h);
        stepped = reached > got + 1e-4f && ahead.hit && ahead.normal[1] >= slopeCos && ahead.point[1] > start[1] - SDL_max(me.height * 0.5f, me.radius) + 0.02f &&
                  ahead.point[1] <= me.groundY + stepHeight + 0.02f;   // measured from the last real footing, so tall ledges can't be climbed a bit at a time
        if (stepped) result.blocked = reached < wanted * 0.7f;
        else memcpy(me.pos, first, sizeof(first));
    }
    memcpy(result.pos, me.pos, sizeof(me.pos));
    if (on_ground(h, me, mask, slopeCos, unused)) me.groundY = me.pos[1] - SDL_max(me.height * 0.5f, me.radius);
    result.grounded = on_ground(h, me, mask, stepped ? any : slopeCos, result.normal);
    return result;
}

void debug_draw() {
    const float green[4] = {0.3f, 1.0f, 0.4f, 1}, blue[4] = {0.4f, 0.7f, 1.0f, 1};
    bodies.each([&](Handle, Body& b) {
        if (b.kind == SPHERE) {
            for (int axis = 0; axis < 3; axis++) circle(b.pos, b.radius, axis, green);
        } else if (b.kind == CAPSULE) {
            float lo[3], hi[3];
            capsule_ends(b, lo, hi);
            circle(lo, b.radius, 1, green);
            circle(hi, b.radius, 1, green);
            circle(lo, b.radius, 0, green); circle(lo, b.radius, 2, green);
            circle(hi, b.radius, 0, green); circle(hi, b.radius, 2, green);
            for (int i = 0; i < 4; i++) {
                float ox = (i == 0 ? 1.0f : i == 1 ? -1.0f : 0.0f) * b.radius, oz = (i == 2 ? 1.0f : i == 3 ? -1.0f : 0.0f) * b.radius;
                float p[3] = {lo[0] + ox, lo[1], lo[2] + oz}, q[3] = {hi[0] + ox, hi[1], hi[2] + oz};
                gfx::line(p, q, green, 0.02f);
            }
        } else if (b.kind == BOX) {
            float corner[8][3];
            for (int i = 0; i < 8; i++)
                for (int a = 0; a < 3; a++)
                    corner[i][a] = b.pos[a] + b.axes[a] * ((i & 1) ? b.half[0] : -b.half[0]) + b.axes[3 + a] * ((i & 2) ? b.half[1] : -b.half[1]) +
                                   b.axes[6 + a] * ((i & 4) ? b.half[2] : -b.half[2]);
            for (int i = 0; i < 8; i++)
                for (int bit = 1; bit < 8; bit <<= 1)
                    if (!(i & bit)) gfx::line(corner[i], corner[i | bit], green, 0.02f);
        } else {
            size_t count = SDL_min(b.tris.size() / 9, (size_t)1500);   // enough to see the shape without flooding the frame
            for (size_t i = 0; i < count; i++) {
                const float* t = &b.tris[i * 9];
                gfx::line(t, t + 3, blue, 0.015f);
                gfx::line(t + 3, t + 6, blue, 0.015f);
                gfx::line(t + 6, t, blue, 0.015f);
            }
        }
    });
}

}  // namespace collide
