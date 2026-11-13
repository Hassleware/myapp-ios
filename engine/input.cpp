#include "input.h"

#include <SDL3/SDL.h>

#include <cmath>

#include "gfx.h"

#if defined(SDL_PLATFORM_IOS)
extern "C" void gamelab_haptic(int kind);   // ios/haptics.mm
#endif

namespace input {
namespace {

struct State {
    Control def;
    bool held = false, wasPressed = false, keyHeld = false;
    Uint64 pointer = 0;
    float sx = 0, sy = 0;
};

const Uint64 MOUSE_POINTER = ~(Uint64)0;
std::vector<State> states;
std::vector<Touch> touchList;
SDL_Sensor *accelSensor = nullptr, *gyroSensor = nullptr;
SDL_Gamepad* gamepad = nullptr;

Touch* touch_for(Uint64 pointer) {
    for (Touch& t : touchList)
        if (t.id == pointer && t.phase != 2) return &t;
    return nullptr;
}

float pad_axis(SDL_GamepadAxis axis) {
    if (!gamepad) return 0;
    float v = SDL_GetGamepadAxis(gamepad, axis) / 32767.0f;
    const float dead = 0.15f;   // worn sticks never rest at exactly zero
    if (fabsf(v) < dead) return 0;
    return SDL_clamp((v - (v < 0 ? -dead : dead)) / (1.0f - dead), -1.0f, 1.0f);
}
bool tapped = false;
float tapX = 0, tapY = 0;

float screen_w() { return gfx::view_width(); }
float screen_h() { return gfx::view_height(); }
float radius_of(const State& s) { return s.def.size * SDL_min(screen_w(), screen_h()); }

void move_stick(State& s, float px, float py) {
    float r = radius_of(s);
    float dx = (px - s.def.x * screen_w()) / r, dy = (py - s.def.y * screen_h()) / r;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) { dx /= len; dy /= len; }
    s.sx = dx;
    s.sy = dy;
}

void pointer_down(Uint64 pointer, float nx, float ny) {
    float px = nx * screen_w(), py = ny * screen_h();
    Touch touch;
    touch.id = pointer == MOUSE_POINTER ? 0 : pointer;
    touch.x = px;
    touch.y = py;
    touchList.push_back(touch);
    for (State& s : states) {
        if (s.held) continue;
        float dx = px - s.def.x * screen_w(), dy = py - s.def.y * screen_h();
        float reach = radius_of(s) * (s.def.isStick ? 1.6f : 1.3f);
        if (dx * dx + dy * dy > reach * reach) continue;
        s.held = true;
        s.pointer = pointer;
        if (s.def.isStick) move_stick(s, px, py); else s.wasPressed = true;
        touchList.back().claimed = true;
        return;
    }
    tapped = true;
    tapX = px;
    tapY = py;
}

void pointer_move(Uint64 pointer, float nx, float ny) {
    if (Touch* t = touch_for(pointer == MOUSE_POINTER ? 0 : pointer)) {
        float px = nx * screen_w(), py = ny * screen_h();
        t->dx += px - t->x;
        t->dy += py - t->y;
        t->x = px;
        t->y = py;
    }
    for (State& s : states)
        if (s.held && s.pointer == pointer && s.def.isStick) move_stick(s, nx * screen_w(), ny * screen_h());
}

void pointer_up(Uint64 pointer) {
    if (Touch* t = touch_for(pointer == MOUSE_POINTER ? 0 : pointer)) t->phase = 2;
    for (State& s : states)
        if (s.held && s.pointer == pointer) {
            s.held = false;
            s.sx = s.sy = 0;
        }
}

void mouse_position(const SDL_Event& event, float x, float y, float& nx, float& ny) {
    int w = 1, h = 1;
    SDL_GetWindowSize(SDL_GetWindowFromID(event.motion.windowID), &w, &h);
    nx = w > 0 ? x / w : 0;
    ny = h > 0 ? y / h : 0;
    gfx::window_to_view(nx, ny);
}

State* find(const std::string& id) {
    for (State& s : states)
        if (s.def.id == id) return &s;
    return nullptr;
}

}  // namespace

void init() {
    if (!SDL_InitSubSystem(SDL_INIT_SENSOR | SDL_INIT_GAMEPAD)) return;
    int count = 0;
    SDL_SensorID* ids = SDL_GetSensors(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_SensorType type = SDL_GetSensorTypeForID(ids[i]);
        if (type == SDL_SENSOR_ACCEL && !accelSensor) accelSensor = SDL_OpenSensor(ids[i]);
        if (type == SDL_SENSOR_GYRO && !gyroSensor) gyroSensor = SDL_OpenSensor(ids[i]);
    }
    SDL_free(ids);
}

void set_controls(const std::vector<Control>& controls) {
    states.clear();
    for (const Control& c : controls) {
        State s;
        s.def = c;
        states.push_back(s);
    }
}

