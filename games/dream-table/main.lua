-- Dream Table: table tennis, wooden mannequin against wooden mannequin.
-- You are the near mannequin. Hold and drag on the left half of the screen to walk it: it follows your finger's
-- movement, wherever you start (a trackpad, not a joystick). Draw a line on the right half to swing: the line's
-- direction picks the stroke from the recorded shots (the way the bat travelled when it was recorded), and its
-- direction and length aim the ball and set its pace. Draw it any time before the ball arrives; the mannequin
-- times the swing and steps to meet the ball. A quick flick up on the left half jumps.
-- Your serve, as in real table tennis: the first line tosses the ball up off your bat; the second is the swing, and
-- you time it: the bat meets the ball where it will be SWING_LEAD seconds after you finish drawing. The serve must
-- bounce on your side, then on the other. Miss it and it's the robot's point.
-- The far mannequin is a polite robot that can't lose.
-- Rules: first to 11, win by 2, serve changes every 2 points (every point from 10-10).
-- ponytail: serves go straight to the other side (no first bounce on your own side) and there is no spin.

local lib = include("lib.lua")

-- The real table, in metres: 2.74 long (along z), 1.525 wide, 0.76 high, net 15.25 cm.
local TABLE_Y, HALF_W, HALF_L, NET_TOP = 0.76, 0.7625, 1.37, 0.76 + 0.1525
local R = 0.03                        -- ball radius (a real one is 2 cm; bigger reads better on screen)
local G = 7.5                         -- a little floatier than real gravity: it's a dream
local BOUNCE = 0.86                   -- how much height a bounce keeps

