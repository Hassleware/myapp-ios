// Runs a game's Lua code and gives it the gfx, input and save functions.
#pragma once
#include <string>

namespace script {

// Loads <gameDir>/main.lua. Saved values are kept in saveFile. Returns false if the game has an error.
bool load(const std::string& gameDir, const std::string& saveFile);
void unload();
void update(float dt);
void draw();

// The current error with its Lua traceback, or empty. A game with an error stops until it is reloaded.
const std::string& error();

// Receives everything a game prints, and its errors.
void set_log_sink(void (*sink)(const char* level, const std::string& message));

}  // namespace script
