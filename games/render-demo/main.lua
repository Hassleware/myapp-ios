-- A tiny toon-shaded island. Drag to turn it, pinch to zoom (on the PC: drag with the mouse, W and S zoom).
-- Everything is made here in Lua: no model or picture files. The look lives in shaders/toon.slang and shaders/water.slang.

local lib = include("lib.lua")

local cam = { yaw = 35, pitch = 24, distance = 15, spin = 0 }
local island, sea, ripples
local pinch                      -- distance between two fingers last frame

-- Colours are picked the usual way (as on a colour picker) and converted for mesh vertices, which want linear light.
local function lin(c) return c ^ 2.2 end

-- The island's height at a point. shaders/water.slang repeats this to know where the shore is: change both together.
local function island_height(x, z)
  return 1.6 * math.exp(-(x * x + z * z) / 10.24) + 0.5 * math.exp(-((x - 2.5) ^ 2 + (z + 1.5) ^ 2) / 3) - 0.35
end

-- A square grid of triangles. height and colour are functions of x and z. Layout "pc": position and colour.
local function grid(size, cells, height, colour)
  local v, idx = {}, {}
  for j = 0, cells do
    for i = 0, cells do
      local x, z = (i / cells - 0.5) * size, (j / cells - 0.5) * size
      local y = height(x, z)
      local r, g, b = colour(x, y, z)
      local n = #v
      v[n + 1], v[n + 2], v[n + 3], v[n + 4], v[n + 5], v[n + 6], v[n + 7] = x, y, z, lin(r), lin(g), lin(b), 1
    end
  end
  for j = 0, cells - 1 do
    for i = 0, cells - 1 do
      local a = j * (cells + 1) + i + 1
      local b, c, d = a + 1, a + cells + 1, a + cells + 2
      local n = #idx
      idx[n + 1], idx[n + 2], idx[n + 3], idx[n + 4], idx[n + 5], idx[n + 6] = a, c, b, b, c, d
    end
  end
  return gfx.new_mesh(v, idx, "pc")
end

-- A normal map of ripples, made from a few sine waves that fit the picture a whole number of times, so it tiles.
local function ripple_texture(size)
  local waves = { { 1, 2, 1.0, 0.3 }, { -2, 1, 0.8, 1.7 }, { 3, -1, 0.5, 2.9 }, { 2, 3, 0.45, 4.1 }, { -4, -3, 0.3, 0.9 }, { 5, 2, 0.25, 5.3 } }
  local rows, tau = {}, math.pi * 2
  for y = 0, size - 1 do
    local row = {}
    for x = 0, size - 1 do
      local u, w = x / size, y / size
      local dx, dy = 0, 0
      for _, k in ipairs(waves) do
        local slope = math.cos((u * k[1] + w * k[2]) * tau + k[4]) * k[3]
        dx, dy = dx + slope * k[1], dy + slope * k[2]
      end
      local nx, ny, nz = lib.normalize(-dx * 0.35, -dy * 0.35, 1)
      row[x + 1] = string.char(math.floor((nx * 0.5 + 0.5) * 255), math.floor((ny * 0.5 + 0.5) * 255), math.floor((nz * 0.5 + 0.5) * 255), 255)
    end
    rows[y + 1] = table.concat(row)
  end
  return gfx.new_texture(size, size, table.concat(rows), "data")
end

function init()
  island = grid(14, 56, island_height, function(_, y)
    if y < 0.05 then return 0.80, 0.70, 0.48 end       -- wet sand
    if y < 0.42 then return 0.96, 0.87, 0.62 end       -- dry sand
    if y < 0.50 then return 0.72, 0.78, 0.42 end       -- a thin strip where grass begins
    return 0.42, 0.74, 0.36                            -- grass
  end)
  sea = grid(160, 64, function() return 0 end, function() return 1, 1, 1 end)
  ripples = ripple_texture(128)
end

