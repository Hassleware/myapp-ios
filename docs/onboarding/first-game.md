# Your first game

You will make a small game where a cube collects coins, with touch controls, a score that survives closing the app, and a layout that fits every iPhone. Every step is a save away from seeing the result.

Keep `python tools/dev.py` running in one terminal.

## 1. Create it

At the `dev.py` prompt:

```
new coin-dash
run coin-dash
```

`new` copies the template into `games/coin-dash/`. You get three files:

| File | What it is |
|---|---|
| `main.lua` | Your game. The engine runs this file |
| `lib.lua` | Helpers written in Lua (timers, state machines, vector maths). It is yours to edit. See [lib.lua helpers](../reference/lib.md) |
| `game.cfg` | Settings read before the game starts: orientation, web hosts, the release backend. All commented out to begin with. See [game.cfg](../reference/game-cfg.md) |

This is the template you are starting from: every function the engine calls, empty, and a title drawn in the middle of the screen.

::: file tools/template/main.lua

## 2. The shape of every game

The engine calls functions in your file by name, if you defined them:

| Function | When | Put here |
|---|---|---|
| `init()` | Once at start, and after every reload | Setting up |
| `fixed_update(dt)` | Exactly 60 times a second | Movement and collision |
| `update(dt)` | Once a frame | Everything else that changes |
| `draw()` | Once a frame | All drawing |

`dt` is the time since the last call, in seconds. Multiply speeds by it so the game runs at the same pace on every device.

The full list is in the [Lua API](../reference/lua-api.md#called-by-the-engine-if-the-game-defines-them).

## 3. Add coins

Replace the contents of `main.lua` with this and save:

```lua
input.controls {
  { type = "stick", id = "move", x = 0.15, y = 0.75, size = 0.14 },
}

local x, z = 0, 0
local coins, score = {}, 0

local function scatter()
  coins = {}
  for i = 1, 8 do
    coins[i] = { x = math.random(-7, 7), z = math.random(-7, 7) }
  end
end

function init()
  x, z, score = 0, 0, 0
  scatter()
end

function fixed_update(dt)
  local mx, my = input.stick("move")
  x = x + mx * 7 * dt
  z = z + my * 7 * dt
  for i = #coins, 1, -1 do                       -- backwards, because we remove while looping
    local c = coins[i]
    if math.abs(c.x - x) < 0.8 and math.abs(c.z - z) < 0.8 then
      table.remove(coins, i)
      score = score + 1
      input.haptic("light")
    end
  end
  if #coins == 0 then scatter() end
end

function draw()
  gfx.clear(0.10, 0.12, 0.18)
  gfx.camera(0, 12, 12, 0, 0, 0, 50)
  gfx.plane(0, 0, 0, 16, 16, 0.22, 0.28, 0.36)
  gfx.cube(x, 0.5, z, 1, 1, 1, 0.36, 0.82, 0.63)
  for _, c in ipairs(coins) do
    gfx.sphere(c.x, 0.5, c.z, 0.6, 1.0, 0.82, 0.20)
  end
  local left, top = gfx.safe()
  gfx.text("SCORE " .. score, left + 80, top + 24, 32, 1, 1, 1)
end
```

On the PC, ++w++ ++a++ ++s++ ++d++ drive the stick. On the phone, drag the on-screen stick.

!!! tip "Things worth noticing"
    - Movement and the coin check are in `fixed_update`, so they behave the same at 30 and 120 frames a second.
    - `gfx.safe()` gives the space the phone needs for its notch and corners. The score is placed inside it, and 80 units in from the left to clear Game Lab's back button.
    - A missing file never crashes a game. It shows a pink stand-in and one warning in the `dev.py` window.

## 4. Keep the best score

`save` stores values between runs. Add a best score:

```lua
local best = save.get("best", 0)
```

and where the score goes up:

```lua
      if score > best then
        best = score
        save.set("best", best)
      end
```

then draw it under the score:

```lua
  gfx.text("BEST " .. best, left + 80, top + 64, 22, 0.8, 0.8, 0.8)
```

Press ++f5++ to restart the game. The best score is still there.

## 5. Make the coins spin

Things that only look nice belong in `update` or `draw`, not `fixed_update`. `sys.time()` is the game clock:

```lua
  for _, c in ipairs(coins) do
    local bob = math.sin(sys.time() * 4 + c.x) * 0.15
    gfx.sphere(c.x, 0.6 + bob, c.z, 0.6, 1.0, 0.82, 0.20)
  end
```

## 6. Check it fits every phone

Close the game window, then at the prompt:

```
check coin-dash
```

This plays the game inside twelve iPhone shapes, upright and sideways, and lists any text, shape or touch control that a notch, island, corner or home bar would cover. Fix anything it reports, usually by using `gfx.safe()`. More in [Screens that fit every iPhone](../guides/layout.md).

## 7. Try these yourself

??? example "Exercise: lock the game to landscape"
    In `games/coin-dash/game.cfg`, remove the `#` from the orientation line and change it to:

    ```ini
    orientation = landscape
    ```

    See [game.cfg](../reference/game-cfg.md) for everything that file can hold.

??? example "Exercise: play a sound for each coin"
    Put a `.wav` file in the game's folder, then:

    ```lua
    local ding = audio.load("ding.wav")
    -- where the coin is collected:
    audio.play(ding, -6)
    ```

    Volumes are in decibels: `0` is full, `-6` about half. See [Audio](../engine/audio.md).

??? example "Exercise: add a jump button"
    Add a second entry to `input.controls`:

    ```lua
    { type = "button", id = "jump", x = 0.88, y = 0.76, size = 0.10, label = "A", key = "space" },
    ```

    `key = "space"` makes it work on the PC too. Read it with `input.pressed("jump")`.

## Where to go from here

- [Lua (gameplay)](../languages/lua.md) if Lua is new to you.
- [Lua API](../reference/lua-api.md) for everything the engine offers.
- [Slang (shaders)](../languages/slang.md) to change how things look.
- [Web calls](../engine/networking.md#the-games-own-backend) to give the game a leaderboard: `python tools/dev.py backend coin-dash`.
