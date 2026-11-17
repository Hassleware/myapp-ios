// Cartoon cumulus. Each puff is a smooth ball whose surface is roughened by noise (cauliflower bumps), lit with
// a soft wrap of sunlight (warm white where the sun reaches, sky-blue shade where it doesn't), darker toward the
// cloud's base, with a silver rim strongest against the sun. Fogged like the rest.
//
// Each vertex's texture u is how high up its cloud it is (0 base, 1 top) and v the bump frequency (per metre);
// gfx.param(1, _, _, _, bumpStrength) sets how deep the bumps are.

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

float3 lin(float3 c) { return pow(c, 2.2); }

float hash3(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise3(float3 x)
{
    float3 i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(hash3(i + float3(0, 0, 0)), hash3(i + float3(1, 0, 0)), f.x),
                     lerp(hash3(i + float3(0, 1, 0)), hash3(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(hash3(i + float3(0, 0, 1)), hash3(i + float3(1, 0, 1)), f.x),
                     lerp(hash3(i + float3(0, 1, 1)), hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}

float bumps(float3 p) { return noise3(p) + 0.5 * noise3(p * 2.1 + 7.3); }

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 cfg = push.params[0];
    float3 n0 = normalize(input.normal);
    float3 p = input.world * input.uv.y;
    const float e = 0.15;
    float b = bumps(p);
    float3 g = float3(bumps(p + float3(e, 0, 0)) - b, bumps(p + float3(0, e, 0)) - b, bumps(p + float3(0, 0, e)) - b) / e;
    float3 n = normalize(n0 - (g - n0 * dot(g, n0)) * cfg.w);

    float3 v = normalize(frame.cameraPos.xyz - input.world);
    float3 sunDir = -normalize(frame.lightDir.xyz);
    float wrap = saturate(dot(n, sunDir) * 0.6 + 0.5);
    float h = input.uv.x;

    float3 shade = lin(float3(0.64, 0.76, 0.96));
    float3 lit = lin(float3(1.0, 0.99, 0.96)) * 1.06;
    float3 c = lerp(shade, lit, smoothstep(0.0, 1.0, wrap));
    c *= lerp(0.86, 1.0, smoothstep(0.0, 0.7, h));            // the base of a cloud is in its own shadow
    c += lin(float3(0.55, 0.75, 1.0)) * saturate(n.y) * 0.08; // skylight on the tops
    c *= lerp(0.72, 1.0, smoothstep(0.2, 1.2, b));                           // a little depth in the crevices

    float rim = pow(1.0 - saturate(dot(n0, v)), 3.0);
    float against = saturate(dot(-v, sunDir)) * 0.6 + 0.4;    // silver lining, brightest against the sun
    c = lerp(c, float3(1.0, 1.0, 1.0), rim * 0.7 * against);
    return float4(applyFog(c, input.world), 1.0);
}
