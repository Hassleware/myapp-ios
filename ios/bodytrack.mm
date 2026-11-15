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
// The camera picture, turned upright (portrait) and shrunk, and each joint's place on it.
static const int CAMERA_W = 270, CAMERA_H = 360;
static std::vector<unsigned char> cameraPixels;
static int cameraSerial = 0;
static std::vector<float> screenPoints;   // x, y per joint, 0 to 1 across and down the upright picture

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

// Every camera frame: an upright, small copy of the picture, and where each tracked joint appears on it.
- (void)session:(ARSession*)session didUpdateFrame:(ARFrame*)frame {
    CVPixelBufferRef image = frame.capturedImage;   // landscape, YCbCr: a brightness plane and a half-size colour plane
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    int width = (int)CVPixelBufferGetWidthOfPlane(image, 0), height = (int)CVPixelBufferGetHeightOfPlane(image, 0);
    const unsigned char* luma = (const unsigned char*)CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const unsigned char* chroma = (const unsigned char*)CVPixelBufferGetBaseAddressOfPlane(image, 1);
    size_t lumaRow = CVPixelBufferGetBytesPerRowOfPlane(image, 0), chromaRow = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    std::vector<unsigned char> out((size_t)CAMERA_W * CAMERA_H * 4);
    for (int v = 0; v < CAMERA_H; v++) {
        for (int u = 0; u < CAMERA_W; u++) {
            // Turned a quarter clockwise: the upright picture's (u, v) is the sensor's (v, height - 1 - u).
            int x = v * width / CAMERA_H, y = height - 1 - u * height / CAMERA_W;
            float Y = luma[(size_t)y * lumaRow + x];
            const unsigned char* c = chroma + (size_t)(y / 2) * chromaRow + (size_t)(x / 2) * 2;
            float cb = c[0] - 128.0f, cr = c[1] - 128.0f;
            unsigned char* p = &out[((size_t)v * CAMERA_W + u) * 4];
            p[0] = (unsigned char)std::clamp(Y + 1.402f * cr, 0.0f, 255.0f);
            p[1] = (unsigned char)std::clamp(Y - 0.344136f * cb - 0.714136f * cr, 0.0f, 255.0f);
            p[2] = (unsigned char)std::clamp(Y + 1.772f * cb, 0.0f, 255.0f);
            p[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);

    std::vector<float> points;
    for (ARAnchor* anchor in frame.anchors) {
        if (![anchor isKindOfClass:[ARBodyAnchor class]] || !((ARBodyAnchor*)anchor).isTracked) continue;
        ARBodyAnchor* body = (ARBodyAnchor*)anchor;
        const simd_float4x4* model = body.skeleton.jointModelTransforms;
        CGSize upright = CGSizeMake(height, width);
        for (NSInteger i = 0; i < body.skeleton.jointCount; i++) {
            simd_float4 world = simd_mul(body.transform, model[i].columns[3]);
            CGPoint p = [frame.camera projectPoint:simd_make_float3(world) orientation:UIInterfaceOrientationPortrait viewportSize:upright];
            points.push_back((float)(p.x / upright.width));
            points.push_back((float)(p.y / upright.height));
        }
        break;
    }

    std::lock_guard<std::mutex> guard(lock);
    cameraPixels.swap(out);
    cameraSerial++;
    screenPoints.swap(points);
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
    cameraPixels.clear();
    screenPoints.clear();
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

extern "C" int gamelab_mocap_camera(unsigned char* rgba, int maxBytes, int* w, int* h, int* serial) {
    std::lock_guard<std::mutex> guard(lock);
    if (cameraPixels.empty() || (int)cameraPixels.size() > maxBytes) return 0;
    memcpy(rgba, cameraPixels.data(), cameraPixels.size());
    *w = CAMERA_W;
    *h = CAMERA_H;
    *serial = cameraSerial;
    return 1;
}

extern "C" int gamelab_mocap_screen(float* xy, int maxJoints) {
    std::lock_guard<std::mutex> guard(lock);
    int count = (int)std::min(screenPoints.size() / 2, (size_t)maxJoints);
    memcpy(xy, screenPoints.data(), (size_t)count * 2 * sizeof(float));
    return count;
}

extern "C" char* gamelab_mocap_take(size_t* size) {
    std::lock_guard<std::mutex> guard(lock);
    if (frameCount == 0) return nullptr;
    *size = take.size();
    char* copy = (char*)malloc(take.size());
    memcpy(copy, take.data(), take.size());
    return copy;
}
