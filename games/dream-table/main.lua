-- Dream Table: table tennis, wooden mannequin against wooden mannequin.
-- You are the near mannequin. Hold and drag on the left half of the screen to walk it: it follows your finger's
-- movement, wherever you start (a trackpad, not a joystick). Draw a line on the right half to swing: the line's
-- direction picks the stroke from the recorded shots (the way the bat travelled when it was recorded), and its
-- direction and length aim the ball and set its pace. Draw it any time before the ball arrives; the mannequin
-- times the swing and steps to meet the ball. On the left half, let go while the finger is still moving fast: flick
-- up and it jumps straight up, following the charted heights (jump_chart.heights); flick any other way and it dashes that way.
-- Both mannequins have no legs: they are jet torsos, a flame under the hips that flares big and blue as a dash or a
-- jump starts, and sputters while the torso flutters on the way down from a jump.
-- Your serve, as in real table tennis: the first line tosses the ball up off your bat, and the charge meter on the
-- right fills as it rises (full at the top). The second line is the swing: drawn any time before the ball falls back,
-- the bat is planned onto the falling ball like any return, and the charge when you drew sets the serve's power. A
-- star falls down the meter with the ball: swing before it reaches the bottom. The serve must bounce on your
-- side, then on the other. Miss it and it's the robot's point.
-- The far mannequin is a polite robot that can't lose.
-- Rules: first to 11, win by 2, serve changes every 2 points (every point from 10-10). A serve bounces on the
-- server's side, then the receiver's.
-- ponytail: the only spin is a wide ball's sidespin (FANTASY.wide_curve); topspin and backspin aren't modelled.

