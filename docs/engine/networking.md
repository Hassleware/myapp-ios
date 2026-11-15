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

## The game's own backend

A game can have server code of its own: accounts, saved progress, scores, matchmaking. It is Python, it lives in **`backends/<game>/server.py`**, and while you develop it runs **on your PC**, inside `dev.py`. It is never sent to the phone or put inside an app.

```
python tools/dev.py backend coin-dash
```

creates two files:

| File | What it is |
|---|---|
| `backends/coin-dash/server.py` | Accounts and sign-in, the part nearly every game and app needs. Add the game's own calls at the bottom |
| `games/coin-dash/auth.lua` | The game's side of it |

Sign-in works like Supercell ID: **no passwords**. The player types their email, gets a 6-digit code, types it in, and the phone stays signed in until they log out. Signing up and signing in are the same steps; the account is made the first time a code is accepted.

```lua
local auth = include("auth.lua")

auth.send_code("ann@example.com", function(ok, err) end)         -- emailed, or printed in the dev.py window
input.keyboard(true, "code")                                     -- iOS offers a code that just arrived in Mail
auth.verify("ann@example.com", "123456", function(ok, err) end)  -- signed in from now on
auth.me(function(user, err) print(user.id, user.email) end)
auth.call("POST", "/save", '{"level": 3}', function(text, err) end)   -- your own calls, sent signed in
auth.logout()      auth.delete()      auth.signed_in()
```

`games/net-demo` has a working sign-in screen built on this.

### Getting the code, just you and your phone

With nothing set up, the server prints each code in the `dev.py` window:

```
  [backend net-demo] sign-in code for ann@example.com: 482913
```

To get real emails instead, give `dev.py` a mail account before starting it. Any account that allows SMTP works; for Gmail, turn on 2-step verification and create an *app password*:

```powershell
$env:GAMELAB_SMTP = "smtp.gmail.com:587"
$env:GAMELAB_SMTP_USER = "you@gmail.com"
$env:GAMELAB_SMTP_PASSWORD = "the app password"
python tools/dev.py
```

These live only in that PowerShell window. Never put them in a file in the repository. The email's subject starts with the code, which is what lets iOS offer it above the keyboard.

### What the sign-in follows

| Practice | In the template |
|---|---|
| Unguessable, single-use, short-lived codes | 6 digits from the operating system's secure random source; one use; 10 minutes; 5 tries, then a new code is needed |
| Nothing secret stored as it is | Only hashes of codes and of session tokens are kept, so a leaked database holds nothing usable |
| No hints about which emails have accounts | Asking for a code answers the same for every address |
| No flooding someone's inbox | One code per email per minute |
| Long, revocable sessions | 256 random bits per token; a year, pushed forward each time it is used; logout and deletion end it |
| No timing leaks | Secrets compared with `hmac.compare_digest` |
| Account deletion inside the app | `DELETE /me`. The App Store requires it of any app that creates accounts |

Not included yet: Sign in with Apple, a daily cap on emails per address, and refresh tokens. On the phone, `auth.lua` keeps the token with `save`, which is a plain file; move it to the iOS Keychain before release.

The game never says where the server is. The engine decides:

| Build | `net.backend` goes to | How |
|---|---|---|
| PC | `backends/<game>/server.py` | `http://127.0.0.1:8765/backend/<game>/...`, answered by `dev.py` |
| Loader on the phone | The same file, on your PC | Through the connection sync already has to `dev.py`, so iOS needs no plain-http exception |
| Standalone release | `game.cfg`'s `backend = https://...` | An ordinary HTTPS request, with all the rules above |

The server side is one function:

::: source tools/backend_template.py handle

`dev.py` loads the file again whenever you save it, so a change to the server is as quick as a change to the game. An error in it shows in the `dev.py` window with its traceback, and the game gets a 500. Data goes in `data.db` next to the file, which git ignores.

How `dev.py` passes a call on:

::: source tools/dev.py backend_module

!!! warning "What the development link is and is not"
    The path from the phone to `dev.py` is plain HTTP on your own Wi-Fi, like the file sync. Codes and tokens cross your network unencrypted while developing, so use test accounts. Players never use it: a release build only knows the HTTPS address in `game.cfg`. Plain `http://` stays refused for `net.request`; only `net.backend`, whose address the engine builds itself, can reach the dev PC.

**For a release**, the same `server.py` runs on its own (`python server.py 8080`) on a server you rent, behind HTTPS, and `game.cfg` gets its address. This path is **not yet tested**. A real deployment also wants a production web server in front and backups of the database.

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
| Plain HTTP to arbitrary servers | Refused. The game's own backend on your PC is reached with `net.backend` instead |

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
