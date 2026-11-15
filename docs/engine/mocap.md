# Motion capture

Game Lab can record a person's movement with the iPhone's back camera and turn it into a `.bvh` file: an animated armature that Blender imports and you retarget onto your own characters. The phone does the tracking itself (Apple's ARKit body tracking), so no PC mocap software is needed.

## Recording a take

1. Keep `python tools/dev.py` running on the PC.
2. On the phone, open the **mocap** game. The first time, iOS asks for the camera.
3. Prop the phone up about 3 metres away at waist height, or have someone hold it, with the whole body in view. It uses the **back** camera (ARKit only tracks bodies with that one). The camera picture shows faintly behind the skeleton (`CAMERA_ALPHA` in `main.lua`), and the skeleton turns blue on the person when they are tracked.
4. Tap **RECORD**. After a 3-second countdown, perform. Tap **STOP**.
5. Tap **SEND**. The take arrives as `captures/take-N.bvh` in the repository (git ignores the folder), and `dev.py` prints where it went.

In Blender: **File > Import > Motion Capture (.bvh)**, scale 1 (one unit is a metre), forward **-Z**, up **Y**. Then retarget the motion onto your armature, for example with Rokoko's free Blender add-on, and export your character as `.glb` with its animations (see [Models and animation](models.md)).

## How it fits together

| Part | Where | Changes need |
|---|---|---|
| Body tracking and recording | `ios/bodytrack.mm` (ARKit) | An app build |
| The Lua functions | `mocap.*` in `engine/script.cpp`, declared in `engine/mocap.h` | An app build |
| The recording screen | `games/mocap/main.lua` | Nothing: it syncs |
| Turning a take into `.bvh` | `backends/mocap/server.py`, run by `dev.py` | Nothing: save and the next take uses it |

The phone sends the raw take (each joint's turn from its parent, every frame, with timestamps) through `net.backend`. The conversion to BVH happens in Python on the PC, so it can be changed without rebuilding the app:

::: source backends/mocap/server.py to_bvh

## Limits

- **iPhone XS / XR or newer** (an A12 chip). On the PC, `mocap.start()` answers that it needs the iPhone.
- **ARKit's skeleton is coarse.** It has 91 joints, but fingers, toes and the face are not tracked; they keep their resting pose. Expect to clean up feet and hands in Blender.
- **One person, facing roughly towards the camera,** in good light.
- **A take is about 10 MB of text a minute** on its way to the PC; the `.bvh` is smaller.
- **Untested on a phone.** The PC side (the screen, sending, and the BVH conversion) has been tested with a stand-in skeleton. Camera permission inside LiveContainer is the biggest unknown: if LiveContainer doesn't declare camera access, iOS refuses the camera to every app inside it.

## The Lua functions

::: lua-api mocap
