// Loader only: keeps the games on this device in step with the dev server on the PC.
// It is left out of release builds, which carry their one game inside the app.
#pragma once
#include <string>

namespace devsync {

struct Command {
    std::string action;   // "play", "menu" or "reload"
    std::string game;
};

// gamesDir: where downloaded games are kept. settingsDir: where server.txt lives.
// build: this app's native-code fingerprint, so the PC can tell when the app is out of date.
void start(const std::string& gamesDir, const std::string& settingsDir, const std::string& build);
void stop();

std::string status();              // one line for the game list screen
bool busy();                       // true while files are being written
bool next_command(Command& out);   // commands typed on the PC
// The app build waiting on the PC ("" if none), and where to fetch it from.
std::string available_build();
std::string update_url();

void log(const char* level, const std::string& game, const std::string& message);

}  // namespace devsync
