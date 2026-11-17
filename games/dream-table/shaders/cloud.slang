// Soft cartoon clouds: each ball is sky blue where it faces you, whiter toward its top, and glows white round its
// rim, so where balls overlap each shows a bright edge. A little sunlight warms the lit side. Fogged like the rest.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

float3 linear(float3 c) { return pow(c, 2.2); }

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 n = normalize(input.normal);
    float3 v = normalize(frame.cameraPos.xyz - input.world);
    float facing = saturate(dot(n, v));
    float rim = pow(1.0 - facing, 2.2);
    float top = saturate(n.y * 0.5 + 0.5);
    float sun = saturate(dot(n, -normalize(frame.lightDir.xyz)));

    float3 blue = linear(float3(0.42, 0.68, 0.97));
    float3 pale = linear(float3(0.86, 0.94, 1.0));
    float3 c = lerp(blue, pale, pow(top, 2.0) * 0.8);
    c = lerp(c, float3(1.0, 1.0, 1.0), rim);
    c += sun * 0.12;
    return float4(applyFog(c, input.world), 1.0);
}
