// The web-call safety rules (engine/net.cpp), checked on every GitHub build before the app is built.
// Build and run (Mac, Linux or WSL):
//   clang++ -std=c++17 -Iengine engine/net.cpp tests/net_test.cpp -o net_test -pthread && ./net_test
// If you change anything in net.cpp, add a case here for what you changed.
#include <cstdio>
#include <cstdlib>

#include "net.h"

#if defined(__APPLE__)
// net.cpp hands real requests to ios/http.mm on Apple platforms. This test only checks addresses, so nothing is sent.
extern "C" int gamelab_http(const char*, const char*, const char* const*, int, const char*, size_t, double, size_t,
                            char**, size_t*, char*, size_t) {
    return 0;
}
#endif

int failures = 0;

void expect(bool refused, const net::Request& r, const char* why) {
    bool wasRefused = !net::check(r).empty();
    printf("%s  %s\n", wasRefused == refused ? "ok  " : "FAIL", why);
    if (wasRefused != refused) failures++;
}

net::Request url(const char* text) {
    net::Request r;
    r.url = text;
    return r;
}

int main() {
    net::set_hosts("example.com");
    expect(false, url("https://example.com/a?b=1"), "a listed host");
    expect(false, url("https://API.example.com:8443"), "a subdomain, any case, another port");
    expect(true, url("http://example.com/"), "plain http is refused");
    expect(true, url("https://evil.com/"), "an unlisted host is refused");
    expect(true, url("https://notexample.com/"), "a host that only ends the same is refused");
    expect(true, url("https://evilexample.com/"), "a host with the listed one glued on is refused");
    expect(true, url("https://example.com.evil.com/"), "the listed host as a subdomain of another is refused");
    expect(true, url("https://example.com@evil.com/"), "a user name before the real host is refused");
    expect(true, url("https://evil.com\\@example.com/"), "a backslash is refused");
    expect(true, url("https://example.com/a b"), "a space is refused");
    expect(true, url("https://example.com:99999/"), "a port over 65535 is refused");
    net::Request r = url("https://example.com/");
    r.headers = {{"X-Test", "a\r\nHost: evil.com"}};
    expect(true, r, "a line break in a header is refused");
    r.headers.clear();
    r.method = "CONNECT";
    expect(true, r, "an unknown method is refused");
    r.method = "POST";
    r.body.assign((1 << 20) + 1, 'x');
    expect(true, r, "a body over 1 MB is refused");
    net::Request b;
    b.backend = true;
    b.url = "/scores?top=10";
    expect(false, b, "a backend path");
    b.url = "scores";
    expect(true, b, "a backend path must start with /");
    b.url = "/a b";
    expect(true, b, "a backend path with a space is refused");
    net::set_hosts("");
    expect(true, url("https://example.com/"), "with no hosts listed, nothing is allowed");
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
