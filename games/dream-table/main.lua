-- Dream Table: table tennis against the computer.
-- Hold the mouse button (or a finger) and move to steer your paddle; WASD or the arrow keys work too.
-- Swing faster for a faster shot; where the ball meets the paddle sets the angle.
-- Rules: first to 11, win by 2, serve changes every 2 points (every point from 10-10).
-- ponytail: serves go straight to the other side (no first bounce on your own side) and there is no spin.

local lib = include("lib.lua")

-- The real table, in metres: 2.74 long (along z), 1.525 wide, 0.76 high, net 15.25 cm.
local TABLE_Y, HALF_W, HALF_L, NET_TOP = 0.76, 0.7625, 1.37, 0.76 + 0.1525
local R = 0.03                        -- ball radius (a real one is 2 cm; bigger reads better on screen)
local G = 7.5                         -- a little floatier than real gravity: it's a dream
local BOUNCE = 0.86                   -- how much height a bounce keeps
local PADDLE_Z, AI_Z = 1.55, -1.55    -- the planes each paddle moves in
local REACH, AI_REACH = 0.22, 0.17    -- how far from a paddle's centre the ball can be and still be hit
local AI_SPEED = 1.9                  -- the computer's top paddle speed, metres a second
local ASSIST = 0.45                   -- how much your paddle drifts towards an incoming ball (0 to 1)

local ball = { x = 0, y = 1, z = 0, vx = 0, vy = 0, vz = 0 }
local me = { x = 0, y = TABLE_Y + 0.25, vx = 0, vy = 0, tx = 0, ty = TABLE_Y + 0.25 }
local cpu = { x = 0, y = TABLE_Y + 0.25 }
local score = { player = 0, cpu = 0 }
local wins = save.get("wins", 0)
local font = gfx.load_font("MyDotFont.ttf", 64)   -- your own dot font, for every word on screen
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

-- state: "serve" (someone holds the ball), "rally", "point" (a short pause), "over"
local state, server, last, bounces = "serve", "player", nil, {}
local message, message_time, timer = "", 0, 0
local key_was_down = false

-- ---------------------------------------------------------------- the opponent: the wooden mannequin
-- It plays the shots you recorded with the mocap game. mannequin.glb is made by tools/blender_mannequin.py from
-- captures/shots-<n>-<k>-<shot>-sam3d.npz, one clip per shot, each with the hit CONTACT seconds in.
-- Every clip also plays mirror-imaged with the bat in the other hand, so 11 recordings give 22 moves.
-- When you hit the ball it plans the whole return: along the ball's flight it finds where each move's bat could
-- meet it, picks one (the height should suit the shot, it should not have to run far, and it likes variety),
-- starts that clip so the recorded hit lands when the ball gets there, and walks to the spot. The ball is only
-- returned if it really touches the bat: miss, and it's your point.
-- Feet: planted feet stay put while the body moves, and step (one at a time) when they fall too far behind;
-- steps the animation makes itself are kept as they are.
local dummy = model.load("mannequin.glb")
local bat_model = model.load("bat.glb")
local anim = lib.animation(dummy)
local CONTACT = 2.0                  -- seconds into every shot clip that the ball is hit (tools/split_shots.py)
-- The middle of each bat's blade, in the bat's own frame (tools/blender_bats.py: along the hand, palm side).
local BLADE = { L = { 0.273, -0.003, -0.033 }, R = { -0.273, -0.003, -0.033 } }
local HIT_RANGE = 0.13               -- the ball's centre this close to the blade's: the bat meets it
local STEP_LENGTH, STEP_TIME, STEP_HEIGHT = 0.16, 0.2, 0.07   -- a foot this far behind steps, taking this long, this high
local LIFTED = 0.04                  -- an ankle the animation raises this far is the animation's own step: leave it
local WALK = 7                       -- how quickly the body heads for its spot (bigger is quicker)
local CROUCH = 0.3                   -- how far the body may sink (knees bending) to play a high shot on a lower ball
local moves, hands, bones, legs, effects = {}, {}, {}, {}, {}
local rest = {}                      -- every bone's turn in the T-pose
local hips_parent
local TOSS = 2.6                      -- how fast the flick sends the ball up to be served (metres a second)
local body = { serve_hand = "R", x = 0, y = 0, ty = 0, z = AI_Z - 0.6, tx = 0, tz = AI_Z - 0.6, vx = 0, vz = 0, turn = { 0, 0, 0, 1 },
               move = nil, plan_for = nil, blade = { 0, 1, AI_Z } }

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

