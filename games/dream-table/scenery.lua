-- Dream Table's world, seen from above: a big flat cloud floor with a ring of puffy clouds round its edge, and in
-- the middle an uneven patch of river-pebble cobbles (stones thinning out into the cloud at its edge) where the
-- table stands. Clouds are drawn with shaders/cloud.slang, the pebbles with shaders/stone.slang. Beyond: a deep blue sky, giant beanstalks rising out of the clouds below, clouds on their tops.
-- Everything is made here in code at the first draw, with a fixed random seed so it is the same every time.

local lib = include("lib.lua")
local S = {}

S.FOG = { 0.55, 0.76, 1.0 }                      -- the haze far away, matched to the sky at the horizon
local PATCH_X, PATCH_Z = 4.6, 7.2                -- the cobble patch's half size (metres); its outline is wobbly
local COBBLE_TILE = 1.2                           -- metres per copy of the cobble picture (it repeats)
local CLOUD_FLOOR_X, CLOUD_FLOOR_Z0, CLOUD_FLOOR_Z1 = 13, -18, 14   -- the flat cloud: half width, far and near end

local seed = 1
local function rnd(a, b)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return a + (b - a) * seed / 2147483648
end

local function byte(v) return string.char(math.floor(math.max(0, math.min(1, v)) * 255 + 0.5)) end

