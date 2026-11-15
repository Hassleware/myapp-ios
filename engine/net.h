// Calls to web APIs over HTTPS. Requests run on their own threads; answers are collected on the main thread.
// Safety rules, all enforced in net.cpp before anything leaves the device:
//   https only, certificates always checked, only hosts the game's game.cfg lists, redirects not followed,
//   no cookies or cache kept, sizes and the number of requests in flight capped.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace net {

struct Request {
    std::string method = "GET", url, body;
    bool backend = false;   // url is a path on this game's own backend, such as "/scores"
    std::vector<std::pair<std::string, std::string>> headers;
    float timeout = 15;   // seconds
};

struct Response {
    int id = 0;
    int status = 0;            // the HTTP status, or 0 when the request failed
    std::string body, error;   // error is empty unless status is 0
};

// The hosts a game may call, from its game.cfg line  hosts = api.example.com other.org  (subdomains included).
void set_hosts(const std::string& list);

// The game's own backend. While developing (the PC build and the loader) it is backends/<game>/server.py,
// reached through dev.py on the PC. In a standalone release it is game.cfg's  backend = https://...  address.
void set_backend(const std::string& game, const std::string& releaseUrl);

// Empty if the request is allowed, otherwise why not. Checked again by send.
std::string check(const Request& request);

// Starts a request and returns its id. A refused request still answers, through poll, with an error.
int send(const Request& request);

// Hands over one finished request. Call until it returns false.
bool poll(Response& out);

}  // namespace net
