// The ball's trail: one smooth ribbon instead of a row of shrinking balls.
//
// The mesh is a fixed strip (tools: built once in main.lua): each vertex has uv.x = how far along the trail it is
// (0 at the ball, 1 at the tail) and uv.y = which edge (-1 or +1). The trail's shape arrives each frame as six
// points, newest first: frame.user[0..3].xyz (gfx.frame_param) and push.params[0..1].xyz (gfx.param).
// frame.user[0].w is the width at the ball. The vertex shader runs a Catmull-Rom curve through the points, so the
// ribbon is smooth even with only six of them, turns it to face the camera, and narrows it toward the tail.
// The fragment shader fades it toward the tail and toward its edges, with a white-hot core near the ball.
// Drawn with gfx.blend("add"), so it glows and needs no sorting.

#include "common.slang"

float3 trailPoint(int i)
{
    i = clamp(i, 0, 5);
    if (i < 4)
        return frame.user[i].xyz;
    return push.params[i - 4].xyz;
}

float3 catmullRom(float3 a, float3 b, float3 c, float3 d, float f)
{
    return 0.5 * (2.0 * b + (c - a) * f + (2.0 * a - 5.0 * b + 4.0 * c - d) * f * f + (3.0 * b - a - 3.0 * c + d) * f * f * f);
}

float3 catmullRomSlope(float3 a, float3 b, float3 c, float3 d, float f)
{
    return 0.5 * ((c - a) + 2.0 * (2.0 * a - 5.0 * b + 4.0 * c - d) * f + 3.0 * (3.0 * b - a - 3.0 * c + d) * f * f);
}

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    float t = input.uv.x;
    float side = input.uv.y;
    float s = t * 5.0;
    int i = min(int(floor(s)), 4);
    float f = s - float(i);
    float3 a = trailPoint(i - 1), b = trailPoint(i), c = trailPoint(i + 1), d = trailPoint(i + 2);
    float3 centre = catmullRom(a, b, c, d, f);
    float3 along = catmullRomSlope(a, b, c, d, f);
    float3 toCamera = normalize(frame.cameraPos.xyz - centre);
    float3 across = cross(along, toCamera);
    float l = length(across);
    across = l > 1e-6 ? across / l : float3(0.0, 1.0, 0.0);   // a ball standing still: no trail to speak of
    float width = frame.user[0].w * (1.0 - 0.85 * t);

    VSOut o;
    o.world = centre + across * side * width;
    o.clip = mul(frame.viewProj, float4(o.world, 1.0));
    o.normal = toCamera;
    o.color = push.color;
    o.uv = input.uv;
    o.tangent = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float t = input.uv.x;
    float edge = 1.0 - input.uv.y * input.uv.y;              // 1 along the middle, 0 at the edges
    float fade = pow(1.0 - t, 1.6);                          // strong at the ball, gone at the tail
    float core = edge * edge * (1.0 - t);
    float3 color = lerp(input.color.rgb, float3(1.0, 1.0, 1.0), core * 0.6);
    return float4(color, input.color.a * fade * edge);       // add blending multiplies by alpha
}
