-- Shows a custom shader. The shader is in shaders/pulse.slang.
-- Edit it and save: tools/dev.py recompiles it and the game reloads.

local t = 0

function update(dt)
  t = t + dt
end

function draw()
  gfx.clear(0.05, 0.05, 0.09)
  gfx.camera(math.sin(t * 0.4) * 9, 4, math.cos(t * 0.4) * 9, 0, 0.5, 0, 55)

  gfx.plane(0, -1, 0, 14, 14, 0.16, 0.17, 0.22)

  gfx.shader("pulse")                       -- everything drawn next uses shaders/pulse.slang
  gfx.sphere(0, 0.8, 0, 2.6, 0.30, 0.75, 1.0)
  gfx.cube(-3.5, 0, 0, 1.5, 1.5, 1.5, 1.0, 0.45, 0.35, t * 40)
  gfx.cube(3.5, 0, 0, 1.5, 1.5, 1.5, 0.55, 1.0, 0.50, -t * 40)
  gfx.shader()                              -- back to the standard shader

  local w, h = gfx.size()
  -- Upright, the notch or island sits at the top, so the caption moves below it.
  gfx.text("CUSTOM SHADER:\nshaders/pulse.slang", 80, h > w and 110 or 24, 18, 1, 1, 1)
end
