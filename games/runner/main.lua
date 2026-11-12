-- A small 3D platformer. Edit this file and save: the game reloads on the PC and on the phone.
--
-- The engine calls three functions if you define them:
--   init()       once, when the game starts or reloads
--   update(dt)   every frame, dt is the seconds since the last frame
--   draw()       every frame, after update

input.controls {
  { type = "stick",  id = "move", x = 0.13, y = 0.74, size = 0.14 },
  { type = "button", id = "jump", x = 0.88, y = 0.76, size = 0.10, label = "A", key = "space" },
}

local GRAVITY, SPEED, JUMP = 28, 7, 11

-- Each platform: centre x, y, z and size x, y, z.
local platforms = {
  { 0, -0.5, 0,   10, 1, 10 },
  { 9, 0.5, 0,    4, 1, 4 },
  { 15, 1.5, 2,   4, 1, 4 },
  { 21, 2.5, -1,  4, 1, 4 },
  { 28, 2.5, 0,   8, 1, 8 },
}
local coinSpots = { { 9, 2, 0 }, { 15, 3, 2 }, { 21, 4, -1 }, { 28, 4, 2 }, { 30, 4, -2 } }

local player, coins, score, spin
local best = save.get("best", 0)

function init()
  player = { x = 0, y = 2, z = 0, vy = 0, grounded = false, facing = 0 }
  coins = {}
  for i, c in ipairs(coinSpots) do coins[i] = { x = c[1], y = c[2], z = c[3], taken = false } end
  score, spin = 0, 0
end

function update(dt)
  local mx, my = input.stick("move")
  player.x = player.x + mx * SPEED * dt
  player.z = player.z + my * SPEED * dt
  if mx ~= 0 or my ~= 0 then player.facing = math.deg(math.atan(mx, my)) end

  if input.pressed("jump") and player.grounded then player.vy = JUMP end
  player.vy = player.vy - GRAVITY * dt
  local previousFeet = player.y - 0.5
  player.y = player.y + player.vy * dt
  player.grounded = false

  for _, p in ipairs(platforms) do
    local top = p[2] + p[5] / 2
    local inside = math.abs(player.x - p[1]) < p[4] / 2 + 0.4 and math.abs(player.z - p[3]) < p[6] / 2 + 0.4
    if inside and player.vy <= 0 and previousFeet >= top - 0.05 and player.y - 0.5 <= top then
      player.y, player.vy, player.grounded = top + 0.5, 0, true
    end
  end

  for _, c in ipairs(coins) do
    local dx, dy, dz = player.x - c.x, player.y - c.y, player.z - c.z
    if not c.taken and dx * dx + dy * dy + dz * dz < 1.2 then
      c.taken = true
      score = score + 1
      if score > best then
        best = score
        save.set("best", best)
      end
    end
  end

  spin = spin + dt * 120
  if player.y < -15 then init() end
end

function draw()
  gfx.clear(0.45, 0.68, 0.92)
  gfx.light(-0.5, -1, -0.35)
  gfx.camera(player.x - 2, player.y + 6, player.z + 11, player.x + 2, player.y, player.z, 55)

  for _, p in ipairs(platforms) do
    gfx.cube(p[1], p[2], p[3], p[4], p[5], p[6], 0.36, 0.62, 0.42)
  end
  for _, c in ipairs(coins) do
    if not c.taken then gfx.cube(c.x, c.y, c.z, 0.6, 0.6, 0.15, 1.0, 0.82, 0.25, spin) end
  end
  gfx.cube(player.x, player.y, player.z, 0.8, 1, 0.8, 0.95, 0.42, 0.32, player.facing)

  local w = gfx.size()
  local label = "COINS " .. score .. "/" .. #coins .. "   BEST " .. math.floor(best)
  local size = math.min(24, (w - 160) / #label)   -- shrink to fit when the phone is held upright
  gfx.text(label, (w - gfx.text_width(label, size)) / 2, 28, size, 1, 1, 1)
end
