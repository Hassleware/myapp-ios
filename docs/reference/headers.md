# C++ headers

Each subsystem's header is its contract: the functions other files may call, with a comment on anything a caller needs to know. Reading a header is the fastest way to learn what a subsystem can do.

These are the files themselves, read when this page was built.

## gfx.h: the renderer

Explained in [Renderer](../engine/renderer.md).

::: file engine/gfx.h

## model.h: models and animation

Explained in [Models and animation](../engine/models.md).

::: file engine/model.h

## collide.h: collision

Explained in [Collision](../engine/collision.md).

::: file engine/collide.h

## audio.h: sound

Explained in [Audio](../engine/audio.md).

::: file engine/audio.h

## input.h: touch, sensors, keyboard

Explained in [Input and keyboard](../engine/input.md).

::: file engine/input.h

## net.h: web calls

Explained in [Web calls](../engine/networking.md).

::: file engine/net.h

## script.h: the Lua host

Explained in [A frame, start to finish](../engine/lifecycle.md).

::: file engine/script.h

## sync.h: Wi-Fi sync (loader only)

Explained in [Wi-Fi sync](../engine/sync.md).

::: file engine/sync.h

## pool.h: handles

Used by every subsystem that hands numbers to Lua.

::: file engine/pool.h
