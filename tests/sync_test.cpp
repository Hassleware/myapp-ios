// Runs the phone's sync code (engine/sync.cpp) against a real dev.py, before the iPhone app is built.
// sync.cpp can't be compiled on Windows, so without this a broken sync only shows up on the phone.
// The GitHub workflow starts dev.py, then runs this. Exit code 0 means every check passed.
//
//   clang++ -std=c++17 -Iengine engine/sync.cpp tests/sync_test.cpp -o sync_test -pthread
//   ./sync_test <repo folder>

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "sync.h"

static int failures = 0;

static void check(const char* what, bool ok) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return in ? text.str() : std::string("<missing>");
}

static bool exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

template <typename Test>
static bool wait_for(Test test, int seconds) {
    for (int i = 0; i < seconds * 10; i++) {
        if (test()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int main(int argc, char** argv) {
    std::string repo = argc > 1 ? argv[1] : ".";
    std::string root = "sync-test-tmp", games = root + "/games", settings = root + "/settings";
    mkdir(root.c_str(), 0755);
    mkdir(games.c_str(), 0755);
    mkdir(settings.c_str(), 0755);
    // Skip the network scan: the dev server is on this machine.
    std::ofstream(settings + "/server.txt") << "127.0.0.1\n";
    // A game the PC no longer has, synced earlier: sync must remove it.
    mkdir((games + "/zz-gone").c_str(), 0755);
    std::ofstream(games + "/zz-gone/main.lua") << "-- old\n";
    std::ofstream(games + "/zz-gone/.sync") << "main.lua\tabc\n";

    devsync::start(games, settings, "sync-test");

    check("finds dev.py and connects",
          wait_for([] { return devsync::status().rfind("Connected", 0) == 0; }, 20));

    std::string want = read_file(repo + "/games/net-demo/main.lua");
    check("downloads a game's files, byte for byte",
          wait_for([&] { return read_file(games + "/net-demo/main.lua") == want; }, 20));
    check("removes a game deleted on the PC", wait_for([&] { return !exists(games + "/zz-gone"); }, 10));

    std::string reply, error;
    int status = devsync::backend("POST", "/backend/net-demo/code", {{"Content-Type", "application/json"}},
                                  "{\"email\":\"ci@example.com\"}", 5, 1 << 20, reply, error);
    check("reaches the game's backend through dev.py", status == 200 && reply.find("\"ok\"") != std::string::npos);
    if (status != 200) printf("      status %d, reply '%s', error '%s'\n", status, reply.c_str(), error.c_str());

    status = devsync::backend("GET", "/backend/no-such-game/x", {}, "", 5, 1 << 20, reply, error);
    check("passes the backend's own status through (404)", status == 404);

    status = devsync::backend("GET", "/backend/net-demo/me", {}, "", 5, 4, reply, error);
    check("refuses a reply larger than the limit", status == 0);

    devsync::stop();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
