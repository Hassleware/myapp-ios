-- Dream Table's world, seen from above: a big flat cloud floor with a ring of puffy clouds round its edge, and in
-- the middle an uneven patch of river-pebble cobbles (stones thinning out into the cloud at its edge) where the
-- table stands. Clouds are drawn with shaders/cloud.slang, the pebbles with shaders/stone.slang. Beyond: a deep blue sky, giant beanstalks rising out of the clouds below, clouds on their tops.
-- Everything is made here in code at the first draw, with a fixed random seed so it is the same every time.

local lib = include("lib.lua")
local S = {}

S.FOG = { 0.55, 0.76, 1.0 }                      -- the haze far away, matched to the sky at the horizon
local PATCH_X, PATCH_Z = 3.1, 5.4                -- the cobble patch's half size (metres); its outline is wobbly
local COBBLE_TILE = 1.6                           -- metres per copy of the cobble picture (it repeats)

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

-- ---------------------------------------------------------------- the cobble patch

-- How far a point is toward the patch's wobbly edge: 0 at the middle, 1 on the edge.
local function wobble(a) return 1 + 0.10 * math.sin(3 * a + 1.3) + 0.07 * math.sin(5 * a + 0.4) + 0.04 * math.sin(9 * a + 2.0) end
local function patch_reach(x, z)
  return math.sqrt((x / PATCH_X) ^ 2 + (z / PATCH_Z) ^ 2) / wobble(math.atan(z / PATCH_Z, x / PATCH_X))
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
      local shade = rnd(0.62, 0.82)
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
        colors[i] = { 0.3 * speck, 0.27 * speck, 0.24 * speck, 0.55 }
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

-- ---------------------------------------------------------------- the cloud ground

-- Smooth value noise, 0 to 1, for the ground's lumps.
local lattice = {}
for i = 0, 63 * 64 + 63 do lattice[i] = rnd(0, 1) end
local function noise(x, z)
  local ix, iz = math.floor(x), math.floor(z)
  local fx, fz = x - ix, z - iz
  fx, fz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
  local function at(a, b) return lattice[(b % 64) * 64 + (a % 64)] end
  local top = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * fx
  local bottom = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * fx
  return top + (bottom - top) * fz
end
local function smooth(a, b, x) local t = math.max(0, math.min(1, (x - a) / (b - a))) return t * t * (3 - 2 * t) end

-- The ground the cobbles lie on: flat cloud under the patch, swelling into big soft mounds round it, then
-- falling away at the far edges so the sky and the cloud sea show beyond.
local GROUND_X, GROUND_Z0, GROUND_Z1 = 6.5, -10, 12
local function ground_height(x, z)
  local ring = smooth(1.12, 2.1, patch_reach(x, z))
  local lumps = noise(x * 0.11 + 3, z * 0.11 + 5) * 0.7 + noise(x * 0.31, z * 0.31) * 0.3
  local h = -0.04 + ring * (0.1 + 0.5 * lumps)
  local edge = math.max(math.abs(x) / GROUND_X, z < 0 and -z / -GROUND_Z0 or z / GROUND_Z1)
  return h - smooth(0.72, 1.0, edge) * 5
end

