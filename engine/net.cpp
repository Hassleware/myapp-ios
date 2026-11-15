// HTTPS requests. The rules in net.h are checked here, in one place, before the platform's own HTTP code is used:
// WinHTTP on Windows, NSURLSession (ios/http.mm) on the iPhone. Both check certificates by default and that is left alone.
#include "net.h"
#if defined(LOADER_SYNC)
#include "sync.h"
#endif

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#elif defined(__APPLE__)
#include <cstdlib>
// Returns the HTTP status, or 0 with a message in error. data is malloc'd and the caller frees it.
extern "C" int gamelab_http(const char* method, const char* url, const char* const* headers, int headerCount,
                            const char* body, size_t bodySize, double timeout, size_t maxBytes,
                            char** data, size_t* size, char* error, size_t errorSize);
#endif

namespace net {
namespace {

const size_t MAX_SEND = 1 << 20;      // 1 MB up
const size_t MAX_BACKEND_SEND = 256 << 20;  // 256 MB up to the game's own backend (a mocap take's video is about 45 MB a minute)
const size_t MAX_RECEIVE = 4 << 20;   // 4 MB down
const int MAX_IN_FLIGHT = 8;

std::vector<std::string> hosts;
std::string gameName, releaseBackend;
std::mutex lock;
// Never freed: a request still running when the app quits must have somewhere to put its answer.
std::vector<Response>* finished = new std::vector<Response>;
std::atomic<int> inFlight{0};
int nextId = 1;

struct Target {
    std::string host, path = "/";
    int port = 443;
    bool secure = true;
};

// No spaces, control characters, backslashes or non-ASCII: anything odd is refused rather than guessed at.
bool plain_text(const std::string& s) {
    for (unsigned char c : s)
        if (c <= 0x20 || c >= 0x7f || c == '\\') return false;
    return true;
}

// Takes a URL apart strictly. Anything odd is refused rather than guessed at, so that this code and the
// platform's own URL reader can never disagree about which host a request goes to.
std::string parse(const std::string& url, Target& out) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return "only https:// addresses are allowed";
    if (!plain_text(url)) return "the address has a space or an unusual character (encode it with %XX)";
    size_t end = url.find_first_of("/?#", scheme.size());
    std::string authority = url.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
    if (authority.find('@') != std::string::npos) return "the address must not contain a user name";
    size_t colon = authority.find(':');
    out.host = authority.substr(0, colon);
    if (colon != std::string::npos) {
        std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return "the address has a bad port";
        out.port = std::atoi(port.c_str());
        if (out.port < 1 || out.port > 65535) return "the address has a bad port";
    }
    if (out.host.empty() || out.host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
        return "the address has a bad host name";
    for (char& c : out.host) c = (char)std::tolower((unsigned char)c);
    if (end != std::string::npos) {
        std::string rest = url.substr(end, url.find('#', end) - end);   // the part after # never goes to a server
        out.path = rest.empty() || rest[0] != '/' ? "/" + rest : rest;
    }
    return "";
}

bool host_allowed(const std::string& host) {
    for (const std::string& allowed : hosts) {
        if (host == allowed) return true;
        if (host.size() > allowed.size() && host.compare(host.size() - allowed.size() - 1, std::string::npos, "." + allowed) == 0) return true;
    }
    return false;
}

std::string check_target(const Request& r, Target& target) {
    if (r.backend) {
        // A path on the game's own backend. The engine, not the game, decides where that is.
        if (r.url.empty() || r.url[0] != '/' || !plain_text(r.url)) return "a backend path starts with / and has no spaces or unusual characters";
#if defined(SINGLE_GAME)
        if (releaseBackend.empty()) return "this game's game.cfg has no  backend = https://...  line for the release build";
        std::string problem = parse(releaseBackend + r.url, target);
        if (!problem.empty()) return problem;
#else
        // ponytail: plain http, to the dev PC only (127.0.0.1 here; the loader reaches it through sync.cpp).
        target.host = "127.0.0.1";
        target.port = 8765;
        target.secure = false;
        target.path = "/backend/" + gameName + r.url;
#endif
    } else {
        std::string problem = parse(r.url, target);
        if (!problem.empty()) return problem;
        if (!host_allowed(target.host)) return "'" + target.host + "' is not in this game's game.cfg (add a line: hosts = " + target.host + ")";
    }
    if (r.method != "GET" && r.method != "POST" && r.method != "PUT" && r.method != "PATCH" && r.method != "DELETE") return "unknown method '" + r.method + "'";
    if (r.body.size() > (r.backend ? MAX_BACKEND_SEND : MAX_SEND)) return r.backend ? "the body is over 256 MB" : "the body is over 1 MB";
    for (const auto& h : r.headers) {
        // A line break inside a header would let its text pose as extra headers or a second request.
        if (h.first.empty() || h.first.find_first_of(": \r\n") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos)
            return "bad header '" + h.first + "'";
        for (unsigned char c : h.first + h.second)
            if (c < 0x20 || c == 0x7f) return "bad header '" + h.first + "'";
    }
    return "";
}

#if defined(_WIN32)
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

void fetch(const Request& r, const Target& t, Response& out) {
    int ms = (int)(r.timeout * 1000);
    HINTERNET session = WinHttpOpen(L"GameLab", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, wide(t.host).c_str(), (INTERNET_PORT)t.port, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, wide(r.method).c_str(), wide(t.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, t.secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = request != nullptr;
    if (ok) {
        WinHttpSetTimeouts(request, ms, ms, ms, ms);
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, noCookies = WINHTTP_DISABLE_COOKIES;
        ok = WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never)) &&
             WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof(noCookies));
    }
    for (const auto& h : r.headers)
        ok = ok && WinHttpAddRequestHeaders(request, wide(h.first + ": " + h.second).c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    DWORD bodySize = (DWORD)r.body.size(), status = 0, statusSize = sizeof(status);
    ok = ok && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodySize ? (void*)r.body.data() : WINHTTP_NO_REQUEST_DATA, bodySize, bodySize, 0) &&
         WinHttpReceiveResponse(request, nullptr) &&
         WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    while (ok) {
        char chunk[8192];
        DWORD got = 0;
        ok = WinHttpReadData(request, chunk, sizeof(chunk), &got) != 0;
        if (!ok || got == 0) break;
        out.body.append(chunk, got);
        if (out.body.size() > MAX_RECEIVE) {
            out.error = "the answer is over 4 MB";
            break;
        }
    }
    if (!ok) out.error = "the request failed (Windows error " + std::to_string(GetLastError()) + ")";
    if (out.error.empty()) out.status = (int)status;
    else out.body.clear();
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}
#elif defined(__APPLE__)
void fetch(const Request& r, const Target& t, Response& out) {
#if defined(LOADER_SYNC)
    if (!t.secure) {
        out.status = devsync::backend(r.method, t.path, r.headers, r.body, (int)r.timeout, MAX_RECEIVE, out.body, out.error);
        return;
    }
#endif
    std::string url = r.backend ? "https://" + t.host + (t.port == 443 ? "" : ":" + std::to_string(t.port)) + t.path : r.url;
    std::vector<const char*> headers;
    for (const auto& h : r.headers) {
        headers.push_back(h.first.c_str());
        headers.push_back(h.second.c_str());
    }
    char* data = nullptr;
    size_t size = 0;
    char error[256] = "";
    out.status = gamelab_http(r.method.c_str(), url.c_str(), headers.data(), (int)r.headers.size(), r.body.data(), r.body.size(),
                              r.timeout, MAX_RECEIVE, &data, &size, error, sizeof(error));
    if (out.status) out.body.assign(data ? data : "", size);
    else out.error = error[0] ? error : "the request failed";
    free(data);
}
#else
// ponytail: no HTTPS on Linux. Add libcurl here if the Linux build ever needs to call APIs.
void fetch(const Request&, const Target&, Response& out) { out.error = "web requests are not built for this platform"; }
#endif

void finish(const Response& response) {
    std::lock_guard<std::mutex> guard(lock);
    finished->push_back(response);
}

// The checks that keep requests where they are meant to go. Run with GAMELAB_NET_CHECK=1 set; prints the result.
void self_check() {
    hosts = {"example.com"};
    int failures = 0;
    auto expect = [&](bool refused, Request r) {
        if (check(r).empty() == refused) {
            ++failures;
            fprintf(stderr, "net self-check FAILED for %s\n", r.url.c_str());
        }
    };
    auto url = [](const char* text) { Request r; r.url = text; return r; };
    expect(false, url("https://example.com/a?b=1"));
    expect(false, url("https://API.example.com:8443"));
    expect(true, url("http://example.com/"));                 // not encrypted
    expect(true, url("https://evil.com/"));                   // not listed
    expect(true, url("https://notexample.com/"));             // only looks like the listed host
    expect(true, url("https://example.com.evil.com/"));
    expect(true, url("https://example.com@evil.com/"));       // "example.com" is a user name here
    expect(true, url("https://evil.com\\@example.com/"));
    expect(true, url("https://example.com/a b"));
    Request r = url("https://example.com/");
    r.headers = {{"X-Test", "a\r\nHost: evil.com"}};
    expect(true, r);
    r.headers.clear();
    r.method = "CONNECT";
    expect(true, r);
    Request b;
    b.backend = true;
    b.url = "/scores?top=10";
    expect(false, b);
    b.url = "scores";                                         // must start with /
    expect(true, b);
    b.url = "/a b";
    expect(true, b);
    hosts.clear();
    fprintf(stderr, failures ? "net self-check: %d FAILED\n" : "net self-check passed\n", failures);
}

}  // namespace

