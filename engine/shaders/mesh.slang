// The engine's standard shader. It draws lit 3D meshes and flat 2D shapes.
//
// To write your own, copy this file into a game's shaders/ folder under a new
// name, change fsMain, and select it from Lua with gfx.shader("yourname").
// tools/dev.py compiles it when you save. On the iPhone, MoltenVK converts the
// compiled shader to Metal Shading Language automatically.

struct Frame
{
    float4x4 viewProj;   // 3D camera
    float4x4 ortho;      // 2D screen space
    float4 lightDir;     // direction the light travels
    float4 params;       // x: seconds since the game started, y: width, z: height in pixels
};
[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;

struct Push
{
    float4x4 model;
    float4 color;
    float4 flags;        // x: 1 for 2D drawing
};
[[vk::push_constant]] Push push;

struct VSIn
{
    float3 position;
    float3 normal;
    float4 color;
};

struct VSOut
{
    float4 clip : SV_Position;
    float3 normal;
    float4 color;
    float3 world;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    float4 world = mul(push.model, float4(input.position, 1.0));
    o.world = world.xyz;
    o.normal = mul(push.model, float4(input.normal, 0.0)).xyz;
    o.color = input.color * push.color;
    if (push.flags.x > 0.5)
        o.clip = mul(frame.ortho, world);
    else
        o.clip = mul(frame.viewProj, world);
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    if (push.flags.x > 0.5)
        return input.color;

    float3 n = normalize(input.normal);
    float diffuse = max(dot(n, -normalize(frame.lightDir.xyz)), 0.0);
    float3 lit = input.color.rgb * (0.35 + 0.65 * diffuse);
    return float4(lit, input.color.a);
}