-- Where things are: main.lua holds the tuning, the mannequins' bodies (jet, jump, dash, lean, retro-burn), your
-- touches and the frame callbacks. The rest is in vec.lua (maths), skeleton.lua (bones and moves), planner.lua
-- (swings), serve_meter.lua (the serve's meter and rating), view.lua (drawing) and rally.lua (ball and rules).
-- NOTES.md has the map, the tuning tables and how to test.

local bone_local = model.bone_local   -- a bone's own transform (needs an app from 21 November 2026 or later)
local lib = include("lib.lua")
local scenery = include("scenery.lua")
budget = include("budget.lua")   -- global, so `lua budget.report()` works from dev.py

-- The real table, in metres: 2.74 long (along z), 1.525 wide, 0.76 high, net 15.25 cm.
local TABLE_Y, HALF_W, HALF_L, NET_TOP = 0.76, 0.7625, 1.37, 0.76 + 0.1525
local BALL_R = 0.03                        -- ball radius (a real one is 2 cm; bigger reads better on screen)
local GRAVITY = 7.5                         -- a little floatier than real gravity: it's a dream
local BOUNCE = 0.86                   -- how much height a bounce keeps

local ball = { x = 0, y = 1, z = 0, vx = 0, vy = 0, vz = 0 }
local bitfont = include("bitfont.lua")   -- your own dot font, baked with its outline (tools/dream-table/make_bitmap_font.py)
-- Feel: what a hit and a bounce sound and look like (tools/dream-table/make_dreamtable_sounds.py makes the sounds).
local FEEL = {
  hit = audio.load("hit_bat.wav"), bounce = audio.load("bounce.wav"),
  stop = 0.07,          -- a hit freezes the game this long (real seconds): hit-stop
  shake = 0.06,         -- and kicks the camera this far (metres), dying away
  stop_until = nil, kick = 0, at = nil,
  landing = nil,        -- where the robot's ball will bounce on your side: { x, z }
}
local FOLLOW_THROUGH = 0.5            -- seconds of a swing played after its hit, then back to the ready pose
local TOP_SPEED = 2.2                -- metres a second: the fastest a body walks (a dash is faster)
local JUMP_COST = 0.35               -- planning: how much a swing that needs a jump is avoided

-- Tuning, one table per thing to tune.
local DASH = {
  dist = 1.6, time = 0.22,           -- a dash covers this far (metres) this quickly (seconds)
  cost = 0.25,                       -- planning: how much a swing that needs a dash is avoided
  brake_start = 0.4,                 -- the retro-burn: from 60% through the dash,
  brake_time = 0.25,                 -- lasting this long after it,
  brake_angle = math.rad(55),        -- the hips turned this far
}
local JET = {                        -- the flame out of the hips
  bits = 9, length = 0.42, size = 0.21,
  colors = { { 1, 0.92, 0.45 }, { 1, 0.45, 0.05 }, { 0.85, 0.1, 0.02 } },        -- core, middle, tip
  flare_colors = { { 0.75, 0.92, 1 }, { 0.15, 0.45, 1 }, { 0.05, 0.12, 0.8 } },
  flare_time = 0.4,                  -- its big blue flare when a dash or jump starts fades over this long
  hover_max = 2.0,                   -- a jump can be held in the air (a hold on the right side) this long, at most
}
local LEAN = {                       -- how the torso tilts
  per_speed = math.rad(4), most = math.rad(30),   -- toward where it's going: per m/s, and at most
  ease = 10,                         -- how quickly the tilt follows (bigger is quicker); it eases back upright
  flutter_hz = 7, flutter_roll = math.rad(12),    -- the fall's flutter: wobbles a second, and how far it rolls
}
local FANTASY = {                    -- the robot's fantasy returns
  sky_chance = 0.25, wide_chance = 0.25,          -- a sky ball (jump!) or a wide ball (dash!)
  wide_curve = 4.0,                  -- a wide ball's sidespin: it curves sideways this hard all the way (m/s^2),
  wide_kick = 1.0,                   -- and kicks a little more off its bounce (m/s)
}
local REACH = {                      -- how close the ball's centre must come to the blade's for the bat to meet it
  robot = 0.13,
  you = 0.18,                        -- yours is more forgiving,
  your_lead = 0.05,                  -- your swing may start this late before contact (its wind-up sped up:
                                     -- play_swing), so a late line still hits,
  your_air_tol = 0.2,                -- and in a jump it takes a ball this far above or below its height
}
local CUE = {                        -- the hints' timing (seconds)
  react = 0.35,                      -- a beat to react before a dash
  retry = 0.08,                      -- between tries of a swing line that can't meet the ball yet
}

local jump_chart = include("jump_chart.lua")   -- the charted jump: heights a frame, and what is worked out from them
local FLICK_SPEED = 900              -- screen units a second: a finger let go this fast is a jump or dash, slower a walk
local AIR_DRIFT, AIR_DRIFT_SPEED = 0.35, 2.0   -- in a jump, a planned swing may drift the jet this far, this fast
local COURT_BACK = HALF_L + 1.8        -- and how far back from the net (as for walking)
local COURT_X = 2.2                  -- how far either side a body may go (metres): room to dash for wide balls
local WALK_EASE = 7                       -- how quickly a body heads for its spot (bigger is quicker)
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

-- The shared game table: what the modules use from this file, and the state more than one of them changes.
local game = {
  state = "serve",                  -- "serve" (someone holds the ball), "toss" (a serve in the air), "rally",
                                     -- "point" (a short pause), "over"
  server = "you", last_hitter = nil, -- who serves, who hit the ball last ("you" or "robot")
  bounces = { you = 0, robot = 0 },  -- bounces on each side since the last hit
  score = { you = 0, robot = 0 }, timer = 0, hint = nil,
  wins = save.get("wins", 0),
}
for k, x in pairs({ AIR_DRIFT = AIR_DRIFT, BALL_R = BALL_R, BOUNCE = BOUNCE, COURT_BACK = COURT_BACK,
                   COURT_X = COURT_X, CROUCH = CROUCH, CUE = CUE, DASH = DASH, FANTASY = FANTASY, FEEL = FEEL,
                   GRAVITY = GRAVITY, HALF_L = HALF_L, HALF_W = HALF_W, JET = JET, JUMP_COST = JUMP_COST,
                   NET_TOP = NET_TOP, REACH = REACH, SERVE = SERVE, TABLE_Y = TABLE_Y, TOP_SPEED = TOP_SPEED,
                   ball = ball, bitfont = bitfont, jump_chart = jump_chart, lib = lib, scenery = scenery }) do game[k] = x end

local vec = include("vec.lua")(lib)
local v, add, sub, qmul = vec.v, vec.add, vec.sub, vec.qmul
local qinv, rot, yaw_turn, toward = vec.qinv, vec.rot, vec.yaw_turn, vec.toward
local function share(t) for k, x in pairs(t) do game[k] = x end return t end   -- a module's functions, for the others
share(vec)
local skeleton = share(include("skeleton.lua")(game))

-- One mannequin. side "robot" stands at the far end facing +z; "you" at the near end, turned half round.
local function new_mannequin(side)
  local m = { side = side, sign = side == "robot" and -1 or 1, anim = lib.animation(skeleton.mannequin_model), legs = {},
              blend = { from = nil, t = 1 }, used = {}, serve_hand = "R", swing_planned = false, aim = nil,
              face = side == "robot" and { 0, 0, 0, 1 } or yaw_turn(math.pi) }
  local z = m.sign * (HALF_L + 0.6)
  m.body = { x = 0, y = 0, sink = 0, jump_height = 0, roll = 0, pivot = v(0, 0, 0), jet = { flare = 0 }, dash = nil,
             jump_t = nil,
             base = m.face, z = z, tx = 0, tz = z, vx = 0, vz = 0, turn = m.face, move = skeleton.moves[1], plan_for = nil,
             blade = v(0, 1, z) }
  m.legs = { "thigh.L", "thigh.R" }                  -- hidden every frame (pose_mannequin): these are jet torsos
  if skeleton.moves[1] then m.anim:play(skeleton.moves[1].clip, { loop = false, speed = 0, time = 0.3, fade = 0 }) end   -- ready
  return m
end

-- Where the model's origin is: the body's place, its height (b.y: sinking for a low ball; b.jump_height: the charted
-- jump), and its pivot: a lean tips it about the hips rather than the floor.
local function height(m) return m.body.y + m.body.jump_height end
local function origin(m) return add(v(m.body.x, height(m), m.body.z), m.body.pivot) end
local function to_world(m, p) return add(origin(m), rot(m.body.turn, p)) end
local function facing(m, move) return qmul(m.face, move.turn) end
local function blade_offset(m, move) return rot(m.face, move.blade) end

-- The charted jump: straight up, following jump_chart.heights frame by frame (b.jump_t: seconds into it).
local function jump_recovering(b) return b.jump_t and math.floor(b.jump_t * jump_chart.fps + 0.5) + 1 >= jump_chart.land end
local function start_jump(m)
  local b = m.body
  if b.jump_t and not jump_recovering(b) then return end
  b.jump_t, b.jet.flare, b.jet.hover_t = 0, JET.flare_time, 0           -- in the recovery: jump again straight away
end

-- A dash: a quick glide along the floor, direction (dx, dz) in the world, dist metres (DASH.dist if not given),
-- in the air too, where it doesn't touch the jump's heights. Once it's done the body walks on to b.goal, the spot
-- the latest plan wants (if any). In a landing recovery it cuts the recovery short.
local function dash(m, dx, dz, dist, time)
  local b = m.body
  if b.dash then return end
  if jump_recovering(b) then b.jump_t, b.jump_height = nil, 0 end
  b.dash, b.jet.flare = { x = dx, z = dz, t = 0, dist = dist or DASH.dist, time = time or DASH.time }, JET.flare_time
end

-- Where the top of a bat's blade is in the world, for the ball to rest on.
local function ball_rest_on_bat(m, side)
  local b = to_world(m, skeleton.blade_in_model(side, m.anim.animator))
  return v(b[1], b[2] + BALL_R + 0.006, b[3])   -- on the rubber (the blade is about a centimetre thick)
end

-- A little burst where bat and ball meet.
local sparks
local function spark(x, y, z) sparks[#sparks + 1] = { x = x, y = y, z = z, t = 0 } end

-- The mannequins, made once the moves are read (skeleton.load_moves).
skeleton.load_moves()
local robot, you = new_mannequin("robot"), new_mannequin("you")
sparks = {}                          -- bursts where bat meets ball: { x, y, z, t }
local walk_drags, swing_lines, fading_lines = {}, {}, {}   -- fingers walking, lines being drawn, finished lines fading out
for k, x in pairs({ you = you, robot = robot, blade_offset = blade_offset, dash = dash, jump_recovering = jump_recovering,
                   origin = origin, to_world = to_world, spark = spark, sparks = sparks, swing_lines = swing_lines,
                   fading_lines = fading_lines }) do game[k] = x end
local meter = share(include("serve_meter.lua")(game))
local planner = share(include("planner.lua")(game))
local view = share(include("view.lua")(game))
local rally = share(include("rally.lua")(game))

-- A swing ends FOLLOW_THROUGH after its hit, blending back to the ready pose: the recordings carry on past the
-- hit, and one (shots-2-4-fh-drive) swings a needless second time at 2.9 s.
local function end_follow_through(m)
  local top = m.anim.layers[#m.anim.layers]
  if top and top.speed > 0 and top.time > skeleton.CONTACT + FOLLOW_THROUGH then
    skeleton.keep_pose(m)
    m.body.move = skeleton.moves[1]
    m.anim:play(skeleton.moves[1].clip, { loop = false, restart = true, speed = 0, time = 0.3, fade = 0 })
  end
end

-- The flutter: on the way down from a charted jump the torso rolls quickly from side to side, like a falling
-- leaf. Looks only: its height still follows jump_chart. Returns the roll as a turn about the facing direction.
local function flutter_roll(m, dt)
  local b = m.body
  local f = b.jump_t and math.floor(b.jump_t * jump_chart.fps + 0.5) + 1
  b.flutter = f and f > jump_chart.peak and f < jump_chart.land
  if b.flutter and not m.swing_planned then        -- not mid-swing: the roll would carry the bat off the ball
    b.roll = math.sin((f - jump_chart.peak) / jump_chart.fps * LEAN.flutter_hz * 2 * math.pi) * LEAN.flutter_roll
  else
    b.roll = lib.approach(b.roll, 0, 5, dt)
  end
  b.jet.flare = math.max(0, b.jet.flare - dt)
  local ahead = rot(b.base, v(0, 0, 1))
  local half = b.roll / 2
  return { ahead[1] * math.sin(half), ahead[2] * math.sin(half), ahead[3] * math.sin(half), math.cos(half) }
end

-- The lean: its measured speed over the floor tips the top toward where it's going, a little when walking and a
-- lot into a dash, easing back upright when it stops. Returns the tilt over the floor (direction times angle).
local function lean(b, dt)
  if dt > 0 then
    local vx, vz = (b.x - (b.px or b.x)) / dt, (b.z - (b.pz or b.z)) / dt
    local e = 1 - math.exp(-LEAN.ease * dt)
    b.lvx, b.lvz = (b.lvx or 0) + (vx - (b.lvx or 0)) * e, (b.lvz or 0) + (vz - (b.lvz or 0)) * e
  end
  b.px, b.pz = b.x, b.z
  local speed = math.sqrt(b.lvx ^ 2 + b.lvz ^ 2)
  if speed <= 0.01 then return 0, 0 end
  local angle = math.min(LEAN.most, speed * LEAN.per_speed)
  return b.lvx / speed * angle, b.lvz / speed * angle
end

-- The retro-burn's weight, 0 to 1: over a dash's last DASH.brake_start of it and DASH.brake_time after. Returns
-- the dash it brakes ({ x, z }) and the eased weight.
local function brake_weight(b, dt)
  local brake = 0
  if b.dash then
    brake = math.max(0, (b.dash.t / b.dash.time - (1 - DASH.brake_start)) / DASH.brake_start)
  elseif b.brake then
    b.brake.t = b.brake.t + dt
    brake = math.max(0, 1 - b.brake.t / DASH.brake_time)
    if brake == 0 then b.brake = nil end
  end
  return b.dash or b.brake, brake * brake * (3 - 2 * brake)
end

-- The retro-burn turns the hips alone: their bottom (and the jet under them) swings toward the dash, blasting
-- the way it was going to stop it, while the spine is given back its turn so the chest, arms and head stay put.
local function retro_burn(m, dash_dir, w)
  local b = m.body
  b.hip_tilt = nil
  if w <= 0 or not dash_dir then return end
  local dir = rot(qinv(b.turn), v(dash_dir.x, 0, dash_dir.z))   -- the dash's direction inside the model
  local kl = math.sqrt(dir[1] * dir[1] + dir[3] * dir[3])
  if kl <= 1e-4 then return end
  local h = DASH.brake_angle * w / 2
  local qh = { -dir[3] / kl * math.sin(h), 0, dir[1] / kl * math.sin(h), math.cos(h) }   -- turns down toward dir
  local a = m.anim.animator
  local _, hips_turn = skeleton.joint("hips", a)
  local _, spine_turn = skeleton.joint("spine", a)
  local new_hips = qmul(qh, hips_turn)
  skeleton.set_turn("hips", skeleton.hips_parent_turn(a), new_hips, a)
  skeleton.set_turn("spine", new_hips, spine_turn, a)
  b.hip_tilt = qh
  b.jet.flare = math.max(b.jet.flare, JET.flare_time * 0.8 * w)
end

-- No legs: a jet torso (the leg bones shrunk to nothing).
local function hide_legs(m)
  for _, thigh in ipairs(m.legs) do
    local t1, t2, t3, q1, q2, q3, q4 = bone_local(skeleton.mannequin_model, m.anim.animator, thigh)
    bone_local(skeleton.mannequin_model, m.anim.animator, thigh, t1, t2, t3, q1, q2, q3, q4, 0.001, 0.001, 0.001)
  end
end

-- The toss: the serving arm lifts after the ball (up and back down over SERVE.lift_time), and the server's bat
-- lies flat while it holds the ball, easing in and out so the toss doesn't snap.
local function serve_arm(m, dt)
  local lift = 0
  if m.lift_t then
    m.lift_t = m.lift_t + dt
    if m.lift_t >= SERVE.lift_time then m.lift_t = nil else lift = math.sin(math.pi * m.lift_t / SERVE.lift_time) end
  end
  skeleton.lift_arm(m, m.serve_hand, lift)
  local holding = (game.state == "serve" or m.lift_t) and (game.server == "robot") == (m == robot)
  m.flat = lib.approach(m.flat or 0, holding and 1 or 0, 12, dt)
  skeleton.flat_bat(m, m.serve_hand, m.flat)
end

-- A mannequin each frame: its animation (mirrored if the move is, blended from the last move), its facing, the
-- flutter and the lean, the retro-burn, the hidden legs, the serving arm; then where its bat is.
local function pose_mannequin(m, dt)
  m.anim:update(dt)
  if m.body.move.mirrored then skeleton.mirror_pose(m.anim.animator) end
  skeleton.blend_pose(m, dt)
  end_follow_through(m)
  local b = m.body
  b.base = toward(b.base, facing(m, b.move), 1 - math.exp(-12 * dt))
  local roll = flutter_roll(m, dt)
  local lx, lz = lean(b, dt)
  local dash_dir, w = brake_weight(b, dt)
  local tilt = roll
  local angle = math.sqrt(lx * lx + lz * lz)
  if angle > 1e-4 then
    local ax, az = lz / angle, -lx / angle                    -- up x direction: turning about it tips up that way
    tilt = qmul({ ax * math.sin(angle / 2), 0, az * math.sin(angle / 2), math.cos(angle / 2) }, roll)
  end
  b.turn = qmul(tilt, b.base)
  b.pivot = sub(v(0, 0.9, 0), rot(tilt, v(0, 0.9, 0)))           -- tip about the hips, not the floor
  retro_burn(m, dash_dir, w)
  hide_legs(m)
  serve_arm(m, dt)
  -- Where the bat meets the ball, for play: from the upright body (the lean and the flutter are looks only, and
  -- would otherwise carry the bat off a ball the plan timed for an upright body).
  m.body.blade = add(v(b.x, height(m), b.z), rot(b.base, skeleton.blade_in_model(m.body.move.hand, m.anim.animator)))
end

-- Its body heads for its spot.
local function move_body(m, dt)
  local b = m.body
  local f_now = b.jump_t and math.floor(b.jump_t * jump_chart.fps + 0.5) + 1
  if f_now and f_now < jump_chart.land then                -- the charted jump goes straight up and down: no walking,
    b.vx, b.vz = 0, 0                                 -- only a jet's drift to line up a planned swing
    if m.swing_planned then
      local dx, dz = b.tx - b.x, b.tz - b.z
      local d = math.sqrt(dx * dx + dz * dz)
      if d > 1e-4 then
        local step = math.min(d, AIR_DRIFT_SPEED * dt)
        b.x, b.z = b.x + dx / d * step, b.z + dz / d * step
      end
    end
  else
    local nx, nz = lib.approach(b.x, b.tx, WALK_EASE, dt), lib.approach(b.z, b.tz, WALK_EASE, dt)
    local vx, vz = (nx - b.x) / dt, (nz - b.z) / dt
    if m.swing_planned then                                   -- heading for a swing's spot: straight there, full speed
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
  b.y = lib.approach(b.y, b.sink, WALK_EASE, dt)
  if b.jump_in then                                   -- a jump a plan has timed
    b.jump_in = b.jump_in - dt
    if b.jump_in <= 0 then b.jump_in = nil start_jump(m) end
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
      b.dash, b.brake = nil, { x = d.x, z = d.z, t = 0 }    -- the retro-burn carries on a moment (pose_mannequin)
      if b.goal then b.tx, b.tz = b.goal[1], b.goal[2] end   -- a plan's spot: walk the rest of the way
    end
  end
  if b.jump_t then                                     -- the charted jump: the height is read, not simulated
    local f = math.floor(b.jump_t * jump_chart.fps + 0.5) + 1      -- this step's frame (fixed steps are 1/60 s)
    -- Held in the air: while b.jet.hold (a finger held on the right side, or Space) and up off the ground, the
    -- jump waits at this frame, up to JET.hover_max a jump; let go and it carries on from here.
    b.jet.hovering = b.jet.hold and jump_chart.air[f] ~= nil and f > 4 and (b.jet.hover_t or 0) < JET.hover_max
    if b.jet.hovering then
      b.jet.hover_t = (b.jet.hover_t or 0) + dt
      b.jet.flare = math.max(b.jet.flare, JET.flare_time * 0.7)    -- the jet burns blue to hold it up
    else
      b.jump_t = b.jump_t + dt
    end
    local walking = f >= jump_chart.land and (math.abs(b.tx - b.x) > 0.01 or math.abs(b.tz - b.z) > 0.01)   -- moving on
    if f > #jump_chart.heights or walking then
      b.jump_t, b.jump_height = nil, 0                         -- the crouch eases out as it walks (b.y heads for b.sink)
      if not walking then b.y = 0 end
    else
      local off = (jump_chart.heights[f] - jump_chart.stand) * jump_chart.unit
      if off > 0 then b.jump_height, b.y = off, 0 else b.jump_height, b.y = 0, off end
    end
  end
end

-- ---------------------------------------------------------------- your touches

-- Left half: a drag walks your mannequin by however far the finger moves (start anywhere: it's a trackpad).
-- Right half: a drawn line is a swing. WASD walks on the PC too.
local MOVE_SCALE = 0.006             -- metres the body moves for each unit the finger moves

-- A swipe that no swing can meet the ball with still swings, at once, the way you swiped (a miss you can see).
local function whiff(m, line)
  if m.swing_planned or line.whiffed then return end
  line.whiffed = true
  local best, like = nil, -9
  for _, move in ipairs(skeleton.moves) do
    local l = move.swing[1] * line.right + move.swing[2] * line.up
    if l > like then best, like = move, l end
  end
  if not best then return end
  skeleton.keep_pose(m)
  m.body.move = best
  planner.play_swing(m, best.clip, 0.25)
end

local function start_swipe(line)
  if game.state == "serve" and game.server == "you" then
    -- Your serve, first line: the toss. The ball pops up off your bat.
    game.state, game.last_hitter, game.bounces = "toss", nil, { you = 0, robot = 0 }
    ball.vx, ball.vy, ball.vz = 0, SERVE.toss, 0
    you.swing_planned, you.toss_y, you.lift_t = false, ball.y, 0
  elseif game.state == "toss" and game.server == "you" and not you.swing_planned and not planner.is_planning(you) then
    -- Second line: the swing, planned onto the falling ball like any return; the charge now sets its power.
    you.aim, you.serve_power = line, math.max(0.25, meter.serve_charge())
    local star = meter.serve_star()
    you.serve_rating = star >= 1 - SERVE.perfect and "perfect" or (star >= SERVE.good and "good" or "okay")
    planner.start_plan(you, function() return planner.plan_your_swing(you, line, true) end)
  elseif game.state == "rally" then
    you.aim = line
    if game.last_hitter == "robot" then
      planner.start_plan(you, function() return planner.plan_your_swing(you, line, false) end, function(ok) if not ok then you.waiting_line = line whiff(you, line) end end)
    else
      you.waiting_line = line                      -- drawn early: it's played as soon as the robot has hit
    end
  elseif game.state == "over" then
    rally.new_game()
  end
end

local space_was = false
-- You jumped or dashed with a swing already planned: plan it again from the new move.
local function replan_your_swing()
  if game.state == "rally" and game.last_hitter == "robot" and you.aim and (you.swing_planned or planner.is_planning(you)) then
    you.swing_planned, you.waiting_line, you.retry_at = false, you.aim, nil
    planner.cancel_plan(you)
  end
end
local function read_touches(dt)
  local w, h = gfx.size()
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then
      if t.x < w / 2 then
        walk_drags[t.id] = { { sys.real_time(), t.x, t.y } }
      else
        swing_lines[t.id] = { { t.x, t.y }, start = sys.real_time() }
      end
    elseif t.phase == "move" then
      if walk_drags[t.id] then
        local d = walk_drags[t.id]
        d[#d + 1] = { sys.real_time(), t.x, t.y }
        while #d > 2 and sys.real_time() - d[1][1] > 0.1 do table.remove(d, 1) end   -- the last tenth of a second
        local b = you.body
        b.tx = lib.clamp(b.tx + t.dx * MOVE_SCALE, -COURT_X, COURT_X)   -- walked to once a jump has landed
        b.tz = lib.clamp(b.tz + t.dy * MOVE_SCALE, HALF_L + 0.25, HALF_L + 1.8)
      elseif swing_lines[t.id] then
        table.insert(swing_lines[t.id], { t.x, t.y })
      end
    elseif t.phase == "up" then
      -- Let go while the finger is still moving fast: a jump that way, whatever the angle. Otherwise just a walk.
      local d = walk_drags[t.id]
      if d and #d >= 2 then
        local a = d[1]
        local span = math.max(sys.real_time() - a[1], 1e-3)
        local vx, vy = (t.x - a[2]) / span, (a[3] - t.y) / span     -- right and up, units a second
        local speed = math.sqrt(vx * vx + vy * vy)
        if speed > FLICK_SPEED then
          if vy > math.abs(vx) then start_jump(you) else you.body.goal = nil dash(you, vx / speed, -vy / speed) end   -- up: jump; else a dash
          replan_your_swing()
        end
      end
      walk_drags[t.id] = nil
      local l = swing_lines[t.id]
      swing_lines[t.id] = nil
      if l then
        local first, final = l[1], l[#l]
        local dx, dy = final[1] - first[1], first[2] - final[2]          -- right and up, in screen units
        local length = math.sqrt(dx * dx + dy * dy)
        if length > 30 then
          start_swipe({ right = dx / length, up = dy / length, length = length })
          fading_lines[#fading_lines + 1] = { points = l, t = 0 }
        elseif game.state == "over" then
          rally.new_game()
        end
      end
    end
  end
  local space = input.key("space")
  if space and not space_was then start_jump(you) replan_your_swing() end   -- the PC's key for the charted jump
  space_was = space
  -- Holding in the air: a finger down on the right side (drawing counts), or Space held. Starting or stopping a
  -- hover changes where you'll be, so a planned swing is planned again.
  local hold = next(swing_lines) ~= nil or space
  if hold ~= (you.body.jet.hold or false) then
    you.body.jet.hold = hold
    if you.body.jump_t then replan_your_swing() end
  end
  local step = 1.4 * dt
  if input.key("a") or input.key("left") then you.body.tx = you.body.tx - step end
  if input.key("d") or input.key("right") then you.body.tx = you.body.tx + step end
  if input.key("w") or input.key("up") then you.body.tz = math.max(HALF_L + 0.25, you.body.tz - step) end
  if input.key("s") or input.key("down") then you.body.tz = math.min(HALF_L + 1.8, you.body.tz + step) end
  for i = #fading_lines, 1, -1 do
    fading_lines[i].t = fading_lines[i].t + dt
    if fading_lines[i].t > 0.5 then table.remove(fading_lines, i) end
  end
end

-- ---------------------------------------------------------------- each frame

function fixed_update(dt)
  planner.run_plans(dt)
  -- The robot plans as soon as you hit; your line drawn early is played as soon as the robot hits.
  if game.state == "rally" and game.last_hitter == "you" and robot.body.plan_for ~= game.bounces then
    robot.body.plan_for = game.bounces
    planner.start_plan(robot, function() return planner.plan_robot_swing(robot) end)
  end
  -- Your waiting swing line: planned once the hint is done (two searches at once would break the Lua budget), and
  -- if it can't meet the ball yet (you haven't jumped or dashed), tried again every CUE.retry seconds.
  if game.state == "rally" and game.last_hitter == "robot" and you.waiting_line then
    if not planner.is_planning(you) and not planner.is_planning("hint") and sys.time() >= (you.retry_at or 0) then
      you.retry_at = sys.time() + CUE.retry
      local line = you.waiting_line
      you.waiting_line = nil
      planner.start_plan(you, function() return planner.plan_your_swing(you, line, false) end, function(ok)
        if not ok and not you.waiting_line then you.waiting_line = line whiff(you, line) end
      end)
    end
  end
  move_body(robot, dt)
  move_body(you, dt)

  if game.state == "serve" then
    -- The server's ball rests on top of their bat.
    local m = game.server == "robot" and robot or you
    local p = ball_rest_on_bat(m, m.serve_hand)
    ball.x, ball.y, ball.z = p[1], p[2], p[3]
  else
    for _ = 1, 2 do rally.step_ball(dt / 2) end                -- half steps, so a fast ball can't skip the net
  end
end

function update(dt)
  budget.update()
  lib.update(dt)
  pose_mannequin(robot, dt)
  pose_mannequin(you, dt)
  for i = #sparks, 1, -1 do
    sparks[i].t = sparks[i].t + dt
    if sparks[i].t > 0.3 then table.remove(sparks, i) end
  end
  read_touches(dt)
  view.follow_camera(dt)

  game.timer = game.timer - dt
  if game.state == "serve" and game.server == "robot" and game.timer <= 0 then
    -- The robot's flick: the ball hops up off the bat, and the swing is planned onto it as it falls.
    game.state, game.last_hitter, game.bounces = "toss", nil, { you = 0, robot = 0 }
    ball.vx, ball.vy, ball.vz = 0, TOSS, 0
    robot.lift_t = 0
    planner.start_plan(robot, function() return planner.plan_robot_swing(robot, robot.serve_hand) end)
  elseif game.state == "point" and game.timer <= 0 then
    rally.start_serve()
  end
  view.update_trail(dt)
end

function init()
  rally.new_game()
end

function draw()
  view.draw_world()
  view.draw_ui()
end
