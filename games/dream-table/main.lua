-- Dream Table: table tennis, wooden mannequin against wooden mannequin.
-- You are the near mannequin. Hold and drag on the left half of the screen to walk it: it follows your finger's
-- movement, wherever you start (a trackpad, not a joystick). Draw a line on the right half to swing: the line's
-- direction picks the stroke from the recorded shots (the way the bat travelled when it was recorded), and its
-- direction and length aim the ball and set its pace. Draw it any time before the ball arrives; the mannequin
-- times the swing and steps to meet the ball. On the left half, let go while the finger is still moving fast: flick
-- up and it jumps straight up, following the charted heights (chart.heights); flick any other way and it dashes that way.
-- Both mannequins have no legs: they are jet torsos, a flame under the hips that flares big and blue as a dash or a
-- jump starts, and sputters while the torso flutters on the way down from a jump.
-- Your serve, as in real table tennis: the first line tosses the ball up off your bat, and the charge meter on the
-- right fills as it rises (full at the top). The second line is the swing: drawn any time before the ball falls back,
-- the bat is planned onto the falling ball like any return, and the charge when you drew sets the serve's power. A
-- star falls down the meter with the ball: swing before it reaches the bottom. The serve must bounce on your
-- side, then on the other. Miss it and it's the robot's point.
-- The far mannequin is a polite robot that can't lose.
-- Rules: first to 11, win by 2, serve changes every 2 points (every point from 10-10).
-- ponytail: serves go straight to the other side (no first bounce on your own side) and there is no spin.

local lib = include("lib.lua")
local scenery = include("scenery.lua")
budget = include("budget.lua")   -- global, so `lua budget.report()` works from dev.py

-- The real table, in metres: 2.74 long (along z), 1.525 wide, 0.76 high, net 15.25 cm.
local TABLE_Y, HALF_W, HALF_L, NET_TOP = 0.76, 0.7625, 1.37, 0.76 + 0.1525
local R = 0.03                        -- ball radius (a real one is 2 cm; bigger reads better on screen)
local G = 7.5                         -- a little floatier than real gravity: it's a dream
local BOUNCE = 0.86                   -- how much height a bounce keeps

local ball = { x = 0, y = 1, z = 0, vx = 0, vy = 0, vz = 0 }
local score = { player = 0, cpu = 0 }
local wins = save.get("wins", 0)
local bitfont = include("bitfont.lua")   -- your own dot font, baked with its outline (tools/make_bitmap_font.py)
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

-- state: "serve" (someone holds the ball), "toss" (a serve in the air), "rally", "point" (a short pause), "over"
local state, server, last, bounces = "serve", "player", nil, { player = 0, cpu = 0 }
local message, message_time, timer = "", 0, 0
local serve_ball, serve_bounced = false, false   -- the ball in play is a serve, and it has bounced on the server's side