local ball = { x = 0, y = 1, z = 0, vx = 0, vy = 0, vz = 0 }
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
local CONTACT = 2.0                  -- seconds into every shot clip that the ball is hit (tools/split_shots.py)
-- The middle of each bat's blade, in the bat's own frame (tools/blender_bats.py: along the hand, palm side).
local BLADE = { L = { 0.273, -0.003, -0.033 }, R = { -0.273, -0.003, -0.033 } }
local HIT_RANGE = 0.13               -- the ball's centre this close to the blade's: the bat meets it
local STEP_LENGTH, STEP_TIME, STEP_HEIGHT = 0.16, 0.16, 0.07  -- a foot this far behind steps, taking this long, this high
local TOP_SPEED = 2.2                -- metres a second: the body can't outrun its feet
local CARIOCA_SPEED = 0.6            -- moving sideways faster than this, the trailing foot crosses over (carioca)
local CROSS_PAST, CROSS_DEPTH = 0.08, 0.09   -- how far past the leading foot it lands, and in front of or behind it
local JUMP_SPEED, GRAVITY = 3.2, 9.8 -- a jump leaves the ground this fast (about half a metre high)
local SWING_LEAD = 0.32              -- your serve swing meets the ball this long after you finish the line
local LIFTED = 0.07                  -- an ankle the animation raises this far above the other is its own step: leave it
local WALK = 7                       -- how quickly a body heads for its spot (bigger is quicker)
local CROUCH = 0.3                   -- how far a body may sink (knees bending) to play a high shot on a lower ball
local TOSS = 2.6                     -- how fast the flick sends the ball up to be served (metres a second)
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
  m.body = { x = 0, y = 0, ty = 0, drop = 0, air = 0, jv = 0, z = z, tx = 0, tz = z, vx = 0, vz = 0, turn = m.face, move = moves[1], plan_for = nil,
             blade = v(0, 1, z) }
  for _, s in ipairs({ "L", "R" }) do
    local ankle = joint("foot." .. s, 0)
    m.legs[#m.legs + 1] = { thigh = "thigh." .. s, shin = "shin." .. s, foot = "foot." .. s, rest = ankle[2] }
  end
  if moves[1] then m.anim:play(moves[1].clip, { loop = false, speed = 0, time = 0.3, fade = 0 }) end   -- ready
  return m
end

local robot, you

-- body.drop lowers the whole body so its lowest foot is on the floor (the recordings hold the hips a little high).
local function to_world(m, p) return add(v(m.body.x, m.body.y - m.body.drop + m.body.air, m.body.z), rot(m.body.turn, p)) end
local function to_model(m, p) return rot(qinv(m.body.turn), sub(p, v(m.body.x, m.body.y - m.body.drop + m.body.air, m.body.z))) end
local function facing(m, move) return qmul(m.face, move.turn) end
local function blade_offset(m, move) return rot(m.face, move.blade) end

-- Switching moves: the pose it was in is kept, and every bone blends from it into the new move over BLEND_TIME.
-- This works between a mirrored and an unmirrored move too, which an animation cross-fade can't.
local function keep_pose(m)
  local turns = {}
  for _, b in ipairs(bones) do turns[b.name] = select(2, joint(b.name, m.anim.animator)) end
  m.blend.from, m.blend.t = { turns = turns, hips = (joint("hips", m.anim.animator)) }, 0
end

local function blend_pose(m, dt)
  local blend = m.blend
  if not blend.from then return end
  blend.t = blend.t + dt / BLEND_TIME
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
    vy = vy - G * dt
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if y - R <= TABLE_Y and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      y, vy, vx, vz = TABLE_Y + R, -vy * BOUNCE, vx * 0.97, vz * 0.97
      if z * m.sign > 0 then own = own + 1 end
    end
    if y < R or own >= 2 then break end
    local hittable = serving and (vy < 0 and own == 0) or (not serving and own == 1 and z * m.sign > HALF_L - 0.2)
    if hittable and step % 2 == 0 then out[#out + 1] = { x = x, y = y, z = z, t = step * dt } end
  end
  return out
end

-- Picks the best (move, point) by cost(move, point, sink, run); then starts it.
local function choose_and_swing(m, points, cost_of)
  local best, at, best_cost, best_sink = nil, nil, math.huge, 0
  for _, move in ipairs(moves) do
    local off = blade_offset(m, move)
    for _, p in ipairs(points) do
      local sx, sz = p.x - off[1], p.z - off[3]                  -- where the body would stand
      if sz * m.sign > HALF_L + 0.2 and p.t > 0.3 then           -- behind the table, with time to swing
        local run = math.sqrt((sx - m.body.x) ^ 2 + (sz - m.body.z) ^ 2)
        local sink = math.max(-CROUCH, math.min(0, p.y - off[2]))   -- crouch for a ball below the shot's height
        local cost = cost_of(move, p, sink, run)
        if cost and cost < best_cost then best, at, best_cost, best_sink = move, p, cost, sink end
      end
    end
  end
  if not best then return false end
  m.used[best] = (m.used[best] or 0) + 1
  keep_pose(m)
  m.body.move, m.armed = best, true
  local off = blade_offset(m, best)
  m.body.tx, m.body.tz, m.body.ty = at.x - off[1], at.z - off[3], best_sink
  m.anim.layers = {}
  m.anim:play(best.clip, { loop = false, restart = true, fade = 0, time = math.max(0, CONTACT - at.t),
                           speed = at.t > CONTACT and CONTACT / at.t or 1 })
  return true
end

-- The robot's choice: the height should suit the shot, it shouldn't run far, and it likes variety.
local function plan_robot(m, serve_hand)
  return choose_and_swing(m, flight(m, serve_hand ~= nil), function(move, p, sink, run)
    if serve_hand and move.hand ~= serve_hand then return nil end
    return math.abs(p.y - blade_offset(m, move)[2] - sink) * 3 - sink * 0.3 + run * (serve_hand and 1.5 or 0.3)
           + (m.used[move] or 0) * 0.35 + math.random() * 0.05
  end)
end

-- Your choice, from the line you drew: the move whose recorded swing runs most like the line, that can meet the
-- ball without much running.
local function plan_line(m, line, serving)
  return choose_and_swing(m, flight(m, serving), function(move, p, sink, run)
    local likeness = move.swing[1] * line.right + move.swing[2] * line.up   -- 1: the same direction
    return (1 - likeness) * 2.5 + math.abs(p.y - blade_offset(m, move)[2] - sink) * 3 - sink * 0.3 + run * 0.8
  end)
end

-- Two-bone IK: thigh and shin turn so the ankle reaches target (inside the model); the foot keeps its turn.
local function reach(m, leg, target)
  local a = m.anim.animator
  local hip, thigh_turn = joint(leg.thigh, a)
  local knee, shin_turn = joint(leg.shin, a)
  local ankle, foot_turn = joint(leg.foot, a)
  local _, hips_turn = joint("hips", a)
  local la, lb = len(sub(knee, hip)), len(sub(ankle, knee))
  local to = sub(target, hip)
  local d = math.min(math.max(len(to), math.abs(la - lb) + 1e-3), la + lb - 1e-3)
  local dir = unit(to)
  -- Which way the knee bends: the animation's knee, plus a steady pull toward the way the body faces, so a nearly
  -- straight leg (whose own bend direction is tiny) can't flip its knee from one frame to the next.
  local function across(p) return sub(p, mul(dir, dot(p, dir))) end
  local forward = rot(qmul(hips_turn, qinv(rest.hips)), v(0, 0, 1))
  local bend = unit(add(across(sub(knee, hip)), mul(unit(across(forward)), 0.08)))
  local c = math.max(-1, math.min(1, (la * la + d * d - lb * lb) / (2 * la * d)))
  local new_knee = add(add(hip, mul(dir, la * c)), mul(bend, la * math.sqrt(1 - c * c)))
  local new_ankle = add(hip, mul(dir, d))
  local turn_thigh = from_to(sub(knee, hip), sub(new_knee, hip))
  local new_thigh = qmul(turn_thigh, thigh_turn)
  local new_shin = qmul(from_to(rot(turn_thigh, sub(ankle, knee)), sub(new_ankle, new_knee)), qmul(turn_thigh, shin_turn))
  set_turn(leg.thigh, hips_turn, new_thigh, a)
  set_turn(leg.shin, new_thigh, new_shin, a)
  set_turn(leg.foot, new_shin, foot_turn, a)
end

-- Planted feet stay flat on the floor while the body moves; one at a time, a foot left too far behind steps.
-- The recordings hold one ankle a few centimetres up nearly all the time (left over from fitting the take to the
-- mannequin), so a foot counts as the animation's own step only when it is LIFTED above the other one.
local function feet(m, dt)
  -- How high the animation holds each ankle above standing on the floor; the body drops so the lowest one is.
  local low = math.huge
  for _, leg in ipairs(m.legs) do low = math.min(low, joint(leg.foot, m.anim.animator)[2] - leg.rest) end
  m.body.drop = lib.approach(m.body.drop, low, 20, dt)
  local wants = {}
  for i, leg in ipairs(m.legs) do
    local wanted = to_world(m, (joint(leg.foot, m.anim.animator)))
    wanted[2] = wanted[2] - (m.body.y - m.body.drop + m.body.air) - leg.rest
    wants[i] = wanted
  end
  low = math.min(wants[1][2], wants[2][2])
  for i, leg in ipairs(m.legs) do
    local wanted = wants[i]
    local lift = wanted[2] - low
    wanted[2] = leg.rest + (lift > LIFTED and lift or 0)          -- on the floor unless the animation steps
    local other = m.legs[3 - i]
    leg.plant = leg.plant or wanted
    local target
    if m.body.air > 0.005 then                                    -- jumping: the feet go up with the body
      target = to_world(m, (joint(leg.foot, m.anim.animator)))
      leg.plant, leg.step = v(target[1], leg.rest, target[3]), nil
    elseif lift > LIFTED then                                     -- the animation's own step: follow it
      leg.plant, leg.step, target = wanted, nil, wanted
    else
      -- Steps alternate (the next may start when the other foot is halfway), a foot left twice a stride behind
      -- steps anyway, and each step lands where the body is heading, so planted feet are never dragged.
      local behind = math.sqrt((wanted[1] - leg.plant[1]) ^ 2 + (wanted[3] - leg.plant[3]) ^ 2)
      local free = not other.step or other.step.t > 0.5
      if not leg.step and behind > STEP_LENGTH and (free or behind > STEP_LENGTH * 2) then
        local lead = STEP_TIME * 1.2
        local to = v(wanted[1] + m.body.vx * lead, wanted[2], wanted[3] + m.body.vz * lead)
        -- Carioca: moving sideways quickly, the trailing foot crosses over the leading one, in front of it and
        -- behind it by turns; the leading foot then steps out as usual.
        local right, ahead = rot(m.body.turn, v(-1, 0, 0)), rot(m.body.turn, v(0, 0, 1))
        local sideways = m.body.vx * right[1] + m.body.vz * right[3]
        if math.abs(sideways) > CARIOCA_SPEED and leg.foot:sub(-1) ~= (sideways > 0 and "R" or "L") then
          m.cross = not m.cross
          local s = sideways > 0 and 1 or -1
          to = add(other.plant, add(mul(right, s * CROSS_PAST), mul(ahead, m.cross and CROSS_DEPTH or -CROSS_DEPTH)))
          to[2] = wanted[2]
        end
        leg.step = { from = leg.plant, to = to, t = 0 }
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
    -- A foot never jumps when it changes between planted, stepping and following the animation: the jump is kept
    -- as an offset that fades out. Otherwise the foot is exactly on its target, so a landed step doesn't slide.
    local mode = m.body.air > 0.005 and "air" or leg.step and "step" or (lift > LIFTED and "anim" or "planted")
    if leg.at and mode ~= leg.mode then leg.offset = sub(leg.at, target) end
    leg.mode = mode
    leg.offset = leg.offset and mul(leg.offset, math.exp(-25 * dt)) or v(0, 0, 0)
    leg.at = add(target, leg.offset)
    reach(m, leg, to_model(m, leg.at))
  end
end

-- Where the top of a bat's blade is in the world, for the ball to rest on.
local function on_bat(m, side)
  local b = to_world(m, blade_in_model(side, m.anim.animator))
  return v(b[1], b[2] + R + 0.012, b[3])
end

-- A little burst where bat and ball meet.
local function spark(x, y, z) effects[#effects + 1] = { x = x, y = y, z = z, t = 0 } end

-- A mannequin each frame: its animation, mirrored if the move is, blended, turned, feet, then where its bat is.
local function pose(m, dt)
  m.anim:update(dt)
  if m.body.move.mirrored then mirror_pose(m.anim.animator) end
  blend_pose(m, dt)
  m.body.turn = toward(m.body.turn, facing(m, m.body.move), 1 - math.exp(-12 * dt))
  feet(m, dt)
  m.body.blade = to_world(m, blade_in_model(m.body.move.hand, m.anim.animator))
end

-- Its body heads for its spot.
local function walk(m, dt)
  local b = m.body
  local nx, nz = lib.approach(b.x, b.tx, WALK, dt), lib.approach(b.z, b.tz, WALK, dt)
  local vx, vz = (nx - b.x) / dt, (nz - b.z) / dt
  local speed = math.sqrt(vx * vx + vz * vz)
  if speed > TOP_SPEED then vx, vz = vx / speed * TOP_SPEED, vz / speed * TOP_SPEED end
  b.vx, b.vz = vx, vz
  b.x, b.z = b.x + vx * dt, b.z + vz * dt
  b.y = lib.approach(b.y, b.ty, WALK, dt)
  if b.air > 0 or b.jv > 0 then                       -- a jump: up, then down under gravity
    b.jv = b.jv - GRAVITY * dt
    b.air = b.air + b.jv * dt
    if b.air <= 0 then b.air, b.jv = 0, 0 end
  end
end

local function draw_mannequin(m)
  local q = m.body.turn
  model.draw(dummy, m.body.x, m.body.y - m.body.drop + m.body.air, m.body.z, q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1, m.anim.animator)
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
local CAM_SIDE, CAM_BACK, CAM_UP = 0.5, 0.8, 0.6
local cam = { x = 0, back = 0 }
local function follow_camera(dt)
  local home = HALF_L + 0.6                          -- where you start: the camera's resting place
  cam.x = lib.approach(cam.x, you.body.x * CAM_SIDE, 4, dt)
  cam.back = lib.approach(cam.back, you.body.z - home, 4, dt)
end
local function camera()
  gfx.camera(cam.x, 4.0 + cam.back * CAM_UP, 6.0 + cam.back * CAM_BACK,
             cam.x * 0.7, 0.6, -0.2 + cam.back * 0.2, 50)
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
local function serve_shot(px, py, pz, tx, depth)
  local own = pz > 0 and 1 or -1
  local best, best_err
  for land = 0.35, 1.06, 0.07 do
    for T = 0.3, 0.9, 0.04 do
      local lx = lib.lerp(px, tx, 0.4)
      local vx, vy, vz = (lx - px) / T, (TABLE_Y + R - py + 0.5 * G * T * T) / T, (own * land - pz) / T
      local bvx, bvy, bvz = vx * 0.97, -(vy - G * T) * BOUNCE, vz * 0.97   -- just after the bounce on its own side
      local tn = -own * land / bvz                                          -- reaching the net
      local t2 = 2 * bvy / G                                                -- back down on the table
      if tn > 0 and math.abs(lx) < HALF_W - 0.1 then
        local over = TABLE_Y + R + bvy * tn - 0.5 * G * tn * tn
        local z2, x2 = own * land + bvz * t2, lx + bvx * t2
        if over > NET_TOP + R + 0.03 and z2 * own < -0.2 and math.abs(z2) < HALF_L - 0.1 and math.abs(x2) < HALF_W - 0.1 then
          local err = math.abs(math.abs(z2) - depth) + math.abs(x2 - tx) * 0.5 + (over - NET_TOP) * 0.5   -- low over the net
          if not best_err or err < best_err then best, best_err = { vx, vy, vz }, err end
        end
      end
    end
  end
  return best
end

local function hit(by, vx, vy, vz, serving)
  ball.vx, ball.vy, ball.vz = vx, vy, vz
  last, bounces = by, { player = 0, cpu = 0 }
  serve_ball, serve_bounced = serving or false, false
  state = "rally"
  if by == "player" then input.haptic("light") end
end

-- Your shot, aimed by the line you drew: it goes the way the line went across, longer lines go deeper and faster.
local function player_hit(serving)
  local line = you.aim or { right = 0, up = 0, length = 200 }
  local power = lib.clamp(line.length / 380, 0.25, 1)
  local tx = lib.clamp(line.right * 0.65, -(HALF_W - 0.1), HALF_W - 0.1)
  local tz = -(0.45 + 0.8 * power)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.5 + 0.6 * power)
  if served then hit("player", served[1], served[2], served[3], true)
  else hit("player", shot(ball.x, ball.y, ball.z, tx, tz, 0.85 - 0.35 * power)) end
  you.armed, you.body.ty = false, 0
  if power > 0.85 and line.up < -0.3 then say("SMASH!") end
end

-- The robot is polite and can't lose: every return (and serve) is a gentle ball landing mid-table on your half,
-- on your side of it, never fast and never out.
local function cpu_hit(serving)
  local tx = lib.clamp(you.body.x * 0.6 + (math.random() - 0.5) * 0.2, -(HALF_W - 0.25), HALF_W - 0.25)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.8)
  if served then hit("cpu", served[1], served[2], served[3], true)
  else hit("cpu", shot(ball.x, ball.y, ball.z, tx, 0.7 + math.random() * 0.2, 0.8)) end
  robot.armed, robot.body.ty = false, 0
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
  you.armed, robot.armed, you.pending = false, false, nil
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

  local function touching(m) return len(sub(v(ball.x, ball.y, ball.z), m.body.blade)) <= HIT_RANGE end

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

-- Your serve swing: right now, timed by you. The stroke the line picks meets the ball where it will be
-- SWING_LEAD seconds from now; too early or too late and the ball is too high or too low for the bat.
local function swing_now(line)
  local t = SWING_LEAD
  local point = { x = ball.x + ball.vx * t, y = ball.y + ball.vy * t - 0.5 * G * t * t, z = ball.z + ball.vz * t, t = t }
  you.aim = line
  return choose_and_swing(you, { point }, function(move, p, sink, run)
    local likeness = move.swing[1] * line.right + move.swing[2] * line.up
    return (1 - likeness) * 2.5 + run * 0.8
  end)
end

local function swing_from(line)
  if state == "serve" and server == "player" then
    -- Your serve, first line: the toss. The ball pops up off your bat.
    state, last, bounces = "toss", nil, { player = 0, cpu = 0 }
    ball.vx, ball.vy, ball.vz = 0, TOSS, 0
    you.armed = false
  elseif state == "toss" and server == "player" and not you.armed then
    swing_now(line)                           -- second line: the swing, when you choose
  elseif state == "rally" then
    you.aim = line
    if last == "cpu" then
      if not plan_line(you, line, false) then you.pending = line end
    else
      you.pending = line                      -- drawn early: it's played as soon as the robot has hit
    end
  elseif state == "over" then
    new_game()
  end
end

local function fingers(dt)
  local w, h = gfx.size()
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then
      if t.x < w / 2 then
        drags[t.id] = { x = t.x, y = t.y, start = sys.real_time(), tx = you.body.tx, tz = you.body.tz }
      else
        lines[t.id] = { { t.x, t.y }, start = sys.real_time() }
      end
    elseif t.phase == "move" then
      if drags[t.id] then
        you.body.tx = lib.clamp(you.body.tx + t.dx * MOVE_SCALE, -1.8, 1.8)
        you.body.tz = lib.clamp(you.body.tz + t.dy * MOVE_SCALE, HALF_L + 0.25, HALF_L + 1.8)
      elseif lines[t.id] then
        table.insert(lines[t.id], { t.x, t.y })
      end
    elseif t.phase == "up" then
      -- A quick flick up on the left half is a jump, not a walk: the walk it started is taken back.
      local d = drags[t.id]
      if d and sys.real_time() - d.start < 0.3 and d.y - t.y > 60 and d.y - t.y > 2 * math.abs(t.x - d.x)
         and you.body.air == 0 then
        you.body.tx, you.body.tz = d.tx, d.tz
        you.body.jv, you.body.air = JUMP_SPEED, 0.001
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
  -- The robot plans as soon as you hit; your line drawn early is played as soon as the robot hits.
  if state == "rally" and last == "player" and robot.body.plan_for ~= bounces then
    robot.body.plan_for = bounces
    plan_robot(robot)
  end
  if state == "rally" and last == "cpu" and you.pending then
    if plan_line(you, you.pending, false) then you.pending = nil end
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
    plan_robot(robot, robot.serve_hand)
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

function draw()
  gfx.clear(0.05, 0.03, 0.11)
  camera()
  gfx.fog(0.05, 0.03, 0.11, 5, 13)
  gfx.light(-0.35, -1, -0.25, 1.0, 0.95, 0.9)
  gfx.ambient(0.34, 0.31, 0.46)
  gfx.shadows(0, TABLE_Y, 0.4, 3.0, 0.65)
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

  -- The two mannequins, a taped bat in each hand.
  gfx.blend("opaque")
  draw_mannequin(robot)
  draw_mannequin(you)

  -- The ball: a real lit ball, its shadow straight below it on the table or the floor (always, so you can read
  -- where it is), and the bursts where bat meets ball.
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

  -- Your lines: the one being drawn, and finished ones fading out.
  local w, h = gfx.size()
  local function dots(l, alpha)
    for i = 2, #l do
      local a, b = l[i - 1], l[i]
      local n = math.max(1, math.floor(math.sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2) / 6))
      for k = 0, n - 1 do gfx.circle(a[1] + (b[1] - a[1]) * k / n, a[2] + (b[2] - a[2]) * k / n, 6, 1, 0.75, 0.9, alpha) end
    end
  end
  for _, l in pairs(lines) do dots(l, 0.9) end
  for _, d in ipairs(drawn) do dots(d.points, 0.9 * (1 - d.t / 0.5)) end
  gfx.rect(w / 2 - 1, h * 0.55, 2, h * 0.3, 1, 1, 1, 0.08)   -- where walking ends and swinging begins

  -- Score and messages, in your dot font.
  gfx.font(font)
  local left, top, right, bottom = gfx.safe()
  gfx.text("DREAM TABLE", left + 80, top + 20, 26, 0.85, 0.75, 1)
  local board = words(string.format("YOU %d  -  %d CPU", score.player, score.cpu))
  gfx.text(board, w - right - gfx.text_width(board, 30) - 24, top + 20, 30, 1, 1, 1)
  local serving = words((state == "serve" or state == "point") and (serve_turn() == "player" and "YOUR SERVE" or "CPU SERVES") or "")
  gfx.text(serving, w - right - gfx.text_width(serving, 18) - 24, top + 60, 18, 0.7, 0.75, 0.9)
  if wins > 0 then gfx.text(words("WINS " .. wins), left + 80, top + 56, 18, 0.7, 0.75, 0.9) end

  local center = nil
  if state == "serve" and server == "player" then center = "DRAW A LINE TO TOSS" end
  if state == "toss" and server == "player" and not you.armed then center = "DRAW AGAIN TO HIT IT" end
  if state == "over" then center = message .. "   TAP TO PLAY AGAIN" end
  if center then center = words(center) gfx.text(center, (w - gfx.text_width(center, 26)) / 2, h * 0.2, 26, 1, 1, 1) end
  if state ~= "over" and sys.real_time() - message_time < 1.6 then
    gfx.text(words(message), (w - gfx.text_width(words(message), 30)) / 2, h * 0.14, 30, 1, 0.85, 0.55)
  end
  local walk_hint, swing_hint = words("DRAG HERE TO MOVE"), words("DRAW HERE TO SWING")
  gfx.text(walk_hint, w * 0.25 - gfx.text_width(walk_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
  gfx.text(swing_hint, w * 0.75 - gfx.text_width(swing_hint, 16) / 2, h - bottom - 50, 16, 0.55, 0.55, 0.7)
end
