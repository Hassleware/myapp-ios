// What the app's three files share: main.cpp (the SDL callbacks, starting and stopping games, hot reload, the log,
// the HUD), game_list.cpp (the list of games) and phone_frame.cpp (the safe area, and on the PC the phone frames
// and the layout check). Internal to the app; the engine's subsystems don't use it.
#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "gfx.h"

namespace app {

// main.cpp
extern SDL_Window* window;
extern std::vector<std::string> games;   // every game that can be started, by folder name
extern std::string current;              // the running game, or empty while the game list shows
extern std::string orientation;          // the running game's game.cfg choice: "landscape", "portrait" or ""
extern gfx::Handle uiFont;               // Game Lab's own font (MyDotFont)
void start_game(const std::string& name);
std::string wrap_text(const std::string& text, size_t width);   // lines of at most width characters

// game_list.cpp
void draw_list();

// phone_frame.cpp
void update_safe_area();                 // tells the renderer which edges the phone keeps for itself
void draw_phone_frame();                 // the PC's iPhone frame over the window (nothing on the phone)
#if !defined(SDL_PLATFORM_IOS)
extern int phoneIndex;                   // the frame shown (F1), -1 for none
extern bool phoneSideways;               // turned (F2)
extern const int phoneCount;
void apply_phone_frame();                // the window takes the chosen phone's shape
extern bool checking;                    // `gamelab --check <game>` is running
extern int checkStep, checkFrames;
void layout_check_tick();
#endif

}  // namespace app
