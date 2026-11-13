// Toon water: two copies of one normal map slide across each other to make the ripples,
// the colour steps from shallow to deep, and foam lines roll in to the shore.
// Slot 2 holds the normal map. The island's shape below must match island_height in main.lua.

#include "common.slang"

float islandHeight(float2 p)
{
    float2 q = p - float2(2.5, -1.5);
    return 1.6 * exp(-dot(p, p) / 10.24) + 0.5 * exp(-dot(q, q) / 3.0) - 0.35;
}

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    float t = frame.params.x;
    float4 world = mul(push.model, float4(input.position, 1.0));
    // A slow swell. It flattens out at the shore so the waterline does not climb the beach.
    float open = saturate(-islandHeight(world.xz) / 0.35);
    world.y += (sin(world.x * 0.35 + t * 0.9) * 0.07 + sin(world.z * 0.5 - t * 1.2) * 0.05) * open;
    o.world = world.xyz;
    o.normal = float3(0, 1, 0);
    o.tangent = float4(1, 0, 0, 1);
    o.color = input.color * push.color;
    o.uv = world.xz * 0.09;
    o.clip = mul(frame.viewProj, world);
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float t = frame.params.x;

    // The two sliding layers, at different sizes and headings so the pattern never visibly repeats.
    float3 a = tex1.Sample(input.uv + float2(t * 0.030, t * 0.018)).xyz * 2.0 - 1.0;
    float3 b = tex1.Sample(input.uv * 2.3 + float2(-t * 0.024, t * 0.041)).xyz * 2.0 - 1.0;
    float2 ripple = a.xy + b.xy;
    float3 n = normalize(float3(ripple.x * 0.55, 1.0, ripple.y * 0.55));

    // 0 at the waterline, 1 in open water. The ripples nudge it so the colour bands have wavy edges.
    float depth = saturate(-islandHeight(input.world.xz) / 0.35);
    float wavy = depth + ripple.x * 0.035;

    float3 shallow = float3(0.33, 0.83, 0.78), mid = float3(0.08, 0.52, 0.72), deep = float3(0.03, 0.26, 0.55);
    float3 color = lerp(shallow, mid, smoothstep(0.30, 0.34, wavy));
    color = lerp(color, deep, smoothstep(0.80, 0.84, wavy));

    float3 sunDir = -normalize(frame.lightDir.xyz);
    float3 toEye = normalize(frame.cameraPos.xyz - input.world);
    color *= lerp(0.72, 1.0, smoothstep(0.45, 0.55, sunShadow(input.world, float3(0, 1, 0))));
    color *= lerp(0.88, 1.0, smoothstep(0.50, 0.54, dot(n, sunDir)));

    // Hard-edged sun glints.
    float glint = smoothstep(0.988, 0.992, dot(n, normalize(sunDir + toEye)));

    // Foam: a ragged band on the sand, and lines that travel in from a little way out.
    float edge = 1.0 - smoothstep(0.05, 0.08, depth + ripple.y * 0.03 + 0.02 * sin(t * 1.4 + input.world.x * 2.0));
    float roll = frac(depth * 5.0 + t * 0.22);
    float lines = smoothstep(0.86, 0.90, roll + ripple.x * 0.12) * (1.0 - smoothstep(0.30, 0.55, depth)) * smoothstep(0.04, 0.10, depth);
    float foam = saturate(edge + lines);

    color = lerp(color, float3(1.0, 1.0, 1.0), saturate(foam + glint));
    float alpha = lerp(0.55, 1.0, smoothstep(0.0, 0.5, depth));   // clear at the shore, solid further out
    return float4(applyFog(color, input.world), saturate(alpha + foam));
}
