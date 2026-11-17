// The far-away world, painted once in Blender: shown exactly as rendered, no lighting and no fog.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    return tex0.Sample(input.uv);
}
