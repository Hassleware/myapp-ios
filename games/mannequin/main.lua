-- The wooden mannequin playing a mocap take. mannequin.glb is made on the PC by tools/blender_mannequin.py
-- (the mannequin, plus the newest take retargeted onto it). Drag to turn the camera round; tap to change clip.

local lib = include("lib.lua")

local dummy = model.load("mannequin.glb")
local anim = lib.animation(dummy)
local clips = model.clips(dummy)
local current, yaw = 1, 0.3

local function play(i)
  current = i
  if clips[i] then anim:play(clips[i].name, { loop = true, fade = 0.3 }) end
end

function init()
  play(1)
end

function update(dt)
  anim:update(dt)
  for _, t in ipairs(input.touches()) do
    if t.phase == "move" then yaw = yaw - t.dx * 0.01 end
    if t.phase == "up" and math.abs(t.dx) < 2 and #clips > 1 then play(current % #clips + 1) end
  end
end

function draw()
  gfx.clear(0.14, 0.13, 0.16)
  gfx.camera(math.sin(yaw) * 4, 1.4, math.cos(yaw) * 4, 0, 0.95, 0, 45)
  gfx.light(-0.4, -1, -0.3, 1.0, 0.95, 0.85)
  gfx.ambient(0.35, 0.33, 0.38)
  gfx.shadows(0, 0.5, 0, 2.5, 0.6)
  gfx.plane(0, 0, 0, 8, 8, 0.32, 0.30, 0.34)
  model.draw(dummy, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, anim.animator)

  local left, top = gfx.safe()
  local name = clips[current] and clips[current].name or "no clips in mannequin.glb"
  gfx.text("MANNEQUIN  -  " .. name, left + 80, top + 24, 26, 1, 1, 1)
  gfx.text(#clips .. " clip(s). Drag to turn, tap for the next.", left + 80, top + 60, 18, 0.7, 0.7, 0.8)
end
