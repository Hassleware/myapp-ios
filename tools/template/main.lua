-- A new game. Edit this file and save: it reloads on the PC and on the phone.

input.controls {
  { type = "stick",  id = "move", x = 0.13, y = 0.74, size = 0.14 },
  { type = "button", id = "jump", x = 0.88, y = 0.76, size = 0.10, label = "A", key = "space" },
}

local x, y, z, vy = 0, 0.5, 0, 0

function init()
  x, y, z, vy = 0, 0.5, 0, 0
end

function update(dt)
  local mx, my = input.stick("move")
  x = x + mx * 6 * dt
  z = z + my * 6 * dt
  if input.pressed("jump") and y <= 0.5 then vy = 9 end
  vy = vy - 25 * dt
  y = math.max(0.5, y + vy * dt)
  if y == 0.5 then vy = 0 end
end

function draw()
  gfx.clear(0.10, 0.12, 0.18)
  gfx.camera(0, 7, 12, 0, 0, 0, 55)
  gfx.plane(0, 0, 0, 16, 16, 0.25, 0.30, 0.38)
  gfx.cube(x, y, z, 1, 1, 1, 0.36, 0.82, 0.63)
  local left, top = gfx.safe()   -- room the phone needs for its notch or island
  gfx.text("NEW GAME", left + 80, top + 24, 28, 1, 1, 1)
end
