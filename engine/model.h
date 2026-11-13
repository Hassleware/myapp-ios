// glTF models: scenes made in Blender, with their meshes, textures, skeletons and animations.
#pragma once
#include <string>
#include <vector>

#include "gfx.h"

namespace model {

typedef uint32_t Handle;

struct Node {
    std::string name;
    int parent = -1;
    float t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};   // where it sits in its parent
    float world[16];                                                 // where it sits in the model, at rest
    int group = -1, skin = -1;                                       // its mesh, if it has one
    std::string extras;                                              // custom properties, as JSON text
    int light = 0;                                                   // 0 none, 1 sun, 2 point, 3 spot
    float lightColor[3] = {1, 1, 1}, lightRange = 0, lightAngle = 0;
    float cameraFov = 0;                                             // degrees, 0 when the node is not a camera
};

struct ClipInfo {
    std::string name;
    float duration = 0;
};

// Returns 0 when the file is missing or broken. Loading the same unchanged file again returns the same handle.
Handle load(const std::string& name);
const std::vector<Node>* nodes(Handle model);
std::vector<ClipInfo> clips(Handle model);
int find_node(Handle model, const std::string& name);
int find_clip(Handle model, const std::string& name);
// The meshes drawn at a node, for building collision from level geometry.
std::vector<gfx::Handle> node_meshes(Handle model, int node);

// One node's position in its parent at a moment of a clip.
bool sample(Handle model, int clip, int node, float time, float t[3], float r[4], float s[3]);

// An animator holds one character's current pose. Several can share a model.
Handle new_animator(Handle model);
void free_animator(Handle animator);
// Sets the pose to a weighted mix of clips, each at its own time.
void pose(Handle animator, const int* clip, const float* time, const float* weight, int count);
bool get_local(Handle animator, int node, float t[3], float r[4], float s[3]);
bool set_local(Handle animator, int node, const float t[3], const float r[4], const float s[3]);
// Where a node (a hand, a head) is in the model right now, for attaching things to it.
bool node_world(Handle model, Handle animator, int node, float out[16]);

// Draws the whole model, or one node's mesh placed directly at `world` when onlyNode is 0 or more.
void draw(Handle model, Handle animator, const float world[16], const float rgba[4], int onlyNode);

// Splits a matrix into position, rotation (a quaternion) and scale.
void to_trs(const float m[16], float t[3], float r[4], float s[3]);

// Called when a game is reloaded or closed. Models whose files have not changed are kept on a reload.
void release(bool everything);

}  // namespace model