void handle_event(const SDL_Event& event) {
    float nx, ny;
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: pointer_down(event.tfinger.fingerID + 1, event.tfinger.x, event.tfinger.y); break;
    case SDL_EVENT_FINGER_MOTION: pointer_move(event.tfinger.fingerID + 1, event.tfinger.x, event.tfinger.y); break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: pointer_up(event.tfinger.fingerID + 1); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        mouse_position(event, event.button.x, event.button.y, nx, ny);
        pointer_down(MOUSE_POINTER, nx, ny);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_position(event, event.motion.x, event.motion.y, nx, ny);
        pointer_move(MOUSE_POINTER, nx, ny);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: pointer_up(MOUSE_POINTER); break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) gamepad = SDL_OpenGamepad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
            SDL_CloseGamepad(gamepad);
            gamepad = nullptr;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.repeat) break;
        for (State& s : states) {
            if (s.def.isStick || s.def.key.empty()) continue;
            if (SDL_GetKeyFromName(s.def.key.c_str()) != event.key.key) continue;
            s.keyHeld = event.type == SDL_EVENT_KEY_DOWN;
            if (s.keyHeld) s.wasPressed = true;
        }
        break;
    default: break;
    }
}

void end_frame() {
    for (State& s : states) s.wasPressed = false;
    tapped = false;
    for (size_t i = 0; i < touchList.size();) {
        Touch& t = touchList[i];
        if (t.phase == 2) {
            touchList.erase(touchList.begin() + (long)i);
            continue;
        }
        t.phase = 1;
        t.dx = t.dy = 0;
        i++;
    }
}

void draw() {
    const float ring[4] = {1, 1, 1, 0.16f}, knob[4] = {1, 1, 1, 0.45f}, lit[4] = {1, 1, 1, 0.38f}, ink[4] = {1, 1, 1, 0.9f};
    for (const State& s : states) {
        float cx = s.def.x * screen_w(), cy = s.def.y * screen_h(), r = radius_of(s);
        if (s.def.isStick) {
            gfx::circle(cx, cy, r, ring);
            gfx::circle(cx + s.sx * r * 0.6f, cy + s.sy * r * 0.6f, r * 0.45f, knob);
        } else {
            gfx::circle(cx, cy, r, (s.held || s.keyHeld) ? lit : ring);
            float size = r * 0.8f;
            gfx::text(s.def.label, cx - gfx::text_width(s.def.label, size) / 2, cy - size / 2, size, ink);
        }
    }
}

void stick(const std::string& id, float& x, float& y) {
    x = y = 0;
    State* s = find(id);
    if (!s) return;
    x = s->sx;
    y = s->sy;
    // On the PC, WASD and the arrow keys drive the first stick.
    if (!states.empty() && s == &states[0]) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) x -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) x += 1;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) y -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) y += 1;
        x += pad_axis(SDL_GAMEPAD_AXIS_LEFTX);
        y += pad_axis(SDL_GAMEPAD_AXIS_LEFTY);
    }
    x = SDL_clamp(x, -1.0f, 1.0f);
    y = SDL_clamp(y, -1.0f, 1.0f);
}

bool down(const std::string& id) {
    State* s = find(id);
    return s && (s->held || s->keyHeld);
}

bool pressed(const std::string& id) {
    State* s = find(id);
    return s && s->wasPressed;
}

bool tap(float& x, float& y) {
    x = tapX;
    y = tapY;
    return tapped;
}

const std::vector<Touch>& touches() { return touchList; }

bool key(const std::string& name) {
    SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return code != SDL_SCANCODE_UNKNOWN && (int)code < count && keys[code];
}

void tilt(float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (accelSensor && SDL_GetSensorData(accelSensor, out, 3))
        for (int i = 0; i < 3; i++) out[i] /= SDL_STANDARD_GRAVITY;
}

void gyro(float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (gyroSensor) SDL_GetSensorData(gyroSensor, out, 3);
}

float pad(const std::string& name) {
    if (!gamepad) return 0;
    SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str());
    if (axis != SDL_GAMEPAD_AXIS_INVALID) return pad_axis(axis);
    SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
    return button != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(gamepad, button) ? 1.0f : 0.0f;
}

void haptic(const std::string& kind) {
    const char* names[] = {"light", "medium", "heavy", "success", "warning", "error"};
    int which = 1;
    for (int i = 0; i < 6; i++)
        if (kind == names[i]) which = i;
#if defined(SDL_PLATFORM_IOS)
    gamelab_haptic(which);
#else
    // No tap motor on a PC. A game controller's rumble stands in for it.
    if (gamepad) SDL_RumbleGamepad(gamepad, 0x4000, (Uint16)(0x3000 + which * 0x1800), 60);
#endif
}

}  // namespace input