function update(dt)
  local fingers = {}
  for _, t in ipairs(input.touches()) do
    if not t.claimed and t.phase ~= "up" then fingers[#fingers + 1] = t end
  end
  if #fingers == 1 then
    cam.spin = -fingers[1].dx * 0.3 / math.max(dt, 0.001)
    cam.pitch = lib.clamp(cam.pitch + fingers[1].dy * 0.2, 6, 80)
    pinch = nil
  elseif #fingers >= 2 then
    local apart = lib.length(fingers[1].x - fingers[2].x, fingers[1].y - fingers[2].y)
    if pinch then cam.distance = cam.distance * pinch / math.max(apart, 1) end
    pinch = apart
  else
    pinch = nil
    cam.spin = lib.approach(cam.spin, 4, 2, dt)        -- let go: it coasts, then drifts round slowly
  end
  cam.yaw = cam.yaw + cam.spin * dt
  if input.key("w") or input.key("up") then cam.distance = cam.distance * (1 - dt) end
  if input.key("s") or input.key("down") then cam.distance = cam.distance * (1 + dt) end
  cam.distance = lib.clamp(cam.distance, 5, 34)
end

local function palm(x, z, lean, height, t)
  local y = island_height(x, z)
  local lx, lz = math.cos(lean), math.sin(lean)
  local topx, topy, topz
  gfx.param(1, 1)                                       -- ink edges on
  for i = 0, 9 do                                       -- the trunk: a bent stack of balls
    local k = i / 9
    topx, topy, topz = x + lx * k * k * 0.9, y + k * height, z + lz * k * k * 0.9
    gfx.sphere(topx, topy, topz, 0.30 - k * 0.10, 0.55, 0.38, 0.24)
  end
  for i = 0, 6 do                                       -- fronds: flattened balls drooping outward, swaying a little
    local turn = i * 360 / 7 + lean * 40
    local droop = 24 + math.sin(t * 1.3 + i) * 5
    local qx, qy, qz, qw = lib.quat_euler(turn, 0, -droop)
    local ox, oy, oz = lib.quat_rotate(qx, qy, qz, qw, 0.75, 0, 0)
    gfx.draw(gfx.SPHERE, topx + ox, topy + oy + 0.05, topz + oz, qx, qy, qz, qw, 1.7, 0.10, 0.50, 0.24, 0.62, 0.30, 1)
  end
  gfx.sphere(topx + 0.14, topy - 0.10, topz, 0.20, 0.36, 0.24, 0.14)
  gfx.sphere(topx - 0.10, topy - 0.12, topz + 0.12, 0.20, 0.36, 0.24, 0.14)
end

local function rock(x, z, size, squash)
  gfx.draw(gfx.SPHERE, x, island_height(x, z) + size * 0.2, z, 0, 0, 0, 1, size, size * squash, size * 0.85, 0.56, 0.57, 0.60, 1)
end

function draw()
  local t = sys.time()
  local sky = { 0.62, 0.84, 0.96 }
  gfx.clear(sky[1], sky[2], sky[3])
  gfx.fog(sky[1], sky[2], sky[3], 22, 75)               -- the sea fades into the sky, so there is no hard horizon
  gfx.ambient(0.52, 0.56, 0.68)
  gfx.light(-0.55, -0.85, -0.35, 0.85, 0.80, 0.68)
  gfx.shadows(0, 0, 0, 9, 1)                            -- full-strength shadows: the toon shader decides how dark they look

  local yaw, pitch = math.rad(cam.yaw), math.rad(cam.pitch)
  local d = cam.distance
  gfx.camera(math.sin(yaw) * math.cos(pitch) * d, 0.8 + math.sin(pitch) * d, math.cos(yaw) * math.cos(pitch) * d, 0, 0.8, 0, 45, 0.3, 300)

  gfx.shader("toon")
  gfx.param(1, 0)
  gfx.draw(island, 0, 0, 0)

  palm(-0.6, 0.4, 0.6, 2.6, t)
  palm(0.9, -0.8, 2.6, 2.1, t + 2)
  palm(2.6, -1.4, 4.4, 1.7, t + 4)
  rock(-2.4, 1.6, 0.9, 0.6)
  rock(-2.9, 1.1, 0.5, 0.7)
  rock(1.6, 2.6, 0.7, 0.55)
  rock(3.9, 0.6, 0.6, 0.6)
  -- A towel and a beach ball.
  local ty = island_height(-1.2, 2.6)
  gfx.cube(-1.2, ty + 0.03, 2.6, 0.9, 0.05, 1.5, 0.92, 0.30, 0.30, 25)
  gfx.cube(-1.2, ty + 0.035, 2.6, 0.9, 0.05, 0.3, 0.98, 0.95, 0.90, 25)
  gfx.sphere(-0.3, island_height(-0.3, 2.9) + 0.22, 2.9, 0.44, 1.0, 0.80, 0.25)

  -- The sea goes last: it is see-through at the shore, so the sand has to be there already.
  gfx.shader("water")
  gfx.texture(2, ripples)
  gfx.blend("alpha")
  gfx.draw(sea, 0, 0, 0)
  gfx.reset()

  local left, top = gfx.safe()
  gfx.text("RENDER DEMO", left + 80, top + 24, 22, 1, 1, 1)
  gfx.text("DRAG TO TURN   PINCH TO ZOOM", left + 80, top + 54, 12, 1, 1, 1, 0.8)
end