-- A bone's place and turn inside the model, as animator a has it now.
local function joint(name, a)
  local x, y, z, qx, qy, qz, qw = model.joint(dummy, a or anim.animator, name)
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

-- From turn a part of the way to turn b, the short way round.
local function toward(a, b, t)
  if a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] < 0 then b = { -b[1], -b[2], -b[3], -b[4] } end
  local q = { lib.lerp(a[1], b[1], t), lib.lerp(a[2], b[2], t), lib.lerp(a[3], b[3], t), lib.lerp(a[4], b[4], t) }
  local l = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)
  return { q[1] / l, q[2] / l, q[3] / l, q[4] / l }
end

-- Switching moves: the pose it was in is kept, and every bone blends from it into the new move over BLEND_TIME.
-- This works between a mirrored and an unmirrored move too, which an animation cross-fade can't.
local BLEND_TIME = 0.18
local blend = { from = nil, t = 1 }
local function keep_pose()
  local turns = {}
  for _, b in ipairs(bones) do turns[b.name] = select(2, joint(b.name)) end
  blend.from, blend.t = { turns = turns, hips = (joint("hips")) }, 0
end
local function blend_pose(dt)
  if not blend.from then return end
  blend.t = blend.t + dt / BLEND_TIME
  if blend.t >= 1 then blend.from = nil return end
  local w = blend.t * blend.t * (3 - 2 * blend.t)
  local turns = {}
  for _, b in ipairs(bones) do turns[b.name] = toward(blend.from.turns[b.name], select(2, joint(b.name)), w) end
  local hips = joint("hips")
  write_pose(turns, add(blend.from.hips, mul(sub(hips, blend.from.hips), w)), anim.animator)
end

-- Where the middle of a hand's bat blade is inside the model.
local function blade_in_model(side, a)
  local wrist, turn = joint("hand." .. side, a)
  return add(wrist, rot(qmul(turn, hands[side].offset), BLADE[side]))
end

local function to_world(p) return add(v(body.x, body.y, body.z), rot(body.turn, p)) end
local function to_model(p) return rot(qinv(body.turn), sub(p, v(body.x, body.y, body.z))) end

