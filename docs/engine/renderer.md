# Renderer

`engine/gfx.cpp` is the renderer and the largest file in the engine. It is the only code that talks to Vulkan. On the iPhone, MoltenVK translates those Vulkan calls to Apple's Metal. Read this page before opening the file.

What it offers is listed in `gfx.h` on the [C++ headers](../reference/headers.md) page. What games see of it is under `gfx` in the [Lua API](../reference/lua-api.md).

## The one idea to hold on to

**Drawing is recorded first and performed later.**

When a game calls `gfx.cube(...)` inside `draw()`, nothing is drawn. The renderer writes down *what* to draw: which mesh, which pipeline, which textures, and its matrix and colour.

::: source engine/gfx.cpp Draw

At the end of the frame, `end_frame` replays everything in the right order. This is what makes it possible to:

- draw the scene twice (once from the sun for shadows, once from the camera) from a single `draw()`;
- sort see-through things from far to near, whatever order the game drew them in;
- draw 2D on top of 3D without the game thinking about it.

## The order of a frame

`end_frame` runs these passes:

| Order | Pass | Contents |
|---|---|---|
| 1 | Shadow | Solid things, seen from the sun, depth only, into a 2048 x 2048 map |
| 2 | Each texture target | Whatever the game drew between `gfx.target(t)` and `gfx.target()` |
| 3 | Screen: sky | The panorama, if any |
| 4 | Screen: solid | Everything opaque |
| 5 | Screen: see-through | Sorted far to near |
| 6 | Screen: 2D | Rectangles, images, text, touch controls, in the order drawn |

## Fixed formats

To stay small, the renderer has exactly one of several things that bigger engines make configurable.

**One vertex format** for every mesh: position, normal, colour, texture coordinate, tangent. 64 bytes.

**One block of per-frame data**, filled once per pass:

::: source engine/gfx.cpp FrameData

**One block of per-draw data**, 128 bytes, sent as push constants:

::: source engine/gfx.cpp PushData

!!! danger "These two structs must match the shaders"
    `FrameData` and `PushData` are the C++ view of `Frame` and `Push` in `engine/shaders/common.slang`. The graphics chip reads the same bytes both describe. Change one side and you must change the other in the same commit, recompile every shader, and ship. Compare them on the [Shader interface](../reference/shader-interface.md) page.

**Four texture slots per material**, bound together as one set.

## Pipelines are made on demand

In Vulkan, a pipeline is a complete recipe: shader, blend mode, depth settings, culling, the pass it is used in. The engine does not ask games to declare these up front. The first time a combination is used, the pipeline is created and remembered.

The cost is a short hitch the first time a new combination appears. On the phone, MoltenVK also translates the shader to Metal at that moment. If that hitch matters in a game, draw one of each thing off-screen during loading.

## Colour

Art tools and screens use sRGB, where numbers are not proportional to light. Lighting maths only works in linear light. The engine handles this in three places so that games never have to:

- Colours passed from Lua are converted to linear on the processor.

    ::: source engine/gfx.cpp srgb_to_linear

- Textures are stored in an sRGB format, so the chip converts them when a shader samples them. Textures loaded with the `"data"` option (normal maps) are left alone.
- The screen is an sRGB format, so the chip converts back when writing.

!!! bug "Known wart"
    Vertex colours in meshes built from Lua with `gfx.new_mesh` are **not** converted. Convert them in Lua (`c ^ 2.2` is close enough) until the engine does it.

## Things to know before changing it

- **Triangles wind counter-clockwise** seen from outside, as in glTF. Back faces are culled unless the game calls `gfx.cull(false)`.
- **The screen is drawn with 4x MSAA** where the device supports it. Texture targets and the shadow map are not multisampled. `GAMELAB_NO_MSAA=1` turns it off on the PC.
- **2D uses a virtual screen whose shorter side is 720 units.**
- **Text** is drawn from a built-in 8x8 font as rectangles, or from a `.ttf` whose letters are drawn into a 1024-pixel atlas the first time each is needed.
- **Desktop loads Vulkan through volk; the iPhone links MoltenVK directly** and must not use volk, because their function names collide. `gfx.cpp` switches on `USE_VOLK`.
- **One frame is in flight at a time**, and freeing a resource waits for the chip to finish. Simple and correct; marked `ponytail:`.
- **Shaders are SPIR-V 1.5**, which is why the engine asks for Vulkan 1.2.

## Working on the renderer

Always run with the validation layers on. They check every Vulkan call and name the rule you broke:

```powershell
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
python tools/dev.py run <game>
```

A clean validation run on the PC is necessary but not sufficient: MoltenVK supports slightly less than a desktop driver. Test anything new on the phone.

To capture a frame without a person at the keyboard, configure the build with `-DGFX_SCREENSHOT=ON` and set `GAMELAB_SHOT=out.bmp`. See [Debugging](../guides/debugging.md).

## Exercises

??? example "1. A game draws a glass window, then a wall behind it. Does the wall show through?"
    Yes. The wall is opaque, so it is drawn in the solid step. The glass (with `gfx.blend("alpha")`) is drawn afterwards in the see-through step, whatever order the Lua used.

??? example "2. Why are shadows only drawn for solid things?"
    The shadow pass replays the opaque list. See-through things would need their own handling; today they cast none.

??? example "3. Where would you add a fifth texture slot?"
    You would have to change the material's descriptor set layout in `gfx.cpp`, add `tex4` to `common.slang`, extend `gfx.texture` in `lua_gfx.cpp`, recompile all shaders and ship. That is why there are four.
