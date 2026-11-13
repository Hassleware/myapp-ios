// Toon shading: light comes in flat steps instead of a smooth fade, and edges facing away get an ink line.
// gfx.param(1, ink, 0, 0, 0): ink = 1 draws the dark edge (good on small round things, wrong on the ground).

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 albedo = tex0.Sample(input.uv) * input.color;
    float3 n = normalize(input.normal);
    float3 sunDir = -normalize(frame.lightDir.xyz);
    float facing = dot(n, sunDir);

    // Three flat steps: in shade, lit, and a bright cap where the surface faces the sun squarely.
    float lit = smoothstep(0.0, 0.05, facing) * smoothstep(0.45, 0.55, sunShadow(input.world, n));
    float cap = smoothstep(0.80, 0.84, facing) * lit;
    float3 light = frame.ambient.rgb + frame.lightColor.rgb * (lit * 0.85 + cap * 0.25);
    float3 color = albedo.rgb * light;

    if (push.params[0].x > 0.5)
    {
        float3 toEye = normalize(frame.cameraPos.xyz - input.world);
        color *= lerp(0.30, 1.0, smoothstep(0.18, 0.24, dot(n, toEye)));
    }
    return float4(applyFog(color, input.world), albedo.a);
}
