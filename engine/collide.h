// Collision: shapes placed in the world, questions asked about them, and a mover for characters.
// There is no physics solver here. Games move things themselves and use these to find out what they touch.
#pragma once
#include <vector>

#include "gfx.h"

namespace collide {

typedef uint32_t Handle;

struct Hit {
    bool hit = false;
    float distance = 0;
    float point[3] = {0, 0, 0}, normal[3] = {0, 1, 0};
    Handle body = 0;
};

struct Moved {
    float pos[3] = {0, 0, 0};
    bool grounded = false, blocked = false;
    float normal[3] = {0, 1, 0};   // of the ground when grounded
};

// layer is a set of bits. A question only sees bodies that share a bit with its mask.
Handle add_sphere(const float pos[3], float radius, uint32_t layer);
Handle add_capsule(const float pos[3], float radius, float height, uint32_t layer);   // upright, pos at its middle
Handle add_box(const float pos[3], const float quat[4], const float halfSize[3], uint32_t layer);
Handle add_mesh(const gfx::MeshData& mesh, const float world[16], uint32_t layer);    // never moves
bool place(Handle body, const float pos[3], const float* quat);
bool position(Handle body, float pos[3]);
void remove(Handle body);
void clear();

Hit raycast(const float origin[3], const float dir[3], float maxDistance, uint32_t mask, Handle ignore = 0);
Hit spherecast(const float origin[3], const float dir[3], float maxDistance, float radius, uint32_t mask, Handle ignore = 0);
void overlap(const float center[3], float radius, uint32_t mask, std::vector<Handle>& out);

// Moves a sphere or capsule by delta, sliding along whatever it meets, climbing steps up to stepHeight
// and treating slopes steeper than slopeDegrees as walls.
Moved slide(Handle body, const float delta[3], uint32_t mask, float stepHeight, float slopeDegrees);

void debug_draw();

}  // namespace collide
