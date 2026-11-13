// What every shader shares with the engine: the data blocks, the vertex layout and the standard
// lighting. A game's shader starts with  #include "common.slang"  and then writes its own
// vsMain and fsMain, calling the helpers below for the parts it doesn't want to change.

struct Frame
{
    float4x4 viewProj;      // 3D camera
    float4x4 ortho;         // 2D screen space
    float4x4 shadowMat;     // world -> the sun's shadow map
    float4 lightDir;        // xyz: direction the sunlight travels. w: shadow strength, 0 = no shadows
    float4 lightColor;      // sun colour
    float4 ambient;         // light that reaches everything
    float4 cameraPos;
    float4 fogColor;        // a: 1 when fog is on
    float4 fogRange;        // x: start distance, y: end distance
    float4 shadowInfo;      // x: one shadow-map pixel, y: depth bias, z: normal offset
    float4 params;          // x: seconds since the game started, y: width, z: height in pixels, w: number of lights
    float4 user[4];         // gfx.frame_param(1..4, ...)
    float4 lightPos[8];     // xyz: position, w: radius
    float4 lightCol[8];     // rgb: colour, a: 1 for a spot light
    float4 lightAim[8];     // spot lights. xyz: direction, w: cosine of the cone's half angle
};
[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;
[[vk::binding(1, 0)]] Sampler2D shadowMap;

// gfx.texture(1..4, handle). Slot 1 is the colour texture, slot 2 the normal map; 3 and 4 are free.
[[vk::binding(0, 1)]] Sampler2D tex0;
[[vk::binding(1, 1)]] Sampler2D tex1;
[[vk::binding(2, 1)]] Sampler2D tex2;
[[vk::binding(3, 1)]] Sampler2D tex3;

struct Push
{
    float4x4 model;
    float4 color;
    float4 flags;           // x: 1 for 2D drawing, y: 1 for unlit, z: alpha cut-off, w: 1 for the sky
    float4 params[2];       // gfx.param(1..2, ...)
};
[[vk::push_constant]] Push push;

struct VSIn
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
    float4 tangent;         // xyz: direction of increasing u, w: handedness. All zero when the mesh has none
};

struct VSOut
{
    float4 clip : SV_Position;
    float3 normal;
    float4 color;
    float3 world;
    float2 uv;
    float4 tangent;
};

// Normals must turn with the surface even when the model is stretched unevenly.
float3 transformNormal(float3 n)
{
    // The matrix of cofactors: the inverse transpose without the division.
    float3 a = push.model[0].xyz, b = push.model[1].xyz, c = push.model[2].xyz;
    return mul(float3x3(cross(b, c), cross(c, a), cross(a, b)), n);
}

VSOut standardVertex(VSIn input)
{
    VSOut o;
    float4 world = mul(push.model, float4(input.position, 1.0));
    o.world = world.xyz;
    o.normal = transformNormal(input.normal);
    o.tangent = float4(mul(push.model, float4(input.tangent.xyz, 0.0)).xyz, input.tangent.w);
    o.color = input.color * push.color;
    o.uv = input.uv;
    if (push.flags.x > 0.5)
        o.clip = mul(frame.ortho, world);
    else
        o.clip = mul(frame.viewProj, world);
    return o;
}

// The surface normal, bent by the normal map in slot 2 when the mesh has tangents.
float3 surfaceNormal(VSOut input)
{
    float3 n = normalize(input.normal);
    if (dot(input.tangent.xyz, input.tangent.xyz) < 0.25)
        return n;
    float3 t = normalize(input.tangent.xyz - n * dot(n, input.tangent.xyz));
    float3 b = cross(n, t) * input.tangent.w;
    float3 m = tex1.Sample(input.uv).xyz * 2.0 - 1.0;
    return normalize(t * m.x + b * m.y + n * m.z);
}

// 1 where the sun reaches, down to (1 - strength) in shadow.
float sunShadow(float3 world, float3 n)
{
    if (frame.lightDir.w <= 0.0)
        return 1.0;
    float4 p = mul(frame.shadowMat, float4(world + n * frame.shadowInfo.z, 1.0));
    float2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0)
        return 1.0;
    float lit = 0.0;
    float t = frame.shadowInfo.x;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
        {
            float depth = shadowMap.SampleLevel(uv + (float2(x, y) - 0.5) * t, 0).r;
            lit += (p.z - frame.shadowInfo.y <= depth) ? 0.25 : 0.0;
        }
    return lerp(1.0, lit, frame.lightDir.w);
}

// All the light arriving at a point: ambient, the sun and the point and spot lights.
float3 lightAt(float3 world, float3 n)
{
    float3 sunDir = -normalize(frame.lightDir.xyz);
    float3 total = frame.ambient.rgb + frame.lightColor.rgb * max(dot(n, sunDir), 0.0) * sunShadow(world, n);
    int count = int(frame.params.w);
    for (int i = 0; i < count; i++)
    {
        float3 to = frame.lightPos[i].xyz - world;
        float dist = length(to);
        float3 dir = to / max(dist, 0.0001);
        float fall = saturate(1.0 - dist / frame.lightPos[i].w);
        float amount = fall * fall * max(dot(n, dir), 0.0);
        if (frame.lightCol[i].a > 0.5)
        {
            float c = dot(-dir, normalize(frame.lightAim[i].xyz));
            amount *= smoothstep(frame.lightAim[i].w, lerp(frame.lightAim[i].w, 1.0, 0.25), c);
        }
        total += frame.lightCol[i].rgb * amount;
    }
    return total;
}

float3 applyFog(float3 color, float3 world)
{
    if (frame.fogColor.a < 0.5)
        return color;
    float d = distance(world, frame.cameraPos.xyz);
    float f = saturate((d - frame.fogRange.x) / max(frame.fogRange.y - frame.fogRange.x, 0.0001));
    return lerp(color, frame.fogColor.rgb, f);
}

// The whole standard look. Custom shaders can call this and then change the result.
float4 standardColor(VSOut input)
{
    float4 albedo = tex0.Sample(input.uv) * input.color;
    if (albedo.a < push.flags.z)
        discard;
    if (push.flags.x > 0.5)
        return albedo;
    if (push.flags.w > 0.5)
        return albedo;                      // the sky: beyond the fog
    if (push.flags.y > 0.5)
        return float4(applyFog(albedo.rgb, input.world), albedo.a);
    float3 n = surfaceNormal(input);
    return float4(applyFog(albedo.rgb * lightAt(input.world, n), input.world), albedo.a);
}
