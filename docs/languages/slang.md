# Slang: the language of shaders

A shader is a small program that runs on the graphics chip. In this engine each shader has two parts: a **vertex** function that places each corner of a triangle on the screen, and a **fragment** function that decides the colour of each pixel inside it. Shaders are written in Slang, which reads almost exactly like HLSL.

Shader changes are **live**: save a `.slang` file and both the PC window and the phone update.

## Your first shader

Shaders live in a game's `shaders/` folder. Create `games/<your-game>/shaders/stripes.slang`:

```hlsl
#include "common.slang"

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    return standardVertex(input);
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 color = standardColor(input);                  // the engine's normal lit look
    float stripe = step(0.5, frac(input.world.y * 4.0));  // 0 or 1, in bands going up
    color.rgb *= lerp(0.6, 1.0, stripe);
    return color;
}
```

Use it from Lua:

```lua
function draw()
  gfx.clear(0.1, 0.1, 0.15)
  gfx.camera(0, 3, 6, 0, 0.5, 0, 50)
  gfx.shader("stripes")                      -- everything after this uses your shader
  gfx.sphere(0, 1, 0, 2, 0.9, 0.4, 0.3)
  gfx.shader()                               -- back to the standard one
  gfx.plane(0, 0, 0, 10, 10, 0.3, 0.3, 0.35)
end
```

Save both. You should see a striped ball on a plain floor. Change the `4.0` and save again.

!!! info "What `dev.py` just did"
    It compiled `stripes.slang` twice, once for each function, into `stripes.vert.spv` and `stripes.frag.spv` next to it. Those files are what the engine loads and what the phone downloads. They are committed to git, so the GitHub build needs no shader compiler.

    ::: source tools/dev.py compile_shader

If the shader has a mistake, the compiler's message appears in the `dev.py` window, and the engine keeps using the last version that worked.

## The engine's standard shader

It is short, because all the work is in functions you can call too:

::: file engine/shaders/mesh.slang

To change the look of a game, copy this file into the game's `shaders/` folder under a new name and edit `fsMain`.

## What a shader can read

Every shader starts with `#include "common.slang"`. That file is the contract between shaders and the C++ renderer. Three things in it matter most.

**The frame block** is the same for everything drawn in a frame: camera, lights, fog, time.

::: source engine/shaders/common.slang Frame

**The push block** is sent with each thing drawn: where it is and what colour.

::: source engine/shaders/common.slang Push

**The vertex** is what each corner of a mesh carries, and `VSOut` is what the vertex function passes on to the fragment function.

::: source engine/shaders/common.slang VSIn VSOut

The full file, with the lighting functions, is on the [Shader interface](../reference/shader-interface.md) page.

!!! danger "Never change these structs in a game's shader"
    The C++ side fills this memory in exactly this layout (`FrameData` and `PushData` in `engine/gfx_internal.h`). If the two disagree, everything draws wrong. Changing them is an engine change: both sides together, then `ship`.

## Sending your own numbers to a shader

| From Lua | Read in Slang as | Lifetime |
|---|---|---|
| `gfx.param(1, x, y, z, w)` | `push.params[0]` | Each thing drawn |
| `gfx.param(2, x, y, z, w)` | `push.params[1]` | Each thing drawn |
| `gfx.frame_param(1..4, x, y, z, w)` | `frame.user[0..3]` | The whole frame |
| `gfx.texture(1..4, t)` | `tex0` to `tex3` | Until changed |
| (automatic) | `frame.params.x` | Seconds since the game started |

Note the off-by-one: Lua counts from 1, Slang from 0.

A dissolve effect, driven from Lua:

```hlsl
float4 fsMain(VSOut input) : SV_Target
{
    float4 color = standardColor(input);
    float noise = frac(sin(dot(input.world.xz, float2(12.9898, 78.233))) * 43758.5453);
    if (noise < push.params[0].x) discard;        // gfx.param(1, amount, 0, 0, 0)
    return color;
}
```

```lua
gfx.shader("dissolve")
gfx.param(1, (math.sin(sys.time()) + 1) / 2, 0, 0, 0)
gfx.cube(0, 1, 0, 2, 2, 2, 0.4, 0.7, 1.0)
```

## The building blocks

These come from `common.slang`. Use them instead of rewriting them.

| Function | Gives you |
|---|---|
| `standardVertex(input)` | The usual vertex work: position on screen, world position, normal |
| `standardColor(input)` | The complete standard look: texture, lights, shadows, fog |
| `surfaceNormal(input)` | The surface direction, bent by the normal map in slot 2 if there is one |
| `lightAt(world, n)` | All light arriving at a point: ambient, sun with shadow, point and spot lights |
| `sunShadow(world, n)` | 1 in sunlight, lower in shadow |
| `applyFog(color, world)` | The colour after distance fog |

How the lights are added up:

::: source engine/shaders/common.slang lightAt

## Slang for people who know another shading language

| You know | In Slang here |
|---|---|
| GLSL `vec3`, `mat4` | `float3`, `float4x4` |
| GLSL `mix`, `fract` | `lerp`, `frac` |
| GLSL `texture(sampler, uv)` | `tex0.Sample(uv)` |
| GLSL `matrix * vector` | `mul(matrix, vector)` |
| GLSL `gl_Position` | The `SV_Position` field of the vertex output |
| HLSL | Nearly everything is the same |

Useful built-ins: `saturate(x)` clamps to 0..1, `step(edge, x)` is 0 or 1, `smoothstep(a, b, x)` is a soft step, `dot`, `cross`, `normalize`, `length`, `reflect`.

## Rules that will save you time

- **The entry points must be called `vsMain` and `fsMain`.** `dev.py` compiles those names.
- **Colours are in linear light inside a shader.** The engine converts the colours you give in Lua, and textures are converted when sampled. Do not apply gamma yourself.
- **See-through things need `gfx.blend("alpha")` in Lua** as well as an alpha below 1 from the shader. The blend mode is a Lua setting, not a shader one.
- **Test on the phone early.** The shader is translated to Apple's Metal language on the device. Almost everything translates, but the phone is the only place to be sure, and its chip is slower than your PC's.

## Exercises

??? example "1. Make the stripes move"
    Add time: `frac(input.world.y * 4.0 + frame.params.x)`.

??? example "2. Toon shading"
    Replace smooth lighting with steps. Compute `float facing = dot(normalize(input.normal), -normalize(frame.lightDir.xyz));` then use `smoothstep(0.0, 0.05, facing)` as the amount of sun, so a surface is either lit or not.

??? example "3. A rim glow"
    ```hlsl
    float3 toEye = normalize(frame.cameraPos.xyz - input.world);
    float rim = pow(1.0 - saturate(dot(normalize(input.normal), toEye)), 3.0);
    color.rgb += float3(0.2, 0.5, 1.0) * rim;
    ```

## Where next

- [Shader interface](../reference/shader-interface.md): all of `common.slang`.
- [Renderer](../engine/renderer.md): what the C++ does before and after your shader runs.
