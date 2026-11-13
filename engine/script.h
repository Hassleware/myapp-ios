// Runs a game's Lua code and gives it the gfx, model, phys, audio, input, save and sys functions.
#pragma once
#include <string>

namespace script {

// Loads <gameDir>/main.lua. Saved values are kept in saveFile. Returns false if the game has an error.
bool load(const std::string& gameDir, const std::string& saveFile);
void unload();
// One frame of the game: fixed_update at a steady 60 a second, then update, then late_update.
void update(float dt);
void draw();
void pause();    // the app is going to the background
void resume();

// Runs a line of Lua typed on the PC and prints what it returns. An error here does not stop the game.
void eval(const std::string& code);

// The current error with its Lua traceback, or empty. A game with an error stops until it is reloaded.
const std::string& error();

float game_time();     // seconds of game time since the game started, slowed or stopped by sys.time_scale
bool hud_wanted();     // the game asked for the frame-time readout with sys.hud(true)
int memory_kb();       // what the game's Lua is holding

// Receives everything a game prints, and its errors.
void set_log_sink(void (*sink)(const char* level, const std::string& message));

}  // namespace script