local function ground_mesh()
  local GX, GZ = 72, 80
  local n, idx = {}, {}
  for j = 0, GZ do
    for i = 0, GX do
      local x = -GROUND_X + 2 * GROUND_X * i / GX
      local z = GROUND_Z0 + (GROUND_Z1 - GROUND_Z0) * j / GZ
      local y = ground_height(x, z)
      local e = 0.3
      local nx, nz = ground_height(x - e, z) - ground_height(x + e, z), ground_height(x, z - e) - ground_height(x, z + e)
      local l = math.sqrt(nx * nx + 4 * e * e + nz * nz)
      for _, v in ipairs({ x, y, z, nx / l, 2 * e / l, nz / l, math.max(0, math.min(1, (y + 1) / 4.5)), 0.35 }) do n[#n + 1] = v end
    end
  end
  for j = 0, GZ - 1 do
    for i = 0, GX - 1 do
      local a = j * (GX + 1) + i + 1
      local b = a + GX + 1
      for _, v in ipairs({ a, b + 1, a + 1, a, b, b + 1 }) do idx[#idx + 1] = v end
    end
  end
  return gfx.new_mesh(n, idx, "pnu")
end

-- A tube through a list of points (each {x, y, z}), radius(t) along it, texture u round and v along.
local function tube_mesh(points, radius, sides, vscale)
  local n, idx, count = {}, {}, #points
  for i = 1, count do
    local p, q, r = points[i], points[math.min(count, i + 1)], points[math.max(1, i - 1)]
    local tx, ty, tz = q[1] - r[1], q[2] - r[2], q[3] - r[3]
    local tl = math.sqrt(tx * tx + ty * ty + tz * tz) + 1e-9
    tx, ty, tz = tx / tl, ty / tl, tz / tl
    local ux, uy, uz = 0, 1, 0                    -- a side direction: across the tube, as level as possible
    if math.abs(ty) > 0.9 then ux, uy = 1, 0 end
    local ax, ay, az = ty * uz - tz * uy, tz * ux - tx * uz, tx * uy - ty * ux
    local al = math.sqrt(ax * ax + ay * ay + az * az)
    ax, ay, az = ax / al, ay / al, az / al
    local bx, by, bz = ty * az - tz * ay, tz * ax - tx * az, tx * ay - ty * ax
    local t = (i - 1) / (count - 1)
    local rad = radius(t)
    for s = 0, sides do
      local a = s / sides * math.pi * 2
      local nx, ny, nz = ax * math.cos(a) + bx * math.sin(a), ay * math.cos(a) + by * math.sin(a), az * math.cos(a) + bz * math.sin(a)
      for _, v in ipairs({ p[1] + nx * rad, p[2] + ny * rad, p[3] + nz * rad, nx, ny, nz, s / sides, t * vscale }) do n[#n + 1] = v end
    end
  end
  for i = 0, count - 2 do
    for s = 0, sides - 1 do
      local a = i * (sides + 1) + s + 1
      local b = a + sides + 1
      for _, v in ipairs({ a, a + 1, b, a + 1, b + 1, b }) do idx[#idx + 1] = v end
    end
  end
  return gfx.new_mesh(n, idx, "pnu")
end

-- ---------------------------------------------------------------- building the world

local made = false
local white, patch, patch_tex, patch_normal, ground = nil, nil, nil, nil, nil
local stalk_tex, stalk_normal, leaf, leaf_tex, leaf_normal = nil, nil, nil, nil, nil
local stalks, puffs, drifting, vines, vine_leaves = {}, {}, {}, {}, {}
local still_clouds = nil

-- A cumulus cloud: a wide base of flattened puffs, then smaller puffs stacked up into a tower `tall` high.
-- Each puff remembers the whole cloud's base and top (for the shader's shading) and its bump size.
local function cumulus(list, x, y, z, wide, tall)
  local levels = math.max(1, math.floor(tall / wide * 3 + 0.5))
  local base, top = y - wide * 0.15, y + tall
  local function puff(px, py, pz, size)
    list[#list + 1] = { px, py, pz, size, rnd(0, 360), base, top, 1.7 / size }
  end
  local count = math.floor(rnd(6, 9))
  for i = 1, count do
    local a = (i - 1) / count * math.pi * 2 + rnd(-0.3, 0.3)
    puff(x + math.cos(a) * wide * rnd(0.3, 0.5), y + rnd(0, 0.1) * wide, z + math.sin(a) * wide * rnd(0.2, 0.35), wide * rnd(0.4, 0.55))
  end
  for l = 1, levels do
    local f = l / (levels + 1)
    local spread = wide * 0.35 * (1 - f * 0.6)
    local ly = y + tall * f
    for k = 1, 4 do
      local a = rnd(0, math.pi * 2)
      puff(x + math.cos(a) * spread, ly + rnd(-0.05, 0.05) * tall, z + math.sin(a) * spread * 0.6, wide * 0.5 * (1 - f * 0.45) * rnd(0.8, 1.1))
    end
  end
  puff(x, y + tall * levels / (levels + 1), z, wide * 0.42)
  -- Small lobes on the upper and outer surface of the big puffs, for a cauliflower outline.
  local first = #list - count - levels * 4
  for i = first, #list do
    local b = list[i]
    for _ = 1, 2 do
      local a, up = rnd(0, math.pi * 2), rnd(0.1, 0.9)
      local r = b[4] * 0.5 * 0.62
      local side = math.sqrt(1 - up * up)
      list[#list + 1] = { b[1] + math.cos(a) * side * r, b[2] + up * r * 0.8, b[3] + math.sin(a) * side * r,
                          b[4] * rnd(0.45, 0.6), rnd(0, 360), base, top, 1.7 / (b[4] * 0.5) }
    end
  end
end

-- All the puffs in a list baked into one mesh, so a whole sky of clouds is one draw. A low-poly ball per puff
-- (the shader's bumps hide the facets); texture u carries how high up its cloud the point is (0 base, 1 top)
-- and v the bump frequency, which is what shaders/cloud.slang reads.
local BALL_RINGS, BALL_SEGS = 10, 16
local ball = {}
for r = 0, BALL_RINGS do
  for sgm = 0, BALL_SEGS do
    local phi, th = math.pi * r / BALL_RINGS, math.pi * 2 * sgm / BALL_SEGS
    ball[#ball + 1] = { math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th) }
  end
end
local ball_idx = {}
for r = 0, BALL_RINGS - 1 do
  for sgm = 0, BALL_SEGS - 1 do
    local a = r * (BALL_SEGS + 1) + sgm
    local b = a + BALL_SEGS + 1
    for _, v in ipairs({ a, a + 1, b, a + 1, b + 1, b }) do ball_idx[#ball_idx + 1] = v end
  end
end

local function puffs_mesh(list)
  local n, idx = {}, {}
  for i, b in ipairs(list) do
    local rx, ry = b[4] * 0.5, b[4] * 0.5 * (b.flat or 0.8)
    local first = (i - 1) * #ball
    for _, v in ipairs(ball) do
      local y = b[2] + v[2] * ry
      local nx, ny, nz = v[1] / rx, v[2] / ry, v[3] / rx
      local l = math.sqrt(nx * nx + ny * ny + nz * nz)
      local h = math.max(0, math.min(1, (y - b[6]) / (b[7] - b[6])))
      for _, x in ipairs({ b[1] + v[1] * rx, y, b[3] + v[3] * rx, nx / l, ny / l, nz / l, h, b[8] }) do n[#n + 1] = x end
    end
    for _, k in ipairs(ball_idx) do idx[#idx + 1] = first + k + 1 end
  end
  return gfx.new_mesh(n, idx, "pnu")
end

local function make()
  made = true
  white = gfx.new_texture(1, 1, string.char(255, 255, 255, 255))
  patch_tex, patch_normal = cobble_textures()
  patch = patch_mesh()
  ground = ground_mesh()
  stalk_tex, stalk_normal = stalk_textures()
  leaf = leaf_mesh()
  leaf_tex, leaf_normal = leaf_textures()

  -- The island's puffy rim: big soft puffs all round the edge where the ground falls away, a few stacked up.
  for k = 0, 33 do
    local a = k / 34 * math.pi * 2 + rnd(-0.05, 0.05)
    local x, z = math.cos(a) * GROUND_X * 0.86, math.sin(a) > 0 and math.sin(a) * GROUND_Z1 * 0.86 or math.sin(a) * -GROUND_Z0 * 0.86
    local size = rnd(2.6, 4.2)
    puffs[#puffs + 1] = { x, rnd(-0.9, -0.4), z, size, rnd(0, 360), -2.5, 1.5, 1.7 / size }
    if k % 5 == 2 and z < 8 then cumulus(puffs, x * 1.05, 0.2, z * 1.05, rnd(2.5, 3.5), rnd(2, 4)) end
  end

  -- Towering cumulus far away, and the cloud sea below the horizon.
  for _, t in ipairs({ { 55, -16, -85, 34, 52 }, { -70, -18, -120, 36, 40 }, { -110, -14, -40, 30, 30 } }) do
    cumulus(puffs, t[1], t[2], t[3], t[4], t[5])
  end
  for _ = 1, 36 do
    local a, d = rnd(0, math.pi * 2), rnd(40, 170)
    local size = rnd(14, 26)
    puffs[#puffs + 1] = { math.cos(a) * d, rnd(-42, -36), math.sin(a) * d, size, rnd(0, 360), -46, -30, 1.7 / size, flat = 0.4 }
  end

  -- Beanstalks far off, rising out of the cloud sea, each with leaves and a cloud on top.
  local spots = { { -26, -48 }, { 20, -40 }, { -9, -70 }, { 40, -66 }, { -44, -30 }, { 7, -95 } }
  for i, p in ipairs(spots) do
    local height = rnd(40, 58)
    local r0, r1 = rnd(1.6, 2.2), rnd(0.5, 0.8)
    local mesh, spine = stalk_mesh(height, rnd(1.2, 2.2), rnd(1.0, 1.8), r0, r1)
    local base = -30
    local leaves = {}
    for k = 1, 7 do
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
    cumulus(top, p[1] + tx, base + ty + 1, p[2] + tz, rnd(5, 7), rnd(3, 5))
    stalks[i] = { mesh = mesh, x = p[1], y = base, z = p[2], leaves = leaves, top = top }
  end

  -- Loose clouds in the distance, swaying slowly.
  for _ = 1, 5 do
    local a = rnd(-math.pi * 0.9, -math.pi * 0.1)
    local d = rnd(55, 120)
    local c = { speed = rnd(0.3, 0.8) }
    cumulus(c, math.cos(a) * d, rnd(4, 26), math.sin(a) * d, rnd(6, 11), rnd(3, 7))
    drifting[#drifting + 1] = c
  end

  -- Vines along the patch's edge, winding over and under the cobbles' fringe, with small leaves.
  for k = 1, 3 do
    local a0 = (k - 1) / 3 * math.pi * 2 + rnd(0, 0.5)
    local span = rnd(1.4, 1.9)
    local pts = {}
    for i = 0, 90 do
      local a = a0 + span * i / 90
      local reach = (1.08 + 0.05 * math.sin(a * 17 + k)) * wobble(a)
      local x, z = math.cos(a) * PATCH_X * reach, math.sin(a) * PATCH_Z * reach
      pts[#pts + 1] = { x, 0.02 + 0.05 * math.max(0, math.sin(a * 23 + k * 2)), z }
      if i % 9 == 4 then
        local side = (i % 18 == 4) and 1 or -1
        local qx, qy, qz, qw = lib.quat_axis(0, 1, 0, -math.deg(a) + 90 * side + rnd(-30, 30))
        vine_leaves[#vine_leaves + 1] = { x, 0.08, z, qx, qy, qz, qw, rnd(0.35, 0.55) }
      end
    end
    vines[#vines + 1] = tube_mesh(pts, function(t) return 0.07 * (1 - t * 0.5) end, 6, 40)
  end

  for _, st in ipairs(stalks) do for _, b in ipairs(st.top) do puffs[#puffs + 1] = b end end
  still_clouds = puffs_mesh(puffs)
  for _, c in ipairs(drifting) do c.mesh = puffs_mesh(c) end
end

-- The light, sky and haze; call before drawing anything else.
function S.lights()
  if not made then make() end
  gfx.shader("sky")
  gfx.sky(white, 1, 1, 1)
  gfx.shader()
  gfx.fog(S.FOG[1], S.FOG[2], S.FOG[3], 60, 260)
  gfx.light(-0.75, -0.6, -0.15, 1.15, 1.05, 0.92)
  gfx.ambient(0.50, 0.58, 0.76)
end

function S.draw(time)
  gfx.cast_shadows(false)
  -- The cloud ground, then the cobbles laid on it (see-through where there are no stones).
  gfx.shader("cloud")
  gfx.cull(false)
  gfx.param(1, 0, 0, 0, 0.7)
  gfx.draw(ground, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.shader("stone")
  gfx.texture(1, patch_tex)
  gfx.texture(2, patch_normal)
  gfx.cutoff(0.5)
  gfx.draw(patch, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.cutoff(0)
  gfx.shader()

  gfx.texture(1, stalk_tex)
  gfx.texture(2, stalk_normal)
  for _, s in ipairs(stalks) do gfx.draw(s.mesh, s.x, s.y, s.z, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1) end
  for _, v in ipairs(vines) do gfx.draw(v, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0.8, 0.95, 0.8, 1) end
  gfx.texture(1, leaf_tex)
  gfx.texture(2, leaf_normal)
  for _, s in ipairs(stalks) do
    for _, l in ipairs(s.leaves) do gfx.draw(leaf, l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[8], l[8], 1, 1, 1, 1) end
  end
  for _, l in ipairs(vine_leaves) do gfx.draw(leaf, l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[8], l[8], 1, 1, 1, 1) end
  gfx.texture(1, nil)
  gfx.texture(2, nil)
  gfx.cull(true)

  gfx.shader("cloud")
  gfx.param(1, 0, 0, 0, 0.45)
  gfx.draw(still_clouds, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  for _, c in ipairs(drifting) do gfx.draw(c.mesh, math.sin(time * c.speed * 0.05) * 12, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1) end
  gfx.shader()
  gfx.cast_shadows(true)
end

return S
