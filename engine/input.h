// Touch controls described by each game, plus keyboard and mouse stand-ins on the PC.
#pragma once
#include <string>
#include <vector>

union SDL_Event;

namespace input {

struct Control {
    bool isStick = false;
    std::string id, label, key;   // key: keyboard key that presses a button on the PC, e.g. "space"
    float x = 0.5f, y = 0.5f;     // centre, 0 to 1 across the screen
    float size = 0.1f;            // radius, as a fraction of the screen's shorter side
};

void set_controls(const std::vector<Control>& controls);
void handle_event(const SDL_Event& event);
void end_frame();                 // call once per frame after the game has updated
void draw();

void stick(const std::string& id, float& x, float& y);
bool down(const std::string& id);
bool pressed(const std::string& id);

// A touch or click that no control claimed this frame, in 2D screen units. Used by the game list.
bool tap(float& x, float& y);

}  // namespace input
