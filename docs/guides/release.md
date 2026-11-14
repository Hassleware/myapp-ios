# Releasing a game

A release is one game built as its own app: no game list, no Wi-Fi sync, just the engine and that game's files.

!!! warning "How much of this is proven"
    The standalone build exists and its pieces are tested separately, but `release <game>` has **not yet been run end to end**, and the App Store path (signing, upload, listing) is **not built**. Treat this page as the plan and the current state, and expect to fix things the first time through.

## Build it

```
python tools/dev.py release <game>
```

::: source tools/dev.py cmd_release

This starts the same GitHub workflow as `ship`, with the game's name as an input. The result lands in `tools/.ipa/<game>.ipa`. It is unsigned, which suits installing through SideStore.

## What is different about a release build

| | Loader | Standalone |
|---|---|---|
| Games inside | All | One |
| Starts on | The game list | The game |
| Wi-Fi sync | Yes | **Not compiled in at all** |
| `lua` commands from the PC | Yes | No |
| Reads files from | Synced copies, then bundled | Bundled only |
| Orientation | Any; each game's `game.cfg` applies while it runs | Fixed in the app's settings from `game.cfg` |

The absence of sync code is a requirement, not a tidy-up. The App Store does not allow an app to download and run code. Lua that ships **inside** the app is fine.

## Before the first release of a game

- [ ] **Pick the bundle identifier and never change it.** It defaults to `com.starter.<game>`. Choose a real one (`com.yourstudio.game`) before the first upload. Apple ties updates, purchases and saved data to it for ever.
- [ ] **Set the orientation** in `game.cfg`.
- [ ] **List the web hosts** in `game.cfg`, and nothing more than the game needs.
- [ ] **Remove development leftovers**: debug text, `sys.hud(true)`, test accounts, any global you added to poke values.
- [ ] **No secrets in the Lua.** An app can be unzipped and read.
- [ ] **File names match in case** everywhere they are loaded. The phone is case-sensitive.
- [ ] **Run `check <game>`** for every screen.
- [ ] **Play it from a cold start on a real phone**, with the PC turned off, to prove it needs nothing from the dev server.
- [ ] **Play it in aeroplane mode.** A game that uses `net.request` must cope with every request failing.

## The App Store path (not built)

What remains, in order:

1. **Join the Apple Developer Program.** About $99 a year. Enrolment can take days, so start early. It also removes the 7-day expiry and 3-app limit of free sideloading.
2. **App icons and a launch screen.** The project has neither yet.
3. **Signing in the workflow.** The build currently skips signing. An upload needs a distribution certificate and provisioning profile, supplied to GitHub as secrets.
4. **Upload**, using an App Store Connect API key.
5. **The listing**: screenshots at the required sizes, description, privacy answers, age rating.
6. **TestFlight** for testers before public release.

In-app purchases, notifications, Game Center and Sign in with Apple all need the paid account as well, and each needs engine work. See [Objective-C++](../languages/objc.md#what-is-likely-to-be-added-here).

## Privacy answers

Apple asks what data an app collects. Today the honest answers are simple:

- The engine itself collects nothing and contacts no server.
- A game contacts only the hosts in its `game.cfg`, and sends only what its Lua sends.
- No cookies, cache or identifiers are stored by the networking code.

If a game adds accounts or analytics, those answers change, and they are the game's responsibility.

## After release

A released app's C++ **and** Lua can only change through an App Store update. The one-second loop is a development tool; it does not exist for players. Plan for that: anything you will want to tune after launch (prices, balance numbers, event dates) should come from your own server through `net.request`, as plain data.