void set_backend(const std::string& game, const std::string& releaseUrl) {
    gameName = game;
    releaseBackend = releaseUrl;
    while (!releaseBackend.empty() && releaseBackend.back() == '/') releaseBackend.pop_back();
}

void set_hosts(const std::string& list) {
    static bool checked = getenv("GAMELAB_NET_CHECK") ? (self_check(), true) : false;
    (void)checked;
    hosts.clear();
    std::string word;
    for (size_t i = 0; i <= list.size(); i++) {
        char c = i < list.size() ? (char)std::tolower((unsigned char)list[i]) : ' ';
        if (c != ' ' && c != ',' && c != '\t') {
            word += c;
        } else if (!word.empty()) {
            hosts.push_back(word);
            word.clear();
        }
    }
}

std::string check(const Request& request) {
    Target target;
    return check_target(request, target);
}

int send(const Request& request) {
    Response early;
    early.id = nextId++;
    Target target;
    early.error = check_target(request, target);
    if (early.error.empty() && inFlight >= MAX_IN_FLIGHT) early.error = "too many requests at once (8 at most)";
    if (!early.error.empty()) {
        finish(early);
        return early.id;
    }
    ++inFlight;
    int id = early.id;
    std::thread([request, target, id] {
        Response response;
        response.id = id;
        fetch(request, target, response);
        finish(response);
        --inFlight;
    }).detach();
    return id;
}

bool poll(Response& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (finished->empty()) return false;
    out = finished->front();
    finished->erase(finished->begin());
    return true;
}

}  // namespace net
