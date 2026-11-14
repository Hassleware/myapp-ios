# Objective-C++: the iPhone-only parts

Almost everything the engine needs from the phone comes through SDL3, a cross-platform library: the window, touch, sensors, sound output, the keyboard. A few things SDL does not offer, and for those the engine asks Apple's frameworks directly. Apple's frameworks speak Objective-C.

There are two such files today, both small:

| File | What it does | Apple framework |
|---|---|---|
| `ios/haptics.mm` | The tap motor | UIKit |
| `ios/http.mm` | HTTPS requests | Foundation (`NSURLSession`) |

A file ending in `.mm` is **Objective-C++**: Objective-C and C++ in the same file. That lets these files be called from the engine with no bridging layer.

!!! warning "These files cannot be compiled on Windows"
    The only compiler for them is on GitHub's Mac. The first time you learn whether your code is right is ten minutes after you `ship`. Keep these files tiny, copy patterns that already work, and read your change twice.

## Reading Objective-C

The syntax looks strange for about an hour. The essentials:

| Objective-C | Means | C++ equivalent |
|---|---|---|
| `[object doThing]` | Call a method | `object->doThing()` |
| `[object setValue:v forKey:k]` | Call with two arguments | `object->setValueForKey(v, k)` |
| `[[Thing alloc] init]` | Make a new object | `new Thing()` |
| `object.property` | Read or set a property | The same |
| `@"text"` | A string object (`NSString`) | `std::string("text")` |
| `nil` | No object | `nullptr` |
| `^(int x) { ... }` | A block: a function you can pass around | A lambda, `[](int x) { ... }` |
| `@interface` / `@implementation` | Declare and define a class | `class` in a header and its `.cpp` |

Argument labels are part of the method's name. `setValue:forHTTPHeaderField:` is one method that takes two arguments.

Memory is managed for you: the files are compiled with **ARC** (automatic reference counting), so there is no `delete` or `release`.

## The whole of the haptics file

::: file ios/haptics.mm

Read it against the table:

- `extern "C" void gamelab_haptic(int kind)` declares a plain C function. This is the door between the two languages: C++ can call a C function without knowing anything about Objective-C.
- `[[UIImpactFeedbackGenerator alloc] initWithStyle:style]` makes Apple's object.
- `[generator impactOccurred]` tells it to fire.

And this is the C++ side that calls it:

::: source engine/input.cpp haptic

The `#if defined(SDL_PLATFORM_IOS)` is what keeps the PC build from looking for a function that only exists on the phone.

## A larger example: HTTPS

`ios/http.mm` shows three more things you will need: a delegate class, a block that runs later, and turning a callback-style API into a function that waits.

::: file ios/http.mm

What to notice:

- **The safety checks are not here.** `engine/net.cpp` has already validated the address, host and headers before this function is called. Platform files do the platform's job and nothing else, so the rules live in one place and are tested on the PC.
- **`GameLabNoRedirect` is a delegate**: an object Apple's code calls back to ask a question. Here the question is "the server wants to send this request somewhere else, may I?" and the answer is always no.
- **The semaphore** makes the function wait for Apple's callback. That is only acceptable because `net.cpp` calls it from its own thread, never from the thread that draws.
- **Data crosses the border as plain C**: `const char*`, sizes, and memory from `malloc` that the C++ side frees. No Objective-C object ever leaks into the engine.

## Adding an iPhone-only feature

Use the haptics pair as your template.

1. **Write `ios/yourthing.mm`** with one or two `extern "C"` functions taking and returning plain C types.
2. **Add it to the build** next to the others:

    ::: source CMakeLists.txt ENGINE_SOURCES

    The lines just below that in `CMakeLists.txt` add `ios/haptics.mm ios/http.mm` for the iPhone and turn on ARC for them. Add your file to both lists. If it uses a framework that is not linked yet, add `"-framework Name"` to the list further down.

3. **Declare and call it from C++** inside `#if defined(SDL_PLATFORM_IOS)`, with something sensible for the PC in the `#else`. A feature that does nothing on the PC should fail quietly, so games still run there.
4. **Expose it to Lua** in `engine/script.cpp`. See [Add an engine feature](../guides/engine-feature.md).
5. **Ship, and test on the phone.** Record in `CLAUDE.md` that it is untested on the device until someone has tried it.

## What is likely to be added here

| Feature | Framework | Notes |
|---|---|---|
| Secure storage for login tokens | Security (Keychain) | Small. Wanted before any real login |
| In-app purchases | StoreKit | Cannot be tested without a paid developer account |
| Notifications | UserNotifications | Needs a paid developer account |
| Sign in with Apple, Game Center | AuthenticationServices, GameKit | The same |

## Exercises

??? example "1. In `http.mm`, find where cookies are turned off"
    The block passed to `dispatch_once` sets `HTTPCookieStorage`, `URLCache` and `URLCredentialStorage` to `nil` on an ephemeral configuration.

??? example "2. Why is `gamelab_http` safe to call from several threads at once?"
    The session is created once, guarded by `dispatch_once`. Every call then uses its own request, its own semaphore and its own result variables.

??? example "3. Translate to C++-style pseudocode: `[request setValue:value forHTTPHeaderField:name];`"
    `request->setValueForHTTPHeaderField(value, name);`
