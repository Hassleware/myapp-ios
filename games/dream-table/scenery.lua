-- Dream Table's world, on a PS1's budget (budget.lua). The heavy work is done once, offline, in Blender by
-- tools/blender_dreamtable_world.py; this file only loads and places what it made:
--   backdrop_l.png, backdrop_r.png  everything far away (sky, volumetric cumulus, cloud sea, beanstalks) rendered
--                                   from the camera's resting place, wrapped on a low-poly dome slice
--   island.glb                      the cloud island, lighting baked into vertex colours, drawn unlit
--   cloud_sprite.png                a few volumetric clouds drifting past as flat cards

local S = {}

S.FOG = { 0.55, 0.76, 1.0 }
local YAW, PITCH = { -55, 55 }, { -50, 35 }       -- what the backdrop pictures cover, in degrees (Blender script)
local DOME = 500                                 -- the backdrop's distance; it moves with the camera

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

-- A flat card facing the camera (+z), 2 wide and 1 tall, for the cloud sprites.
local function card_mesh()
  return gfx.new_mesh({ -1, -0.5, 0, 0, 0, 1, 0, 1,  1, -0.5, 0, 0, 0, 1, 1, 1,  1, 0.5, 0, 0, 0, 1, 1, 0,  -1, 0.5, 0, 0, 0, 1, 0, 0 },
                      { 1, 2, 3, 1, 3, 4 }, "pnu")
end

local made = false
local back_l, back_r, dome_l, dome_r, island, cloud, card
-- Drifting cloud cards: x, y, z, size, sway speed.
local CLOUDS = { { -34, 5, -45, 16, 0.25 }, { 26, 9, -55, 20, 0.18 }, { -8, 14, -80, 26, 0.12 }, { 45, 2, -38, 12, 0.3 } }

local function make()
  made = true
  back_l, back_r = gfx.load_texture("backdrop_l.png", "clamp"), gfx.load_texture("backdrop_r.png", "clamp")
  dome_l = dome_mesh(YAW[1], 0, PITCH[1], PITCH[2], 4, 6)
  dome_r = dome_mesh(0, YAW[2], PITCH[1], PITCH[2], 4, 6)
  island = model.load("island.glb")
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
