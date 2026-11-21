# Glossary

Terms you will meet in the code, in `dev.py`'s messages and in conversation.

## The project

**Loader**
:   The Game Lab app as installed on a test phone: every game bundled inside, a list to pick from, and Wi-Fi sync. The opposite of a standalone build.

**Standalone build**
:   One game built as its own app, with no game list and no sync code. This is what would go to the App Store. Made with `release <game>`.

**Live change / native change**
:   A change to data (Lua, shaders, assets) that travels over Wi-Fi, versus a change to compiled code that needs a new app. See [How work reaches the phone](shipping.md).

**Fingerprint, build code**
:   An eight-character code computed from all the native source files. It identifies exactly which C++ an app was built from.

**Hot reload**
:   The engine noticing a game's files changed and restarting that game's Lua, without restarting the app.

**Handle**
:   A number the engine gives Lua to stand for something it loaded or made (a texture, a mesh, a sound). Lua never holds a pointer. A handle to something that has been freed is detected and ignored. See `engine/handles.h` in [C++ headers](../reference/headers.md).

**Transform**
:   Up to ten numbers describing where something is: position, rotation (a quaternion) and scale.

**2D units**
:   The measurements used for on-screen drawing. The screen's shorter side is always 720 units, whatever the phone.

**Safe area**
:   The part of the screen not covered by a notch, Dynamic Island, rounded corner or home bar. `gfx.safe()` reports it.

## Graphics

**Vulkan**
:   The graphics interface the engine is written against. It is native on Windows and Android.

**MoltenVK**
:   A library that runs Vulkan on top of Metal, Apple's own graphics interface. It is how one renderer works on the iPhone.

**Slang**
:   The shading language shaders are written in. Close to HLSL.

**SPIR-V, `.spv`**
:   The compiled form of a shader that Vulkan loads. `dev.py` produces it from `.slang` files.

**Pipeline**
:   Vulkan's word for one complete way of drawing: which shader, which blending, whether depth is tested. The engine makes one the first time each combination is used.

**Push constants**
:   A small block of numbers sent with each thing drawn: its position matrix, colour and your `gfx.param` values.

**Draw call**
:   One instruction to the graphics chip to draw one mesh. Fewer is faster. ++f3++ shows the count.

**MSAA**
:   A way of smoothing jagged edges by sampling each pixel several times. The screen uses four samples.

**sRGB, linear**
:   Two ways of storing brightness. Colours you type are sRGB (how screens and art tools work); lighting maths needs linear. The engine converts for you.

**glTF, `.glb`**
:   The model file format the engine loads. Blender exports it.

**Skinning**
:   Bending a character's mesh to follow its skeleton. Done on the processor here, so any shader works on characters.

## Collision and sound

**Body**
:   One collision shape registered with the engine: a sphere, capsule, box or mesh.

**Layer, mask**
:   Bit sets that decide which bodies a collision question can see.

**Raycast**
:   Asking what a straight line hits first.

**Voice**
:   One sound currently playing. Playing the same sound twice gives two voices.

**Decibels (dB)**
:   The unit volumes are given in. `0` is full volume, `-6` about half, `-80` silent.

## Tools and the phone

**`dev.py`**
:   The one tool: watches files, serves the phone, builds, ships, checks layouts, serves this handbook.

**SideStore, LiveContainer**
:   Apps that let a phone install software without the App Store. Game Lab runs inside LiveContainer on the test phone.

**IPA**
:   An iPhone app as a file.

**Validation layers**
:   Part of the Vulkan SDK that checks every Vulkan call the engine makes and reports mistakes. Used on the PC when changing the renderer.

**ATS (App Transport Security)**
:   The iOS rule that apps may only make encrypted `https` connections unless they declare an exception.
