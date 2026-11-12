#include "sync.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace devsync {
namespace {

const int PORT = 8765;

std::thread worker;
std::atomic<bool> running{false}, writing{false};
std::mutex lock;
std::string statusText = "Looking for your PC...";
std::deque<Command> commands;
std::deque<std::string> logs;
std::string gamesRoot, settingsRoot, buildId, offeredBuild, serverHost;

void set_status(const std::string& text) {
    std::lock_guard<std::mutex> guard(lock);
    statusText = text;
}

void nap(int milliseconds) {
    for (int waited = 0; waited < milliseconds && running; waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ---- files ----

bool read_text(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
    fclose(f);
    return true;
}

void make_dirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool write_file(const std::string& path, const std::string& data) {
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) make_dirs(path.substr(0, slash));
    std::string temp = path + ".part";
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok && rename(temp.c_str(), path.c_str()) == 0;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    return s;
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 40 || name[0] == '-') return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

// Only plain relative paths, so a bad file list can't write outside the game's folder.
bool safe_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) return false;
    for (const std::string& part : split(path, '/'))
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}

// ---- a very small HTTP client ----

int connect_to(const std::string& host, int timeoutMs) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result = connect(fd, (sockaddr*)&addr, sizeof(addr));
    if (result != 0 && errno == EINPROGRESS) {
        pollfd p = {fd, POLLOUT, 0};
        int problem = 0;
        socklen_t len = sizeof(problem);
        if (poll(&p, 1, timeoutMs) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &problem, &len) == 0 && problem == 0) result = 0;
    }
    if (result != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return fd;
}

std::string url_encode(const std::string& path) {
    std::string out;
    char hex[4];
    for (unsigned char c : path) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

bool request(const std::string& host, const std::string& method, const std::string& target, const std::string& body, std::string& response) {
    int fd = connect_to(host, 1500);
    if (fd < 0) return false;
    std::string message = method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    int sendFlags = 0;
#ifdef MSG_NOSIGNAL
    sendFlags = MSG_NOSIGNAL;
#endif
    size_t sent = 0;
    while (sent < message.size()) {
        ssize_t n = send(fd, message.data() + sent, message.size() - sent, sendFlags);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += (size_t)n;
    }
    std::string raw;
    char chunk[16384];
    ssize_t n;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) raw.append(chunk, (size_t)n);
    close(fd);
    size_t split_at = raw.find("\r\n\r\n");
    if (split_at == std::string::npos || raw.compare(0, 12, "HTTP/1.1 200") != 0) return false;
    response = raw.substr(split_at + 4);
    return true;
}

bool get(const std::string& host, const std::string& target, std::string& response) { return request(host, "GET", target, "", response); }

bool is_server(const std::string& host) {
    std::string reply;
    return get(host, "/api/hello", reply) && reply.compare(0, 7, "gamelab") == 0;
}

// ---- finding the PC ----

// Tries every address on the phone's own Wi-Fi network at once and returns the one running the dev server.
std::string scan_network() {
    std::vector<uint32_t> bases;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "";
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t ip = ntohl(((sockaddr_in*)it->ifa_addr)->sin_addr.s_addr);
        bases.push_back(ip & 0xFFFFFF00u);
    }
    freeifaddrs(list);

    for (uint32_t base : bases) {
        std::vector<pollfd> sockets;
        std::vector<std::string> hosts;
        for (uint32_t last = 1; last < 255; last++) {
            in_addr address;
            address.s_addr = htonl(base | last);
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &address, text, sizeof(text));
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(PORT);
            addr.sin_addr = address;
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            connect(fd, (sockaddr*)&addr, sizeof(addr));
            sockets.push_back({fd, POLLOUT, 0});
            hosts.push_back(text);
        }
        std::vector<std::string> open;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        std::vector<bool> done(sockets.size(), false);
        while (std::chrono::steady_clock::now() < deadline) {
            if (poll(sockets.data(), (nfds_t)sockets.size(), 100) <= 0) continue;
            for (size_t i = 0; i < sockets.size(); i++) {
                if (done[i] || !(sockets[i].revents & (POLLOUT | POLLERR | POLLHUP))) continue;
                int problem = 0;
                socklen_t len = sizeof(problem);
                if (getsockopt(sockets[i].fd, SOL_SOCKET, SO_ERROR, &problem, &len) == 0 && problem == 0 && !(sockets[i].revents & (POLLERR | POLLHUP)))
                    open.push_back(hosts[i]);
                done[i] = true;
                sockets[i].events = 0;
            }
        }
        for (const pollfd& s : sockets) close(s.fd);
        for (const std::string& host : open)
            if (is_server(host)) return host;
    }
    return "";
}

std::string find_server() {
    // server.txt lets you type the PC's address by hand if the search can't find it.
    std::string saved;
    if (read_text(settingsRoot + "/server.txt", saved)) {
        saved = trim(saved);
        size_t colon = saved.find(':');
        if (colon != std::string::npos) saved = saved.substr(0, colon);
        if (!saved.empty() && is_server(saved)) return saved;
    }
    std::string found = scan_network();
    if (!found.empty()) write_file(settingsRoot + "/server.txt", found + "\n");
    return found;
}

