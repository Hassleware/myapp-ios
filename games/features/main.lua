-- A tour of what the engine can do, in one small scene. Each numbered part below is one feature
-- to copy into a real game. The level and the character come from .glb files; see tools/make_demo_assets.py.

local lib = include("lib.lua")
lib.strict()

input.controls {
  { type = "stick",  id = "move", x = 0.13, y = 0.74, size = 0.14 },
  { type = "button", id = "jump", x = 0.88, y = 0.76, size = 0.10, label = "A", key = "space" },
  { type = "button", id = "look", x = 0.88, y = 0.48, size = 0.07, label = "?", key = "tab" },
}

local WORLD, PLAYER = 1, 2          -- collision layers
local assets = {}
local level, coins, zone, lamp = {}, {}, nil, nil
local player = { x = 0, y = 1, z = 4, vy = 0, yaw = 0, grounded = false, speed = 5 }
local cam = { yaw = 25, pitch = 22, distance = 7, x = 0, y = 3, z = 10 }
local hero, sparks, coinCue
local score, message, messageTime, showDebug, idle = 0, "", 0, false, 0
local mirror, pyramid
score, message = 0, ""

lib.words({ en = { title = "FEATURES", coins = "COINS", hint = "DRAG TO LOOK   ?: COLLISION SHAPES" },
            fr = { title = "FONCTIONS", coins = "PIECES", hint = "GLISSER POUR REGARDER" } }, "en")

