-- A new game. Save this file and it reloads on the PC and on the phone.
-- The engine calls the functions below if they exist; delete the ones you don't need.
-- Every engine function is in the handbook (python tools/dev.py docs), under Reference > Lua API.

local lib = include("lib.lua")   -- helpers written in Lua: timers, state machines, vectors (see lib.lua)

-- Once at start, and again after every reload. Set up the game here.
function init()
end

-- Exactly 60 times a second: movement, collision, anything that must not depend on the frame rate.
function fixed_update(dt)
end

-- Once a frame, before drawing.
function update(dt)
  lib.update(dt)   -- moves lib.after / lib.run timers on
end

-- Once a frame. All drawing goes here: 3D first, then 2D on top.
function draw()
  gfx.clear(0.08, 0.09, 0.13)

  -- 3D: a camera, a light, and things in the world.
  gfx.camera(0, 4, 8, 0, 0, 0, 50)
  gfx.light(-0.4, -1, -0.5)

  -- 2D: the screen's shorter side is 720 units. Keep text inside gfx.safe() to clear the notch and corners.
  local w, h = gfx.size()
  local left, top = gfx.safe()
  local title = "NEW GAME"
  gfx.text(title, (w - gfx.text_width(title, 48)) / 2, h / 2 - 24, 48, 1, 1, 1)
  gfx.text("edit main.lua", left + 80, top + 24, 22, 0.6, 0.65, 0.75)
end

-- The app went to the background (save here) and came back.
function on_pause() end
function on_resume() end
