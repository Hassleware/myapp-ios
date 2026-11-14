# Lua: the language of gameplay

Lua is a small scripting language made to be embedded in programs written in C and C++. It is widely used in games (Roblox, World of Warcraft add-ons, many studio engines). You can learn all of it in a day. This page teaches the parts you will use here, then how this project uses them.

Try every example by pasting it into a game's `main.lua` and saving. `print` output appears in the `dev.py` window.

## The language in ten minutes

### Values and variables

```lua
local score = 0                 -- a number (whole or fractional; Lua 5.4 has both)
local name = "coin"             -- a string
local alive = true              -- a boolean
local nothing = nil             -- "no value"

score = score + 1               -- there is no += in Lua
local text = name .. " x" .. score     -- .. joins strings
```

!!! warning "Always write `local`"
    A variable without `local` is global: visible everywhere, and silently `nil` if you misspell it. Call `lib.strict()` once at the top of a game and a misspelt global becomes an error instead.

### Tables: the only data structure

A table is a list, a record and a dictionary in one.

```lua
local coins = { 10, 20, 30 }              -- a list. It counts from 1, not 0
print(coins[1], #coins)                   -- 10   3      (# is the length)

local player = { x = 0, y = 1, name = "Ann" }     -- a record
player.x = player.x + 1
player["name"] = "Bo"                     -- the same thing as player.name

table.insert(coins, 40)                   -- add to the end
table.remove(coins, 1)                    -- remove the first
```

### Control flow

```lua
if score > 10 then
  print("big")
elseif score > 5 then
  print("medium")
else
  print("small")
end

for i = 1, 5 do print(i) end              -- 1 2 3 4 5
for i = 10, 1, -1 do print(i) end         -- counting down

for index, value in ipairs(coins) do      -- a list, in order
  print(index, value)
end
for key, value in pairs(player) do        -- every field of a record, in no fixed order
  print(key, value)
end

while score < 100 do score = score * 2 end
```

Only `nil` and `false` count as false. **Zero and the empty string are true**, unlike in C or Python.

```lua
local speed = options.speed or 5          -- "or" gives a default when the left side is nil
local label = alive and "ALIVE" or "DEAD" -- the usual way to pick one of two values
```

### Functions

```lua
local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function size() return 720, 1280 end    -- functions can return several values
local w, h = size()
```

Functions are values. You can store them in tables and pass them to other functions, which is how callbacks work:

```lua
net.request({ url = "https://api.github.com/zen" }, function(status, body, err)
  print(status, body, err)
end)
```

### Methods and the colon

`a:b(c)` is shorthand for `a.b(a, c)`. You will see it on strings and on the objects `lib.lua` makes:

```lua
local s = "hello"
print(s:upper(), s:sub(1, 2), #s)         -- HELLO   he   5
```

## How this project uses Lua

### A game is a folder with a `main.lua`

The engine runs `main.lua` from top to bottom once, then calls your functions by name. This is the part of the engine that does the calling:

::: source engine/script.cpp update

Read it and you know the order for certain: text input, answers to web requests, `fixed_update` (zero or more times), `update`, then `late_update`.

### What you can and cannot use

Games get the engine's tables plus Lua's `table`, `string`, `math`, `utf8` and `coroutine` libraries. This is where that is decided:

::: source engine/script.cpp load

There is no `io`, `os`, `require` or `debug`. A game cannot read arbitrary files or run programs, by design: games are downloaded over Wi-Fi and must stay harmless.

| Instead of | Use |
|---|---|
| `require("x")`, `dofile` | `include("x.lua")`, which reads from the game's own folder |
| `io.open` to save progress | `save.get` and `save.set` |
| `os.time`, `os.clock` | `sys.time()` and `sys.real_time()` |
| `math.pow(a, b)` (removed in Lua 5.4) | `a ^ b` |

### Splitting a game across files

`include` runs another file and gives back whatever it returns. The convention is that a file builds a table and returns it:

```lua
-- enemies.lua
local enemies = {}
function enemies.spawn(x, z) ... end
return enemies
```

```lua
-- main.lua
local enemies = include("enemies.lua")
enemies.spawn(3, 4)
```

Any pure-Lua library works the same way: drop the file in the game's folder and `include` it. Nothing else can be loaded, so libraries that need C code will not work.

### State survives nothing, so set it up in `init`

A hot reload throws away the whole Lua state and runs `main.lua` again. Anything you want kept across a reload has to go through `save`. In return, a reload always starts clean, so you never debug leftovers from an earlier version of your code.

### Scripts that wait

Lua's coroutines let a function pause and continue later. `lib.lua` wraps them so you can write a sequence of events top to bottom:

::: source tools/template/lib.lua lib.run lib.wait lib.after

```lua
local lib = include("lib.lua")

lib.run(function()
  door.open = true
  lib.wait(2)             -- two seconds pass, across many frames
  door.open = false
end)

function update(dt)
  lib.update(dt)          -- this is what moves the waiting scripts on
end
```

### Speed

Lua is fast enough for the logic of a game: hundreds of objects updated each frame is fine. Two habits keep it that way:

- **Do not create tables every frame.** `local p = {x, y, z}` inside `update`, for a thousand objects, makes work for the garbage collector. Reuse tables, or pass plain numbers. This is why the engine's functions take `x, y, z` rather than a vector table.
- **Let the engine do the heavy work.** Collision tests against a level, skinning a character and mixing sound all happen in C++.

++f3++ shows how much memory Lua is holding.

## Exercises

??? example "1. Why does this print `nil`?"
    ```lua
    local list = { "a", "b", "c" }
    print(list[0])
    ```
    Lists count from 1. `list[1]` is `"a"`.

??? example "2. Write a function that returns the nearest coin to a point"
    ```lua
    local function nearest(coins, x, z)
      local best, best_distance
      for _, c in ipairs(coins) do
        local d = (c.x - x) ^ 2 + (c.z - z) ^ 2
        if not best or d < best_distance then
          best, best_distance = c, d
        end
      end
      return best
    end
    ```
    Comparing squared distances avoids a square root for every coin.

??? example "3. Make a message appear for three seconds after the game starts"
    ```lua
    local lib = include("lib.lua")
    local message = "GO!"
    function init() lib.after(3, function() message = nil end) end
    function update(dt) lib.update(dt) end
    function draw()
      gfx.clear(0, 0, 0)
      if message then gfx.text(message, 100, 100, 60, 1, 1, 1) end
    end
    ```

## Where next

- [Lua API](../reference/lua-api.md): every function the engine provides.
- [lib.lua helpers](../reference/lib.md): what is already written for you in Lua.
- The reference manual at lua.org is short and readable; chapter 6 covers the standard libraries.
