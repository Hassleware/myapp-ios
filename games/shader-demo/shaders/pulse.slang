// A custom shader: bands of light sweep up whatever is drawn with it.
// common.slang holds the data the engine sends every shader, and the standard lighting.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 base = standardColor(input);
    float time = frame.params.x;

    float band = 0.5 + 0.5 * sin(input.world.y * 6.0 - time * 4.0);
    return float4(base.rgb + band * band * 0.12, base.a);
}
