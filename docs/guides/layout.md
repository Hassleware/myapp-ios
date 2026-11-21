# Screens that fit every iPhone

iPhones differ in size and shape, and each hides part of the screen behind rounded corners, a notch or Dynamic Island, and the home bar. A layout that looks right in a plain window can have its score under the notch on a real phone. This page is how to avoid that.

## The coordinate system

2D drawing uses **units**, not pixels. The screen's **shorter side is always 720 units**. The longer side is whatever the phone's shape makes it.

```lua
local w, h = gfx.size()        -- for example 720 x 1558 upright on an iPhone 11
```

So a button 100 units wide is the same fraction of the screen's width on every phone, and text 28 units tall is equally readable everywhere.

## The safe area

```lua
local left, top, right, bottom = gfx.safe()
```

These are how far to stay in from each edge. Draw backgrounds edge to edge. Keep everything a player must **read or touch** inside the insets.

```lua
function draw()
  local w, h = gfx.size()
  local left, top, right, bottom = gfx.safe()

  gfx.rect(0, 0, w, h, 0.1, 0.1, 0.15)                       -- background: the whole screen
  gfx.text("SCORE 12", left + 80, top + 24, 32, 1, 1, 1)      -- top-left, inside
  gfx.text("PAUSE", w - right - 140, top + 24, 32, 1, 1, 1)   -- top-right, inside
end
```

The top-left starts 80 units in because Game Lab draws its own back button in that corner while you are developing. A standalone release has no back button.

Lay things out from the edges inward: `left + margin`, `w - right - margin - width`. Never from fixed numbers.

Touch controls placed with `input.controls` use fractions of the screen and are kept clear of the safe area by the engine.

## Both ways up

If a game should only be played one way, say so in its `game.cfg`:

```ini
orientation = landscape
```

Otherwise the phone may be turned, `gfx.size()` will change, and the layout must work both ways. Compute positions inside `draw()` from the current size rather than once in `init()`.

## The check

```
python tools/dev.py check <game>
```

This plays the game inside twelve iPhone shapes, from the iPhone 11 to the 17 Pro Max, upright and sideways (or only the orientation `game.cfg` allows). For each, it lists any text, rectangle, circle or touch control that a corner, notch, island or home bar covers.

**Run it after any change to a game's 2D drawing or controls.**

How it works: the engine logs the box of everything drawn in 2D, and compares those boxes against the hidden regions of each phone.

::: source engine/phone_frame.cpp layout_check_frame

Its limits, so you know what a pass means:

- **It only sees what the game draws in its first moments.** A menu that appears later, or a game-over screen, is not checked. Give yourself a way to start the game directly in that screen (a global you can set, or a line you temporarily add) and run the check again.
- **Anything covering more than 60% of the screen is treated as background** and ignored.
- **The phone sizes are approximate.** The real phone is the final word.

## Looking at it yourself

In the PC window, ++f1++ steps through the same phone shapes and draws the corners, notch or island, home bar and the phone's body. ++f2++ turns it. The team's test phone is the iPhone 11 shape, the first frame.

```powershell
$env:GAMELAB_DEVICE = "1u"         # start in frame 1, upright
python tools/dev.py run <game>
```

## A layout that adapts

```lua
local function layout()
  local w, h = gfx.size()
  local left, top, right, bottom = gfx.safe()
  local x, width = left + 30, w - left - right - 60        -- a column inside the safe area
  return x, top + 30, width
end
```

The test game builds its whole screen from one function like this, called from both `update` (to know where the buttons are) and `draw` (to draw them), so the two can never disagree:

::: source games/net-demo/main.lua layout

## Checklist

- [ ] Nothing a player reads or touches is placed from a fixed number near an edge
- [ ] Positions are computed in `draw()` from `gfx.size()` and `gfx.safe()`
- [ ] `game.cfg` locks the orientation, or the layout works both ways
- [ ] Text fields are in the top half, clear of the keyboard
- [ ] `check <game>` reports nothing covered
- [ ] Later screens (menus, game over) were reached and checked too
- [ ] It was looked at on a real phone
