-- Dream Table's world, on a PS1's budget (budget.lua). The heavy work is done once, offline, in Blender by
-- tools/blender_dreamtable_world.py; this file only loads and places what it made:
--   backdrop_l.png, backdrop_r.png  everything far away (sky, volumetric cumulus, cloud sea, beanstalks) rendered
--                                   from the camera's resting place, wrapped on a low-poly dome slice
--   island.glb                      the cloud island and vines, lighting baked into vertex colours, drawn unlit
--   pebbles.png                     the cobble patch's repeating stones, lighting baked in
--   cloud_sprite.png                a few volumetric clouds drifting past as flat cards
-- The patch's outline must match PATCH_X, PATCH_Z and wobble() in the Blender script.

local S = {}

S.FOG = { 0.55, 0.76, 1.0 }
local PATCH_X, PATCH_Z = 3.1, 5.4
local COBBLE_TILE = 1.6                          -- metres per copy of pebbles.png
local YAW, PITCH = { -55, 55 }, { -50, 35 }       -- what the backdrop pictures cover, in degrees (Blender script)
local DOME = 500                                 -- the backdrop's distance; it moves with the camera

local function wobble(a) return 1 + 0.10 * math.sin(3 * a + 1.3) + 0.07 * math.sin(5 * a + 0.4) + 0.04 * math.sin(9 * a + 2.0) end
local function patch_reach(x, z)
  return math.sqrt((x / PATCH_X) ^ 2 + (z / PATCH_Z) ^ 2) / wobble(math.atan(z / PATCH_Z, x / PATCH_X))
end

-- Part of a sphere round the camera, from yaw0 to yaw1 and pitch1 down to pitch0 (degrees), its picture spread
-- linearly over it (as Blender's equirectangular camera made it).
local function dome_mesh(yaw0, yaw1, pitch0, pitch1, cols, rows)
  local n, idx = {}, {}
  for j = 0, rows do
    for i = 0, cols do
      local yaw, pitch = math.rad(yaw0 + (yaw1 - yaw0) * i / cols), math.rad(pitch1 - (pitch1 - pitch0) * j / rows)
      local x, y, z = math.sin(yaw) * math.cos(pitch), math.sin(pitch), -math.cos(yaw) * math.cos(pitch)
      for _, v in ipairs({ x * DOME, y * DOME, z * DOME, -x, -y, -z, i / cols, j / rows }) do n[#n + 1] = v end
    end
  end
  for j = 0, rows - 1 do
    for i = 0, cols - 1 do
      local a = j * (cols + 1) + i + 1
      local b = a + cols + 1
      for _, v in ipairs({ a, a + 1, b, a + 1, b + 1, b }) do idx[#idx + 1] = v end
    end
  end
  return gfx.new_mesh(n, idx, "pnu")
end

-- The cobble patch: a grid whose vertex alpha is 1 in the middle and fades past the wobbly edge. pebbles.png
-- holds each stone's number in alpha, so with gfx.cutoff whole stones drop out toward the edge.
local function patch_mesh()
  local GX, GZ, X, Z = 10, 14, PATCH_X * 1.2, PATCH_Z * 1.2
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

-- A flat card facing the camera (+z), 2 wide and 1 tall, for the cloud sprites.
local function card_mesh()
  return gfx.new_mesh({ -1, -0.5, 0, 0, 0, 1, 0, 1,  1, -0.5, 0, 0, 0, 1, 1, 1,  1, 0.5, 0, 0, 0, 1, 1, 0,  -1, 0.5, 0, 0, 0, 1, 0, 0 },
                      { 1, 2, 3, 1, 3, 4 }, "pnu")
end

local made = false
local back_l, back_r, dome_l, dome_r, island, pebbles, patch, cloud, card
-- Drifting cloud cards: x, y, z, size, sway speed.
local CLOUDS = { { -34, 5, -45, 16, 0.25 }, { 26, 9, -55, 20, 0.18 }, { -8, 14, -80, 26, 0.12 }, { 45, 2, -38, 12, 0.3 } }

local function make()
  made = true
  back_l, back_r = gfx.load_texture("backdrop_l.png", "clamp"), gfx.load_texture("backdrop_r.png", "clamp")
  dome_l = dome_mesh(YAW[1], 0, PITCH[1], PITCH[2], 4, 6)
  dome_r = dome_mesh(0, YAW[2], PITCH[1], PITCH[2], 4, 6)
  island = model.load("island.glb")
  pebbles = gfx.load_texture("pebbles.png")
  patch = patch_mesh()
  cloud = gfx.load_texture("cloud_sprite.png", "clamp")
  card = card_mesh()
end

-- The light and haze; call before drawing anything else.
function S.lights()
  if not made then make() end
  gfx.fog(S.FOG[1], S.FOG[2], S.FOG[3], 60, 260)
  gfx.light(-0.75, -0.6, -0.15, 1.15, 1.05, 0.92)
  gfx.ambient(0.50, 0.58, 0.76)
end

-- ex, ey, ez: where the camera is (the backdrop stays round it).
function S.draw(time, ex, ey, ez)
  gfx.cast_shadows(false)
  gfx.unlit(true)
  gfx.depth(false, false)
  gfx.cull(false)
  gfx.shader("backdrop")
  gfx.texture(1, back_l)
  gfx.draw(dome_l, ex, ey, ez, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.texture(1, back_r)
  gfx.draw(dome_r, ex, ey, ez, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.shader()
  gfx.cull(true)
  gfx.depth(true, true)

  model.draw(island, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.unlit(false)

  gfx.texture(1, pebbles)
  gfx.cutoff(0.5)
  gfx.draw(patch, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1)
  gfx.cutoff(0)

  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.depth(true, false)
  gfx.texture(1, cloud)
  for _, c in ipairs(CLOUDS) do
    gfx.draw(card, c[1] + math.sin(time * c[5] * 0.1) * 6, c[2], c[3], 0, 0, 0, 1, c[4], c[4], 1, 1, 1, 1, 1)
  end
  gfx.texture(1, nil)
  gfx.depth(true, true)
  gfx.blend("opaque")
  gfx.unlit(false)
  gfx.cast_shadows(true)
end

return S
