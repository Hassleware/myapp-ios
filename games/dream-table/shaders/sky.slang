// The sky, worked out per pixel from the direction you look: deep blue overhead, bright toward the horizon,
// a pale cloud sea below it; a sun with a soft glow; and thin wispy clouds drifting slowly high up.
// Used with gfx.sky (the texture is ignored).

#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

float3 lin(float3 c) { return pow(c, 2.2); }

float hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float noise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float fbm(float2 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++)
    {
        s += a * noise(p);
        p = p * 2.03 + float2(17.1, 3.7);
        a *= 0.5;
    }
    return s;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 dir = normalize(input.world - frame.cameraPos.xyz);
    float up = dir.y;
    float3 sunDir = -normalize(frame.lightDir.xyz);

    float3 zenith = lin(float3(0.04, 0.22, 0.70));
    float3 mid = lin(float3(0.14, 0.45, 0.92));
    float3 horizon = lin(float3(0.50, 0.75, 1.0));
    float3 below = lin(float3(0.36, 0.60, 0.96));
    float3 c;
    if (up >= 0.0)
    {
        c = lerp(horizon, mid, smoothstep(0.0, 0.15, up));
        c = lerp(c, zenith, smoothstep(0.18, 0.75, up));
    }
    else
        c = lerp(horizon, below, smoothstep(0.0, -0.35, up));

    // Wispy clouds: noise on a high flat layer, stretched into streaks, thinning toward the horizon.
    if (up > 0.0)
    {
        float2 p = dir.xz / (up + 0.12) * 0.9 + float2(frame.params.x * 0.004, 0.0);
        float n = fbm(float2(p.x * 0.7, p.y * 2.2));
        float w = smoothstep(0.52, 0.85, n) * smoothstep(0.01, 0.2, up);
        float3 wisp = lerp(lin(float3(0.84, 0.92, 1.0)), float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.9, n));
        c = lerp(c, wisp, w * 0.8);
    }

    // The sun and its glow.
    float s = saturate(dot(dir, sunDir));
    c += lin(float3(1.0, 0.95, 0.85)) * (pow(s, 900.0) * 6.0 + pow(s, 60.0) * 0.35 + pow(s, 6.0) * 0.12);
    return float4(c, 1.0);
}
