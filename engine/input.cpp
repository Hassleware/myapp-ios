#include "input.h"

#include <SDL3/SDL.h>

#include <cmath>

#include "gfx.h"

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
    for (State& s : states) {
        if (s.held) continue;
        float dx = px - s.def.x * screen_w(), dy = py - s.def.y * screen_h();
        float reach = radius_of(s) * (s.def.isStick ? 1.6f : 1.3f);
        if (dx * dx + dy * dy > reach * reach) continue;
        s.held = true;
        s.pointer = pointer;
        if (s.def.isStick) move_stick(s, px, py); else s.wasPressed = true;
        return;
    }
    tapped = true;
    tapX = px;
    tapY = py;
}

void pointer_move(Uint64 pointer, float nx, float ny) {
    for (State& s : states)
        if (s.held && s.pointer == pointer && s.def.isStick) move_stick(s, nx * screen_w(), ny * screen_h());
}

void pointer_up(Uint64 pointer) {
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
}

State* find(const std::string& id) {
    for (State& s : states)
        if (s.def.id == id) return &s;
    return nullptr;
}

}  // namespace

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

}  // namespace input
