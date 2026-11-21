-- Drawing: the 3D world (draw_world) and the 2D layer (draw_ui), the camera, the ball's trail, messages.
-- Made by main.lua: include("view.lua")(game) returns the functions below; the shared game table carries
-- everything it uses from elsewhere (listed at the top) and the state the files share (game.state, game.score...).
return function(game)
-- from main.lua
local BALL_R, FEEL, HALF_L, HALF_W, JET = game.BALL_R, game.FEEL, game.HALF_L, game.HALF_W, game.JET
local NET_TOP, TABLE_Y, ball, bitfont = game.NET_TOP, game.TABLE_Y, game.ball, game.bitfont
local fading_lines, jump_recovering, lib, origin = game.fading_lines, game.jump_recovering, game.lib, game.origin
local robot, scenery, sparks, swing_lines = game.robot, game.scenery, game.sparks, game.swing_lines
local to_world, you = game.to_world, game.you
-- from vec.lua
local qmul, rot, v = game.qmul, game.rot, game.v
-- from skeleton.lua
local bat_model, hands, joint, mannequin_model = game.bat_model, game.hands, game.joint, game.mannequin_model
-- from serve_meter.lua
local draw_meter, draw_rating = game.draw_meter, game.draw_rating

local trail = {}                      -- where the ball was, newest first, one every TRAIL_STEP seconds
local TRAIL_STEP, TRAIL_WIDTH = 1 / 30, 0.05
local trail_clock = 0

