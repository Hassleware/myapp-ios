# Motion capture

Game Lab can record a person's movement with the iPhone's back camera and turn it into a `.bvh` file: an animated armature that Blender imports and you retarget onto your own characters. The phone does the tracking itself (Apple's ARKit body tracking), so no PC mocap software is needed.

## Recording a take

1. Keep `python tools/dev.py` running on the PC.
2. On the phone, open the **mocap** game. The first time, iOS asks for the camera.
3. Prop the phone up about 3 metres away at waist height, or have someone hold it, with the whole body in view. It uses the **back** camera (ARKit only tracks bodies with that one). The camera picture shows faintly behind the skeleton (`CAMERA_ALPHA` in `main.lua`), and the skeleton turns blue on the person when they are tracked.
4. Tap **RECORD**. After a 3-second countdown, perform. Tap **STOP**.
5. Tap **SEND**. The take arrives as `captures/take-N.bvh` in the repository (git ignores the folder), and `dev.py` prints where it went.

In Blender: **File > Import > Motion Capture (.bvh)**, scale 1 (one unit is a metre), forward **-Z**, up **Y**. Then retarget the motion onto your armature, for example with Rokoko's free Blender add-on, and export your character as `.glb` with its animations (see [Models and animation](models.md)).

## GVHMR: a stronger model on the PC

While recording, the phone also films the take (H.264, 30 frames a second). SEND uploads the video too, and the PC runs **GVHMR** (world-grounded human motion recovery, SIGGRAPH Asia 2024) on its graphics card. GVHMR is more accurate than ARKit at depth and keeps feet planted. The phone shows the PC's progress, then replays GVHMR's result (**REPLAY** switches between that and the live camera). The result is saved as `captures/<name>-gvhmr.bvh`.

| Step | Where |
|---|---|
| The phone writes the video | `ios/bodytrack.mm` (`AVAssetWriter`); `mocap.video()` hands it to Lua |
| The PC receives it, makes it upright at 30 fps (ffmpeg), runs GVHMR, tracks the job | `backends/mocap/server.py` (`/video`, `/job`, `/result`) |
| GVHMR itself, without its video renders, then the `.bvh` and the phone's joint file | `tools/gvhmr/run_take.py`, run in WSL Ubuntu |
| Installing GVHMR (Miniforge, PyTorch 2.10 for RTX 50 cards, PyTorch3D, the weights) | `tools/gvhmr/install.sh` |
| Checking downloaded model files can't run code when loaded | `tools/gvhmr/scan_checkpoint.py` |

### Setting it up on a new PC

1. Windows with an NVIDIA card and WSL Ubuntu.
2. `wsl -d Ubuntu -- bash /mnt/c/<path to repo>/tools/gvhmr/install.sh`
3. The body model, which needs a free account: register at smpl-x.is.tue.mpg.de, download **SMPL-X v1.1 (NPZ+PKL)**, and put `SMPLX_NEUTRAL.npz` in the repo's `tools/.bin/body_models/smplx/` (git ignores `tools/.bin`; the licence forbids sharing the file). Run the install script again to copy it in.

!!! warning "Licences"
    GVHMR's weights and the SMPL-X body model are for **non-commercial research**. Motion made this way is fine for learning and prototypes; a game you sell needs a commercial licence (Meshcapade sells SMPL-X licences) or a different model.

The model weights came from a Hugging Face mirror because GVHMR's Google Drive was over its download limit. Each file was checked with `scan_checkpoint.py` before use; YOLO came from Ultralytics' own release.

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
