-- Dream Table's world: the table stands on a cobbled floor on a big floating cloud, under a deep blue sky with
-- giant beanstalks rising out of the clouds below, puffy clouds on their tops and drifting about.
-- Everything is made here in code at the first draw: two textures (sky, cobbles) and a few meshes.

local lib = include("lib.lua")
local S = {}

local FLOOR_W, FLOOR_Z0, FLOOR_Z1 = 5, -8, 10    -- the cobbled floor: width, far and near end (metres)
local COBBLE_TILE = 1.6                           -- metres per copy of the cobble texture
S.FOG = { 0.62, 0.80, 1.0 }                      -- the haze far away, matched to the sky at the horizon

-- A repeatable random sequence, so the world is the same on every reload.
local seed = 1
local function rnd(a, b)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return a + (b - a) * seed / 2147483648
end

local function pack(w, h, pixel)
  local rows = {}
  for y = 0, h - 1 do
    local row = {}
    for x = 0, w - 1 do
      local r, g, b = pixel(x, y)
      row[#row + 1] = string.char(math.floor(math.min(1, r) * 255), math.floor(math.min(1, g) * 255), math.floor(math.min(1, b) * 255), 255)
    end
    rows[#rows + 1] = table.concat(row)
  end
  return table.concat(rows)
end

-- The sky, wrapped round the camera: top to bottom it runs zenith to straight down.
-- Deep blue overhead, bright blue at the horizon, pale haze below (the cloud sea under the island).
local function sky_texture()
  local W, H = 64, 256
  return gfx.new_texture(W, H, pack(W, H, function(_, y)
    local up = 1 - (y + 0.5) / H * 2                  -- 1 at the top, 0 at the horizon, -1 below
    if up > 0 then
      local t = up ^ 0.55
      return 0.36 + (0.03 - 0.36) * t, 0.66 + (0.10 - 0.66) * t, 1.0 + (0.62 - 1.0) * t
    end
    local t = math.min(1, -up * 6)
    return 0.36 + (0.82 - 0.36) * t, 0.66 + (0.92 - 0.66) * t, 1.0
  end), "clamp")
end

-- Cobbles: a jittered grid of stones (each pixel takes its nearest stone), mortar where two are nearly as
-- close, each stone its own grey-green, domed a little. Wraps seamlessly.
local function cobble_texture()
  local N, CELLS = 256, 7
  local stones = {}
  for cy = 0, CELLS - 1 do
    for cx = 0, CELLS - 1 do
      stones[cy * CELLS + cx] = { (cx + rnd(0.15, 0.85)) / CELLS, (cy + rnd(0.15, 0.85)) / CELLS, rnd(0.82, 1.08), rnd(-0.03, 0.03) }
    end
  end
  return gfx.new_texture(N, N, pack(N, N, function(x, y)
    local u, v = (x + 0.5) / N, (y + 0.5) / N
    local gx, gy = math.floor(u * CELLS), math.floor(v * CELLS)
    local d1, d2, best = 9, 9, nil
    for oy = -1, 1 do
      for ox = -1, 1 do
        local cx, cy = gx + ox, gy + oy
        local s = stones[(cy % CELLS) * CELLS + (cx % CELLS)]
        local dx, dy = u - (s[1] + (cx - cx % CELLS) / CELLS), v - (s[2] + (cy - cy % CELLS) / CELLS)
        local d = math.sqrt(dx * dx + dy * dy)
        if d < d1 then d2, d1, best = d1, d, s elseif d < d2 then d2 = d end
      end
    end
    local edge = (d2 - d1) * CELLS                   -- 0 on the line between two stones
    local k = best[3] * (0.86 + 0.14 * math.min(1, edge * 3)) * (1 - d1 * CELLS * 0.18)
    if edge < 0.07 then k = 0.55 end                 -- mortar
    return 0.72 * k, (0.76 + best[4]) * k, 0.70 * k
  end))
end

-- A quad lying flat, its texture repeating every `tile` metres.
local function floor_mesh(x0, z0, x1, z1, tile)
  local n = {}
  for _, c in ipairs({ { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } }) do
    for _, v in ipairs({ c[1], 0, c[2], 0, 1, 0, c[1] / tile, c[2] / tile }) do n[#n + 1] = v end
  end
  return gfx.new_mesh(n, { 1, 3, 2, 1, 4, 3 }, "pnu")
end

-- A beanstalk: a tube winding up round a slow spiral, thick at the root and thin at the top.
local function stalk_mesh(height, wind, turns, r0, r1)
  local n, idx, SIDES, STEPS = {}, {}, 10, 48
  local function spine(t)
    local a = t * turns * math.pi * 2
    return math.sin(a) * wind * (1 - t * 0.5), t * height, math.cos(a) * wind * (1 - t * 0.5)
  end
  for i = 0, STEPS do
    local t = i / STEPS
    local x, y, z = spine(t)
    local x2, y2, z2 = spine(math.min(1, t + 0.01))
    local x0, y0, z0 = spine(math.max(0, t - 0.01))
    local tx, ty, tz = x2 - x0, y2 - y0, z2 - z0
    local tl = math.sqrt(tx * tx + ty * ty + tz * tz)
    tx, ty, tz = tx / tl, ty / tl, tz / tl
    -- two directions across the tube
    local ax, ay, az = tz, 0, -tx
    local al = math.sqrt(ax * ax + az * az) + 1e-6
    ax, az = ax / al, az / al
    local bx, by, bz = ty * az - tz * ay, tz * ax - tx * az, tx * ay - ty * ax
    local r = r0 + (r1 - r0) * t
    for s = 0, SIDES do
      local a = s / SIDES * math.pi * 2
      local nx, ny, nz = ax * math.cos(a) + bx * math.sin(a), ay * math.cos(a) + by * math.sin(a), az * math.cos(a) + bz * math.sin(a)
      for _, v in ipairs({ x + nx * r, y + ny * r, z + nz * r, nx, ny, nz, s / SIDES, t * 8 }) do n[#n + 1] = v end
    end
  end
  for i = 0, STEPS - 1 do
    for s = 0, SIDES - 1 do
      local a = i * (SIDES + 1) + s + 1
      local b = a + SIDES + 1
      for _, v in ipairs({ a, a + 1, b, a + 1, b + 1, b }) do idx[#idx + 1] = v end
    end
  end
  return gfx.new_mesh(n, idx, "pnu"), spine
end

-- A cloud: a handful of balls, the biggest in the middle and flattened underneath.
local function new_cloud(x, y, z, size)
  local balls = {}
  local count = math.floor(rnd(5, 8))
  for i = 1, count do
    local a = (i - 1) / count * math.pi * 2 + rnd(-0.3, 0.3)
    local spread = size * rnd(0.35, 0.6)
    balls[i] = { x + math.cos(a) * spread, y + rnd(0, 0.3) * size, z + math.sin(a) * spread * 0.6, size * rnd(0.55, 0.85) }
  end
  balls[#balls + 1] = { x, y + size * 0.3, z, size * 1.1 }
  return balls
end

local made = false
local sky, cobbles, floor, stalks, clouds, island = nil, nil, nil, {}, {}, {}

local function make()
  made = true
  sky, cobbles = sky_texture(), cobble_texture()
  floor = floor_mesh(-FLOOR_W / 2, FLOOR_Z0, FLOOR_W / 2, FLOOR_Z1, COBBLE_TILE)

  -- The island: big cloud balls along the floor's edges and under it, so the floor sits in the cloud.
  local function lump(x, z, size) island[#island + 1] = { x, -0.275 * size + rnd(0.1, 0.45), z, size } end   -- tops just above the floor
  for z = FLOOR_Z0, FLOOR_Z1, 1.3 do
    for _, side in ipairs({ -1, 1 }) do
      lump(side * (FLOOR_W / 2 + rnd(0.2, 0.9)), z + rnd(-0.4, 0.4), rnd(2.0, 3.2))
      lump(side * (FLOOR_W / 2 + rnd(2.0, 3.5)), z + rnd(-0.4, 0.4), rnd(2.8, 4.0))
    end
  end
  for x = -FLOOR_W / 2 - 2, FLOOR_W / 2 + 2, 1.4 do
    lump(x + rnd(-0.3, 0.3), FLOOR_Z0 - rnd(0.3, 1.2), rnd(2.2, 3.4))
  end
  for z = FLOOR_Z0, FLOOR_Z1, 3 do
    for x = -FLOOR_W / 2, FLOOR_W / 2, 3 do island[#island + 1] = { x, -2.2, z, 4.5 } end
  end

  -- Beanstalks far off, rising out of the cloud sea below, each with a cloud on top and a few leaves.
  local spots = { { -26, -48 }, { 20, -40 }, { -9, -70 }, { 40, -66 }, { -44, -30 }, { 7, -95 } }
  for i, p in ipairs(spots) do
    local height = rnd(40, 58)
    local mesh, spine = stalk_mesh(height, rnd(1.2, 2.2), rnd(1.0, 1.8), rnd(1.6, 2.2), rnd(0.5, 0.8))
    local base = -30
    local tx, ty, tz = spine(1)
    local leaves = {}
    for k = 1, 4 do
      local t = rnd(0.35, 0.9)
      local lx, ly, lz = spine(t)
      local a, size = rnd(0, math.pi * 2), rnd(3, 5)
      local out = size * 0.8 + 1                     -- the leaf sticks out sideways from the stalk
      leaves[k] = { p[1] + lx + math.cos(a) * out, base + ly, p[2] + lz + math.sin(a) * out, -math.deg(a), size }
    end
    stalks[i] = { mesh = mesh, x = p[1], y = base, z = p[2], leaves = leaves,
                  cloud = new_cloud(p[1] + tx, base + ty + 1.5, p[2] + tz, rnd(4, 6)) }
  end

  -- Loose clouds drifting in the distance.
  for _ = 1, 14 do
    local a = rnd(-math.pi * 0.85, -math.pi * 0.15)
    local d = rnd(45, 110)
    clouds[#clouds + 1] = { cloud = new_cloud(math.cos(a) * d, rnd(-6, 22), math.sin(a) * d, rnd(3, 7)), speed = rnd(0.3, 0.8) }
  end
end

local STALK, LEAF, CLOUD = { 0.24, 0.62, 0.42 }, { 0.20, 0.72, 0.30 }, { 1, 1, 1 }

local function draw_balls(balls, dx)
  for _, b in ipairs(balls) do
    gfx.draw(gfx.SPHERE, b[1] + dx, b[2], b[3], 0, 0, 0, 1, b[4], b[4] * 0.8, b[4], CLOUD[1], CLOUD[2], CLOUD[3], 1)
  end
end

-- The light, sky and haze; call before drawing anything else.
function S.lights()
  if not made then make() end
  gfx.sky(sky, 1, 1, 1)
  gfx.fog(S.FOG[1], S.FOG[2], S.FOG[3], 50, 240)
  gfx.light(-0.45, -0.8, -0.35, 1.05, 1.0, 0.92)
  gfx.ambient(0.52, 0.60, 0.78)
end

function S.draw(time)
  gfx.cull(false)
  gfx.texture(1, cobbles)
  gfx.draw(floor, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.texture(1, nil)
  gfx.cull(true)

  gfx.cast_shadows(false)
  for _, b in ipairs(island) do
    gfx.draw(gfx.SPHERE, b[1], b[2], b[3], 0, 0, 0, 1, b[4], b[4] * 0.55, b[4], 0.97, 0.98, 1, 1)
  end
  gfx.cull(false)
  for _, s in ipairs(stalks) do
    gfx.draw(s.mesh, s.x, s.y, s.z, 0, 0, 0, 1, 1, 1, 1, STALK[1], STALK[2], STALK[3], 1)
    for _, l in ipairs(s.leaves) do
      local qx, qy, qz, qw = lib.quat_euler(l[4], 0, 25)
      gfx.draw(gfx.SPHERE, l[1], l[2], l[3], qx, qy, qz, qw, l[5] * 1.6, 0.25, l[5], LEAF[1], LEAF[2], LEAF[3], 1)
    end
  end
  gfx.cull(true)
  for _, s in ipairs(stalks) do draw_balls(s.cloud, 0) end
  for _, c in ipairs(clouds) do
    draw_balls(c.cloud, math.sin(time * c.speed * 0.05) * 12)   -- a slow sway from side to side
  end
  gfx.cast_shadows(true)
end

return S
