// Motion capture with ARKit body tracking. iPhone only: implemented in ios/bodytrack.mm.
// Everywhere else these stubs answer that it isn't available, so games still run on the PC.
#pragma once
#include <cstddef>
#include <cstdio>
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
#endif