// ---- syncing games ----

std::map<std::string, std::string> read_record(const std::string& game) {
    std::map<std::string, std::string> record;
    std::string text;
    if (!read_text(gamesRoot + "/" + game + "/.sync", text)) return record;
    for (const std::string& line : split(text, '\n')) {
        size_t tab = line.find('\t');
        if (tab != std::string::npos) record[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return record;
}

void write_record(const std::string& game, const std::map<std::string, std::string>& record) {
    std::string text;
    for (const auto& entry : record) text += entry.first + "\t" + entry.second + "\n";
    write_file(gamesRoot + "/" + game + "/.sync", text);
}

// Downloads new and changed files for every game on the PC, and removes files deleted there.
bool sync_games(const std::string& host) {
    std::string manifest;
    if (!get(host, "/api/manifest", manifest)) return false;
    std::map<std::string, std::map<std::string, std::string>> wanted;
    for (const std::string& line : split(manifest, '\n')) {
        std::vector<std::string> cols = split(line, '\t');
        if (cols.size() == 3 && valid_name(cols[0]) && safe_path(cols[1])) wanted[cols[0]][cols[1]] = trim(cols[2]);
    }

    bool ok = true;
    writing = true;
    for (const auto& game : wanted) {
        std::map<std::string, std::string> record = read_record(game.first);
        std::string dir = gamesRoot + "/" + game.first + "/";
        for (const auto& file : game.second) {
            struct stat info;
            if (record[file.first] == file.second && stat((dir + file.first).c_str(), &info) == 0) continue;
            std::string data;
            if (get(host, "/files/" + game.first + "/" + url_encode(file.first), data) && write_file(dir + file.first, data)) record[file.first] = file.second;
            else ok = false;
        }
        for (auto it = record.begin(); it != record.end();) {
            if (game.second.count(it->first)) {
                ++it;
                continue;
            }
            if (safe_path(it->first)) remove((dir + it->first).c_str());
            it = record.erase(it);
        }
        write_record(game.first, record);
    }
    writing = false;
    return ok;
}

void run() {
    std::string host, lastRevision, lastCommand;
    bool firstContact = true;
    int failures = 0;
    while (running) {
        if (host.empty()) {
            set_status("Looking for your PC...");
            host = find_server();
            if (host.empty()) {
                set_status("No dev server found. Run: python tools/dev.py");
                nap(3000);
                continue;
            }
            firstContact = true;
            lastRevision.clear();
            failures = 0;
        }

        // Reply: revision, command id, action, game, app build held by the PC - one per line.
        std::string reply;
        if (!get(host, "/api/state?build=" + url_encode(buildId), reply)) {
            if (++failures >= 3) host.clear();
            set_status("Lost the PC, searching again...");
            nap(1000);
            continue;
        }
        failures = 0;
        std::vector<std::string> lines = split(reply, '\n');
        while (lines.size() < 5) lines.push_back("");
        {
            std::lock_guard<std::mutex> guard(lock);
            offeredBuild = trim(lines[4]);
            serverHost = host;
        }

        if (lines[0] != lastRevision) {
            set_status("Syncing...");
            if (sync_games(host)) lastRevision = lines[0];
        }
        if (firstContact) {
            lastCommand = lines[1];   // don't replay a command typed before this app connected
            firstContact = false;
        } else if (lines[1] != lastCommand) {
            lastCommand = lines[1];
            std::lock_guard<std::mutex> guard(lock);
            commands.push_back({trim(lines[2]), trim(lines[3])});
        }

        std::deque<std::string> outgoing;
        {
            std::lock_guard<std::mutex> guard(lock);
            outgoing.swap(logs);
        }
        for (const std::string& entry : outgoing) {
            std::string ignored;
            request(host, "POST", "/api/log", entry, ignored);
        }

        set_status("Connected to your PC at " + host);
        nap(1000);
    }
}

}  // namespace

void start(const std::string& gamesDir, const std::string& settingsDir, const std::string& build) {
    if (running) return;
    gamesRoot = gamesDir;
    settingsRoot = settingsDir;
    buildId = build;
    make_dirs(gamesRoot);
    running = true;
    worker = std::thread(run);
}

void stop() {
    if (!running) return;
    running = false;
    if (worker.joinable()) worker.join();
}

std::string status() {
    std::lock_guard<std::mutex> guard(lock);
    return statusText;
}

bool busy() { return writing; }

bool next_command(Command& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (commands.empty()) return false;
    out = commands.front();
    commands.pop_front();
    return true;
}

std::string available_build() {
    std::lock_guard<std::mutex> guard(lock);
    return offeredBuild;
}

std::string update_url() {
    std::lock_guard<std::mutex> guard(lock);
    return "http://" + serverHost + ":" + std::to_string(PORT) + "/app/GameLab.ipa";
}

void log(const char* level, const std::string& game, const std::string& message) {
    std::lock_guard<std::mutex> guard(lock);
    if (logs.size() < 200) logs.push_back(std::string(level) + "\t" + game + "\t" + message);
}

}  // namespace devsync