-- Builds a texture from pixel(x, y) -> r, g, b [, a].
local function texture(w, h, pixel, options)
  local rows = {}
  for y = 0, h - 1 do
    local row = {}
    for x = 0, w - 1 do
      local r, g, b, a = pixel(x, y)
      row[#row + 1] = byte(r) .. byte(g) .. byte(b) .. byte(a or 1)
    end
    rows[#rows + 1] = table.concat(row)
  end
  return gfx.new_texture(w, h, table.concat(rows), options)
end

-- A normal map from a height function over a w x h grid (heights in texels), wrapping if asked.
local function normal_map(w, h, height, strength, wrap)
  local function at(x, y)
    if wrap then x, y = x % w, y % h else x, y = math.max(0, math.min(w - 1, x)), math.max(0, math.min(h - 1, y)) end
    return height[y * w + x + 1]
  end
  return texture(w, h, function(x, y)
    local nx, ny = (at(x - 1, y) - at(x + 1, y)) * strength, (at(x, y - 1) - at(x, y + 1)) * strength
    local l = math.sqrt(nx * nx + ny * ny + 1)
    return nx / l * 0.5 + 0.5, ny / l * 0.5 + 0.5, 1 / l * 0.5 + 0.5
  end, "data" .. (wrap and "" or " clamp"))
end

-- ---------------------------------------------------------------- sky

-- Wrapped round the camera, top to bottom from straight up to straight down: dark blue high up, light blue at
-- the horizon, pale below (the cloud sea under the floating cloud).
local function sky_texture()
  local W, H = 8, 256
  local DARK, LIGHT, BELOW = { 0.01, 0.05, 0.36 }, { 0.48, 0.76, 1.0 }, { 0.80, 0.90, 1.0 }
  return texture(W, H, function(_, y)
    local up = 1 - (y + 0.5) / H * 2
    if up >= 0 then
      local t = math.min(1, up / 0.12) ^ 0.7
      return LIGHT[1] + (DARK[1] - LIGHT[1]) * t, LIGHT[2] + (DARK[2] - LIGHT[2]) * t, LIGHT[3] + (DARK[3] - LIGHT[3]) * t
    end
    local t = math.min(1, -up * 5)
    return LIGHT[1] + (BELOW[1] - LIGHT[1]) * t, LIGHT[2] + (BELOW[2] - LIGHT[2]) * t, LIGHT[3] + (BELOW[3] - LIGHT[3]) * t
  end, "clamp")
end

-- ---------------------------------------------------------------- the cobble patch

-- How far a point is toward the patch's wobbly edge: 0 at the middle, 1 on the edge.
local function patch_reach(x, z)
  local a = math.atan(z / PATCH_Z, x / PATCH_X)
  local wobble = 1 + 0.10 * math.sin(3 * a + 1.3) + 0.07 * math.sin(5 * a + 0.4) + 0.04 * math.sin(9 * a + 2.0)
  return math.sqrt((x / PATCH_X) ^ 2 + (z / PATCH_Z) ^ 2) / wobble
end

-- River-pebble cobbles: rows of smooth oval stones, long side across the path, packed tight with dark gaps.
-- One repeating picture (COBBLE_TILE metres). Each stone is domed (the normal map) and has its own shade, and
-- its own number in the alpha channel; the patch mesh fades its vertex alpha toward the edge, and gfx.cutoff
-- drops a stone once its number times that alpha falls below one half. So toward the edge whole stones go
-- missing (the fade is slow next to a stone's size), the gaps go first, and the patch frays into the cloud.
local function cobble_textures()
  local N, ROWS, COLS = 320, 16, 10
  local CW, CH = N / COLS, N / ROWS              -- 32 x 20 texels a cell: stones about 12 x 7.5 cm
  local stones = {}
  for i = 0, ROWS - 1 do
    for j = 0, COLS - 1 do
      local shade = rnd(0.42, 0.72)
      local warm = rnd(0.0, 0.06)
      stones[i * COLS + j] = {
        x = (j + 0.5 + (i % 2) * 0.5 + rnd(-0.2, 0.2)) * CW, y = (i + 0.5 + rnd(-0.18, 0.18)) * CH,
        a = CW * 0.5 * rnd(0.8, 1.15), b = CH * 0.5 * rnd(0.8, 1.15), turn = rnd(-0.35, 0.35),
        keep = rnd(0.55, 1.0), r = shade + warm, g = shade + warm * 0.4, b2 = shade - warm * 0.6 }
    end
  end
  local heights, colors = {}, {}
  for y = 0, N - 1 do
    local gi = math.floor(y / CH)
    for x = 0, N - 1 do
      local best, bq = nil, 1
      for ii = gi - 1, gi + 1 do
        local gj = math.floor((x - (ii % 2) * 0.5 * CW) / CW)
        for jj = gj - 1, gj + 1 do
          local st = stones[(ii % ROWS) * COLS + (jj % COLS)]
          local dx = x - (st.x + (jj - jj % COLS) * CW)
          local dy = y - (st.y + (ii - ii % ROWS) * CH)
          local c, sn = math.cos(st.turn), math.sin(st.turn)
          local u, v = (dx * c + dy * sn) / st.a, (-dx * sn + dy * c) / st.b
          local q = u * u + v * v
          if q < bq then best, bq = st, q end
        end
      end
      local i = y * N + x + 1
      local speck = rnd(0.95, 1.05)
      if best then
        local dome = math.sqrt(1 - bq)
        heights[i] = dome * 7
        local k = (0.68 + 0.32 * dome) * speck     -- darker where it dips into the gaps
        colors[i] = { best.r * k, best.g * k, best.b2 * k, best.keep }
      else
        heights[i] = 0
        colors[i] = { 0.2 * speck, 0.18 * speck, 0.16 * speck, 0.55 }
      end
    end
  end
  local color = texture(N, N, function(x, y) local c = colors[y * N + x + 1] return c[1], c[2], c[3], c[4] end)
  return color, normal_map(N, N, heights, 1.0, true)
end

-- The patch: a grid whose vertex alpha is 1 in the middle and fades past the wobbly edge (see cobble_textures).
local function patch_mesh()
  local GX, GZ, X, Z = 32, 48, PATCH_X * 1.2, PATCH_Z * 1.2
  local n, idx = {}, {}
  for j = 0, GZ do
    for i = 0, GX do
      local x, z = -X + 2 * X * i / GX, -Z + 2 * Z * j / GZ
      local reach = patch_reach(x, z)
      local alpha = reach < 0.7 and 1 or reach < 1 and 1 - (reach - 0.7) / 0.3 * 0.5 or math.max(0, 0.5 - (reach - 1) * 5)
      for _, v in ipairs({ x, 0, z, 0, 1, 0, x / COBBLE_TILE, z / COBBLE_TILE, 1, 1, 1, alpha }) do n[#n + 1] = v end
    end
  end
  for j = 0, GZ - 1 do
    for i = 0, GX - 1 do
      local a = j * (GX + 1) + i + 1
      local b = a + GX + 1
      for _, v in ipairs({ a, b + 1, a + 1, a, b, b + 1 }) do idx[#idx + 1] = v end
    end
  end
  return gfx.new_mesh(n, idx, "pnuc")
end

-- ---------------------------------------------------------------- meshes

-- Flat-shaded triangles: each corner {x, y, z, u, v}, the normal worked out per triangle, and an optional
-- colour per triangle from color(nx, ny, nz).
local function flat_mesh(tris, color)
  local n, idx = {}, {}
  for _, t in ipairs(tris) do
    local a, b, c = t[1], t[2], t[3]
    local ux, uy, uz = b[1] - a[1], b[2] - a[2], b[3] - a[3]
    local vx, vy, vz = c[1] - a[1], c[2] - a[2], c[3] - a[3]
    local nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    local l = math.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
    nx, ny, nz = nx / l, ny / l, nz / l
    for _, p in ipairs({ a, b, c }) do
      for _, v in ipairs({ p[1], p[2], p[3], nx, ny, nz, p[4] or 0, p[5] or 0 }) do n[#n + 1] = v end
      if color then local r, g, bl = color(nx, ny, nz) for _, v in ipairs({ r, g, bl, 1 }) do n[#n + 1] = v end end
      idx[#idx + 1] = #idx + 1
    end
  end
  return gfx.new_mesh(n, idx, color and "pnuc" or "pnu")
end

-- A flat quad lying at height y, with texture coordinates 0 to 1 across it.
local function quad_mesh(x0, z0, x1, z1)
  return gfx.new_mesh({ x0, 0, z0, 0, 1, 0, 0, 0,  x1, 0, z0, 0, 1, 0, 1, 0,  x1, 0, z1, 0, 1, 0, 1, 1,  x0, 0, z1, 0, 1, 0, 0, 1 },
                      { 1, 3, 2, 1, 4, 3 }, "pnu")
end

-- A beanstalk: a tube winding up a slow spiral, thick at the root and thin at the top. The texture runs round
-- it (u) and along it (v, repeating).
local function stalk_mesh(height, wind, turns, r0, r1)
  local n, idx, SIDES, STEPS = {}, {}, 10, 40
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
    local ax, az = tz, -tx
    local al = math.sqrt(ax * ax + az * az) + 1e-6
    ax, az = ax / al, az / al
    local bx, by, bz = ty * az, tz * ax - tx * az, -ty * ax
    local r = r0 + (r1 - r0) * t
    for s = 0, SIDES do
      local a = s / SIDES * math.pi * 2
      local nx, ny, nz = ax * math.cos(a) + bx * math.sin(a), by * math.sin(a), az * math.cos(a) + bz * math.sin(a)
      for _, v in ipairs({ x + nx * r, y + ny * r, z + nz * r, nx, ny, nz, s / SIDES, t * height / 6 }) do n[#n + 1] = v end
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

-- A leaf, one unit long from its stalk end (at the origin) to its tip along +x: a pointed oval folded a little
-- along its middle vein and drooping toward the tip. Texture: u along it, v across.
local function leaf_mesh()
  local STEPS, tris, rows = 8, {}, {}
  for i = 0, STEPS do
    local t = i / STEPS
    local w = 0.45 * math.sin(math.pi * t) ^ 0.8 + 0.02
    local droop = -0.22 * t * t
    rows[i] = { { t, droop, -w, t, 0 }, { t, droop + 0.05 * (1 - t), 0, t, 0.5 }, { t, droop, w, t, 1 } }
  end
  for i = 0, STEPS - 1 do
    for j = 1, 2 do
      local a, b, c, d = rows[i][j], rows[i][j + 1], rows[i + 1][j], rows[i + 1][j + 1]
      tris[#tris + 1] = { a, b, c }
      tris[#tris + 1] = { b, d, c }
    end
  end
  return flat_mesh(tris)
end

-- Beanstalk skin: ridges running along it, lighter on their backs, with streaks; and its normal map.
local function stalk_textures()
  local W, H, RIDGES = 64, 128, 7
  local heights, streak = {}, {}
  for x = 0, W - 1 do streak[x] = rnd(0.9, 1.1) end
  for y = 0, H - 1 do
    for x = 0, W - 1 do heights[y * W + x + 1] = (0.5 + 0.5 * math.cos(x / W * math.pi * 2 * RIDGES)) * 3 end
  end
  local color = texture(W, H, function(x, y)
    local ridge = heights[y * W + x + 1] / 3
    local k = (0.75 + 0.3 * ridge) * streak[x] * (0.97 + 0.03 * math.sin(y / H * math.pi * 6 + x))
    return 0.20 * k, 0.56 * k, 0.33 * k
  end)
  return color, normal_map(W, H, heights, 0.6, true)
end

-- Leaf skin: green, a pale middle vein, side veins slanting toward the tip, darker at the rim.
local function leaf_textures()
  local W, H = 128, 64
  local heights = {}
  local function vein(x, y)
    local u, v = x / W, y / H
    local across = math.abs(v - 0.5) * 2
    local mid = math.max(0, 1 - math.abs(v - 0.5) * H / 2.5)
    local side = math.max(0, 1 - math.abs(((u - across * 0.25) * 9) % 1 - 0.5) * 2 * 6) * (1 - across) * (u < 0.95 and 1 or 0)
    return math.max(mid, side * 0.6), across
  end
  for y = 0, H - 1 do
    for x = 0, W - 1 do heights[y * W + x + 1] = vein(x, y) * 2 end
  end
  local color = texture(W, H, function(x, y)
    local v, across = vein(x, y)
    local k = 1 - across * across * 0.3
    return (0.22 + 0.35 * v) * k, (0.66 + 0.2 * v) * k, (0.26 + 0.25 * v) * k
  end, "clamp")
  return color, normal_map(W, H, heights, 0.8, false)
end

-- ---------------------------------------------------------------- building the world

local made = false
local sky, patch, patch_tex, patch_normal, cloud_floor = nil, nil, nil, nil, nil
local stalk_tex, stalk_normal, leaf, leaf_tex, leaf_normal = nil, nil, nil, nil, nil
local stalks, puffs, drifting = {}, {}, {}

local function add_cloud(list, x, y, z, size)
  local count = math.floor(rnd(4, 7))
  for i = 1, count do
    local a = (i - 1) / count * math.pi * 2 + rnd(-0.4, 0.4)
    local spread = size * rnd(0.4, 0.65)
    local s = size * rnd(0.6, 0.9)
    list[#list + 1] = { x + math.cos(a) * spread, y + rnd(0, 0.25) * size, z + math.sin(a) * spread * 0.7, s, rnd(0, 360) }
  end
  list[#list + 1] = { x, y + size * 0.3, z, size * 1.15, rnd(0, 360) }
end

local function make()
  made = true
  sky = sky_texture()
  patch_tex, patch_normal = cobble_textures()
  patch = patch_mesh()
  cloud_floor = quad_mesh(-CLOUD_FLOOR_X, CLOUD_FLOOR_Z0, CLOUD_FLOOR_X, CLOUD_FLOOR_Z1)
  stalk_tex, stalk_normal = stalk_textures()
  leaf = leaf_mesh()
  leaf_tex, leaf_normal = leaf_textures()

  -- Real clouds all round the flat cloud's edge.
  local function edge_cloud(x, z) add_cloud(puffs, x, rnd(-0.5, 0.3), z, rnd(2.2, 3.6)) end
  for z = CLOUD_FLOOR_Z0, CLOUD_FLOOR_Z1, 3.2 do
    edge_cloud(-CLOUD_FLOOR_X + rnd(-0.5, 0.5), z + rnd(-0.6, 0.6))
    edge_cloud(CLOUD_FLOOR_X + rnd(-0.5, 0.5), z + rnd(-0.6, 0.6))
  end
  for x = -CLOUD_FLOOR_X, CLOUD_FLOOR_X, 3.2 do
    edge_cloud(x + rnd(-0.6, 0.6), CLOUD_FLOOR_Z0 + rnd(-0.5, 0.5))
  end

  -- Beanstalks far off, rising out of the cloud sea below, each with leaves and a cloud on top.
  local spots = { { -26, -48 }, { 20, -40 }, { -9, -70 }, { 40, -66 }, { -44, -30 }, { 7, -95 } }
  for i, p in ipairs(spots) do
    local height = rnd(40, 58)
    local r0, r1 = rnd(1.6, 2.2), rnd(0.5, 0.8)
    local mesh, spine = stalk_mesh(height, rnd(1.2, 2.2), rnd(1.0, 1.8), r0, r1)
    local base = -30
    local leaves = {}
    for k = 1, 6 do
      local t = rnd(0.35, 0.92)
      local lx, ly, lz = spine(t)
      local a = rnd(0, math.pi * 2)
      local r = r0 + (r1 - r0) * t
      local ax, ay, az, aw = lib.quat_axis(0, 1, 0, -math.deg(a))
      local qx, qy, qz, qw = lib.quat_mul(ax, ay, az, aw, lib.quat_axis(0, 0, 1, rnd(5, 30)))
      leaves[k] = { p[1] + lx + math.cos(a) * r * 0.8, base + ly, p[2] + lz + math.sin(a) * r * 0.8, qx, qy, qz, qw, rnd(5, 8) }
    end
    local tx, ty, tz = spine(1)
    local top = {}
    add_cloud(top, p[1] + tx, base + ty + 1.5, p[2] + tz, rnd(4, 6))
    stalks[i] = { mesh = mesh, x = p[1], y = base, z = p[2], leaves = leaves, top = top }
  end

  -- Loose clouds in the distance, swaying slowly.
  for _ = 1, 12 do
    local a = rnd(-math.pi * 0.85, -math.pi * 0.15)
    local d = rnd(45, 110)
    local c = { speed = rnd(0.3, 0.8) }
    add_cloud(c, math.cos(a) * d, rnd(-6, 22), math.sin(a) * d, rnd(3, 7))
    drifting[#drifting + 1] = c
  end
end

local function draw_puffs(list, dx)
  for _, b in ipairs(list) do
    local qx, qy, qz, qw = lib.quat_axis(0, 1, 0, b[5])
    gfx.draw(gfx.SPHERE, b[1] + dx, b[2], b[3], qx, qy, qz, qw, b[4], b[4] * 0.75, b[4], 1, 1, 1, 1)
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
  -- The flat cloud, then the cobbles laid on it (see-through where there are no stones).
  gfx.draw(cloud_floor, 0, -0.005, 0, 0, 0, 0, 1, 1, 1, 1, 0.76, 0.84, 0.97, 1)
  gfx.texture(1, patch_tex)
  gfx.texture(2, patch_normal)
  gfx.cutoff(0.5)
  gfx.shader("stone")
  gfx.draw(patch, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.shader()
  gfx.cutoff(0)

  gfx.cast_shadows(false)
  gfx.cull(false)
  gfx.texture(1, stalk_tex)
  gfx.texture(2, stalk_normal)
  for _, s in ipairs(stalks) do gfx.draw(s.mesh, s.x, s.y, s.z, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1) end
  gfx.texture(1, leaf_tex)
  gfx.texture(2, leaf_normal)
  for _, s in ipairs(stalks) do
    for _, l in ipairs(s.leaves) do gfx.draw(leaf, l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[8], l[8], 1, 1, 1, 1) end
  end
  gfx.texture(1, nil)
  gfx.texture(2, nil)
  gfx.cull(true)

  gfx.shader("cloud")
  draw_puffs(puffs, 0)
  for _, s in ipairs(stalks) do draw_puffs(s.top, 0) end
  for _, c in ipairs(drifting) do draw_puffs(c, math.sin(time * c.speed * 0.05) * 12) end
  gfx.shader()
  gfx.cast_shadows(true)
end

return S
