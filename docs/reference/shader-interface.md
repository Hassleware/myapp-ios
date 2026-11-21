# Shader interface

`engine/shaders/common.slang` is included by every shader. It declares the data the engine sends to the graphics chip and provides the standard lighting as functions. The tutorial is [Slang](../languages/slang.md); this page is the reference.

## From Lua to a shader

| Lua | Slang |
|---|---|
| `gfx.param(1, x, y, z, w)` | `push.params[0]` |
| `gfx.param(2, x, y, z, w)` | `push.params[1]` |
| `gfx.frame_param(n, x, y, z, w)`, n from 1 to 4 | `frame.user[n - 1]` |
| `gfx.texture(1, t)` to `gfx.texture(4, t)` | `tex0` to `tex3` |
| The colour passed to a draw call | `push.color`, already multiplied into `input.color` by `standardVertex` |
| `gfx.camera`, `gfx.light`, `gfx.ambient`, `gfx.fog`, `gfx.shadows`, the point and spot lights | Fields of `frame` |
| (automatic) | `frame.params`: x seconds since the game started, y and z the size in pixels, w the number of lights |

## The two sides of the contract

The C++ renderer fills memory in exactly the layout the shader declares. These pairs must always match.

### Per frame

The shader's view:

::: source engine/shaders/common.slang Frame

The engine's view:

::: source engine/gfx_internal.h FrameData

### Per draw

The shader's view:

::: source engine/shaders/common.slang Push

The engine's view:

::: source engine/gfx_internal.h PushData

!!! danger "Changing either pair"
    Change both sides in the same commit, recompile every shader (`python tools/dev.py shaders`), and `ship`. Game shaders already on a phone were compiled against the old layout and will draw wrongly until they are recompiled and synced.

## The whole file

::: file engine/shaders/common.slang

## The engine's own shaders

::: file engine/shaders/mesh.slang

::: file engine/shaders/shadow.slang
