-- Fruit Ninja-style watermelons, built only on input.touches(). Swipe fast through a melon:
-- it is cut along the exact line you drew, and the cut faces show the flesh and the seeds
-- that line passes through. All of it is Lua; nothing here needs an app build.

local lib = include("lib.lua")

local SIZE = 1.6          -- a melon's diameter, in world units
local GRAVITY = 12
local SLASH_SPEED = 1200  -- 2D units a second; slower drags don't cut

local melons, pieces, juice, trails, score = {}, {}, {}, {}, 0
local sphere = {}         -- the melon's triangles: each corner is {x, y, z, nx, ny, nz, u, v}, radius 0.5
local sphere_mesh, rind
local cap_users = {}      -- cut-face texture -> how many pieces still show it

-- ---------------------------------------------------------------- textures, made from numbers

local function texture(w, h, colour)
  local bytes = {}
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local r, g, b = colour((i + 0.5) / w, (j + 0.5) / h)
      bytes[#bytes + 1] = string.char(math.floor(r), math.floor(g), math.floor(b), 255)
    end
  end
  return gfx.new_texture(w, h, table.concat(bytes), "clamp")
end

-- Dark wavy stripes running pole to pole.
local function make_rind()
  return texture(128, 64, function(u, v)
    local wave = math.sin(u * math.pi * 2 * 9 + 0.7 * math.sin(v * math.pi * 5 + u * 20))
    local speck = math.random() * 18
    if wave > 0.15 then return 40 + speck, 92 + speck, 38 end
    return 110 + speck, 165 + speck, 70
  end)
end

-- ---------------------------------------------------------------- the melon's shape

local function build_sphere()
  local slices, rings = 24, 14
  local grid = {}
  for r = 0, rings do
    local lat = math.pi * r / rings
    for s = 0, slices do
      local lon = math.pi * 2 * s / slices
      local nx, ny, nz = math.sin(lat) * math.cos(lon), math.cos(lat), math.sin(lat) * math.sin(lon)
      grid[r * (slices + 1) + s + 1] = { nx * 0.5, ny * 0.5, nz * 0.5, nx, ny, nz, s / slices, r / rings }
    end
  end
  local flat = {}
  local function corner(p)
    sphere[#sphere + 1] = p
    for i = 1, 8 do flat[#flat + 1] = p[i] end
  end
  for r = 0, rings - 1 do
    for s = 0, slices - 1 do
      local a = grid[r * (slices + 1) + s + 1]
      local b = grid[r * (slices + 1) + s + 2]
      local c = grid[(r + 1) * (slices + 1) + s + 1]
      local d = grid[(r + 1) * (slices + 1) + s + 2]
      corner(a) corner(c) corner(b)
      corner(b) corner(c) corner(d)
    end
  end
  return gfx.new_mesh(flat)
end

-- The part of the melon on one side of the plane  dot(n, p) = k  (side 1 or -1), as a new mesh.
local function clip_half(nx, ny, nz, k, side)
  local out = {}
  local function push(p) for i = 1, 8 do out[#out + 1] = p[i] end end
  local function between(a, b, t)
    local p = {}
    for i = 1, 8 do p[i] = a[i] + (b[i] - a[i]) * t end
    return p
  end
  local tri, dist = {}, {}
  for i = 1, #sphere, 3 do
    local inside = 0
    for j = 1, 3 do
      local p = sphere[i + j - 1]
      tri[j] = p
      dist[j] = side * (p[1] * nx + p[2] * ny + p[3] * nz - k)
      if dist[j] >= 0 then inside = inside + 1 end
    end
    if inside == 3 then
      push(tri[1]) push(tri[2]) push(tri[3])
    elseif inside > 0 then
      -- The triangle crosses the cut: keep the part on our side.
      local poly = {}
      for j = 1, 3 do
        local a, b, da, db = tri[j], tri[j % 3 + 1], dist[j], dist[j % 3 + 1]
        if da >= 0 then poly[#poly + 1] = a end
        if (da >= 0) ~= (db >= 0) then poly[#poly + 1] = between(a, b, da / (da - db)) end
      end
      for j = 2, #poly - 1 do push(poly[1]) push(poly[j]) push(poly[j + 1]) end
    end
  end
  return gfx.new_mesh(out)
end

-- The flat cut face: a disc in the plane, facing away from the half it closes.
local function cap_mesh(cx, cy, cz, t1, t2, radius, facing)
  local out, steps = {}, 40
  local function push(a, b, u, v)
    out[#out + 1] = cx + (t1[1] * a + t2[1] * b) * radius
    out[#out + 1] = cy + (t1[2] * a + t2[2] * b) * radius
    out[#out + 1] = cz + (t1[3] * a + t2[3] * b) * radius
    for _, x in ipairs({ facing[1], facing[2], facing[3], u, v }) do out[#out + 1] = x end
  end
  for s = 0, steps - 1 do
    local a0, a1 = math.pi * 2 * s / steps, math.pi * 2 * (s + 1) / steps
    push(0, 0, 0.5, 0.5)
    push(math.cos(a0), math.sin(a0), 0.5 + 0.5 * math.cos(a0), 0.5 + 0.5 * math.sin(a0))
    push(math.cos(a1), math.sin(a1), 0.5 + 0.5 * math.cos(a1), 0.5 + 0.5 * math.sin(a1))
  end
  return gfx.new_mesh(out)
end

-- What the knife reveals: colour comes from how far each point of the cut is from the melon's
-- middle, so a cut near the edge is mostly rind, and only the seeds the plane passes through show.
local function cap_texture(m, n, k, t1, t2, radius)
  local seeds = {}
  for _, s in ipairs(m.seeds) do
    local e = s[1] * n[1] + s[2] * n[2] + s[3] * n[3] - k
    if math.abs(e) < 0.03 then
      local a = s[1] * t1[1] + s[2] * t1[2] + s[3] * t1[3]
      local b = s[1] * t2[1] + s[2] * t2[2] + s[3] * t2[3]
      local len = math.sqrt(a * a + b * b) + 1e-6
      seeds[#seeds + 1] = { a, b, a / len, b / len, 0.022 * math.sqrt(1 - (e / 0.03) ^ 2) }
    end
  end
  return texture(64, 64, function(u, v)
    local a, b = (u * 2 - 1) * radius, (v * 2 - 1) * radius
    for _, s in ipairs(seeds) do                           -- teardrops, pointing outwards
      local da, db = a - s[1], b - s[2]
      local along, across = da * s[3] + db * s[4], -da * s[4] + db * s[3]
      if (along / (1.7 * s[5])) ^ 2 + (across / s[5]) ^ 2 < 1 then return 35, 22, 20 end
    end
    local d = math.sqrt(k * k + a * a + b * b) / 0.5
    if d > 0.94 then return 45, 100, 40 end                -- dark rind
    if d > 0.88 then return 150, 200, 110 end              -- light rind
    if d > 0.82 then return 235, 225, 190 end              -- the pale band
    if d > 0.77 then return 245, 140, 130 end
    local grain = math.random() * 22
    return 225 - grain, 38 + grain * 0.5, 52
  end)
end

-- ---------------------------------------------------------------- throwing and cutting

local function view_half_width()
  local w, h = gfx.size()
  return 13 * math.tan(math.rad(25)) * w / h
end

local function toss()
  local half = math.min(view_half_width() * 0.6, 5)
  local x = (math.random() * 2 - 1) * half
  local seeds = {}
  for i = 1, 70 do
    local dx, dy, dz = lib.normalize(math.random() * 2 - 1, math.random() * 2 - 1, math.random() * 2 - 1)
    local r = 0.17 + math.random() * 0.17
    seeds[i] = { dx * r, dy * r, dz * r }
  end
  local ax, ay, az = lib.normalize(math.random() * 2 - 1, math.random() * 2 - 1, math.random() * 2 - 1)
  table.insert(melons, {
    x = x, y = -7, z = math.random() * 2 - 1,
    vx = -x * 0.35 + (math.random() * 2 - 1), vy = 14.5 + math.random() * 2.5, vz = 0,
    q = { lib.quat_euler(math.random() * 360, math.random() * 360, 0) },
    axis = { ax, ay, az }, spin = 60 + math.random() * 140, seeds = seeds,
  })
end

local function camera()
  gfx.camera(0, 1, 13, 0, 1, 0, 50)
end

local function splash(x, y, z)
  for _ = 1, 26 do
    local dx, dy, dz = lib.normalize(math.random() * 2 - 1, math.random() * 2 - 1, math.random() * 2 - 1)
    local speed = 2 + math.random() * 4
    juice[#juice + 1] = { x = x, y = y, z = z, vx = dx * speed, vy = dy * speed + 2, vz = dz * speed, life = 0.6 + math.random() * 0.4 }
  end
end

-- Cut a melon along the screen line from (ax, ay) to (bx, by).
local function cut(index, ax, ay, bx, by)
  local m = melons[index]
  -- The knife is the plane through the eye and the swipe line: both screen points' rays lie in it.
  local ox, oy, oz, d1x, d1y, d1z = gfx.unproject(ax, ay)
  local _, _, _, d2x, d2y, d2z = gfx.unproject(bx, by)
  local wx, wy, wz = lib.cross(d1x, d1y, d1z, d2x, d2y, d2z)
  if lib.length(wx, wy, wz) < 1e-6 then return end
  wx, wy, wz = lib.normalize(wx, wy, wz)

  -- The same plane in the melon's own (turned, unit-size) space.
  local qx, qy, qz, qw = m.q[1], m.q[2], m.q[3], m.q[4]
  local nx, ny, nz = lib.quat_rotate(-qx, -qy, -qz, qw, wx, wy, wz)
  local k = -lib.dot(wx, wy, wz, m.x - ox, m.y - oy, m.z - oz) / SIZE
  k = lib.clamp(k, -0.42, 0.42)   -- a grazing touch still takes off a slice

  local n = { nx, ny, nz }
  local t1x, t1y, t1z
  if math.abs(nx) < 0.9 then t1x, t1y, t1z = lib.cross(nx, ny, nz, 1, 0, 0) else t1x, t1y, t1z = lib.cross(nx, ny, nz, 0, 1, 0) end
  local t1 = { lib.normalize(t1x, t1y, t1z) }
  local t2 = { lib.cross(nx, ny, nz, t1[1], t1[2], t1[3]) }
  local radius = math.sqrt(0.25 - k * k) * 0.995
  local face = cap_texture(m, n, k, t1, t2, radius)
  cap_users[face] = 2

  -- Two halves fly apart across the cut, tumbling about the line of the swipe.
  local sx, sy, sz = lib.normalize(d2x - d1x, d2y - d1y, d2z - d1z)
  local ex, ey, ez = lib.normalize(ox - m.x, oy - m.y, oz - m.z)          -- towards the eye
  for _, side in ipairs({ 1, -1 }) do
    -- Turn each half so its cut face swings round to face you.
    local fx, fy, fz = -wx * side, -wy * side, -wz * side
    local turn = lib.dot(ex, ey, ez, lib.cross(sx, sy, sz, fx, fy, fz)) >= 0 and 1 or -1
    table.insert(pieces, {
      dome = clip_half(nx, ny, nz, k, side),
      cap = cap_mesh(nx * k, ny * k, nz * k, t1, t2, radius, { -nx * side, -ny * side, -nz * side }),
      face = face,
      x = m.x, y = m.y, z = m.z,
      vx = m.vx + wx * side * 2.5, vy = m.vy * 0.4 + wy * side * 2.5, vz = m.vz + wz * side * 2.5,
      q = { qx, qy, qz, qw }, axis = { sx, sy, sz }, spin = 260 * turn,
    })
  end
  splash(m.x, m.y, m.z)
  table.remove(melons, index)
  score = score + 1
  input.haptic("light")
end

-- ---------------------------------------------------------------- the game

function init()
  sphere = {}
  sphere_mesh = build_sphere()
  rind = make_rind()
end

-- Moves and turns anything with x, y, z, velocity, rotation q and a spin about axis.
local function fly(thing, dt)
  thing.vy = thing.vy - GRAVITY * dt
  thing.x, thing.y, thing.z = thing.x + thing.vx * dt, thing.y + thing.vy * dt, thing.z + thing.vz * dt
  local q = thing.q
  local rx, ry, rz, rw = lib.quat_axis(thing.axis[1], thing.axis[2], thing.axis[3], thing.spin * dt)
  local x, y, z, w = lib.quat_mul(rx, ry, rz, rw, q[1], q[2], q[3], q[4])
  local len = math.sqrt(x * x + y * y + z * z + w * w)
  q[1], q[2], q[3], q[4] = x / len, y / len, z / len, w / len
end

function update(dt)
  if dt <= 0 then return end
  camera()   -- so gfx.project and gfx.unproject below use this camera
  if math.random() < dt * 1.2 then toss() end

  for i = #melons, 1, -1 do
    fly(melons[i], dt)
    if melons[i].y < -9 then table.remove(melons, i) end
  end
  for i = #pieces, 1, -1 do
    local p = pieces[i]
    fly(p, dt)
    if p.y < -9 then
      gfx.free_mesh(p.dome)
      gfx.free_mesh(p.cap)
      cap_users[p.face] = cap_users[p.face] - 1
      if cap_users[p.face] == 0 then gfx.free_texture(p.face) cap_users[p.face] = nil end
      table.remove(pieces, i)
    end
  end
  for i = #juice, 1, -1 do
    local j = juice[i]
    j.vy = j.vy - GRAVITY * dt
    j.x, j.y, j.z = j.x + j.vx * dt, j.y + j.vy * dt, j.z + j.vz * dt
    j.life = j.life - dt
    if j.life <= 0 then table.remove(juice, i) end
  end

  -- Fingers: each frame, a finger's movement is a short line from where it was to where it is.
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then trails[t.id] = {} end
    local trail = trails[t.id]
    if trail then
      trail[#trail + 1] = { x = t.x, y = t.y, age = 0 }
      if math.sqrt(t.dx * t.dx + t.dy * t.dy) / dt > SLASH_SPEED then
        local ax, ay = t.x - t.dx, t.y - t.dy
        for i = #melons, 1, -1 do
          local m = melons[i]
          local cx, cy = gfx.project(m.x, m.y, m.z)
          local ex = gfx.project(m.x + SIZE / 2, m.y, m.z)
          if cx and ex and lib.segment_hits(ax, ay, t.x, t.y, cx, cy, math.abs(ex - cx)) then
            cut(i, ax, ay, t.x, t.y)
          end
        end
      end
      if t.phase == "up" then trails[t.id] = nil end
    end
  end

  for _, trail in pairs(trails) do
    for i = #trail, 1, -1 do
      trail[i].age = trail[i].age + dt
      if trail[i].age > 0.2 then table.remove(trail, i) end
    end
  end
end

function draw()
  gfx.clear(0.32, 0.20, 0.12)
  camera()
  gfx.light(-0.4, -1, -0.6, 1.0, 0.95, 0.85)
  gfx.ambient(0.38, 0.36, 0.40)

  gfx.cull(false)
  gfx.texture(1, rind)
  for _, m in ipairs(melons) do
    gfx.draw(sphere_mesh, m.x, m.y, m.z, m.q[1], m.q[2], m.q[3], m.q[4], SIZE, SIZE, SIZE, 1, 1, 1, 1)
  end
  for _, p in ipairs(pieces) do
    gfx.texture(1, rind)
    gfx.draw(p.dome, p.x, p.y, p.z, p.q[1], p.q[2], p.q[3], p.q[4], SIZE, SIZE, SIZE, 1, 1, 1, 1)
    gfx.texture(1, p.face)
    gfx.draw(p.cap, p.x, p.y, p.z, p.q[1], p.q[2], p.q[3], p.q[4], SIZE, SIZE, SIZE, 1, 1, 1, 1)
  end
  gfx.texture(1, nil)
  gfx.cull(true)

  if #juice > 0 then
    local drops = {}
    for _, j in ipairs(juice) do
      local a = math.min(1, j.life * 2)
      for _, v in ipairs({ j.x, j.y, j.z, 0.13, 0.85, 0.08, 0.15, a }) do drops[#drops + 1] = v end
    end
    gfx.blend("alpha")
    gfx.particles(drops)
    gfx.blend("opaque")
  end

  for _, trail in pairs(trails) do
    for _, p in ipairs(trail) do
      gfx.circle(p.x, p.y, 14 * (1 - p.age / 0.2), 1, 1, 1, 0.8)
    end
  end
  local left, top = gfx.safe()
  gfx.text("SLICED " .. score, left + 80, top + 24, 32, 1, 1, 1)
end

-- Does the line from a to b pass within r of point c?
function lib.segment_hits(ax, ay, bx, by, cx, cy, r)
  local dx, dy = bx - ax, by - ay
  local len2 = dx * dx + dy * dy
  local t = len2 > 0 and lib.clamp(((cx - ax) * dx + (cy - ay) * dy) / len2, 0, 1) or 0
  local px, py = ax + dx * t - cx, ay + dy * t - cy
  return px * px + py * py <= r * r
end
