// Draws meshes into the sun's shadow map. Only depth is kept.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    if (tex0.Sample(input.uv).a * input.color.a < push.flags.z)
        discard;
    return float4(0, 0, 0, 1);
}
