// Motion capture: ARKit body tracking through the back camera. Records the armature's motion, frame by frame,
// as text that backends/<game>/server.py turns into a .bvh file Blender can import.
// Needs an A12 chip or newer (iPhone XS / XR and later). Declared for C++ in engine/mocap.h.
#import <ARKit/ARKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

static std::mutex lock;
static std::vector<std::string> names;
static std::vector<int> parents;
static std::vector<float> positions;   // the latest pose: each joint in the body's own space, metres, y up
static bool tracked = false;
static std::string problem;
static bool recording = false;
static std::string take;               // header and frames, as text
static int frameCount = 0;
static double startTime = 0;

static void append(std::string& out, float v) {
    char text[24];
    snprintf(text, sizeof(text), " %.5g", v);
    out += text;
}

@interface GameLabBody : NSObject <ARSessionDelegate>
@end

@implementation GameLabBody
- (void)use:(NSArray<__kindof ARAnchor*>*)anchors {
    for (ARAnchor* anchor in anchors) {
        if (![anchor isKindOfClass:[ARBodyAnchor class]]) continue;
        ARBodyAnchor* body = (ARBodyAnchor*)anchor;
        ARSkeleton3D* skeleton = body.skeleton;
        NSInteger count = skeleton.jointCount;
        const simd_float4x4* model = skeleton.jointModelTransforms;
        const simd_float4x4* local = skeleton.jointLocalTransforms;
        std::lock_guard<std::mutex> guard(lock);
        if (names.empty()) {
            for (NSInteger i = 0; i < count; i++) {
                names.push_back(skeleton.definition.jointNames[i].UTF8String);
                parents.push_back(skeleton.definition.parentIndices[i].intValue);
            }
        }
        tracked = body.isTracked;
        positions.resize((size_t)count * 3);
        for (NSInteger i = 0; i < count; i++) {
            positions[i * 3] = model[i].columns[3].x;
            positions[i * 3 + 1] = model[i].columns[3].y;
            positions[i * 3 + 2] = model[i].columns[3].z;
        }
        if (!recording || !tracked) continue;

        if (frameCount == 0) {
            // The header: each joint's name, parent and its offset from the parent, measured on this person.
            startTime = CACurrentMediaTime();
            take = "gamelab-mocap 1\njoints " + std::to_string(count) + "\n";
            for (NSInteger i = 0; i < count; i++) {
                take += names[i] + "\t" + std::to_string(parents[i]) + "\t";
                append(take, local[i].columns[3].x);
                append(take, local[i].columns[3].y);
                append(take, local[i].columns[3].z);
                take += "\n";
            }
            take += "frames\n";
        }
        // A frame: seconds since the start, where the body is in the room, and every joint's turn from its parent.
        append(take, (float)(CACurrentMediaTime() - startTime));
        simd_float4x4 root = body.transform;
        append(take, root.columns[3].x);
        append(take, root.columns[3].y);
        append(take, root.columns[3].z);
        simd_quatf turn = simd_quaternion(root);
        for (int k = 0; k < 4; k++) append(take, turn.vector[k]);
        for (NSInteger i = 0; i < count; i++) {
            simd_quatf q = simd_quaternion(local[i]);
            for (int k = 0; k < 4; k++) append(take, q.vector[k]);
        }
        take += "\n";
        frameCount++;
    }
}

- (void)session:(ARSession*)session didAddAnchors:(NSArray<__kindof ARAnchor*>*)anchors { [self use:anchors]; }
- (void)session:(ARSession*)session didUpdateAnchors:(NSArray<__kindof ARAnchor*>*)anchors { [self use:anchors]; }
- (void)session:(ARSession*)session didFailWithError:(NSError*)error {
    std::lock_guard<std::mutex> guard(lock);
    problem = error.localizedDescription.UTF8String;
}
@end

static ARSession* session;
static GameLabBody* listener;

extern "C" int gamelab_mocap_start(char* error, size_t size) {
    if (!ARBodyTrackingConfiguration.isSupported) {
        strlcpy(error, "this iPhone can't track bodies (it needs an A12 chip or newer)", size);
        return 0;
    }
    if (!session) {
        session = [[ARSession alloc] init];
        listener = [[GameLabBody alloc] init];
        session.delegate = listener;   // called on the main thread, between frames
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        problem.clear();
        tracked = false;
    }
    [session runWithConfiguration:[[ARBodyTrackingConfiguration alloc] init]
                          options:ARSessionRunOptionResetTracking | ARSessionRunOptionRemoveExistingAnchors];
    return 1;
}

extern "C" void gamelab_mocap_stop(void) {
    [session pause];
    std::lock_guard<std::mutex> guard(lock);
    recording = false;
    tracked = false;
}

extern "C" int gamelab_mocap_pose(float* out, int* parentsOut, int maxJoints, int* jointCount, char* error, size_t errorSize) {
    std::lock_guard<std::mutex> guard(lock);
    strlcpy(error, problem.c_str(), errorSize);
    int count = (int)std::min(names.size(), (size_t)maxJoints);
    *jointCount = count;
    for (int i = 0; i < count && (size_t)i * 3 + 2 < positions.size(); i++) {
        out[i * 3] = positions[i * 3];
        out[i * 3 + 1] = positions[i * 3 + 1];
        out[i * 3 + 2] = positions[i * 3 + 2];
        parentsOut[i] = parents[i];
    }
    return tracked ? 1 : 0;
}

extern "C" void gamelab_mocap_record(int on) {
    std::lock_guard<std::mutex> guard(lock);
    if (on && !recording) {
        take.clear();
        frameCount = 0;
    }
    recording = on != 0;
}

extern "C" int gamelab_mocap_frames(void) {
    std::lock_guard<std::mutex> guard(lock);
    return frameCount;
}

extern "C" char* gamelab_mocap_take(size_t* size) {
    std::lock_guard<std::mutex> guard(lock);
    if (frameCount == 0) return nullptr;
    *size = take.size();
    char* copy = (char*)malloc(take.size());
    memcpy(copy, take.data(), take.size());
    return copy;
}
