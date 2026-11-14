# Web calls

`engine/net.cpp` lets a game call a web API over HTTPS: log in, fetch a leaderboard, post a score. It is deliberately narrow. A game downloaded over Wi-Fi must not be able to send data anywhere it likes, so the rules are enforced in the engine where a script cannot bypass them.

## Using it

```lua
net.request({
  url = "https://api.example.com/scores",
  method = "POST",
  headers = { ["Content-Type"] = "application/json" },
  body = '{"name":"Ann","score":42}',
  timeout = 10,
}, function(status, body, err)
  if err then
    print("could not reach the server: " .. err)
  elseif status == 200 then
    print("saved: " .. body)
  else
    print("the server said " .. status)
  end
end)
```

`net.request` returns immediately. The function you pass is called on a later frame with either `status, body` or `nil, nil, err`. The game keeps running in between: show a spinner.

And in the game's `game.cfg`:

```ini
hosts = api.example.com
```

Without that line the request is refused, and the error message tells you exactly what to add.

A complete working example:

::: file games/net-demo/main.lua

## The rules

All of them are checked in one place, before anything leaves the device.

| Rule | Why |
|---|---|
| `https://` only | Nothing is ever sent unencrypted |
| Certificates are always checked | A fake server cannot pose as the real one. There is no switch to turn this off |
| The host must be listed in `game.cfg` | A game can only talk to the servers it declares. Subdomains of a listed host are allowed |
| Addresses with a user name, backslash, space or non-ASCII character are refused | These are the tricks used to make an address look like one host while going to another |
| Line breaks in headers are refused | They would let text pose as extra headers or a second request |
| Redirects are not followed | A redirect could carry a login header to a host the game never listed |
| No cookies, cache or stored credentials | Nothing is remembered between requests or written to disk |
| 1 MB up, 4 MB down, 8 requests at once, 1 to 120 second timeout | A bug or a bad server cannot exhaust memory or battery |

!!! danger "Do not loosen these"
    Each rule closes a specific hole. If a feature seems to need one relaxed, the feature needs a different design. Ask before changing `net.cpp`.

### The check itself

::: source engine/net.cpp parse host_allowed check_target

`parse` is strict on purpose. The engine reads the address, and so does the platform's own HTTP code. If the two could ever disagree about which host an address names, a crafted address could pass our check and go somewhere else. Refusing anything unusual removes that possibility.

### Proof that it holds

The checks have their own test, built into the engine:

::: source engine/net.cpp self_check

Run it with:

```powershell
$env:GAMELAB_NET_CHECK = "1"
python tools/dev.py run net-demo
```

It prints `net self-check passed`. **If you change anything in `net.cpp`, run this**, and add a case for whatever you changed.

## How a request travels

```
 Lua: net.request(...)                      main thread
        │
        ▼
 script.cpp  l_net_request ── stores your function, keyed by request id
        │
        ▼
 net.cpp  send ── checks the rules ── refused? answer queued with the reason
        │
        ▼  (a new thread per request)
 fetch ── WinHTTP on Windows  /  ios/http.mm (NSURLSession) on the iPhone
        │
        ▼
 finished queue  ◀── the answer, protected by a lock
        │
        ▼  next frame, main thread
 script::update ── net::poll ── calls your function
```

::: source engine/net.cpp send poll

Two things to notice:

- **Lua is only ever touched from the main thread.** The worker threads know nothing about Lua. They put a plain struct in a queue and the main thread collects it.
- **An answer for a game that has since been reloaded is dropped.** The stored functions are cleared when a game unloads, so a late answer finds nothing waiting and is discarded.

The platform code is short. Windows uses WinHTTP:

::: source engine/net.cpp fetch

The iPhone side is `ios/http.mm`, walked through on the [Objective-C++](../languages/objc.md) page.

## JSON

The engine has no JSON functions, and needs none. JSON libraries exist in pure Lua: put one (for example rxi's `json.lua`) next to `main.lua` and

```lua
local json = include("json.lua")
local data = json.decode(body)
local text = json.encode({ name = "Ann", score = 42 })
```

## Secrets

!!! danger "Never put a secret API key in a game"
    An app is a zip file. Anyone can open it and read every Lua file inside. A key that must stay secret belongs on a server you control; the app talks to your server, and your server talks to the service.

What an app *may* hold is what one player's own login gives it: a token that only unlocks that player's data.

`save.set` writes a plain file. That is fine for scores and settings and wrong for a long-lived login token. Until the engine has secure storage (the iOS Keychain), keep tokens in memory and ask the player to log in again after a restart, or add the Keychain first.

## Not built yet

| Missing | Notes |
|---|---|
| Response headers | Only the status and body are returned |
| A live two-way connection (WebSocket or UDP) | Needed for real-time multiplayer. Polling with `net.request` covers lobbies and turn-based play |
| Uploading or downloading files | Bodies are strings in memory |
| Secure token storage | See above |
| HTTPS on the Linux build | Requests answer with an error there |
| Plain HTTP to a server on your own PC | Refused, like all `http://`. A development backend needs either an HTTPS tunnel or an engine change |

## Exercises

??? example "1. Which of these does the engine allow, if `hosts = example.com`?"
    - `https://example.com/a` : yes
    - `https://api.example.com/a` : yes, a subdomain
    - `https://notexample.com/` : no, a different host that merely ends the same
    - `https://example.com@evil.com/` : no, here `example.com` is a user name and the host is `evil.com`
    - `http://example.com/` : no, not encrypted

    Every one of these is a case in `self_check`.

??? example "2. Poll a server once every two seconds"
    ```lua
    local lib = include("lib.lua")
    lib.run(function()
      while true do
        net.request({ url = "https://api.example.com/updates" }, function(status, body) handle(status, body) end)
        lib.wait(2)
      end
    end)
    function update(dt) lib.update(dt) end
    ```

??? example "3. What happens to a request still in flight when the game hot-reloads?"
    It completes on its thread, the answer is queued, and `script::update` finds no function stored for that id and drops it.
