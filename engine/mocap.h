// Motion capture with ARKit body tracking. iPhone only: implemented in ios/bodytrack.mm.
// Everywhere else these stubs answer that it isn't available, so games still run on the PC.
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
extern "C" {
int gamelab_mocap_start(char* error, size_t size);   // 1 if tracking started; otherwise why not
void gamelab_mocap_stop(void);
// The latest pose: joint positions in the body's space (x, y, z each), parents (-1 for the root). 1 if a body is tracked.
int gamelab_mocap_pose(float* positions, int* parents, int maxJoints, int* jointCount, char* error, size_t errorSize);
void gamelab_mocap_record(int on);                   // turning it on starts a new take
int gamelab_mocap_frames(void);                      // frames in the current take
char* gamelab_mocap_take(size_t* size);              // the take as text (malloc'd; free it), or null
char* gamelab_mocap_video(size_t* size);             // the take's video as an .mp4 (malloc'd), once finished, or null
// The camera picture, upright and small, as RGBA. serial changes with each new picture. 1 if there is one.
int gamelab_mocap_camera(unsigned char* rgba, int maxBytes, int* w, int* h, int* serial);
// Each joint on that picture, 0 to 1 across and down (x, y pairs). Returns the joint count, 0 while untracked.
int gamelab_mocap_screen(float* xy, int maxJoints);
}
#else
inline int gamelab_mocap_start(char* error, size_t size) {
    snprintf(error, size, "motion capture needs the iPhone (ARKit body tracking)");
    return 0;
}
inline void gamelab_mocap_stop(void) {}
inline int gamelab_mocap_pose(float*, int*, int, int* jointCount, char* error, size_t) { *jointCount = 0; error[0] = 0; return 0; }
inline void gamelab_mocap_record(int) {}
inline int gamelab_mocap_frames(void) { return 0; }
inline char* gamelab_mocap_take(size_t*) { return nullptr; }
inline char* gamelab_mocap_video(size_t*) { return nullptr; }
// With GAMELAB_FAKE_CAMERA=1 set, the PC shows a moving test picture, so the mocap screen can be worked on here.
inline int gamelab_mocap_camera(unsigned char* rgba, int maxBytes, int* w, int* h, int* serial) {
    static int frame = 0;
    if (!getenv("GAMELAB_FAKE_CAMERA") || maxBytes < 270 * 360 * 4) return 0;
    *w = 270;
    *h = 360;
    *serial = ++frame;
    for (int y = 0; y < 360; y++)
        for (int x = 0; x < 270; x++) {
            unsigned char* p = rgba + (y * 270 + x) * 4;
            p[0] = (unsigned char)(x + frame);
            p[1] = (unsigned char)(y - frame);
            p[2] = (unsigned char)(((x / 30 + y / 30 + frame / 20) % 2) * 255);
            p[3] = 255;
        }
    return 1;
}
inline int gamelab_mocap_screen(float*, int) { return 0; }
#endif
