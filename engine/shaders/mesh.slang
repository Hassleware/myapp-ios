// The engine's standard shader. It draws lit, textured 3D meshes and flat 2D shapes.
//
// To write your own, copy this file into a game's shaders/ folder under a new
// name, change fsMain, and select it from Lua with gfx.shader("yourname").
// tools/dev.py compiles it when you save. On the iPhone, MoltenVK converts the
// compiled shader to Metal Shading Language automatically.
// Everything a shader can read is listed in engine/shaders/common.slang.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    return standardColor(input);
}