-- The trail's ribbon: a fixed strip that shaders/trail.slang bends along the ball's last positions each frame.
local trail_mesh
do
  local numbers, indices, SEGMENTS = {}, {}, 40
  for i = 0, SEGMENTS do
    for _, side in ipairs({ -1, 1 }) do
      for _, n in ipairs({ 0, 0, 0, 0, 1, 0, i / SEGMENTS, side }) do numbers[#numbers + 1] = n end   -- p, n, uv
    end
    if i < SEGMENTS then
      local a = i * 2 + 1
      for _, n in ipairs({ a, a + 1, a + 2, a + 2, a + 1, a + 3 }) do indices[#indices + 1] = n end
    end
  end
  trail_mesh = gfx.new_mesh(numbers, indices, "pnu")
end

-- The ball, one unit across (60 triangles: at 4 cm nobody sees more).
local ball_mesh
do
  local n, idx, RINGS, SEGS = {}, {}, 5, 6
  for r = 0, RINGS do
    for sgm = 0, SEGS do
      local phi, th = math.pi * r / RINGS, math.pi * 2 * sgm / SEGS
      local x, y, z = math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th)
      for _, v in ipairs({ x * 0.5, y * 0.5, z * 0.5, x, y, z, 0, 0 }) do n[#n + 1] = v end
    end
  end
  for r = 0, RINGS - 1 do
    for sgm = 0, SEGS - 1 do
      local a = r * (SEGS + 1) + sgm + 1
      local b = a + SEGS + 1
      for _, v in ipairs({ a, a + 1, b, a + 1, b + 1, b }) do idx[#idx + 1] = v end
    end
  end
  ball_mesh = gfx.new_mesh(n, idx, "pnu")
end

-- Soft blob shadows: a flat square (two triangles) with a round, soft-edged picture, 32 x 32.
local blob_mesh = gfx.new_mesh({ -0.5, 0, -0.5, 0, 1, 0, 0, 0,  0.5, 0, -0.5, 0, 1, 0, 1, 0,  0.5, 0, 0.5, 0, 1, 0, 1, 1,  -0.5, 0, 0.5, 0, 1, 0, 0, 1 },
                               { 1, 3, 2, 1, 4, 3 }, "pnu")
local blob_tex
do
  local px = {}
  for y = 0, 31 do
    for x = 0, 31 do
      local d = math.sqrt(((x + 0.5) / 16 - 1) ^ 2 + ((y + 0.5) / 16 - 1) ^ 2)
      local a = math.max(0, math.min(1, (1 - d) / 0.55))
      px[#px + 1] = string.char(255, 255, 255, math.floor(a * a * (3 - 2 * a) * 255))
    end
  end
  blob_tex = gfx.new_texture(32, 32, table.concat(px), "clamp")
end

-- The jet: a flame out of the bottom of the torso, flickering. For JET.flare_time after a dash or a jump starts it is
-- bigger and blue, fading back to orange; while the torso flutters down it sputters. Soft blended particles (not
-- additive, which vanishes against white cloud), two
-- triangles each, added to `out` (gfx.particles' eight numbers each).
local function add_flame_particles(m, out, time)
  local b = m.body
  local base = to_world(m, (joint("hips", m.anim.animator)))
  local down = rot(qmul(b.turn, b.hip_tilt or { 0, 0, 0, 1 }), v(0, -1, 0))   -- out of the hips, as they're turned
  local k = b.jet.flare / JET.flare_time
  k = k * k
  local length = JET.length * (1 + 1.4 * k) * (0.85 + 0.15 * math.sin(time * 41 + b.x * 7))
  if b.flutter then length = length * (0.3 + 0.7 * math.abs(math.sin(time * 23))) end
  local bx, by, bz, dx, dy, dz = base[1], base[2], base[3], down[1], down[2], down[3]
  for i = 0, JET.bits - 1 do
    local t = i / (JET.bits - 1)
    local wob = 0.012 * math.sin(time * 53 + i * 2.3)
    local along = 0.06 + t * length
    local c1, c2 = t < 0.5 and JET.colors[1] or JET.colors[2], t < 0.5 and JET.colors[2] or JET.colors[3]
    local f1, f2 = t < 0.5 and JET.flare_colors[1] or JET.flare_colors[2], t < 0.5 and JET.flare_colors[2] or JET.flare_colors[3]
    local u = (t < 0.5 and t or t - 0.5) * 2
    local n = #out
    out[n + 1], out[n + 2], out[n + 3] = bx + dx * along + wob, by + dy * along, bz + dz * along - wob
    out[n + 4] = JET.size * (1 - 0.65 * t) * (1 + 0.9 * k)
    for ch = 1, 3 do                                  -- orange to blue by how much of the flare is left
      local fire = c1[ch] + (c2[ch] - c1[ch]) * u
      out[n + 4 + ch] = fire + (f1[ch] + (f2[ch] - f1[ch]) * u - fire) * k
    end
    out[n + 8] = 0.95 - 0.75 * t
  end
end

local function draw_mannequin(m)
  local q = m.body.turn
  local o = origin(m)
  model.draw(mannequin_model, o[1], o[2], o[3], q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1, m.anim.animator)
  for _, hand in pairs(hands) do
    local wrist, turn = joint(hand.bone, m.anim.animator)
    local at, t = to_world(m, wrist), qmul(qmul(q, turn), hand.offset)
    model.draw(bat_model, at[1], at[2], at[3], t[1], t[2], t[3], t[4], 1, 1, 1, 1, 1, 1, 1, nil, hand.node)
  end
end
local message, message_time = "", 0
local function show_message(text) message, message_time = text, sys.real_time() end

-- Every word on screen starts with a capital and goes on in lower case ("Your Serve"), except the key names WASD,
-- which stay in capitals. The title is drawn as it is, in full capitals.
local CAPITALS = { WASD = true }
local function title_case(s)
  return (s:gsub("%a[%a']*", function(w)
    if CAPITALS[w:upper()] then return w:upper() end
    return w:sub(1, 1):upper() .. w:sub(2):lower()
  end))
end

-- The camera: behind and above you (portrait, game.cfg), following your mannequin by less than it moves.
-- Walk left or right and it pans that way by CAM.side of the distance; walk back from the table and it pulls back
-- and up (zooms out), walk in and it comes in and down (zooms in). It glides after you rather than jumping.
local CAM = {
  side = 0.85, back = 0.8, up = 0.6, -- follows this much of your sideways move; walking back pulls it back and up
  height = 3.4, look = 1.0,          -- eye height and the height it looks at: low enough to see the sky
  x = 0, pull = 0,                   -- where it is now: across, and pulled back
}
local function follow_camera(dt)
  local home = HALF_L + 0.6                          -- where you start: the camera's resting place
  CAM.x = lib.approach(CAM.x, you.body.x * CAM.side, 4, dt)
  CAM.pull = lib.approach(CAM.pull, you.body.z - home, 4, dt)
end
local function camera()
  CAM.ex, CAM.ey, CAM.ez = CAM.x, CAM.height + CAM.pull * CAM.up, 6.0 + CAM.pull * CAM.back
  if FEEL.kick > 0.001 then                           -- a hit's camera kick, dying away
    CAM.ex, CAM.ey = CAM.ex + (math.random() - 0.5) * FEEL.kick, CAM.ey + (math.random() - 0.5) * FEEL.kick
  end
  gfx.camera(CAM.ex, CAM.ey, CAM.ez, CAM.x * 0.7, CAM.look, -0.2 + CAM.pull * 0.2, 50)
end

-- ---------------------------------------------------------------- drawing

-- Text in the bitmap font (bitfont.lua: baked with its outline and shadow).
local text, text_width = bitfont.draw, bitfont.width
local last_draw                       -- when the last frame was drawn (real time)

-- The 3D world, in order: sky and island, shadows and the floor spot, table and net, mannequins, marks, jets,
-- ball and its shadow and bursts, the trail.
local function draw_world()
  gfx.clear(scenery.FOG[1], scenery.FOG[2], scenery.FOG[3])
  camera()
  scenery.lights()
  -- The world (sky dome, cloud island, cloud cards: scenery.lua), then the table and its lines.
  scenery.draw(sys.real_time(), CAM.ex, CAM.ey, CAM.ez)

  -- Blob shadows, as a PS1 game would have them: soft dark shapes on the ground under the table and each body. A real shadow map would draw everything twice (budget.lua).
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.depth(true, false)
  gfx.cull(false)
  gfx.texture(1, blob_tex)
  gfx.draw(blob_mesh, -0.2, 0.004, 0, 0, 0, 0, 1, HALF_W * 2 + 0.5, 1, HALF_L * 2 + 0.5, 0.1, 0.14, 0.35, 0.75)
  for _, m in ipairs({ robot, you }) do
    gfx.draw(blob_mesh, m.body.x, 0.006, m.body.z, 0, 0, 0, 1, 0.9, 1, 0.7, 0.1, 0.14, 0.35, 0.45 / (1 + m.body.jump_height * 2))
  end
  -- Where to stand: a glowing spot on the floor (pink once you're on it).
  if game.hint and game.state == "rally" and game.last_hitter == "robot" then
    local now = sys.time()
    local on = math.sqrt((you.body.x - game.hint.x) ^ 2 + (you.body.z - game.hint.z) ^ 2) < 0.15
    local r, g, bl = 1, on and 0.45 or 0.85, on and 0.8 or 0.25
    local pulse = 0.85 + 0.15 * math.sin(now * 12)
    gfx.draw(blob_mesh, game.hint.x, 0.012, game.hint.z, 0, 0, 0, 1, 0.6 * pulse, 1, 0.6 * pulse, r, g, bl, 0.85)
  end
  gfx.texture(1, nil)
  gfx.cull(true)
  gfx.depth(true, true)
  gfx.blend("opaque")
  gfx.unlit(false)
  gfx.cube(0, TABLE_Y - 0.015, 0, HALF_W * 2, 0.03, HALF_L * 2, 0.16, 0.22, 0.62)
  local ly = TABLE_Y + 0.001
  for _, sx in ipairs({ -1, 1 }) do
    gfx.cube(sx * (HALF_W - 0.01), ly, 0, 0.02, 0.002, HALF_L * 2, 0.95, 0.95, 1)
    gfx.cube(0, ly, sx * (HALF_L - 0.01), HALF_W * 2, 0.002, 0.02, 0.95, 0.95, 1)
    for _, sz in ipairs({ -1, 1 }) do gfx.cube(sx * 0.62, (TABLE_Y - 0.04) / 2, sz * 1.15, 0.05, TABLE_Y - 0.04, 0.05, 0.08, 0.07, 0.12) end
  end
  gfx.cube(0, ly, 0, 0.006, 0.002, HALF_L * 2, 0.95, 0.95, 1)

  -- Net and posts.
  for _, sx in ipairs({ -1, 1 }) do gfx.cube(sx * (HALF_W + 0.13), TABLE_Y + 0.08, 0, 0.02, 0.16, 0.02, 0.85, 0.85, 0.9) end
  gfx.cube(0, NET_TOP - 0.008, 0, (HALF_W + 0.15) * 2, 0.016, 0.008, 1, 1, 1)
  gfx.blend("alpha")
  gfx.cube(0, TABLE_Y + 0.07, 0, (HALF_W + 0.15) * 2, 0.14, 0.004, 0.75, 0.8, 1, 0, 0.35)

  -- The two mannequins, a taped bat in each hand.
  gfx.blend("opaque")
  draw_mannequin(robot)
  draw_mannequin(you)
  -- Where the robot's ball will bounce on your side (a ring on the table), and your bat glowing while a swing is
  -- loaded (from the moment you draw it until it meets the ball).
  local marks = {}
  if FEEL.landing and game.state == "rally" and game.last_hitter == "robot" then
    for k = 0, 11 do
      local a = k / 12 * math.pi * 2
      for _, v in ipairs({ FEEL.landing[1] + math.cos(a) * 0.1, TABLE_Y + 0.01, FEEL.landing[2] + math.sin(a) * 0.1, 0.035, 0.6, 1, 1, 0.95 }) do marks[#marks + 1] = v end
    end
    for _, v in ipairs({ FEEL.landing[1], TABLE_Y + 0.01, FEEL.landing[2], 0.06, 1, 1, 1, 0.9 }) do marks[#marks + 1] = v end
  end
  if you.swing_planned or you.waiting_line then
    local p = you.body.blade
    local glow = 0.16 + 0.04 * math.sin(sys.real_time() * 14)
    for _, v in ipairs({ p[1], p[2], p[3], glow, 1, 0.5, 0.85, 0.75 }) do marks[#marks + 1] = v end
  end
  if #marks > 0 then
    gfx.unlit(true)
    gfx.blend("alpha")
    gfx.depth(true, false)
    gfx.texture(1, blob_tex)
    gfx.particles(marks)
    gfx.texture(1, nil)
    gfx.depth(true, true)
    gfx.blend("opaque")
    gfx.unlit(false)
  end
  local flames = {}
  add_flame_particles(robot, flames, sys.real_time())
  add_flame_particles(you, flames, sys.real_time())
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.depth(true, false)
  gfx.texture(1, blob_tex)
  gfx.particles(flames)
  gfx.texture(1, nil)
  gfx.depth(true, true)
  gfx.blend("opaque")
  gfx.unlit(false)

  -- The ball: a real lit ball, its shadow straight below it on the table or the floor (always, so you can read
  -- where it is), and the bursts where bat meets ball.
  gfx.draw(ball_mesh, ball.x, ball.y, ball.z, 0, 0, 0, 1, BALL_R * 2, BALL_R * 2, BALL_R * 2, 1.0, 0.93, 0.8, 1)
  local over = math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L and ball.y >= TABLE_Y
  local floor = over and TABLE_Y + 0.003 or 0.003
  local size = BALL_R * 3.2 / (1 + (ball.y - floor) * 0.4)
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.cull(false)
  gfx.texture(1, blob_tex)
  gfx.draw(blob_mesh, ball.x, floor, ball.z, 0, 0, 0, 1, size * 1.4, 1, size * 1.4, 0, 0, 0, 0.75)
  gfx.texture(1, nil)
  gfx.cull(true)
  local spark_particles = {}   -- the bursts: square sparks flying out, and a flash (two triangles each)
  for _, e in ipairs(sparks) do
    local age = e.t / 0.3
    local ring = 0.06 + 0.45 * age
    for k = 0, 15 do
      local a = k / 16 * math.pi * 2
      for _, v in ipairs({ e.x + math.cos(a) * ring, e.y + math.sin(a) * ring, e.z, 0.05 * (1 - age), 1, 0.85, 0.4, 1 - age }) do spark_particles[#spark_particles + 1] = v end
    end
    for _, v in ipairs({ e.x, e.y, e.z, 0.32 * (1 - age), 1, 1, 0.9, 0.85 * (1 - age) }) do spark_particles[#spark_particles + 1] = v end
  end
  if #spark_particles > 0 then gfx.particles(spark_particles) end
  gfx.blend("opaque")
  gfx.unlit(false)

  -- The trail: six points (the ball now, then its last five places) for shaders/trail.slang to draw a smooth
  -- glowing ribbon through.
  local points = { { ball.x, ball.y, ball.z } }
  for i = 1, 5 do points[i + 1] = trail[i] or points[i] end
  for i = 1, 4 do gfx.frame_param(i, points[i][1], points[i][2], points[i][3], TRAIL_WIDTH) end
  gfx.param(1, points[5][1], points[5][2], points[5][3], 0)
  gfx.param(2, points[6][1], points[6][2], points[6][3], 0)
  gfx.shader("trail")
  gfx.blend("add")
  gfx.cull(false)
  gfx.depth(true, false)
  gfx.draw(trail_mesh, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1.0, 0.55, 0.8, 0.9)
  gfx.depth(true, true)
  gfx.cull(true)
  gfx.blend("opaque")
  gfx.shader()

end

-- The 2D layer, in order: your swipes, score and messages, the move cue, the serve meter and its rating, the
-- controls' labels, the budget meter.
local function draw_ui()
  local w, h = gfx.size()
  -- Your lines: the one being drawn, and finished ones fading out.
  -- Your swipe: a glowing trail, thick and bright at the finger, tapering and fading behind it. Soft round spots
  -- close enough together to read as one stroke (2D drawing has no turned quads).
  local function trail(l, alpha)
    local total = 0
    for i = 2, #l do total = total + math.sqrt((l[i][1] - l[i - 1][1]) ^ 2 + (l[i][2] - l[i - 1][2]) ^ 2) end
    local along = 0
    for i = 2, #l do
      local a, b = l[i - 1], l[i]
      local seg = math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
      local n = math.max(1, math.floor(seg / 4))
      for k = 0, n - 1 do
        local u = (along + seg * k / n) / math.max(total, 1)     -- 0 at the start, 1 at the finger
        local size = 6 + 20 * u * u
        local px, py = a[1] + (b[1] - a[1]) * k / n, a[2] + (b[2] - a[2]) * k / n
        gfx.image(blob_tex, px - size / 2, py - size / 2, size, size, 1, 0.55 + 0.4 * u, 0.85 + 0.15 * u, alpha * (0.25 + 0.75 * u))
      end
      along = along + seg
    end
  end
  for _, l in pairs(swing_lines) do trail(l, 0.95) end
  for _, d in ipairs(fading_lines) do trail(d.points, 0.95 * (1 - d.t / 0.5)) end
  gfx.rect(w / 2 - 1, h * 0.55, 2, h * 0.3, 1, 1, 1, 0.08)   -- where walking ends and swinging begins

  -- Score and messages, in your dot font.
  local left, top, right, bottom = gfx.safe()
  text("DREAM TABLE", left + 80, top + 20, 26, 0.85, 0.75, 1)
  local board = title_case(string.format("YOU %d  -  %d ROBOT", game.score.you, game.score.robot))
  text(board, w - right - text_width(board, 30) - 24, top + 20, 30, 1, 1, 1)
  local serving = title_case((game.state == "serve" or game.state == "point") and (game.serve_turn() == "you" and "YOUR SERVE" or "ROBOT SERVES") or "")
  text(serving, w - right - text_width(serving, 20) - 24, top + 62, 20, 0.85, 0.9, 1)
  if game.wins > 0 then text(title_case("WINS " .. game.wins), left + 80, top + 58, 20, 0.85, 0.9, 1) end

  local center = nil
  if game.state == "serve" and game.server == "you" then center = "DRAW A LINE TO TOSS" end
  if game.state == "toss" and game.server == "you" and not you.swing_planned then center = "DRAW AGAIN TO SERVE" end
  if game.state == "over" then center = message .. "   TAP TO PLAY AGAIN" end
  if center then center = title_case(center) text(center, (w - text_width(center, 26)) / 2, h * 0.2, 26, 1, 1, 1) end
  if game.state ~= "over" and sys.real_time() - message_time < 1.6 then
    text(title_case(message), (w - text_width(title_case(message), 30)) / 2, h * 0.14, 30, 1, 0.85, 0.55)
  end
  -- The next move and when: a fixed white target ring, and a coloured ring closing onto it. When they meet it's
  -- the moment: the target turns green and says "Now!". One move at a time (dash, jump), each dropped once
  -- it's done.
  if game.hint and game.hint.moves and game.state == "rally" and game.last_hitter == "robot" then
    local now, b = sys.time(), you.body
    local done = {
      dash = b.dash ~= nil or math.sqrt((b.x - game.hint.x) ^ 2 + (b.z - game.hint.z) ^ 2) < 0.3,
      jump = b.jump_t ~= nil and not jump_recovering(b),
    }
    local cue
    for _, mv in ipairs(game.hint.moves) do
      if not done[mv.kind] and now < mv.at + 0.2 then cue = mv break end
    end
    if cue then
      local CUES = { dash = { game.hint.x < b.x and "<< Dash" or "Dash >>", 1, 0.6, 0.25 }, jump = { "Jump", 0.4, 0.85, 1 } }
      local c = CUES[cue.kind]
      local cx, cy, target = w / 2, h * 0.8, 44                -- below your mannequin
      local left = cue.at - now
      local ready = left <= 0.08                          -- the moment (and a little after)
      for k = 0, 23 do                                    -- the target
        local a = k / 24 * math.pi * 2
        local col = ready and { 0.4, 1, 0.5 } or { 1, 1, 1 }
        gfx.image(blob_tex, cx + math.cos(a) * target - 5, cy + math.sin(a) * target - 5, 10, 10, col[1], col[2], col[3], 0.9)
      end
      if not ready then                                   -- the closing ring: one second away is 180 units out
        local rr = target + math.min(1.2, left) * 180
        for k = 0, 23 do
          local a = k / 24 * math.pi * 2
          gfx.image(blob_tex, cx + math.cos(a) * rr - 8, cy + math.sin(a) * rr - 8, 16, 16, c[2], c[3], c[4], 0.95)
        end
        text(c[1], cx - text_width(c[1], 30) / 2, cy - 15, 30, c[2], c[3], c[4])
      else
        local size = 52 + 10 * math.sin(now * 30)
        text("Now!", cx - text_width("Now!", size) / 2, cy - size / 2, size, 0.4, 1, 0.5)
        text(c[1], cx - text_width(c[1], 26) / 2, cy + target + 12, 26, c[2], c[3], c[4])
      end
    end
  end
  local frame_dt = math.min(0.1, sys.real_time() - (last_draw or sys.real_time()))
  if FEEL.stop_until and sys.real_time() >= FEEL.stop_until then FEEL.stop_until = nil sys.time_scale(1) end
  FEEL.kick = FEEL.kick * math.exp(-14 * frame_dt)
  last_draw = sys.real_time()
  draw_meter(blob_tex, w, h, right, sys.real_time(), frame_dt)
  draw_rating(w, h)
  local walk_hint, swing_hint = title_case("DRAG HERE TO MOVE"), title_case("DRAW HERE TO SWING")
  text(walk_hint, w * 0.25 - text_width(walk_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
  text(swing_hint, w * 0.75 - text_width(swing_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
  budget.draw(left + 16, top + 100)
end


-- The trail: the ball's position every TRAIL_STEP seconds (none while it is held for a serve).
local function update_trail(dt)
  trail_clock = trail_clock + dt
  if game.state == "serve" then trail = {} end
  if trail_clock >= TRAIL_STEP or #trail == 0 then
    trail_clock = 0
    table.insert(trail, 1, { ball.x, ball.y, ball.z })
    if #trail > 5 then table.remove(trail) end
  end
end
local function clear_trail() trail = {} end

return {
  show_message = show_message, follow_camera = follow_camera, update_trail = update_trail, clear_trail = clear_trail,
  draw_world = draw_world, draw_ui = draw_ui,
}
end