-- Reads every shot clip once: which shot, which way it faces, where the blade is at the hit; and its mirror image.
local function learn_moves()
  moves, bones, legs, hands = {}, {}, {}, {}
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
    local ankle = joint("foot." .. side, 0)
    legs[#legs + 1] = { thigh = "thigh." .. side, shin = "shin." .. side, foot = "foot." .. side, rest = ankle[2] }
  end

  local scratch = model.animator(dummy)
  for _, clip in ipairs(model.clips(dummy)) do
    local side, kind = clip.name:match("%-(%a%a)%-(%a+)$")
    if side == "fh" or side == "bh" then
      model.pose(dummy, scratch, clip.name, 0, 1)
      local left, right = joint("thigh.L", scratch), joint("thigh.R", scratch)
      local fx, fz = -(left[3] - right[3]), left[1] - right[1]   -- facing, from the body's left (+x faces +z)
      model.pose(dummy, scratch, clip.name, math.min(CONTACT, clip.duration), 1)
      local blade = blade_in_model("R", scratch)
      for _, mirrored in ipairs({ false, true }) do
        local turn = yaw_turn(-math.atan(mirrored and -fx or fx, fz))   -- turned to face +z, the player's end
        local b = mirrored and v(-blade[1], blade[2], blade[3]) or blade
        moves[#moves + 1] = { clip = clip.name, kind = kind, side = side, mirrored = mirrored, turn = turn,
                              hand = mirrored and "L" or "R", blade = rot(turn, b), used = 0 }
      end
    end
  end
  if moves[1] then
    body.move, body.turn = moves[1], moves[1].turn
    anim:play(moves[1].clip, { loop = false, speed = 0, time = 0.3, fade = 0 })   -- standing ready
  end
end

-- The ball's flight from now on: the points where the opponent may hit it (after one bounce on its side,
-- off the end of the table), with how long until it gets there.
local function flight(serving)
  local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
  local own, dt, out = bounces.cpu or 0, 1 / 120, {}
  for step = 1, 300 do
    vy = vy - G * dt
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if y - R <= TABLE_Y and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      y, vy, vx, vz = TABLE_Y + R, -vy * BOUNCE, vx * 0.97, vz * 0.97
      if z < 0 then own = own + 1 end
    end
    if y < R or own >= 2 then break end
    local hittable = serving and (vy < 0 and own == 0) or (not serving and own == 1 and z < -HALF_L + 0.2)
    if hittable and step % 2 == 0 then out[#out + 1] = { x = x, y = y, z = z, t = step * dt } end
  end
  return out
end

-- Plans the return of the ball you just hit (or its own serve, with that hand): which move, where to stand, when
-- to start the swing.
local function plan(serve_hand)
  local points = flight(serve_hand ~= nil)
  local best, at, best_cost, best_sink = nil, nil, math.huge, 0
  for _, m in ipairs(moves) do
    for _, p in ipairs(points) do
      local sx, sz = p.x - m.blade[1], p.z - m.blade[3]          -- where the body would stand
      local ok = sz < -HALF_L - 0.2 and p.t > 0.3                -- behind the table, with time to swing
      if serve_hand then ok = ok and m.hand == serve_hand end
      if ok then
        local run = math.sqrt((sx - body.x) ^ 2 + (sz - body.z) ^ 2)
        local sink = math.max(-CROUCH, math.min(0, p.y - m.blade[2]))   -- crouch for a ball below the shot's height
        local cost = math.abs(p.y - m.blade[2] - sink) * 3 - sink * 0.3 + run * (serve_hand and 1.5 or 0.3)
                     + m.used * 0.35 + math.random() * 0.05
        if cost < best_cost then best, at, best_cost, best_sink = m, p, cost, sink end
      end
    end
  end
  if not best then return end
  best.used = best.used + 1
  keep_pose()
  body.move = best
  body.tx, body.tz, body.ty = at.x - best.blade[1], at.z - best.blade[3], best_sink
  anim.layers = {}
  anim:play(best.clip, { loop = false, restart = true, fade = 0, time = math.max(0, CONTACT - at.t),
                         speed = at.t > CONTACT and CONTACT / at.t or 1 })
end

-- Two-bone IK: thigh and shin turn so the ankle reaches target (inside the model); the foot keeps its turn.
local function reach(leg, target)
  local hip, thigh_turn = joint(leg.thigh)
  local knee, shin_turn = joint(leg.shin)
  local ankle, foot_turn = joint(leg.foot)
  local _, hips_turn = joint("hips")
  local a, b = len(sub(knee, hip)), len(sub(ankle, knee))
  local to = sub(target, hip)
  local d = math.min(math.max(len(to), math.abs(a - b) + 1e-3), a + b - 1e-3)
  local dir = unit(to)
  -- Which way the knee bends: the animation's knee, plus a steady pull toward the way the body faces, so a nearly
  -- straight leg (whose own bend direction is tiny) can't flip its knee from one frame to the next.
  local function across(p) return sub(p, mul(dir, dot(p, dir))) end
  local forward = rot(qmul(hips_turn, qinv(rest.hips)), v(0, 0, 1))
  local bend = unit(add(across(sub(knee, hip)), mul(unit(across(forward)), 0.08)))
  local c = math.max(-1, math.min(1, (a * a + d * d - b * b) / (2 * a * d)))
  local new_knee = add(add(hip, mul(dir, a * c)), mul(bend, a * math.sqrt(1 - c * c)))
  local new_ankle = add(hip, mul(dir, d))
  local turn_thigh = from_to(sub(knee, hip), sub(new_knee, hip))
  local new_thigh = qmul(turn_thigh, thigh_turn)
  local new_shin = qmul(from_to(rot(turn_thigh, sub(ankle, knee)), sub(new_ankle, new_knee)), qmul(turn_thigh, shin_turn))
  set_turn(leg.thigh, hips_turn, new_thigh, anim.animator)
  set_turn(leg.shin, new_thigh, new_shin, anim.animator)
  set_turn(leg.foot, new_shin, foot_turn, anim.animator)
end

-- Planted feet stay put while the body moves; one at a time, a foot left too far behind steps.
local function feet(dt)
  for i, leg in ipairs(legs) do
    local wanted = to_world((joint(leg.foot)))
    wanted[2] = wanted[2] - body.y            -- the animation's own ankle height off the ground: crouching bends the knees
    local other = legs[3 - i]
    leg.plant = leg.plant or wanted
    local target
    if wanted[2] - leg.rest > LIFTED then                         -- the animation's own step: follow it
      leg.plant, leg.step, target = wanted, nil, wanted
    else
      if not leg.step and not other.step and
         math.sqrt((wanted[1] - leg.plant[1]) ^ 2 + (wanted[3] - leg.plant[3]) ^ 2) > STEP_LENGTH then
        leg.step = { from = leg.plant, to = v(wanted[1] + body.vx * 0.12, wanted[2], wanted[3] + body.vz * 0.12), t = 0 }
      end
      if leg.step then
        leg.step.t = math.min(1, leg.step.t + dt / STEP_TIME)
        local s = leg.step.t * leg.step.t * (3 - 2 * leg.step.t)
        target = add(leg.step.from, mul(sub(leg.step.to, leg.step.from), s))
        target[2] = wanted[2] + math.sin(math.pi * leg.step.t) * STEP_HEIGHT
        if leg.step.t >= 1 then leg.plant, leg.step = leg.step.to, nil end
      else
        target = v(leg.plant[1], wanted[2], leg.plant[3])
      end
    end
    -- Eased, so a foot never jumps when it changes between planted, stepping and following the animation.
    leg.at = leg.at and add(leg.at, mul(sub(target, leg.at), 1 - math.exp(-30 * dt))) or target
    reach(leg, to_model(leg.at))
  end
end

-- A little burst where bat and ball meet.
-- Where the top of a bat's blade is in the world, for the ball to rest on.
local function on_bat(side)
  local b = to_world(blade_in_model(side, anim.animator))
  return v(b[1], b[2] + R + 0.012, b[3])
end

local function spark(x, y, z) effects[#effects + 1] = { x = x, y = y, z = z, t = 0 } end

-- The mannequin, each frame after the animation: mirror if the move is mirrored, feet, then where its bat is.
local function pose_opponent(dt)
  if #moves == 0 then return end
  if body.move.mirrored then mirror_pose(anim.animator) end
  blend_pose(dt)
  body.turn = toward(body.turn, body.move.turn, 1 - math.exp(-12 * dt))
  feet(dt)
  body.blade = to_world(blade_in_model(body.move.hand, anim.animator))
end

local function side_of(z) return z > 0 and "player" or "cpu" end
local function other(who) return who == "player" and "cpu" or "player" end
local function say(text) message, message_time = text, sys.real_time() end

local function camera()
  gfx.camera(0, 3.0, 4.4, 0, 0.5, -0.8, 60)   -- portrait (game.cfg): far and high enough to see the whole table
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

local function hit(by, vx, vy, vz)
  ball.vx, ball.vy, ball.vz = vx, vy, vz
  last, bounces = by, { player = 0, cpu = 0 }
  state = "rally"
  if by == "player" then input.haptic("light") end
end

local function player_hit()
  local speed = math.sqrt(me.vx * me.vx + me.vy * me.vy)
  local power = lib.clamp(speed / 2.5, 0, 1)
  -- Where the ball meets the paddle angles the shot; the paddle's sideways swing adds to it.
  local tx = lib.clamp(ball.x + (ball.x - me.x) * 2.5 + me.vx * 0.15, -HALF_W + 0.1, HALF_W - 0.1)
  local tz = -(0.45 + 0.75 * power)
  hit("player", shot(ball.x, ball.y, ball.z, tx, tz, 0.72 - 0.32 * power))
  if power > 0.8 then say("SMASH!") end
end

-- The computer is a polite robot that can't lose: every return (and serve) is a gentle ball landing mid-table on
-- your half, on your paddle's side, never fast and never out.
local function cpu_hit()
  local tx = lib.clamp(me.x * 0.6 + (math.random() - 0.5) * 0.2, -(HALF_W - 0.25), HALF_W - 0.25)
  hit("cpu", shot(ball.x, ball.y, ball.z, tx, 0.7 + math.random() * 0.2, 0.8))
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
  trail = {}
end

local function new_game()
  score = { player = 0, cpu = 0 }
  start_serve()
  say("FIRST TO 11")
end

function init()
  learn_moves()
  new_game()
end

-- ---------------------------------------------------------------- the ball

-- Where the ball will cross the plane z = plane, following its flight and table bounces. nil if it won't.
local function predict(plane)
  local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
  local dt = 1 / 120
  for step = 1, 360 do
    vy = vy - G * dt
    local nz = z + vz * dt
    if (z - plane) * (nz - plane) <= 0 then return x, y, step * dt end
    x, y, z = x + vx * dt, y + vy * dt, nz
    if y - R <= TABLE_Y and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      y, vy = TABLE_Y + R, -vy * BOUNCE
    end
  end
end

local function step_ball(dt)
  local px, py, pz = ball.x, ball.y, ball.z
  ball.vy = ball.vy - G * dt
  ball.x, ball.y, ball.z = ball.x + ball.vx * dt, ball.y + ball.vy * dt, ball.z + ball.vz * dt

  -- The net: below its top, the ball stops and drops back.
  if pz * ball.z <= 0 and pz ~= ball.z and ball.y - R < NET_TOP and math.abs(ball.x) < HALF_W + 0.15 then
    ball.z = pz > 0 and R or -R
    ball.vz, ball.vx = -ball.vz * 0.15, ball.vx * 0.4
    if state == "rally" then say("NET") end
  end

  -- The table.
  if ball.y - R <= TABLE_Y and ball.vy < 0 and py - R >= TABLE_Y - 0.02 and math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L then
    ball.y, ball.vy = TABLE_Y + R, -ball.vy * BOUNCE
    ball.vx, ball.vz = ball.vx * 0.97, ball.vz * 0.97
    local side = side_of(ball.z)
    if state == "rally" then
      if side == side_of(last == "player" and 1 or -1) then
        award(other(last), "it bounced on the hitter's own side")
      else
        bounces[side] = bounces[side] + 1
        if bounces[side] >= 2 and side == "cpu" and #moves > 0 then
          spark(ball.x, ball.y, ball.z) cpu_hit()    -- it can't lose: a short ball is still returned
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

  -- The mannequin's serve: the tossed ball is hit when it meets the bat on the way down; dropped, it tries again.
  if state == "toss" then
    if ball.vy < 0 and len(sub(v(ball.x, ball.y, ball.z), body.blade)) <= HIT_RANGE then
      spark(ball.x, ball.y, ball.z)
      cpu_hit()
      body.ty = 0
      body.serve_hand = body.serve_hand == "R" and "L" or "R"   -- next serve with the other hand
    elseif ball.y - R <= 0.02 then
      start_serve()
    end
  end

  -- The paddles. A paddle only plays a ball that has bounced once on its own side.
  if state == "rally" then
    if last == "cpu" and bounces.player == 1 and pz < PADDLE_Z and ball.z >= PADDLE_Z then
      local dx, dy = ball.x - me.x, ball.y - me.y
      if dx * dx + dy * dy <= REACH * REACH then ball.z = PADDLE_Z - 0.01 spark(ball.x, ball.y, ball.z) player_hit() end
    elseif last == "player" and bounces.cpu == 1 and #moves > 0 then
      -- The mannequin returns it only if its bat really touches it.
      -- Its bat should meet the ball; if the swing ever misses, it still returns it before it reaches the floor.
      local rescue = ball.vy < 0 and ball.y < 0.35
      if len(sub(v(ball.x, ball.y, ball.z), body.blade)) <= HIT_RANGE or rescue then
        spark(ball.x, ball.y, ball.z) cpu_hit() body.ty = 0
      end
    elseif last == "player" and bounces.cpu == 1 and pz > AI_Z and ball.z <= AI_Z then
      local dx, dy = ball.x - cpu.x, ball.y - cpu.y
      if dx * dx + dy * dy <= AI_REACH * AI_REACH then ball.z = AI_Z + 0.01 cpu_hit() end
    end
  end
end

-- ---------------------------------------------------------------- each frame

function fixed_update(dt)
  -- Your paddle: towards where you point, drawn a little towards the incoming ball.
  local tx, ty = me.tx, me.ty
  if state == "rally" and last == "cpu" and ball.vz > 0 and ball.z > 0 then
    local bx, by = predict(PADDLE_Z)
    if bx then tx, ty = lib.lerp(tx, bx, ASSIST), lib.lerp(ty, by, ASSIST) end
  end
  local nx, ny = lib.approach(me.x, tx, 22, dt), lib.approach(me.y, ty, 22, dt)
  me.vx, me.vy = lib.lerp(me.vx, (nx - me.x) / dt, 0.5), lib.lerp(me.vy, (ny - me.y) / dt, 0.5)
  me.x, me.y = nx, ny

  -- The computer: heads for where the ball will cross its plane, no faster than AI_SPEED.
  local gx, gy = 0, TABLE_Y + 0.25
  if state == "rally" and last == "player" and ball.vz < 0 and ball.z < 0 then   -- it reads the ball once it crosses the net
    local bx, by = predict(AI_Z)
    if bx then gx, gy = bx, by end
  elseif state == "serve" and server == "cpu" then
    gx, gy = 0.2, TABLE_Y + 0.22
  end
  -- The mannequin: as soon as you hit, it plans the return and heads for its spot.
  if state == "rally" and last == "player" and body.plan_for ~= bounces and #moves > 0 then
    body.plan_for = bounces   -- one plan per ball you hit
    plan()
  end
  local nx, nz = lib.approach(body.x, body.tx, WALK, dt), lib.approach(body.z, body.tz, WALK, dt)
  body.vx, body.vz = (nx - body.x) / dt, (nz - body.z) / dt
  body.x, body.z = nx, nz
  body.y = lib.approach(body.y, body.ty, WALK, dt)

  local dx, dy = gx - cpu.x, gy - cpu.y
  local far = math.sqrt(dx * dx + dy * dy)
  local reach = AI_SPEED * dt
  if far > reach then dx, dy = dx / far * reach, dy / far * reach end
  cpu.x, cpu.y = cpu.x + dx, cpu.y + dy

  if state == "serve" then
    -- The server holds the ball just in front of their paddle.
    local p, toward = server == "player" and me or cpu, server == "player" and -1 or 1
    ball.x, ball.y = p.x + 0.04, p.y + 0.14 + math.sin(sys.real_time() * 4) * 0.02
    ball.z = (server == "player" and PADDLE_Z or AI_Z) + toward * 0.06
    if server == "cpu" and #moves > 0 then   -- balanced on top of the mannequin's serving bat
      local p = on_bat(body.serve_hand)
      ball.x, ball.y, ball.z = p[1], p[2], p[3]
    end
  else
    for _ = 1, 2 do step_ball(dt / 2) end                -- half steps, so a fast ball can't skip the net
  end
end

function update(dt)
  lib.update(dt)
  anim:update(dt)
  pose_opponent(dt)
  for i = #effects, 1, -1 do
    effects[i].t = effects[i].t + dt
    if effects[i].t > 0.3 then table.remove(effects, i) end
  end
  camera()   -- so gfx.unproject below uses this camera

  -- Pointing: hold the mouse button (or a finger) and move. The paddle follows the point on its plane.
  local pressed = false
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then pressed = true end
    local ox, oy, oz, rx, ry, rz = gfx.unproject(t.x, t.y)
    if rz < -1e-4 then
      local s = (PADDLE_Z - oz) / rz
      me.tx, me.ty = ox + rx * s, oy + ry * s
    end
  end
  local speed = 1.8 * dt
  if input.key("a") or input.key("left") then me.tx = me.tx - speed end
  if input.key("d") or input.key("right") then me.tx = me.tx + speed end
  if input.key("w") or input.key("up") then me.ty = me.ty + speed end
  if input.key("s") or input.key("down") then me.ty = me.ty - speed end
  me.tx, me.ty = lib.clamp(me.tx, -1.3, 1.3), lib.clamp(me.ty, TABLE_Y + 0.02, TABLE_Y + 0.9)
  local key = input.key("space")
  pressed = pressed or (key and not key_was_down)
  key_was_down = key

  timer = timer - dt
  if state == "serve" and server == "player" and pressed then
    local tx = lib.clamp(-me.x * 0.6 + (math.random() - 0.5) * 0.6, -0.6, 0.6)
    hit("player", shot(ball.x, ball.y, ball.z, tx, -0.75 - math.random() * 0.4, 0.7))
  elseif state == "serve" and server == "cpu" and timer <= 0 and #moves > 0 then
    -- The flick: the ball hops up off the bat, and the swing is planned onto it as it falls.
    state, last, bounces = "toss", nil, { player = 0, cpu = 0 }
    ball.vx, ball.vy, ball.vz = 0, TOSS, 0
    plan(body.serve_hand)
  elseif state == "serve" and server == "cpu" and timer <= 0 then
    hit("cpu", shot(ball.x, ball.y, ball.z, (math.random() * 2 - 1) * 0.55, 0.6 + math.random() * 0.6, 0.7))
  elseif state == "point" and timer <= 0 then
    start_serve()
  elseif state == "over" and pressed then
    new_game()
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

local function paddle(x, y, z, r, g, b, a)
  gfx.cube(x, y - 0.13, z, 0.03, 0.1, 0.02, 0.35, 0.22, 0.14, 0, a)                              -- handle
  gfx.draw(gfx.SPHERE, x, y, z, 0, 0, 0, 1, 0.17, 0.17, 0.025, r, g, b, a)                      -- blade
end

function draw()
  gfx.clear(0.05, 0.03, 0.11)
  camera()
  gfx.fog(0.05, 0.03, 0.11, 5, 13)
  gfx.light(-0.35, -1, -0.25, 1.0, 0.95, 0.9)
  gfx.ambient(0.34, 0.31, 0.46)
  gfx.shadows(0, TABLE_Y, 0, 2.3, 0.65)
  gfx.point_light(0, 2.2, 0, 4.5, 0.55, 0.35, 0.9)

  -- Floor, table, lines, legs.
  gfx.plane(0, 0, 0, 16, 16, 0.14, 0.10, 0.22)
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

  -- The mannequin with a taped bat in each hand.
  gfx.blend("opaque")
  if #moves > 0 then
    local q = body.turn
    model.draw(dummy, body.x, body.y, body.z, q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1, anim.animator)
    for _, hand in pairs(hands) do
      local wrist, turn = joint(hand.bone)
      local at, t = to_world(wrist), qmul(qmul(q, turn), hand.offset)
      model.draw(bat_model, at[1], at[2], at[3], t[1], t[2], t[3], t[4], 1, 1, 1, 1, 1, 1, 1, nil, hand.node)
    end
  end
  gfx.blend("alpha")
  -- Paddles: the computer's in blue, yours in red and slightly see-through so it never hides the ball.
  if #moves == 0 then paddle(cpu.x, cpu.y, AI_Z, 0.2, 0.45, 1.0, 1) end   -- no mannequin clips: the old blue paddle
  paddle(me.x, me.y, PADDLE_Z, 1.0, 0.25, 0.3, 0.8)
  gfx.blend("opaque")

  -- The ball: a real lit ball, its shadow straight below it on the table or the floor (always, so you can read
  -- where it is), the bursts where bat meets ball, and a fading trail.
  gfx.sphere(ball.x, ball.y, ball.z, R * 2, 1.0, 0.93, 0.8)
  local over = math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L and ball.y >= TABLE_Y
  local floor = over and TABLE_Y + 0.003 or 0.003
  local size = R * 3.2 / (1 + (ball.y - floor) * 0.4)
  gfx.unlit(true)
  gfx.blend("alpha")
  gfx.draw(gfx.SPHERE, ball.x, floor, ball.z, 0, 0, 0, 1, size, 0.002, size, 0, 0, 0, 0.75)
  for _, e in ipairs(effects) do
    local age = e.t / 0.3
    local ring = 0.04 + 0.22 * age
    for k = 0, 11 do
      local a = k / 12 * math.pi * 2
      gfx.sphere(e.x + math.cos(a) * ring, e.y + math.sin(a) * ring, e.z, 0.024 * (1 - age), 1, 0.9, 0.5, 1 - age)
    end
    gfx.sphere(e.x, e.y, e.z, 0.16 * (1 - age), 1, 1, 0.85, 0.7 * (1 - age))
  end
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

  -- Score and messages, in your dot font.
  gfx.font(font)
  local w, h = gfx.size()
  local left, top, right = gfx.safe()
  gfx.text("DREAM TABLE", left + 80, top + 20, 26, 0.85, 0.75, 1)
  local board = string.format("YOU %d  -  %d CPU", score.player, score.cpu)
  gfx.text(board, w - right - gfx.text_width(board, 30) - 24, top + 20, 30, 1, 1, 1)
  local serving = (state == "serve" or state == "point") and (serve_turn() == "player" and "YOUR SERVE" or "CPU SERVES") or ""
  gfx.text(serving, w - right - gfx.text_width(serving, 18) - 24, top + 60, 18, 0.7, 0.75, 0.9)
  if wins > 0 then gfx.text("WINS " .. wins, left + 80, top + 56, 18, 0.7, 0.75, 0.9) end

  local center = nil
  if state == "serve" and server == "player" then center = "CLICK OR SPACE TO SERVE" end
  if state == "over" then center = message .. "   CLICK TO PLAY AGAIN" end
  if center then gfx.text(center, (w - gfx.text_width(center, 26)) / 2, h * 0.2, 26, 1, 1, 1) end
  if state ~= "over" and sys.real_time() - message_time < 1.6 then
    gfx.text(message, (w - gfx.text_width(message, 30)) / 2, h * 0.14, 30, 1, 0.85, 0.55)
  end
  gfx.text("HOLD THE MOUSE AND MOVE   -   WASD", left + 80, h - 60, 16, 0.55, 0.55, 0.7)
end