-- ---------------------------------------------------------------- the mannequins
-- Both play the shots you recorded with the mocap game. mannequin.glb is made by tools/blender_mannequin.py from
-- captures/shots-<n>-<k>-<shot>-sam3d.npz, one clip per shot, each with the hit CONTACT seconds in. Every clip also
-- plays mirror-imaged with the bat in the other hand, so 11 recordings give 22 moves.
-- A swing is planned onto the ball's flight: which move, where its bat meets the ball, where to stand for that,
-- and when to start the clip so the recorded hit lands as the ball gets there. The ball is only hit if it really
-- touches the bat. Feet stay planted and step when the body moves away from them; the animation's own steps stay.
local dummy = model.load("mannequin.glb")
local bat_model = model.load("bat.glb")
-- Feel: what a hit and a bounce sound and look like (tools/make_dreamtable_sounds.py makes the sounds).
local FEEL = {
  hit = audio.load("hit_bat.wav"), bounce = audio.load("bounce.wav"),
  stop = 0.07,          -- a hit freezes the game this long (real seconds): hit-stop
  shake = 0.06,         -- and kicks the camera this far (metres), dying away
  stop_until = nil, kick = 0, at = nil,
  landing = nil,        -- where the robot's ball will bounce on your side: { x, z }
}
local FOLLOW_THROUGH = 0.5            -- seconds of a shot played after its hit, then back to the ready pose
local CONTACT = 2.0                  -- seconds into every shot clip that the ball is hit (tools/split_shots.py)
-- The middle of each bat's blade, in the bat's own frame (tools/blender_bats.py: along the hand, palm side).
local BLADE = { L = { 0.273, -0.003, -0.033 }, R = { -0.273, -0.003, -0.033 } }
local HIT_RANGE = 0.13               -- the ball's centre this close to the blade's: the bat meets it (the robot's)
local YOUR_HIT_RANGE, YOUR_LEAD = 0.18, 0.05   -- yours is more forgiving, and your swing may start this late before
                                               -- contact (its wind-up sped up: play_swing), so a late line still hits
local YOUR_AIR_TOL = 0.2                       -- in a jump, your swing takes a ball this far above or below its height
local TOP_SPEED = 2.2                -- metres a second: the body can't outrun its feet

local chart = include("chart.lua")   -- the charted jump: heights a frame, and what is worked out from them
local FLUTTER_HZ, FLUTTER_ROLL = 7, math.rad(12)   -- the fall's flutter: wobbles a second, and how far it rolls
local FLICK_SPEED = 900              -- screen units a second: a finger let go this fast is a jump or dash, slower a walk
local DASH_DIST, DASH_TIME = 1.6, 0.22    -- a dash covers this far (metres) this quickly (seconds)
local FLARE_TIME = 0.4               -- the flame's big blue flare when a dash or jump starts fades over this long
local AIR_DRIFT, AIR_DRIFT_SPEED = 0.35, 2.0   -- in a jump, a planned swing may drift the jet this far, this fast
local COURT_BACK = HALF_L + 1.8        -- and how far back from the net (as for walking)
local COURT_X = 2.2                  -- how far either side a body may go (metres): room to dash for wide balls
local JUMP_COST, DASH_COST = 0.35, 0.25   -- planning: how much a swing that needs a jump or a dash is avoided
local SKY_CHANCE, WIDE_CHANCE = 0.25, 0.25   -- the robot's fantasy returns: a sky ball (jump!) or a wide ball (dash!)
local WIDE_CURVE, WIDE_KICK = 4.0, 1.0   -- a wide ball's sidespin: it curves sideways this hard all the way (m/s^2),
                                         -- and kicks a little more off its bounce (m/s)
local LEAN_PER_SPEED, LEAN_MOST = math.rad(4), math.rad(30)   -- tilt toward where it's going: per m/s, and at most
local LEAN_EASE = 10                 -- how quickly the tilt follows (bigger is quicker); it eases back upright
local WALK = 7                       -- how quickly a body heads for its spot (bigger is quicker)
local CROUCH = 0.3                   -- how far a body may sink (knees bending) to play a high shot on a lower ball
local TOSS = 2.6                     -- how fast the robot's flick sends the ball up to be served (metres a second)
-- Your serve, all in one table:
local SERVE = {
  reach = 0.8,        -- a late serve swing: the torso glides this far into place, however short the time
  last = 0.75,        -- the star lands when the ball has fallen to this far above where it was tossed: the last
                      -- moment a swing still meets it cleanly (about 0.65 s after the top)
  perfect = 0.4,      -- ratings: swung in the star's last 40% is perfect,
  good = 0.25,        -- after it has fallen 25% good
  lead = 0.1,         -- your serve may start its swing part-way through: in time until the star lands
  toss = 3.0,         -- your toss (m/s): with g it rises for a second, filling the meter
  g = 3.0,            -- a tossed ball floats (gravity until it's hit): the star falls slowly enough to time
  lift = math.rad(55), lift_time = 0.45,   -- the serving arm follows the toss up this far, over this long
}
local BLEND_TIME = 0.18              -- switching moves: every bone blends from the old pose over this long
local moves, hands, bones, effects = {}, {}, {}, {}
local rest = {}                      -- every bone's turn in the T-pose
local hips_parent

local function v(x, y, z) return { x, y, z } end
local function add(a, b) return v(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
local function sub(a, b) return v(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
local function mul(a, s) return v(a[1] * s, a[2] * s, a[3] * s) end
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
local function len(a) return math.sqrt(dot(a, a)) end
local function unit(a) local l = len(a) return l > 1e-9 and mul(a, 1 / l) or v(0, 1, 0) end
local function cross(a, b) return v(lib.cross(a[1], a[2], a[3], b[1], b[2], b[3])) end
local function qmul(a, b) return { lib.quat_mul(a[1], a[2], a[3], a[4], b[1], b[2], b[3], b[4]) } end
local function qinv(q) return { -q[1], -q[2], -q[3], q[4] } end
local function rot(q, p) return v(lib.quat_rotate(q[1], q[2], q[3], q[4], p[1], p[2], p[3])) end
local function mirror_turn(q) return { q[1], -q[2], -q[3], q[4] } end   -- the turn seen in a mirror across x = 0
local function yaw_turn(angle) return { 0, math.sin(angle / 2), 0, math.cos(angle / 2) } end

local function from_to(a, b)
  a, b = unit(a), unit(b)
  local c = dot(a, b)
  if c < -0.9999 then
    local axis = unit(math.abs(a[1]) < 0.9 and cross(a, v(1, 0, 0)) or cross(a, v(0, 1, 0)))
    return { axis[1], axis[2], axis[3], 0 }
  end
  local axis = cross(a, b)
  local q = { axis[1], axis[2], axis[3], 1 + c }
  local l = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)
  return { q[1] / l, q[2] / l, q[3] / l, q[4] / l }
end

-- From turn a part of the way to turn b, the short way round.
local function toward(a, b, t)
  if a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] < 0 then b = { -b[1], -b[2], -b[3], -b[4] } end
  local q = { lib.lerp(a[1], b[1], t), lib.lerp(a[2], b[2], t), lib.lerp(a[3], b[3], t), lib.lerp(a[4], b[4], t) }
  local l = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)
  return { q[1] / l, q[2] / l, q[3] / l, q[4] / l }
end

-- A bone's place and turn inside the model, as animator a has it now (0: the T-pose).
local function joint(name, a)
  local x, y, z, qx, qy, qz, qw = model.joint(dummy, a, name)
  return v(x, y, z), { qx, qy, qz, qw }
end

-- Sets a bone's turn inside the model, through its turn within its parent.
local function set_turn(name, parent_turn, turn, a)
  local t1, t2, t3, _, _, _, _, s1, s2, s3 = model["local"](dummy, a, name)
  local q = qmul(qinv(parent_turn), turn)
  model["local"](dummy, a, name, t1, t2, t3, q[1], q[2], q[3], q[4], s1, s2, s3)
end

local function partner(name)
  if name:sub(-2) == ".L" then return name:sub(1, -3) .. ".R" end
  if name:sub(-2) == ".R" then return name:sub(1, -3) .. ".L" end
  return name
end

-- Sets the whole pose: every bone's turn inside the model, and where the hips are.
local function write_pose(turns, hips, a)
  local ppos, pturn = joint(hips_parent, a)
  local at = rot(qinv(pturn), sub(hips, ppos))
  local _, _, _, q1, q2, q3, q4, s1, s2, s3 = model["local"](dummy, a, "hips")
  model["local"](dummy, a, "hips", at[1], at[2], at[3], q1, q2, q3, q4, s1, s2, s3)
  for _, b in ipairs(bones) do
    local parent_turn = turns[b.parent] or select(2, joint(b.parent, a))
    set_turn(b.name, parent_turn, turns[b.name], a)
  end
end

-- Turns the whole pose into its mirror image: left does what right did, and the other way round.
local function mirror_pose(a)
  local new = {}
  for _, b in ipairs(bones) do
    local _, t = joint(partner(b.name), a)
    new[b.name] = qmul(mirror_turn(qmul(t, qinv(rest[partner(b.name)]))), rest[b.name])
  end
  local hips = joint("hips", a)
  write_pose(new, v(-hips[1], hips[2], hips[3]), a)
end

-- Where the middle of a hand's bat blade is inside the model.
local function blade_in_model(side, a)
  local wrist, turn = joint("hand." .. side, a)
  return add(wrist, rot(qmul(turn, hands[side].offset), BLADE[side]))
end

-- Reads every shot clip once: which shot, which way it faces, where the blade is at the hit and which way it is
-- travelling then (the swing); and the mirror image of each.
local function learn_moves()
  moves, bones, hands = {}, {}, {}
  local nodes = model.nodes(dummy)
  local function depth(n) local d = 0 while n.parent do d, n = d + 1, nodes[n.parent] end return d end
  for _, n in ipairs(nodes) do
    local skeleton = n.name:match("%.[LR]$") or n.name == "hips" or n.name == "spine" or n.name == "chest"
                     or n.name == "neck" or n.name == "head"
    if skeleton and n.parent then
      bones[#bones + 1] = { name = n.name, parent = nodes[n.parent].name, depth = depth(n) }
      if n.name == "hips" then hips_parent = nodes[n.parent].name end
    end
  end
  table.sort(bones, function(a, b) return a.depth < b.depth end)   -- parents before children
  for _, b in ipairs(bones) do rest[b.name] = select(2, joint(b.name, 0)) end
  for _, side in ipairs({ "L", "R" }) do
    hands[side] = { bone = "hand." .. side, node = "bat." .. side, offset = qinv(rest["hand." .. side]) }
  end

  local scratch = model.animator(dummy)
  for _, clip in ipairs(model.clips(dummy)) do
    local side, kind = clip.name:match("%-(%a%a)%-(%a+)$")
    if side == "fh" or side == "bh" then
      model.pose(dummy, scratch, clip.name, 0, 1)
      local left, right = joint("thigh.L", scratch), joint("thigh.R", scratch)
      local fx, fz = -(left[3] - right[3]), left[1] - right[1]   -- facing, from the body's left (+x faces +z)
      model.pose(dummy, scratch, clip.name, CONTACT - 0.15, 1)
      local before = blade_in_model("R", scratch)
      model.pose(dummy, scratch, clip.name, CONTACT + 0.15, 1)
      local after = blade_in_model("R", scratch)
      model.pose(dummy, scratch, clip.name, CONTACT, 1)
      local blade = blade_in_model("R", scratch)
      for _, mirrored in ipairs({ false, true }) do
        local turn = yaw_turn(-math.atan(mirrored and -fx or fx, fz))   -- turned to face +z
        local flip = mirrored and -1 or 1
        local b = v(blade[1] * flip, blade[2], blade[3])
        local swing = rot(turn, v((after[1] - before[1]) * flip, after[2] - before[2], after[3] - before[3]))
        -- The swing as the player sees it from behind: across (to the body's right is +) and up.
        local s_right, s_up = -swing[1], swing[2]
        local l = math.sqrt(s_right * s_right + s_up * s_up)
        moves[#moves + 1] = { clip = clip.name, kind = kind, side = side, mirrored = mirrored, turn = turn,
                              hand = mirrored and "L" or "R", blade = rot(turn, b),
                              swing = { s_right / math.max(l, 1e-6), s_up / math.max(l, 1e-6) } }
      end
    end
  end
end

-- One mannequin. side "cpu" stands at the far end facing +z; "player" at the near end, turned half round.
local function new_mannequin(side)
  local m = { side = side, sign = side == "cpu" and -1 or 1, anim = lib.animation(dummy), legs = {},
              blend = { from = nil, t = 1 }, used = {}, serve_hand = "R", armed = false, aim = nil,
              face = side == "cpu" and { 0, 0, 0, 1 } or yaw_turn(math.pi) }
  local z = m.sign * (HALF_L + 0.6)
  m.body = { x = 0, y = 0, ty = 0, drop = 0, air = 0, roll = 0, pivot = v(0, 0, 0), flare = 0, dash = nil, chart = nil,
             base = m.face, z = z, tx = 0, tz = z, vx = 0, vz = 0, turn = m.face, move = moves[1], plan_for = nil,
             blade = v(0, 1, z) }
  m.legs = { "thigh.L", "thigh.R" }                  -- hidden every frame (pose): these are jet torsos
  if moves[1] then m.anim:play(moves[1].clip, { loop = false, speed = 0, time = 0.3, fade = 0 }) end   -- ready
  return m
end

local robot, you

-- body.drop lowers the whole body so its lowest foot is on the floor (the recordings hold the hips a little high).
-- Where the model's origin is: the body's place, its height (drop onto its feet, jumps, sinking), and pivot: tipping
-- over (a dive, a fall) turns it about the hips rather than the feet.
local function height(m) return m.body.y - m.body.drop + m.body.air end
local function origin(m) return add(v(m.body.x, height(m), m.body.z), m.body.pivot) end
local function to_world(m, p) return add(origin(m), rot(m.body.turn, p)) end
local function to_model(m, p) return rot(qinv(m.body.turn), sub(p, origin(m))) end
local function facing(m, move) return qmul(m.face, move.turn) end
local function blade_offset(m, move) return rot(m.face, move.blade) end

-- Switching moves: the pose it was in is kept, and every bone blends from it into the new move over BLEND_TIME.
-- This works between a mirrored and an unmirrored move too, which an animation cross-fade can't.
local function keep_pose(m)
  local turns = {}
  for _, b in ipairs(bones) do turns[b.name] = select(2, joint(b.name, m.anim.animator)) end
  m.blend.from, m.blend.t, m.blend.time = { turns = turns, hips = (joint("hips", m.anim.animator)) }, 0, nil
end

local function blend_pose(m, dt)
  local blend = m.blend
  if not blend.from then return end
  blend.t = blend.t + dt / (blend.time or BLEND_TIME)
  if blend.t >= 1 then blend.from = nil return end
  local w = blend.t * blend.t * (3 - 2 * blend.t)
  local turns = {}
  for _, b in ipairs(bones) do turns[b.name] = toward(blend.from.turns[b.name], select(2, joint(b.name, m.anim.animator)), w) end
  local hips = joint("hips", m.anim.animator)
  write_pose(turns, add(blend.from.hips, mul(sub(hips, blend.from.hips), w)), m.anim.animator)
end

-- The ball's flight from now on: the points where mannequin m may hit it (after one bounce on its side, off the end
-- of the table; or, serving, on the way down before any bounce), with how long until it gets there.
local function flight(m, serving)
  local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
  local own, dt, out = serving and 0 or (bounces[m.side] or 0), 1 / 120, {}
  for step = 1, 300 do
    vy = vy - (serving and state == "toss" and SERVE.g or G) * dt   -- a toss floats until it's hit
    vx = vx + (ball.curve or 0) * dt                                -- a wide ball's sidespin
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if y - R <= TABLE_Y and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      y, vy, vx, vz = TABLE_Y + R, -vy * BOUNCE, vx * 0.97, vz * 0.97
      if z * m.sign > 0 then
        own = own + 1
        if own == 1 and ball.kick and m == you then vx = vx + ball.kick end   -- a wide ball kicks out sideways
      end
    end
    if y < R or own >= 2 then break end
    local hittable = serving and (vy < 0 and own == 0) or (not serving and own == 1 and z * m.sign > HALF_L - 0.2)
    -- every 1/20 s and only at heights a bat can reach, jumping included: the move search is costly (budget.lua)
    if hittable and step % 6 == 0 and y > 0.3 and y < 3.4 then out[#out + 1] = { x = x, y = y, z = z, t = step * dt } end
  end
  return out
end

-- Plans are coroutines, spread over a few fixed steps (PLAN_SLICE moves tried per step) so no frame goes over the
-- Lua budget (budget.lua). start_plan replaces any plan in progress; run_plans moves them on each fixed step and
-- calls done(result) when one finishes.
local PLAN_SLICE = 6
local plans = {}
local function start_plan(m, fn, done) plans[m] = { co = coroutine.create(fn), done = done } end
local function run_plans(dt)
  for m, p in pairs(plans) do
    local ok, result = coroutine.resume(p.co, dt)
    if not ok then plans[m] = nil error(result) end
    if coroutine.status(p.co) == "dead" then
      plans[m] = nil
      if p.done then p.done(result) end
    end
  end
end

-- The charted jump: straight up, following chart.heights frame by frame.
local function recovering(b) return b.chart and math.floor(b.chart * chart.fps + 0.5) + 1 >= chart.land end
local function chart_jump(m)
  local b = m.body
  if b.chart and not recovering(b) then return end
  b.chart, b.flare = 0, FLARE_TIME                          -- in the recovery: jump again straight away
end

-- A dash: a quick glide along the floor, direction (dx, dz) in the world, dist metres (DASH_DIST if not given),
-- in the air too, where it doesn't touch the jump's heights. Once it's done the body walks on to b.goal, the spot
-- the latest plan wants (if any). In a landing recovery it cuts the recovery short.
local function dash(m, dx, dz, dist, time)
  local b = m.body
  if b.dash then return end
  if recovering(b) then b.chart, b.air = nil, 0 end
  b.dash, b.flare = { x = dx, z = dz, t = 0, dist = dist or DASH_DIST, time = time or DASH_TIME }, FLARE_TIME
end

-- Starts a shot so its hit lands t seconds from now. With time to spare it plays at its own speed (slower if t is
-- longer than the wind-up); with little time the last WINDUP seconds of wind-up play sped up (up to WINDUP / 0.05,
-- nine times), so a late swing still visibly swings at once, and the blend into it is as quick.
local WINDUP = 0.45
local function play_swing(m, clip, t)
  m.anim.layers = {}
  if t >= WINDUP then
    m.anim:play(clip, { loop = false, restart = true, fade = 0, time = math.max(0, CONTACT - t), speed = t > CONTACT and CONTACT / t or 1 })
  else
    t = math.max(t, 0.05)
    m.anim:play(clip, { loop = false, restart = true, fade = 0, time = CONTACT - WINDUP, speed = WINDUP / t })
    m.blend.time = math.min(BLEND_TIME, t * 0.3)
  end
end

-- Picks the best (move, point) by cost(move, point, fit, run, blade offset), fit being how far the bat would be off
-- the ball's height (crouching counts a little), then starts it. The body can meet a point three ways:
--   on the ground, crouching up to CROUCH;
--   in the air on the charted jump: already jumping (the chart says how high it will be then), or a jump planned to
--   start so the chart's height at contact is the one needed (chart.best_frame); no walking once it's up;
--   with a dash when walking can't get there in time (on the ground or in the air).
-- Jumps and dashes cost a little extra (JUMP_COST, DASH_COST) and are then made by the plan itself. A sky ball
-- (ball.call == "jump") may only be played in the air. The blade's offset is worked out once per move, not per
-- point: this search is the game's biggest piece of Lua work (budget.lua).
local function walk_reach(t) return TOP_SPEED * math.max(0, t - 0.1) end
local function dash_need(run, walk_time, t)    -- false: walking will do; true: a dash will; nil: neither
  if run <= walk_reach(walk_time) then return false end
  if t >= DASH_TIME and run <= DASH_DIST + walk_reach(walk_time - DASH_TIME) then return true end
  return nil
end
local function choose_and_swing(m, points, cost_of, hand, lead, peek)
  lead = lead or 0.3                                             -- the least time to swing in (a serve: less)
  local b = m.body
  local f_now = b.chart and math.floor(b.chart * chart.fps + 0.5) + 1
  local flying = f_now and f_now < chart.land
  local sky = ball.call == "jump" and m == you
  -- Only the robot (and the hints, which only look) jumps and dashes by itself; your jumps and dashes are yours. Your
  -- swing plans with what your body is doing: walking, already up in a jump, or where a dash you started will end.
  local assist = m == robot or peek
  local bx, bz, busy = b.x, b.z, 0
  if b.dash then
    local d = b.dash
    local u = d.t / d.time
    local rest = d.dist * (1 - u) * (1 - u)                      -- the eased glide still to come
    bx, bz, busy = b.x + d.x * rest, b.z + d.z * rest, d.time - d.t
  end
  local best, at, best_cost, how, best_sink, best_delay, best_dash = nil, nil, math.huge
  local waited = 0                                               -- seconds spent planning, over several steps
  for i, move in ipairs(moves) do
    if i % PLAN_SLICE == 0 and coroutine.isyieldable() then waited = waited + coroutine.yield() end
    if hand and move.hand ~= hand then goto next_move end   -- a serve is with one given hand
    local off = blade_offset(m, move)
    for _, p in ipairs(points) do
      local t = p.t - waited
      local sx, sz = p.x - off[1], p.z - off[3]                  -- where the body would be
      if sz * m.sign > HALF_L + 0.2 and sz * m.sign < COURT_BACK and t > lead and math.abs(sx) <= COURT_X then   -- on court, in time
        local run = math.sqrt((sx - bx) ^ 2 + (sz - bz) ^ 2)
        local need = p.y - off[2]                                  -- how high the body must be at contact
        local fc = f_now and f_now + math.floor(t * chart.fps + 0.5)
        if flying and chart.air[fc] then                                 -- already jumping, still up then
          local dash = false                                        -- in the air the jet drifts AIR_DRIFT to line up
          if run > AIR_DRIFT then dash = assist and dash_need(run, 0, t) or nil end
          local miss = math.abs(chart.air[fc] - need)
          if dash ~= nil and miss <= (assist and chart.air_tol or YOUR_AIR_TOL) then
            local cost = cost_of(move, p, miss, run, off) + (dash and DASH_COST or 0)
            if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "air", 0, nil, dash end
          end
        elseif not sky and not (flying and fc < chart.land) then   -- on the ground
          local sink = math.max(-CROUCH, math.min(0, need))
          local dash = dash_need(run, t - busy, t)
          if dash and not assist then dash = nil end                -- it needs a dash you haven't made
          if dash == nil and state == "toss" and m == you and run <= SERVE.reach then dash = true end   -- your serve: glide into place
          if dash ~= nil then
            local cost = cost_of(move, p, math.abs(need - sink) - sink * 0.1, run, off) + (dash and DASH_COST or 0)
            if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "ground", sink, nil, dash end
          end
        end
        if assist and not flying and need > 0.2 then               -- a jump, timed to meet it (the robot's)
          local f = chart.best_frame[math.floor(need / 0.02 + 0.5)]
          local delay = f and t - (f - 1) / chart.fps
          if delay and delay >= 0 then
            local dash = dash_need(run, delay, t)
            if dash ~= nil then
              local cost = cost_of(move, p, math.abs(chart.air[f] - need), run, off) + JUMP_COST + (dash and DASH_COST or 0)
              if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "jump", 0, delay, dash end
            end
          end
        end
      end
    end
    ::next_move::
  end
  if not best then return false end
  if peek then                                                   -- only looking (the hints): say what it would do
    local off = blade_offset(m, best)
    return { x = at.x - off[1], z = at.z - off[3], how = how, dash = best_dash, delay = best_delay, contact = at.t - waited }
  end
  m.used[best] = (m.used[best] or 0) + 1
  keep_pose(m)
  b.move, m.armed = best, true
  local off = blade_offset(m, best)
  local sx, sz = at.x - off[1], at.z - off[3]
  b.tx, b.tz, b.ty, b.goal = sx, sz, best_sink, { sx, sz }
  if best_dash then                                              -- dash toward the spot (then walk the rest)
    local dx, dz = sx - b.x, sz - b.z
    local run = math.sqrt(dx * dx + dz * dz)
    dash(m, dx / run, dz / run, math.min(run, DASH_DIST), math.min(DASH_TIME, (at.t - waited) * 0.7))
  end
  if how == "jump" then b.jump_in = best_delay end
  m.anim.layers = {}
  local t = at.t - waited                                         -- from now, after the time spent planning
  play_swing(m, best.clip, t)
  return true
end

-- The robot's choice: the height should suit the shot, it shouldn't run far, and it likes variety.
local function plan_robot(m, serve_hand)
  return choose_and_swing(m, flight(m, serve_hand ~= nil), function(move, p, fit, run, off)
    return fit * 3 + run * (serve_hand and 1.5 or 0.3)
           + (m.used[move] or 0) * 0.35 + math.random() * 0.05
  end, serve_hand)
end

-- Hints, once the robot has hit: where to stand for the cleanest return, and whether it calls for a jump (and
-- when) or a dash (and which way). Worked out like your own swing would be, without making it.
local hint = nil
local CUE_REACT, SWING_CUE = 0.35, 0.4   -- seconds: a beat to react before a dash; draw the swing this long before contact
local CUE_GAP = 0.6                      -- seconds between the swing cue and a jump cue
local PLAN_RETRY = 0.08                  -- seconds between tries of a swing line that can't meet the ball yet
local function start_hint()
  hint = nil
  start_plan("hint", function()
    return choose_and_swing(you, flight(you, false), function(move, p, fit, run) return fit * 3 + run * 0.8 end, nil, nil, true)
  end, function(r)
    if not r then return end
    local now = sys.time()
    hint = { x = r.x, z = r.z, how = r.how, dash = r.dash, jump_at = r.delay and now + r.delay, contact_at = now + r.contact }
    -- The moments to act, in order: dash soon (a beat to react), the jump when the plan would, and the swing line
    -- by SWING_CUE before contact (the swing needs that long to wind up).
    hint.moves = {}
    if r.dash then hint.moves[#hint.moves + 1] = { at = now + CUE_REACT, kind = "dash" } end
    if r.how == "jump" and r.delay then hint.moves[#hint.moves + 1] = { at = now + r.delay, kind = "jump" } end
    -- The swing line can be drawn early (it waits, and is planned again when you jump), so with a jump it comes
    -- first, CUE_GAP before it, and the two cues never land together.
    local swing_at = now + r.contact - SWING_CUE
    if r.how == "jump" and r.delay then swing_at = math.min(swing_at, now + r.delay - CUE_GAP) end
    -- (No swing cue: draw your line any time after the robot's shot. Only the special moves get one.)
    table.sort(hint.moves, function(a, b) return a.at < b.at end)
  end)
end

-- Your choice, from the line you drew: the move whose recorded swing runs most like the line, that can meet the
-- ball without much running.
local function plan_line(m, line, serving)
  return choose_and_swing(m, flight(m, serving), function(move, p, fit, run, off)
    local likeness = move.swing[1] * line.right + move.swing[2] * line.up   -- 1: the same direction
    return (1 - likeness) * 2.5 + fit * 3 + run * 0.8
  end, nil, serving and SERVE.lead or YOUR_LEAD)
end

-- A turn from three axes (where the turn sends x, y and z), as a quaternion.
local function axes_turn(x, y, z)
  local tr = x[1] + y[2] + z[3]
  if tr > 0 then
    local s4 = math.sqrt(tr + 1) * 2
    return { (y[3] - z[2]) / s4, (z[1] - x[3]) / s4, (x[2] - y[1]) / s4, s4 / 4 }
  elseif x[1] > y[2] and x[1] > z[3] then
    local s4 = math.sqrt(1 + x[1] - y[2] - z[3]) * 2
    return { s4 / 4, (y[1] + x[2]) / s4, (z[1] + x[3]) / s4, (y[3] - z[2]) / s4 }
  elseif y[2] > z[3] then
    local s4 = math.sqrt(1 + y[2] - x[1] - z[3]) * 2
    return { (y[1] + x[2]) / s4, s4 / 4, (z[2] + y[3]) / s4, (z[1] - x[3]) / s4 }
  end
  local s4 = math.sqrt(1 + z[3] - x[1] - y[2]) * 2
  return { (z[1] + x[3]) / s4, (z[2] + y[3]) / s4, s4 / 4, (x[2] - y[1]) / s4 }
end

-- Holding the ball for a serve: the serving hand turns (by weight w, 0 to 1) so its bat lies flat, face up, still
-- pointing the way it did, and the ball rests on top. The blade runs along the bat's x and faces along its z
-- (tools/blender_bats.py).
local function flat_bat(m, side, w)
  if w <= 0 then return end
  local a, hand = m.anim.animator, hands[side]
  local _, turn = joint(hand.bone, a)
  local t = qmul(qmul(m.body.turn, turn), hand.offset)          -- the bat's turn in the world, as animated
  local x, z = rot(t, v(1, 0, 0)), rot(t, v(0, 0, 1))
  local h = math.sqrt(x[1] * x[1] + x[3] * x[3])
  if h < 1e-3 then x, h = rot(m.body.turn, v(0, 0, 1)), 1 end
  x = v(x[1] / h, 0, x[3] / h)
  local up = v(0, z[2] >= 0 and 1 or -1, 0)                   -- whichever face is nearer up stays up
  local flat = qmul(qmul(qinv(m.body.turn), axes_turn(x, cross(up, x), up)), qinv(hand.offset))
  local new = { lib.quat_slerp(turn[1], turn[2], turn[3], turn[4], flat[1], flat[2], flat[3], flat[4], w) }
  set_turn(hand.bone, select(2, joint("forearm." .. side, a)), new, a)
end

-- The toss's follow-through: the serving arm turns up from the shoulder by SERVE.lift times w (0 to 1), the hand
-- rising in front of the body (about the model's side axis).
local function lift_arm(m, side, w)
  if w <= 0 then return end
  local a = m.anim.animator
  local _, turn = joint("upper_arm." .. side, a)
  local h = -SERVE.lift * w / 2
  set_turn("upper_arm." .. side, select(2, joint("shoulder." .. side, a)), qmul({ math.sin(h), 0, 0, math.cos(h) }, turn), a)
end

-- Where the top of a bat's blade is in the world, for the ball to rest on.
local function on_bat(m, side)
  local b = to_world(m, blade_in_model(side, m.anim.animator))
  return v(b[1], b[2] + R + 0.006, b[3])   -- on the rubber (the blade is about a centimetre thick)
end

-- A little burst where bat and ball meet.
local function spark(x, y, z) effects[#effects + 1] = { x = x, y = y, z = z, t = 0 } end

-- A mannequin each frame: its animation, mirrored if the move is, blended, turned, feet, then where its bat is.
local function pose(m, dt)
  m.anim:update(dt)
  if m.body.move.mirrored then mirror_pose(m.anim.animator) end
  blend_pose(m, dt)
  -- A shot ends FOLLOW_THROUGH after its hit, blending back to the ready pose: the recordings carry on past the
  -- hit, and one (shots-2-4-fh-drive) swings a needless second time at 2.9 s.
  local top = m.anim.layers[#m.anim.layers]
  if top and top.speed > 0 and top.time > CONTACT + FOLLOW_THROUGH then
    keep_pose(m)
    m.body.move = moves[1]
    m.anim:play(moves[1].clip, { loop = false, restart = true, speed = 0, time = 0.3, fade = 0 })
  end
  local b = m.body
  b.base = toward(b.base, facing(m, b.move), 1 - math.exp(-12 * dt))
  -- The flutter: on the way down from a charted jump the torso rolls quickly from side to side, like a falling
  -- leaf. Looks only: its height still follows the chart.
  local f = b.chart and math.floor(b.chart * chart.fps + 0.5) + 1
  b.flutter = f and f > chart.peak and f < chart.land
  if b.flutter and not m.armed then                    -- not mid-swing: the roll would carry the bat off the ball b.roll = math.sin((f - chart.peak) / chart.fps * FLUTTER_HZ * 2 * math.pi) * FLUTTER_ROLL
  else b.roll = lib.approach(b.roll, 0, 5, dt) end
  b.flare = math.max(0, b.flare - dt)
  local ahead = rot(b.base, v(0, 0, 1))
  local half = b.roll / 2
  local roll = { ahead[1] * math.sin(half), ahead[2] * math.sin(half), ahead[3] * math.sin(half), math.cos(half) }
  -- The lean: its measured speed over the floor tips the top toward where it's going, a little when walking and a
  -- lot into a dash, easing back upright when it stops.
  if dt > 0 then
    local vx, vz = (b.x - (b.px or b.x)) / dt, (b.z - (b.pz or b.z)) / dt
    local e = 1 - math.exp(-LEAN_EASE * dt)
    b.lvx, b.lvz = (b.lvx or 0) + (vx - (b.lvx or 0)) * e, (b.lvz or 0) + (vz - (b.lvz or 0)) * e
  end
  b.px, b.pz = b.x, b.z
  local speed = math.sqrt(b.lvx ^ 2 + b.lvz ^ 2)
  local tilt = roll
  if speed > 0.01 then
    local angle = math.min(LEAN_MOST, speed * LEAN_PER_SPEED) / 2
    local ax, az = b.lvz / speed, -b.lvx / speed                -- up x direction: turning about it tips up that way
    tilt = qmul({ ax * math.sin(angle), 0, az * math.sin(angle), math.cos(angle) }, roll)
  end
  b.turn = qmul(tilt, b.base)
  b.pivot = sub(v(0, 0.9, 0), rot(tilt, v(0, 0.9, 0)))           -- tip about the hips, not the floor
  for _, thigh in ipairs(m.legs) do                   -- no legs: a jet torso (the leg bones shrunk to nothing)
    local t1, t2, t3, q1, q2, q3, q4 = model["local"](dummy, m.anim.animator, thigh)
    model["local"](dummy, m.anim.animator, thigh, t1, t2, t3, q1, q2, q3, q4, 0.001, 0.001, 0.001)
  end
  -- The toss: the serving arm lifts after the ball (up and back down over SERVE.lift_time), the bat kept flat.
  local lift = 0
  if m.lift_t then
    m.lift_t = m.lift_t + dt
    if m.lift_t >= SERVE.lift_time then m.lift_t = nil else lift = math.sin(math.pi * m.lift_t / SERVE.lift_time) end
  end
  lift_arm(m, m.serve_hand, lift)
  local holding = (state == "serve" or m.lift_t) and (server == "cpu") == (m == robot)
  m.flat = lib.approach(m.flat or 0, holding and 1 or 0, 12, dt)   -- eases in and out: no snap at the toss
  flat_bat(m, m.serve_hand, m.flat)
  -- Where the bat meets the ball, for play: from the upright body (the lean and the flutter are looks only, and
  -- would otherwise carry the bat off a ball the plan timed for an upright body).
  m.body.blade = add(v(b.x, height(m), b.z), rot(b.base, blade_in_model(m.body.move.hand, m.anim.animator)))
end

-- Its body heads for its spot.
local function walk(m, dt)
  local b = m.body
  local f_now = b.chart and math.floor(b.chart * chart.fps + 0.5) + 1
  if f_now and f_now < chart.land then                -- the charted jump goes straight up and down: no walking,
    b.vx, b.vz = 0, 0                                 -- only a jet's drift to line up a planned swing
    if m.armed then
      local dx, dz = b.tx - b.x, b.tz - b.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d > 1e-4 then
        local step = math.min(d, AIR_DRIFT_SPEED * dt)
        b.x, b.z = b.x + dx / d * step, b.z + dz / d * step
      end
    end
  else
    local nx, nz = lib.approach(b.x, b.tx, WALK, dt), lib.approach(b.z, b.tz, WALK, dt)
    local vx, vz = (nx - b.x) / dt, (nz - b.z) / dt
    if m.armed then                                   -- heading for a swing's spot: straight there, full speed
      local dx, dz = b.tx - b.x, b.tz - b.z
      local d = math.sqrt(dx * dx + dz * dz)
      local sp = math.min(TOP_SPEED, d / dt)
      vx, vz = d > 0 and dx / d * sp or 0, d > 0 and dz / d * sp or 0
    end
    local speed = math.sqrt(vx * vx + vz * vz)
    if speed > TOP_SPEED then vx, vz = vx / speed * TOP_SPEED, vz / speed * TOP_SPEED end
    b.vx, b.vz = vx, vz
    b.x, b.z = b.x + vx * dt, b.z + vz * dt
  end
  b.y = lib.approach(b.y, b.ty, WALK, dt)
  if b.jump_in then                                   -- a jump a plan has timed
    b.jump_in = b.jump_in - dt
    if b.jump_in <= 0 then b.jump_in = nil chart_jump(m) end
  end
  if b.dash then                                      -- a dash: a quick glide, slowing at its end; also in the air
    local d = b.dash
    local before = d.t
    d.t = math.min(d.time, d.t + dt)
    local function eased(t) local u = t / d.time return 1 - (1 - u) * (1 - u) end
    local step = (eased(d.t) - eased(before)) * d.dist
    b.x = lib.clamp(b.x + d.x * step, -COURT_X, COURT_X)
    b.z = m.sign * lib.clamp(m.sign * (b.z + d.z * step), HALF_L + 0.25, HALF_L + 1.8)
    b.tx, b.tz = b.x, b.z
    if d.t >= d.time then
      b.dash = nil
      if b.goal then b.tx, b.tz = b.goal[1], b.goal[2] end   -- a plan's spot: walk the rest of the way
    end
  end
  if b.chart then                                     -- the charted jump: the height is read, not simulated
    local f = math.floor(b.chart * chart.fps + 0.5) + 1      -- this step's frame (fixed steps are 1/60 s)
    b.chart = b.chart + dt
    local walking = f >= chart.land and (math.abs(b.tx - b.x) > 0.01 or math.abs(b.tz - b.z) > 0.01)   -- moving on
    if f > #chart.heights or walking then
      b.chart, b.air = nil, 0                         -- the crouch eases out as it walks (b.y heads for b.ty)
      if not walking then b.y = 0 end
    else
      local off = (chart.heights[f] - chart.stand) * chart.unit
      if off > 0 then b.air, b.y = off, 0 else b.air, b.y = 0, off end
    end
  end
end

-- Your serve's charge, 0 to 1: how far up the toss has gone (full at the top, and it stays full on the way down).
local function serve_charge()
  if state ~= "toss" or server ~= "player" then return 0 end
  return ball.vy > 0 and 1 - ball.vy / SERVE.toss or 1
end

-- How far the star has fallen, 0 to 1: from the top of the toss to SERVE.last, the last moment to swing.
local function serve_star()
  if state ~= "toss" or server ~= "player" or ball.vy > 0 or not you.toss_y then return 0 end
  local apex = you.toss_y + SERVE.toss * SERVE.toss / (2 * SERVE.g)
  return math.min(1, math.max(0, (apex - ball.y) / (apex - you.toss_y - SERVE.last)))
end

-- The serve meter: a long capsule on the right edge that slides in for your serve and out once it's hit. It fills
-- from the bottom with the charge, a smooth gradient from cool blue through gold to hot pink, shimmering when full;
-- once the ball is falling a star with a sparkle trail drops down inside it, reaching the bottom at the last moment
-- to swing (SERVE.last). Drawn as thin horizontal slices, each as wide as the capsule is at that height.
local METER_H, METER_W, METER_SLICES = 340, 30, 48
local meter = { show = 0 }
local GRADIENT = { { 0.25, 0.65, 1 }, { 0.3, 0.95, 0.85 }, { 1, 0.85, 0.25 }, { 1, 0.45, 0.2 }, { 1, 0.35, 0.7 } }
local function gradient(t)                          -- 0 at the bottom, 1 at the top
  local f = t * (#GRADIENT - 1)
  local i = math.min(#GRADIENT - 1, math.floor(f) + 1)
  local a, b, u = GRADIENT[i], GRADIENT[i + 1], f - (i - 1)
  return a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u, a[3] + (b[3] - a[3]) * u
end
local function capsule_width(y, h, w)               -- y from the top; rounded at both ends
  local r = w / 2
  local d = y < r and r - y or (y > h - r and y - (h - r) or 0)
  return d >= r and 0 or 2 * math.sqrt(r * r - d * d)
end
local function draw_meter(tex, w, h, right, now, dt)
  local wanted = server == "player" and (state == "serve" or state == "toss") and not (state == "toss" and you.armed and ball.vy < -3)
  meter.show = lib.approach(meter.show, wanted and 1 or 0, 9, dt)
  if meter.show < 0.01 then return end
  local slide = 1 - meter.show
  slide = slide * slide * (3 - 2 * slide)
  local x = w - right - 46 + slide * 140               -- the capsule's middle; slides in from beyond the right edge
  local y0 = h * 0.5 - METER_H / 2                     -- its top
  local charge = serve_charge()
  local full = charge >= 0.999
  local a = meter.show
  local step = METER_H / METER_SLICES
  for i = 0, METER_SLICES - 1 do
    local y = i * step
    local mid = y + step / 2
    local rim = capsule_width(mid, METER_H, METER_W + 8)    -- a dark rim
    if rim > 0 then gfx.rect(x - rim / 2, y0 - 4 + y * (METER_H + 8) / METER_H, rim, step * (METER_H + 8) / METER_H + 0.5, 0.05, 0.06, 0.16, 0.7 * a) end
  end
  for i = 0, METER_SLICES - 1 do
    local y = i * step
    local cw = capsule_width(y + step / 2, METER_H, METER_W)
    if cw > 0 then
      local t = 1 - (y + step / 2) / METER_H           -- 0 at the bottom
      local r, g, b = gradient(t)
      if t <= charge then
        if full then                                   -- full: a bright shimmer running up the capsule
          local pulse = 0.5 + 0.5 * math.sin(now * 10 - t * 9)
          r, g, b = r + (1 - r) * pulse * 0.45, g + (1 - g) * pulse * 0.45, b + (1 - b) * pulse * 0.45
        end
        gfx.rect(x - cw / 2, y0 + y, cw, step + 0.5, r, g, b, a)
      else
        gfx.rect(x - cw / 2, y0 + y, cw, step + 0.5, r * 0.3, g * 0.3, b * 0.35, 0.55 * a)   -- not yet filled
      end
    end
  end
  gfx.rect(x - METER_W * 0.28, y0 + METER_W * 0.4, METER_W * 0.14, METER_H - METER_W * 0.8, 1, 1, 1, 0.22 * a)   -- a shine
  if full then
    local glow = METER_W * 3.2
    gfx.image(tex, x - glow / 2, y0 - glow * 0.25, glow, glow * 0.6, 1, 0.5, 0.8, (0.25 + 0.15 * math.sin(now * 10)) * a)
  end
  -- The perfect zone: a pink bracket beside the stretch where the star's last SERVE.perfect falls.
  local zy = y0 + METER_W / 2 + (1 - SERVE.perfect) * (METER_H - METER_W)
  local zb = y0 + METER_H - METER_W / 2
  local bx = x - METER_W / 2 - 14
  gfx.rect(bx, zy - 3, 6, zb - zy + 6, 0.12, 0.05, 0.12, 0.6 * a)                -- a dark edge, so it reads on cloud
  gfx.rect(bx + 1, zy - 2, 4, zb - zy + 4, 1, 0.4, 0.8, a)
  gfx.rect(bx + 1, zy - 2, 12, 4, 1, 0.4, 0.8, a)
  gfx.rect(bx + 1, zb - 2, 12, 4, 1, 0.4, 0.8, a)
  -- The star: from the top once the ball turns over, down to the bottom at the last moment to swing.
  if state == "toss" and ball.vy <= 0 and you.toss_y then
    local sy = y0 + METER_W / 2 + serve_star() * (METER_H - METER_W)
    for k = 1, 4 do                                    -- the sparkle trail above it
      local ty = sy - k * 14
      if ty > y0 then
        local sz = 10 - k * 2
        gfx.image(tex, x - sz / 2 + math.sin(now * 20 + k) * 3, ty - sz / 2, sz, sz, 1, 1, 0.8, (0.8 - k * 0.15) * a)
      end
    end
    local spin = 1 + 0.15 * math.sin(now * 18)
    gfx.image(tex, x - 30 * spin, sy - 30 * spin, 60 * spin, 60 * spin, 1, 0.95, 0.6, 0.6 * a)   -- its glow
    bitfont.draw("*", x - 24 * spin, sy - 34 * spin, 72 * spin, 1, 0.97, 0.6, a)
  end
end

-- The serve's rating, popping up in your font when the serve is hit (or missed). It's the timing only: the charge
-- is the serve's speed and distance, not its rating. Perfect (pink): swung while the star is in the lower
-- SERVE.perfect of its fall (marked on the capsule); Good (gold): after it's fallen SERVE.good; Okay (green): earlier;
-- Fail (dark grey): a missed serve.
local RATINGS = { perfect = { "Perfect!", 1, 0.45, 0.8 }, good = { "Good", 1, 0.8, 0.2 },
                  okay = { "Okay", 0.35, 0.85, 0.35 }, fail = { "Fail", 0.3, 0.3, 0.33 } }
local rating = nil                                   -- { kind, t }
local function rate_serve(kind) rating = { kind = kind, at = sys.time() } end
local function draw_rating(w, h)
  if not rating then return end
  rating.t = sys.time() - rating.at                    -- game time: it pauses with the game
  if rating.t > 1.4 then rating = nil return end
  local r = RATINGS[rating.kind]
  local pop = rating.t < 0.15 and rating.t / 0.15 * 1.25 or (rating.t < 0.3 and 1.25 - (rating.t - 0.15) / 0.15 * 0.25 or 1)
  local fade = rating.t > 1.1 and 1 - (rating.t - 1.1) / 0.3 or 1
  local size = 56 * pop
  local word = r[1]
  bitfont.draw(word, (w - bitfont.width(word, size)) / 2, h * 0.3 - size / 2, size, r[2], r[3], r[4], fade)
end

-- The jet: a flame out of the bottom of the torso, flickering. For FLARE_TIME after a dash or a jump starts it is
-- bigger and blue, fading back to orange; while the torso flutters down it sputters. Soft blended particles (not
-- additive, which vanishes against white cloud), two
-- triangles each, added to `out` (gfx.particles' eight numbers each).
local FLAME_BITS, FLAME_LENGTH, FLAME_SIZE = 9, 0.42, 0.21
local FLAME = { { 1, 0.92, 0.45 }, { 1, 0.45, 0.05 }, { 0.85, 0.1, 0.02 } }      -- core, middle, tip
local FLARE = { { 0.75, 0.92, 1 }, { 0.15, 0.45, 1 }, { 0.05, 0.12, 0.8 } }
local function flame(m, out, time)
  local b = m.body
  local base = to_world(m, (joint("hips", m.anim.animator)))
  local down = rot(b.turn, v(0, -1, 0))
  local k = b.flare / FLARE_TIME
  k = k * k
  local length = FLAME_LENGTH * (1 + 1.4 * k) * (0.85 + 0.15 * math.sin(time * 41 + b.x * 7))
  if b.flutter then length = length * (0.3 + 0.7 * math.abs(math.sin(time * 23))) end
  local bx, by, bz, dx, dy, dz = base[1], base[2], base[3], down[1], down[2], down[3]
  for i = 0, FLAME_BITS - 1 do
    local t = i / (FLAME_BITS - 1)
    local wob = 0.012 * math.sin(time * 53 + i * 2.3)
    local along = 0.06 + t * length
    local c1, c2 = t < 0.5 and FLAME[1] or FLAME[2], t < 0.5 and FLAME[2] or FLAME[3]
    local f1, f2 = t < 0.5 and FLARE[1] or FLARE[2], t < 0.5 and FLARE[2] or FLARE[3]
    local u = (t < 0.5 and t or t - 0.5) * 2
    local n = #out
    out[n + 1], out[n + 2], out[n + 3] = bx + dx * along + wob, by + dy * along, bz + dz * along - wob
    out[n + 4] = FLAME_SIZE * (1 - 0.65 * t) * (1 + 0.9 * k)
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
  model.draw(dummy, o[1], o[2], o[3], q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1, m.anim.animator)
  for _, hand in pairs(hands) do
    local wrist, turn = joint(hand.bone, m.anim.animator)
    local at, t = to_world(m, wrist), qmul(qmul(q, turn), hand.offset)
    model.draw(bat_model, at[1], at[2], at[3], t[1], t[2], t[3], t[4], 1, 1, 1, 1, 1, 1, 1, nil, hand.node)
  end
end

-- ---------------------------------------------------------------- words, camera

local function side_of(z) return z > 0 and "player" or "cpu" end
local function other(who) return who == "player" and "cpu" or "player" end
local function say(text) message, message_time = text, sys.real_time() end

-- Every word on screen starts with a capital and goes on in lower case ("Your Serve"), except CPU and the
-- key names WASD, which stay in capitals. The title is drawn as it is, in full capitals.
local CAPITALS = { CPU = true, WASD = true }
local function words(s)
  return (s:gsub("%a[%a']*", function(w)
    if CAPITALS[w:upper()] then return w:upper() end
    return w:sub(1, 1):upper() .. w:sub(2):lower()
  end))
end

-- The camera: behind and above you (portrait, game.cfg), following your mannequin by less than it moves.
-- Walk left or right and it pans that way by CAM_SIDE of the distance; walk back from the table and it pulls back
-- and up (zooms out), walk in and it comes in and down (zooms in). It glides after you rather than jumping.
local CAM_SIDE, CAM_BACK, CAM_UP = 0.85, 0.8, 0.6
local CAM_HEIGHT, CAM_LOOK = 3.4, 1.0   -- eye height and the height it looks at: low enough to see the sky
local cam = { x = 0, back = 0 }
local function follow_camera(dt)
  local home = HALF_L + 0.6                          -- where you start: the camera's resting place
  cam.x = lib.approach(cam.x, you.body.x * CAM_SIDE, 4, dt)
  cam.back = lib.approach(cam.back, you.body.z - home, 4, dt)
end
local function camera()
  cam.ex, cam.ey, cam.ez = cam.x, CAM_HEIGHT + cam.back * CAM_UP, 6.0 + cam.back * CAM_BACK
  if FEEL.kick > 0.001 then                           -- a hit's camera kick, dying away
    cam.ex, cam.ey = cam.ex + (math.random() - 0.5) * FEEL.kick, cam.ey + (math.random() - 0.5) * FEEL.kick
  end
  gfx.camera(cam.ex, cam.ey, cam.ez, cam.x * 0.7, CAM_LOOK, -0.2 + cam.back * 0.2, 50)
end

-- ---------------------------------------------------------------- shots

-- The velocity that carries the ball from p to land at (tx, tz) on the table in about T seconds,
-- raising the arc until it clears the net.
local function shot(px, py, pz, tx, tz, T)
  local vx, vy, vz
  for _ = 1, 30 do
    vx, vz = (tx - px) / T, (tz - pz) / T
    vy = (TABLE_Y + R - py + 0.5 * G * T * T) / T
    local tn = -pz / vz                                  -- when it reaches the net
    if py + vy * tn - 0.5 * G * tn * tn > NET_TOP + R + 0.04 then break end
    T = T + 0.04
  end
  return vx, vy, vz
end

-- A serve: the ball must bounce once on the server's side, then once on the other. Tries landing spots on the
-- server's side and flight times, follows each through its bounce, and keeps the one whose second bounce lands
-- nearest depth metres past the net, toward tx, clearing the net.
-- Plays a serve through the same ball physics as step_ball (half steps of a 1/60 s tick): true if it bounces once
-- on the server's side, clears the net and lands on the other side.
local function serve_ok(px, py, pz, vx, vy, vz)
  local own, dt, x, y, z, first = pz > 0 and 1 or -1, 1 / 120, px, py, pz, false
  for _ = 1, 360 do
    local ox, oz = x, z
    local oy = y
    vy = vy - G * dt
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if oz * z <= 0 and oz ~= z and y - R < NET_TOP and math.abs(x) < HALF_W + 0.15 then return false end   -- the net
    if y - R <= TABLE_Y and vy < 0 and oy - R >= TABLE_Y - 0.02 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      if z * own > 0 then
        if first then return false end                   -- twice on its own side
        first = true
      else
        return first                                     -- on the other side: good only after its own side
      end
      y, vy, vx, vz = TABLE_Y + R, -vy * BOUNCE, vx * 0.97, vz * 0.97
    end
    if y < 0 then return false end
  end
  return false
end

-- A serve: the ball must bounce once on the server's side, then once on the other. Tries landing spots on the
-- server's side and flight times, ranks them by how near the second bounce lands to depth metres past the net,
-- toward tx, and keeps the best one that really works when played through the ball's own physics (serve_ok).
local function serve_shot(px, py, pz, tx, depth)
  local own = pz > 0 and 1 or -1
  local found = {}
  for land = 0.35, 1.06, 0.07 do
    for T = 0.3, 0.9, 0.04 do
      local lx = lib.clamp(lib.lerp(px, tx, 0.4), -(HALF_W - 0.15), HALF_W - 0.15)   -- on the table, even from wide
      local vx, vy, vz = (lx - px) / T, (TABLE_Y + R - py + 0.5 * G * T * T) / T, (own * land - pz) / T
      local bvx, bvy, bvz = vx * 0.97, -(vy - G * T) * BOUNCE, vz * 0.97   -- just after the bounce on its own side
      local tn = -own * land / bvz                                          -- reaching the net
      local t2 = 2 * bvy / G                                                -- back down on the table
      if tn > 0 then
        local over = TABLE_Y + R + bvy * tn - 0.5 * G * tn * tn
        local z2, x2 = own * land + bvz * t2, lx + bvx * t2
        if over > NET_TOP + R + 0.03 and z2 * own < -0.2 and math.abs(z2) < HALF_L - 0.1 and math.abs(x2) < HALF_W - 0.1 then
          found[#found + 1] = { vx, vy, vz, err = math.abs(math.abs(z2) - depth) + math.abs(x2 - tx) * 0.5 + (over - NET_TOP) * 0.5 }
        end
      end
    end
  end
  table.sort(found, function(a, b) return a.err < b.err end)
  for i = 1, math.min(#found, 8) do
    local f = found[i]
    if serve_ok(px, py, pz, f[1], f[2], f[3]) then return f end
  end
  return nil
end

local function hit(by, vx, vy, vz, serving)
  ball.vx, ball.vy, ball.vz = vx, vy, vz
  ball.call, ball.kick, ball.curve = nil, nil, nil
  last, bounces = by, { player = 0, cpu = 0 }
  serve_ball, serve_bounced = serving or false, false
  state = "rally"
  audio.play(FEEL.hit, by == "player" and 1 or 0.7, by == "player" and 1 or 1.08)
  FEEL.stop_until = sys.real_time() + (by == "player" and FEEL.stop or FEEL.stop * 0.6)
  FEEL.kick = by == "player" and FEEL.shake or FEEL.shake * 0.4
  sys.time_scale(0)
  if by == "player" then input.haptic("medium") end
  FEEL.landing = nil
end

-- Where the robot's ball will first bounce on your side, played through the ball's own physics (sidespin too).
local function predict_landing()
  FEEL.landing = nil
  do
    local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
    for _ = 1, 400 do
      vy = vy - G / 120
      vx = vx + (ball.curve or 0) / 120
      local py = y
      x, y, z = x + vx / 120, y + vy / 120, z + vz / 120
      if y - R <= TABLE_Y and py - R >= TABLE_Y - 0.02 and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
        if z > 0 then FEEL.landing = { x, z } end
        break
      end
      if y < 0 then break end
    end
  end
end

-- Your shot, aimed by the line you drew: it goes the way the line went across, longer lines go deeper and faster.
local function player_hit(serving)
  local line = you.aim or { right = 0, up = 0, length = 200 }
  local power = serving and you.serve_power or lib.clamp(line.length / 380, 0.25, 1)   -- a serve: the meter's charge
  local tx = lib.clamp(line.right * 0.65, -(HALF_W - 0.1), HALF_W - 0.1)
  local tz = -(0.45 + 0.8 * power)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.5 + 0.6 * power)
  if serving then rate_serve(you.serve_rating or "okay") end
  hint = nil
  if served then hit("player", served[1], served[2], served[3], true)
  else hit("player", shot(ball.x, ball.y, ball.z, tx, tz, 0.85 - 0.35 * power)) end
  you.armed, you.body.ty, you.body.goal = false, 0, nil
  if power > 0.85 and line.up < -0.3 then say("SMASH!") end
end

-- The robot is polite and can't lose: most returns (and every serve) are a gentle ball landing mid-table on your
-- half, on your side of it. But it's dream table tennis, so some returns call for your fantasy moves:
--   a sky ball: a tall lob landing deep that bounces up over your head, to be played in the air (jump!);
--   a wide ball: landing by the side line away from you, it kicks out sideways off its bounce (dash!).
local function cpu_hit(serving)
  local tx = lib.clamp(you.body.x * 0.6 + (math.random() - 0.5) * 0.2, -(HALF_W - 0.25), HALF_W - 0.25)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.8)
  local roll = math.random()
  if served then hit("cpu", served[1], served[2], served[3], true)
  elseif roll < SKY_CHANCE then
    hit("cpu", shot(ball.x, ball.y, ball.z, tx, 1.15, 1.7))
    ball.call = "jump"
    say("SKY BALL - JUMP!")
  elseif roll < SKY_CHANCE + WIDE_CHANCE then
    -- A banana shot: aimed in from the side line by as far as the sidespin will carry it, so it bends out
    -- through the air, lands by the side line away from you, and keeps curving after the bounce.
    local side = you.body.x > 0 and -1 or 1                      -- away from you
    local land, T = side * (HALF_W - 0.12), 0.7
    for _ = 1, 2 do                                              -- shot() may lengthen the flight to clear the net
      local vx, vy, vz = shot(ball.x, ball.y, ball.z, land - side * 0.5 * WIDE_CURVE * T * T, 0.85, T)
      T = (0.85 - ball.z) / vz
    end
    hit("cpu", shot(ball.x, ball.y, ball.z, land - side * 0.5 * WIDE_CURVE * T * T, 0.85, T))
    ball.curve, ball.kick, ball.call = side * WIDE_CURVE, side * WIDE_KICK, "dash"   -- a call only: any way that reaches it will do
    say("WIDE BALL - DASH!")
  else hit("cpu", shot(ball.x, ball.y, ball.z, tx, 0.7 + math.random() * 0.2, 0.8)) end
  robot.armed, robot.body.ty, robot.body.goal = false, 0, nil
  if not serving then predict_landing() end
  start_hint()
end

-- ---------------------------------------------------------------- points and serving

local function serve_turn()
  local total = score.player + score.cpu
  if score.player >= 10 and score.cpu >= 10 then return total % 2 == 0 and "player" or "cpu" end
  return math.floor(total / 2) % 2 == 0 and "player" or "cpu"
end

local function award(who, why)
  if state ~= "rally" then return end
  score[who] = score[who] + 1
  say((who == "player" and "YOUR POINT" or "CPU POINT") .. "  -  " .. why)
  if who == "player" then input.haptic("success") end
  local a, b = score[who], score[other(who)]
  if a >= 11 and a - b >= 2 then
    state = "over"
    if who == "player" then wins = wins + 1 save.set("wins", wins) end
    say(who == "player" and "YOU WIN!" or "THE CPU WINS")
  else
    state, timer = "point", 1.2
  end
end

local function start_serve()
  server, state, timer, last = serve_turn(), "serve", 1.0, nil
  ball.vx, ball.vy, ball.vz = 0, 0, 0
  ball.call, ball.kick, ball.curve = nil, nil, nil      -- a sky or wide ball's call ends with its point
  FEEL.landing = nil
  hint = nil
  you.armed, robot.armed, you.pending = false, false, nil
  for _, m in ipairs({ you, robot }) do                 -- back to their spots behind the table to serve or receive
    m.body.tx, m.body.tz, m.body.goal = lib.clamp(m.body.x, -0.6, 0.6), m.sign * (HALF_L + 0.6), nil
  end
  plans = {}
  trail = {}
end

local function new_game()
  score = { player = 0, cpu = 0 }
  start_serve()
  say("FIRST TO 11")
end

function init()
  learn_moves()
  robot, you = new_mannequin("cpu"), new_mannequin("player")
  new_game()
end

-- ---------------------------------------------------------------- the ball

local function step_ball(dt)
  local px, py, pz = ball.x, ball.y, ball.z
  ball.vy = ball.vy - (state == "toss" and SERVE.g or G) * dt   -- a tossed ball floats until it's hit
  ball.vx = ball.vx + (ball.curve or 0) * dt                      -- a wide ball's sidespin
  ball.x, ball.y, ball.z = ball.x + ball.vx * dt, ball.y + ball.vy * dt, ball.z + ball.vz * dt

  -- The net: below its top, the ball stops and drops back.
  if pz * ball.z <= 0 and pz ~= ball.z and ball.y - R < NET_TOP and math.abs(ball.x) < HALF_W + 0.15 then
    ball.z = pz > 0 and R or -R
    ball.vz, ball.vx = -ball.vz * 0.15, ball.vx * 0.4
    if state == "rally" then say("NET") end
  end

  -- The table.
  if ball.y - R <= TABLE_Y and ball.vy < 0 and py - R >= TABLE_Y - 0.02 and math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L then
    audio.play(FEEL.bounce, math.min(1, 0.3 + math.abs(ball.vy) * 0.15), 1)
    if ball.z > 0 then FEEL.landing = nil end
    ball.y, ball.vy = TABLE_Y + R, -ball.vy * BOUNCE
    ball.vx, ball.vz = ball.vx * 0.97, ball.vz * 0.97
    local side = side_of(ball.z)
    if ball.kick and side == "player" then ball.vx, ball.kick = ball.vx + ball.kick, nil end   -- a wide ball kicks
    if state == "rally" then
      local own_side = side == side_of(last == "player" and 1 or -1)
      if serve_ball and not serve_bounced then
        if own_side then serve_bounced = true                       -- a serve: first on the server's own side
        else award(other(last), "a serve must bounce on your own side first") end
      elseif own_side then
        award(other(last), "it bounced on the hitter's own side")
      else
        serve_ball = false
        bounces[side] = bounces[side] + 1
        if bounces[side] >= 2 and side == "cpu" then
          spark(ball.x, ball.y, ball.z) cpu_hit()    -- the robot can't lose: a short ball is still returned
        elseif bounces[side] >= 2 then award(last, "two bounces") end
      end
    end
  end

  -- The floor, or gone for good.
  if ball.y - R <= 0 then
    ball.y, ball.vy = R, -ball.vy * 0.5
    if state == "rally" then
      if bounces[other(last)] >= 1 then award(last, "not returned") else award(other(last), "out") end
    end
  end

  local function touching(m) return len(sub(v(ball.x, ball.y, ball.z), m.body.blade)) <= (m == you and YOUR_HIT_RANGE or HIT_RANGE) end

  -- Serves: the tossed ball is hit when it meets the server's bat on the way down; dropped, it's served again.
  if state == "toss" then
    local m = server == "cpu" and robot or you
    if ball.vy < 0 and touching(m) and (m == robot or you.armed) then   -- yours only once you've swung
      spark(ball.x, ball.y, ball.z)
      if m == robot then
        cpu_hit(true)
        robot.serve_hand = robot.serve_hand == "R" and "L" or "R"   -- next serve with the other hand
      else
        player_hit(true)
      end
    elseif ball.y - R <= 0.02 then
      if server == "player" then                                     -- a missed serve is the other side's point
        state, last = "rally", "player"
        award("cpu", "missed serve")
        rate_serve("fail")
      else
        start_serve()
      end
    end
  end

  -- Returns: a bat only plays a ball that has bounced once on its own side.
  if state == "rally" then
    if last == "cpu" and bounces.player == 1 and you.armed and touching(you) then
      spark(ball.x, ball.y, ball.z) player_hit()
    elseif last == "player" and bounces.cpu == 1 then
      -- The robot's bat should meet the ball; if its swing ever misses, it still returns it before the floor.
      if touching(robot) or (ball.vy < 0 and ball.y < 0.35) then spark(ball.x, ball.y, ball.z) cpu_hit() end
    end
  end
end

-- ---------------------------------------------------------------- your fingers

-- Left half: a drag walks your mannequin by however far the finger moves (start anywhere: it's a trackpad).
-- Right half: a drawn line is a swing. WASD walks on the PC too.
local MOVE_SCALE = 0.006             -- metres the body moves for each unit the finger moves
local drags, lines, drawn = {}, {}, {}   -- fingers walking, lines being drawn, finished lines fading out

-- A swipe that no swing can meet the ball with still swings, at once, the way you swiped (a miss you can see).
local function whiff(m, line)
  if m.armed or line.whiffed then return end
  line.whiffed = true
  local best, like = nil, -9
  for _, move in ipairs(moves) do
    local l = move.swing[1] * line.right + move.swing[2] * line.up
    if l > like then best, like = move, l end
  end
  if not best then return end
  keep_pose(m)
  m.body.move = best
  play_swing(m, best.clip, 0.25)
end

local function swing_from(line)
  if state == "serve" and server == "player" then
    -- Your serve, first line: the toss. The ball pops up off your bat.
    state, last, bounces = "toss", nil, { player = 0, cpu = 0 }
    ball.vx, ball.vy, ball.vz = 0, SERVE.toss, 0
    you.armed, you.toss_y, you.lift_t = false, ball.y, 0
  elseif state == "toss" and server == "player" and not you.armed and not plans[you] then
    -- Second line: the swing, planned onto the falling ball like any return; the charge now sets its power.
    you.aim, you.serve_power = line, math.max(0.25, serve_charge())
    local star = serve_star()
    you.serve_rating = star >= 1 - SERVE.perfect and "perfect" or (star >= SERVE.good and "good" or "okay")
    start_plan(you, function() return plan_line(you, line, true) end)
  elseif state == "rally" then
    you.aim = line
    if last == "cpu" then
      start_plan(you, function() return plan_line(you, line, false) end, function(ok) if not ok then you.pending = line whiff(you, line) end end)
    else
      you.pending = line                      -- drawn early: it's played as soon as the robot has hit
    end
  elseif state == "over" then
    new_game()
  end
end

local space_was = false
-- You jumped or dashed with a swing already planned: plan it again from the new move.
local function moved_on_your_own()
  if state == "rally" and last == "cpu" and you.aim and (you.armed or plans[you]) then
    you.armed, you.pending, you.retry_at = false, you.aim, nil
    plans[you] = nil
  end
end
local function fingers(dt)
  local w, h = gfx.size()
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then
      if t.x < w / 2 then
        drags[t.id] = { { sys.real_time(), t.x, t.y } }
      else
        lines[t.id] = { { t.x, t.y }, start = sys.real_time() }
      end
    elseif t.phase == "move" then
      if drags[t.id] then
        local d = drags[t.id]
        d[#d + 1] = { sys.real_time(), t.x, t.y }
        while #d > 2 and sys.real_time() - d[1][1] > 0.1 do table.remove(d, 1) end   -- the last tenth of a second
        local b = you.body
        b.tx = lib.clamp(b.tx + t.dx * MOVE_SCALE, -COURT_X, COURT_X)   -- walked to once a jump has landed
        b.tz = lib.clamp(b.tz + t.dy * MOVE_SCALE, HALF_L + 0.25, HALF_L + 1.8)
      elseif lines[t.id] then
        table.insert(lines[t.id], { t.x, t.y })
      end
    elseif t.phase == "up" then
      -- Let go while the finger is still moving fast: a jump that way, whatever the angle. Otherwise just a walk.
      local d = drags[t.id]
      if d and #d >= 2 then
        local a = d[1]
        local span = math.max(sys.real_time() - a[1], 1e-3)
        local vx, vy = (t.x - a[2]) / span, (a[3] - t.y) / span     -- right and up, units a second
        local speed = math.sqrt(vx * vx + vy * vy)
        if speed > FLICK_SPEED then
          if vy > math.abs(vx) then chart_jump(you) else you.body.goal = nil dash(you, vx / speed, -vy / speed) end   -- up: jump; else a dash
          moved_on_your_own()
        end
      end
      drags[t.id] = nil
      local l = lines[t.id]
      lines[t.id] = nil
      if l then
        local first, final = l[1], l[#l]
        local dx, dy = final[1] - first[1], first[2] - final[2]          -- right and up, in screen units
        local length = math.sqrt(dx * dx + dy * dy)
        if length > 30 then
          swing_from({ right = dx / length, up = dy / length, length = length })
          drawn[#drawn + 1] = { points = l, t = 0 }
        elseif state == "over" then
          new_game()
        end
      end
    end
  end
  local space = input.key("space")
  if space and not space_was then chart_jump(you) moved_on_your_own() end   -- the PC's key for the charted jump
  space_was = space
  local step = 1.4 * dt
  if input.key("a") or input.key("left") then you.body.tx = you.body.tx - step end
  if input.key("d") or input.key("right") then you.body.tx = you.body.tx + step end
  if input.key("w") or input.key("up") then you.body.tz = math.max(HALF_L + 0.25, you.body.tz - step) end
  if input.key("s") or input.key("down") then you.body.tz = math.min(HALF_L + 1.8, you.body.tz + step) end
  for i = #drawn, 1, -1 do
    drawn[i].t = drawn[i].t + dt
    if drawn[i].t > 0.5 then table.remove(drawn, i) end
  end
end

-- ---------------------------------------------------------------- each frame

function fixed_update(dt)
  run_plans(dt)
  -- The robot plans as soon as you hit; your line drawn early is played as soon as the robot hits.
  if state == "rally" and last == "player" and robot.body.plan_for ~= bounces then
    robot.body.plan_for = bounces
    start_plan(robot, function() return plan_robot(robot) end)
  end
  -- Your waiting swing line: planned once the hint is done (two searches at once would break the Lua budget), and
  -- if it can't meet the ball yet (you haven't jumped or dashed), tried again every PLAN_RETRY seconds.
  if state == "rally" and last == "cpu" and you.pending then
    if not plans[you] and not plans.hint and sys.time() >= (you.retry_at or 0) then
      you.retry_at = sys.time() + PLAN_RETRY
      local line = you.pending
      you.pending = nil
      start_plan(you, function() return plan_line(you, line, false) end, function(ok)
        if not ok and not you.pending then you.pending = line whiff(you, line) end
      end)
    end
  end
  walk(robot, dt)
  walk(you, dt)

  if state == "serve" then
    -- The server's ball rests on top of their bat.
    local m = server == "cpu" and robot or you
    local p = on_bat(m, m.serve_hand)
    ball.x, ball.y, ball.z = p[1], p[2], p[3]
  else
    for _ = 1, 2 do step_ball(dt / 2) end                -- half steps, so a fast ball can't skip the net
  end
end

function update(dt)
  budget.update()
  lib.update(dt)
  pose(robot, dt)
  pose(you, dt)
  for i = #effects, 1, -1 do
    effects[i].t = effects[i].t + dt
    if effects[i].t > 0.3 then table.remove(effects, i) end
  end
  fingers(dt)
  follow_camera(dt)

  timer = timer - dt
  if state == "serve" and server == "cpu" and timer <= 0 then
    -- The robot's flick: the ball hops up off the bat, and the swing is planned onto it as it falls.
    state, last, bounces = "toss", nil, { player = 0, cpu = 0 }
    ball.vx, ball.vy, ball.vz = 0, TOSS, 0
    robot.lift_t = 0
    start_plan(robot, function() return plan_robot(robot, robot.serve_hand) end)
  elseif state == "point" and timer <= 0 then
    start_serve()
  end

  -- The trail: the ball's position every TRAIL_STEP seconds (none while it is held for a serve).
  trail_clock = trail_clock + dt
  if state == "serve" then trail = {} end
  if trail_clock >= TRAIL_STEP or #trail == 0 then
    trail_clock = 0
    table.insert(trail, 1, { ball.x, ball.y, ball.z })
    if #trail > 5 then table.remove(trail) end
  end
end

-- ---------------------------------------------------------------- drawing

-- Text with a black outline: the words drawn in black around it, then in colour on top.
local text, text_width = bitfont.draw, bitfont.width

function draw()
  gfx.clear(scenery.FOG[1], scenery.FOG[2], scenery.FOG[3])
  camera()
  scenery.lights()
  -- The world (sky, cloud island, cobbles, beanstalks), then the table, lines, legs.
  scenery.draw(sys.real_time(), cam.ex, cam.ey, cam.ez)

  -- Blob shadows, as a PS1 game would have them: soft dark shapes on the ground under the table, each body and
  -- each planted foot. A real shadow map would draw everything twice (budget.lua).
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.depth(true, false)
  gfx.cull(false)
  gfx.texture(1, blob_tex)
  gfx.draw(blob_mesh, -0.2, 0.004, 0, 0, 0, 0, 1, HALF_W * 2 + 0.5, 1, HALF_L * 2 + 0.5, 0.1, 0.14, 0.35, 0.75)
  for _, m in ipairs({ robot, you }) do
    gfx.draw(blob_mesh, m.body.x, 0.006, m.body.z, 0, 0, 0, 1, 0.9, 1, 0.7, 0.1, 0.14, 0.35, 0.45 / (1 + m.body.air * 2))
  end
  -- Where to stand: a glowing spot on the floor (pink once you're on it).
  if hint and state == "rally" and last == "cpu" then
    local now = sys.time()
    local on = math.sqrt((you.body.x - hint.x) ^ 2 + (you.body.z - hint.z) ^ 2) < 0.15
    local r, g, bl = 1, on and 0.45 or 0.85, on and 0.8 or 0.25
    local pulse = 0.85 + 0.15 * math.sin(now * 12)
    gfx.draw(blob_mesh, hint.x, 0.012, hint.z, 0, 0, 0, 1, 0.6 * pulse, 1, 0.6 * pulse, r, g, bl, 0.85)
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
  if FEEL.landing and state == "rally" and last == "cpu" then
    for k = 0, 11 do
      local a = k / 12 * math.pi * 2
      for _, v in ipairs({ FEEL.landing[1] + math.cos(a) * 0.1, TABLE_Y + 0.01, FEEL.landing[2] + math.sin(a) * 0.1, 0.035, 0.6, 1, 1, 0.95 }) do marks[#marks + 1] = v end
    end
    for _, v in ipairs({ FEEL.landing[1], TABLE_Y + 0.01, FEEL.landing[2], 0.06, 1, 1, 1, 0.9 }) do marks[#marks + 1] = v end
  end
  if you.armed or you.pending then
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
  flame(robot, flames, sys.real_time())
  flame(you, flames, sys.real_time())
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
  gfx.draw(ball_mesh, ball.x, ball.y, ball.z, 0, 0, 0, 1, R * 2, R * 2, R * 2, 1.0, 0.93, 0.8, 1)
  local over = math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L and ball.y >= TABLE_Y
  local floor = over and TABLE_Y + 0.003 or 0.003
  local size = R * 3.2 / (1 + (ball.y - floor) * 0.4)
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.cull(false)
  gfx.texture(1, blob_tex)
  gfx.draw(blob_mesh, ball.x, floor, ball.z, 0, 0, 0, 1, size * 1.4, 1, size * 1.4, 0, 0, 0, 0.75)
  gfx.texture(1, nil)
  gfx.cull(true)
  local sparks = {}   -- the bursts: square sparks flying out, and a flash (two triangles each)
  for _, e in ipairs(effects) do
    local age = e.t / 0.3
    local ring = 0.06 + 0.45 * age
    for k = 0, 15 do
      local a = k / 16 * math.pi * 2
      for _, v in ipairs({ e.x + math.cos(a) * ring, e.y + math.sin(a) * ring, e.z, 0.05 * (1 - age), 1, 0.85, 0.4, 1 - age }) do sparks[#sparks + 1] = v end
    end
    for _, v in ipairs({ e.x, e.y, e.z, 0.32 * (1 - age), 1, 1, 0.9, 0.85 * (1 - age) }) do sparks[#sparks + 1] = v end
  end
  if #sparks > 0 then gfx.particles(sparks) end
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

  -- Your lines: the one being drawn, and finished ones fading out.
  local w, h = gfx.size()
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
  for _, l in pairs(lines) do trail(l, 0.95) end
  for _, d in ipairs(drawn) do trail(d.points, 0.95 * (1 - d.t / 0.5)) end
  gfx.rect(w / 2 - 1, h * 0.55, 2, h * 0.3, 1, 1, 1, 0.08)   -- where walking ends and swinging begins

  -- Score and messages, in your dot font.
  local left, top, right, bottom = gfx.safe()
  text("DREAM TABLE", left + 80, top + 20, 26, 0.85, 0.75, 1)
  local board = words(string.format("YOU %d  -  %d CPU", score.player, score.cpu))
  text(board, w - right - text_width(board, 30) - 24, top + 20, 30, 1, 1, 1)
  local serving = words((state == "serve" or state == "point") and (serve_turn() == "player" and "YOUR SERVE" or "CPU SERVES") or "")
  text(serving, w - right - text_width(serving, 20) - 24, top + 62, 20, 0.85, 0.9, 1)
  if wins > 0 then text(words("WINS " .. wins), left + 80, top + 58, 20, 0.85, 0.9, 1) end

  local center = nil
  if state == "serve" and server == "player" then center = "DRAW A LINE TO TOSS" end
  if state == "toss" and server == "player" and not you.armed then center = "DRAW AGAIN TO SERVE" end
  if state == "over" then center = message .. "   TAP TO PLAY AGAIN" end
  if center then center = words(center) text(center, (w - text_width(center, 26)) / 2, h * 0.2, 26, 1, 1, 1) end
  if state ~= "over" and sys.real_time() - message_time < 1.6 then
    text(words(message), (w - text_width(words(message), 30)) / 2, h * 0.14, 30, 1, 0.85, 0.55)
  end
  -- The next move and when: a fixed white target ring, and a coloured ring closing onto it. When they meet it's
  -- the moment: the target turns green and says "Now!". One move at a time (dash, jump, swing), each dropped once
  -- it's done.
  if hint and hint.moves and state == "rally" and last == "cpu" then
    local now, b = sys.time(), you.body
    local done = {
      dash = b.dash ~= nil or math.sqrt((b.x - hint.x) ^ 2 + (b.z - hint.z) ^ 2) < 0.3,
      jump = b.chart ~= nil and not recovering(b),
      swing = you.armed or you.pending ~= nil or plans[you] ~= nil,
    }
    local cue
    for _, mv in ipairs(hint.moves) do
      if not done[mv.kind] and now < mv.at + 0.2 then cue = mv break end
    end
    if cue then
      local CUES = { dash = { hint.x < b.x and "<< Dash" or "Dash >>", 1, 0.6, 0.25 }, jump = { "Jump", 0.4, 0.85, 1 },
                     swing = { "Swing", 1, 0.45, 0.8 } }
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
  local frame_dt = math.min(0.1, sys.real_time() - (meter.at or sys.real_time()))
  if FEEL.stop_until and sys.real_time() >= FEEL.stop_until then FEEL.stop_until = nil sys.time_scale(1) end
  FEEL.kick = FEEL.kick * math.exp(-14 * frame_dt)
  meter.at = sys.real_time()
  draw_meter(blob_tex, w, h, right, sys.real_time(), frame_dt)
  draw_rating(w, h)
  local walk_hint, swing_hint = words("DRAG HERE TO MOVE"), words("DRAW HERE TO SWING")
  text(walk_hint, w * 0.25 - text_width(walk_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
  text(swing_hint, w * 0.75 - text_width(swing_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
  budget.draw(left + 16, top + 100)
end
