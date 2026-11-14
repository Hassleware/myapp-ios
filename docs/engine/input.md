# Input and keyboard

`engine/input.cpp` turns SDL's events into things a game can ask about: on-screen sticks and buttons, raw touches, tilt, game pads, the phone's keyboard and the clipboard. On the PC, the mouse stands in for a finger and the keyboard for the sticks and buttons, so a game is playable in both places without extra code.

## What it offers

::: file engine/input.h

## Three levels of touch

Use the highest level that does the job.

### 1. Declared controls

Describe sticks and buttons once. The engine draws them, tracks which finger is on which, and gives you their state.

```lua
input.controls {
  { type = "stick",  id = "move", x = 0.13, y = 0.74, size = 0.14 },
  { type = "button", id = "jump", x = 0.88, y = 0.76, size = 0.10, label = "A", key = "space" },
}

function fixed_update(dt)
  local mx, my = input.stick("move")          -- each from -1 to 1
  if input.pressed("jump") then ... end       -- true on the frame it went down
  if input.down("jump") then ... end          -- true while held
end
```

`x` and `y` run from 0 to 1 across the screen. `size` is a fraction of the screen's shorter side. On the PC, ++w++ ++a++ ++s++ ++d++ or the arrow keys drive the first stick, and each button's `key` presses it.

### 2. Raw touches

For anything else (dragging, pinching, tapping things in the world) read the fingers yourself:

```lua
for _, t in ipairs(input.touches()) do
  if not t.claimed then                       -- a stick or button did not take it
    if t.phase == "down" then start_drag(t.id, t.x, t.y)
    elseif t.phase == "move" then drag(t.id, t.dx, t.dy)
    elseif t.phase == "up" then end_drag(t.id) end
  end
end
```

Positions are in 2D units, the same ones `gfx.rect` uses. Each finger keeps its `id` from down to up, which is how you tell two fingers apart for a pinch.

### 3. A tap on the world

Turn a screen point into a ray and ask collision what it hits:

```lua
local ox, oy, oz, dx, dy, dz = gfx.unproject(t.x, t.y)
local distance, x, y, z, nx, ny, nz, body = phys.raycast(ox, oy, oz, dx, dy, dz, 100)
```

## Sensors, pads and the tap motor

| Call | Gives |
|---|---|
| `input.tilt()` | Which way is down, in units of gravity. Zero on the PC |
| `input.gyro()` | How fast the phone is turning |
| `input.pad("leftx")` | A game controller's axis or button, if one is connected |
| `input.haptic("light")` | A tap from the phone's motor: `light`, `medium`, `heavy`, `success`, `warning`, `error` |

Haptics do nothing on the PC. Use them sparingly: a tap on every frame is unpleasant and drains the battery.

## The keyboard

The phone's keyboard is off until a game asks for it.

```lua
local text = ""

function on_text(s)                 -- typed or pasted characters
  text = text .. s
end

function on_key(name)               -- "backspace" or "return"
  if name == "backspace" then text = text:sub(1, (utf8.offset(text, -1) or 1) - 1) end
  if name == "return" then input.keyboard(false) end
end

input.keyboard(true, "email")       -- show it
```

The game owns the string and draws it. There is no text-field widget: you draw a rectangle and the text inside it, as the test game does.

::: source games/net-demo/main.lua on_text on_key

!!! tip "Backspace and non-English text"
    One character can be several bytes. `text:sub(1, -2)` would cut an accented letter in half. `utf8.offset(text, -1)` finds where the last whole character starts.

### Kinds of keyboard

| Kind | Keyboard shown | Notes |
|---|---|---|
| `"text"` | Ordinary | Autocorrect and capital letters on |
| `"email"` | With `@` and `.` handy | Autocorrect off |
| `"number"` | Digits | |
| `"password"` | Ordinary | Autocorrect off. Draw dots yourself, not the text |
| `"code"` | Digits | iOS offers a one-time code that just arrived by SMS above the keyboard |

How the kind is turned into what iOS understands:

::: source engine/input.cpp keyboard

Two details in that function matter for privacy. Autocorrect is only enabled for plain text, so the keyboard never learns passwords, codes or email addresses. And `"code"` uses a type that SDL marks as a one-time code for iOS, which is what makes the SMS suggestion appear.

### From key press to your function

Typed text is not delivered the instant it arrives. `input.cpp` queues it:

::: source engine/input.cpp take_typed

and `script::update` hands the queue to the game at the start of the next update, before `fixed_update`. So `on_text` always runs at a predictable point in the frame, never in the middle of your drawing.

### Layout

The keyboard covers roughly the bottom half of the screen and the engine does not move your drawing out of its way. **Put text fields in the top half.**

## The clipboard

```lua
local pasted = input.paste()        -- iOS asks the player's permission
input.copy("ABCD-1234")
```

There is no long-press "Paste" bubble, because there is no native text field. Give the player a button that calls `input.paste()`. On the PC, ++ctrl+v++ works while the keyboard is "up".

Pasted text is limited to 4,096 characters.

## Exercises

??? example "1. Limit a field to six digits"
    ```lua
    function on_text(s)
      text = (text .. s:gsub("%D", "")):sub(1, 6)
    end
    ```
    `%D` matches anything that is not a digit, so a pasted "Your code is 123456" keeps only `123456`.

??? example "2. Show a password as dots"
    `gfx.text(string.rep("*", utf8.len(text)), x, y, 28, 1, 1, 1)`

??? example "3. Implement pinch to zoom"
    Collect the unclaimed touches. When there are exactly two, measure the distance between them each frame and scale your zoom by this frame's distance divided by last frame's.
