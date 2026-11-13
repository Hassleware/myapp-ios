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

// One finger on the screen (or the mouse on the PC), for games that want more than sticks and buttons.
struct Touch {
    unsigned long long id = 0;
    float x = 0, y = 0;       // in 2D screen units
    float dx = 0, dy = 0;     // movement since the last frame
    int phase = 0;            // 0 just went down, 1 held or moving, 2 just lifted
    bool claimed = false;     // a stick or button took it
};

void init();                      // opens the tilt sensors and any game controller
void set_controls(const std::vector<Control>& controls);
void handle_event(const SDL_Event& event);
void end_frame();                 // call once per frame after the game has updated
void draw();

void stick(const std::string& id, float& x, float& y);
bool down(const std::string& id);
bool pressed(const std::string& id);

const std::vector<Touch>& touches();
bool key(const std::string& name);                 // a keyboard key by name ("space", "a", "left"), for testing on the PC
void tilt(float out[3]);                           // which way is down, in units of gravity. Zero on the PC
void gyro(float out[3]);                           // turning speed in radians a second
float pad(const std::string& name);                // a game controller's axis (-1 to 1) or button (0 or 1), by SDL's name
void haptic(const std::string& kind);              // "light", "medium", "heavy", "success", "warning", "error"

// A touch or click that no control claimed this frame, in 2D screen units. Used by the game list.
bool tap(float& x, float& y);

}  // namespace input
