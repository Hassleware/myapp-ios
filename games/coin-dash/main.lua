input.controls {
  { type = "stick", id = "move", x = 0.15, y = 0.75, size = 0.14 },
}

local x, z = 0, 0
local coins, score = {}, 0
local best = save.get("best", 0)

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
  if score > best then
    best = score
    save.set("best", best)
  end
  if #coins == 0 then scatter() end

end

function draw()
  gfx.clear(0.10, 0.12, 0.18)
  gfx.camera(0, 12, 12, 0, 0, 0, 50)
  gfx.plane(0, 0, 0, 16, 16, 0.22, 0.28, 0.36)
  gfx.cube(x, 0.5, z, 1, 1, 1, 0.36, 0.82, 0.63)
  for _, c in ipairs(coins) do
    local t = sys.time()
    local bob = math.sin(t * 4 + c.x) * 0.15
    local turn = t * 3 + c.z               -- each coin spins at its own offset
    -- a squashed sphere standing on its edge (thin in z), turned about the up axis
    gfx.draw(gfx.SPHERE, c.x, 0.7 + bob, c.z,  0, math.sin(turn / 2), 0, math.cos(turn / 2),  0.6, 0.6, 0.15,  1.0, 0.82, 0.20, 1)
  end
  local left, top = gfx.safe()
  gfx.text("SCORE " .. score, left + 80, top + 24, 32, 1, 1, 1)
  gfx.text("BEST " .. best, left + 80, top + 64, 22, 0.8, 0.8, 0.8)
end