function init()
  -- 1. Textures, a font, sounds. A missing file gives a pink stand-in and one line in the dev.py window.
  assets.checker = gfx.load_texture("checker.png", "nearest")
  assets.spark = gfx.load_texture("spark.png", "clamp")
  assets.sky = gfx.load_texture("sky.jpg", "clamp")
  assets.font = gfx.load_font("DejaVuSans.ttf", 48)
  assets.jump = audio.load("jump.wav")
  assets.music = audio.load("music.ogg", true)        -- true: streamed from the file, for long music
  coinCue = lib.cue({ "coin.wav" }, { volume = -4, pitch_spread = 0.12 })
  audio.play(assets.music, -14, 1, true, lib.MUSIC)

  -- 2. A level made in Blender. Its meshes become the scenery and the collision; its empties become game objects.
  assets.level = model.load("level.glb")
  phys.model(assets.level, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, WORLD)
  for _, node in ipairs(model.nodes(assets.level)) do
    local kind = node.extras.type
    if kind == "spawn" then
      player.x, player.y, player.z, player.speed = node.x, node.y, node.z, node.extras.speed
    elseif kind == "coin" then
      coins[#coins + 1] = { x = node.x, y = node.y, z = node.z, value = node.extras.value }
    elseif kind == "zone" then
      zone = node
    elseif node.light then
      lamp = node
    end
  end

  -- 3. A character: a capsule for collision, a skinned model for looks.
  player.body = phys.capsule(player.x, player.y, player.z, 0.35, 1.6, PLAYER)
  assets.hero = model.load("hero.glb")
  hero = lib.animation(assets.hero)
  hero:play("wave")
  hero:event("hop", 0.5, function() sparks:burst(6, player.x, player.y - 0.8, player.z) end)

  -- 4. Particles, a mesh built in Lua, and a texture to draw into.
  sparks = lib.emitter({ texture = assets.spark, color = { 1, 0.85, 0.3 }, size = 0.35, life = 0.8, blend = "add" })
  pyramid = gfx.new_mesh({
     0, 1, 0,  0.5, 0,     -1, 0,  1,  0, 1,      1, 0,  1,  1, 1,
     0, 1, 0,  0.5, 0,      1, 0,  1,  0, 1,      1, 0, -1,  1, 1,
     0, 1, 0,  0.5, 0,      1, 0, -1,  0, 1,     -1, 0, -1,  1, 1,
     0, 1, 0,  0.5, 0,     -1, 0, -1,  0, 1,     -1, 0,  1,  1, 1,
  }, nil, "pu")                                        -- positions and texture coordinates; normals are worked out
  mirror = gfx.new_target(256, 256)
end

-- Fixed 60-a-second steps keep movement and collision the same at any frame rate.
function fixed_update(dt)
  local mx, my = input.stick("move")
  local moving = math.abs(mx) + math.abs(my) > 0.1
  idle = moving and 0 or idle + dt
  if idle > 4 and sys.time() < 60 then mx, my, moving = 0.25, -1, true end   -- left alone: wander off, as a demo

  -- 5. Move relative to the camera, and let the engine slide the capsule along walls, up steps and slopes.
  local yaw = math.rad(cam.yaw)
  local dx = (mx * math.cos(yaw) + my * math.sin(yaw)) * player.speed
  local dz = (my * math.cos(yaw) - mx * math.sin(yaw)) * player.speed
  if moving then player.yaw = math.deg(math.atan(dx, dz)) end
  if input.pressed("jump") and player.grounded then
    player.vy = 8
    audio.play(assets.jump, -6, 1, false, lib.SFX)
    input.haptic("light")
    hero:play("hop", { loop = false, fade = 0.05, restart = true })
  end
  player.vy = player.vy - 22 * dt
  local x, y, z, grounded = phys.move(player.body, dx * dt, player.vy * dt, dz * dt, WORLD, 0.35, 50)
  player.x, player.y, player.z, player.grounded = x, y, z, grounded
  if grounded and player.vy < 0 then player.vy = 0 end
  if y < -15 then phys.place(player.body, 0, 2, 4) player.vy = 0 end
  if hero:finished() or (grounded and player.vy == 0 and hero.layers[#hero.layers].clip ~= "hop") then hero:play("wave", { speed = moving and 2.2 or 1 }) end
end

function update(dt)
  lib.update(dt)
  hero:update(dt)
  sparks:update(dt)
  messageTime = messageTime - dt
  if input.pressed("look") then showDebug = not showDebug end

  -- 6. Raw touches: a finger that no stick or button took turns the camera.
  for _, t in ipairs(input.touches()) do
    if not t.claimed then
      cam.yaw = cam.yaw - t.dx * 0.25
      cam.pitch = lib.clamp(cam.pitch + t.dy * 0.2, 5, 70)
    end
  end

  -- 7. Asking the collision world a question: which coins and zones does the player touch?
  for i = #coins, 1, -1 do
    local c = coins[i]
    if lib.length(c.x - player.x, c.y - player.y, c.z - player.z) < 1.0 then
      table.remove(coins, i)
      score = score + c.value
      coinCue:play(c.x, c.y, c.z)
      sparks:burst(24, c.x, c.y, c.z)
      input.haptic("success")
    end
  end
  if zone and math.abs(player.x - zone.x) < zone.sx / 2 and math.abs(player.z - zone.z) < zone.sz / 2 then
    message, messageTime = zone.extras.message, 0.2
  end
end

-- The camera moves last, after everything it follows has moved.
function late_update(dt)
  local yaw, pitch = math.rad(cam.yaw), math.rad(cam.pitch)
  local dx, dy, dz = math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch)
  local tx, ty, tz = player.x, player.y + 0.6, player.z
  -- 8. A ball swept from the player towards the camera stops it going through walls.
  local hit = phys.spherecast(tx, ty, tz, dx, dy, dz, cam.distance, 0.3, WORLD)
  local d = hit and math.max(hit - 0.1, 0.8) or cam.distance
  cam.x = lib.approach(cam.x, tx + dx * d, 12, dt)
  cam.y = lib.approach(cam.y, ty + dy * d, 12, dt)
  cam.z = lib.approach(cam.z, tz + dz * d, 12, dt)
end

local function scene()
  gfx.sky(assets.sky)
  model.draw(assets.level)

  -- The character, turned to face the way it walks.
  local qx, qy, qz, qw = lib.quat_euler(player.yaw, 0, 0)
  local feet = player.y - 0.8
  model.draw(assets.hero, player.x, feet, player.z, qx, qy, qz, qw, 0.8, 0.8, 0.8, 1, 1, 1, 1, hero.animator)
  -- 9. Something attached to a joint: a ball riding on the character's "Hand".
  local jx, jy, jz = model.joint(assets.hero, hero.animator, "Hand")
  local hx, hy, hz = lib.attach(player.x, feet, player.z, qx, qy, qz, qw, 0.8, jx, jy, jz)
  gfx.sphere(hx, hy, hz, 0.25, 0.3, 0.8, 1.0)

  for _, c in ipairs(coins) do
    local cx, cy, cz, cw = lib.quat_euler(sys.time() * 180, 0, 90)
    gfx.draw(gfx.SPHERE, c.x, c.y + math.sin(sys.time() * 3 + c.x) * 0.1, c.z, cx, cy, cz, cw, 0.6, 0.12, 0.6, 1.0, 0.82, 0.25, 1)
  end

  gfx.texture(1, assets.checker)
  gfx.draw(pyramid, -2, 0, -4, 0, 0, 0, 1, 1, 1.5, 1, 0.9, 0.4, 0.4, 1)
  gfx.texture(1, nil)
  gfx.cube(3, 1.0, 6.5, 2, 2, 0.2, 0.4, 0.8, 1.0, 0, 0.4)      -- see-through things are sorted and drawn last
  gfx.cube(3.6, 1.0, 7.5, 2, 2, 0.2, 1.0, 0.5, 0.6, 20, 0.4)
  sparks:draw()
end

function draw()
  gfx.clear(0.45, 0.62, 0.85)
  gfx.ambient(0.30, 0.33, 0.40)
  gfx.light(-0.5, -1, -0.35, 0.95, 0.9, 0.8)
  if lamp then gfx.point_light(lamp.x, lamp.y, lamp.z, lamp.range, lamp.r * 1.5, lamp.g * 1.5, lamp.b * 1.5) end
  gfx.fog(0.55, 0.68, 0.85, 18, 60)
  gfx.shadows(player.x, player.y, player.z, 14, 0.65)

  -- 10. The same scene drawn into a texture from above, then shown as a small map.
  gfx.target(mirror)
  gfx.clear(0.1, 0.12, 0.16)
  gfx.camera(player.x, player.y + 16, player.z + 0.01, player.x, player.y, player.z, -18)
  scene()
  gfx.target()

  gfx.camera(cam.x, cam.y, cam.z, player.x, player.y + 0.6, player.z, 55)
  scene()
  if showDebug then phys.debug() end

  -- 2D on top, kept clear of the notch and home bar.
  local left, top, right = gfx.safe()
  local w = gfx.size()
  gfx.font(assets.font)
  gfx.text(lib.tr("title"), left + 80, top + 18, 34, 1, 1, 1)
  gfx.text(lib.tr("coins") .. " " .. score, left + 80, top + 60, 22, 1, 0.9, 0.4)
  if messageTime > 0 then
    gfx.text(message, (w - gfx.text_width(message, 22)) / 2, top + 110, 22, 1, 1, 1)
  end
  gfx.font()
  gfx.text(lib.tr("hint"), left + 80, top + 92, 12, 1, 1, 1, 0.7)
  gfx.rect(w - right - 176, top + 14, 164, 164, 0, 0, 0, 0.5)
  gfx.image(mirror, w - right - 170, top + 20, 152, 152)
  for _, c in ipairs(coins) do lib.label("+" .. c.value, c.x, c.y + 0.7, c.z, 14, 1, 0.9, 0.4) end
end

function on_pause() print("features: paused") end
function on_resume() print("features: back") end
