-- The wooden mannequin playing a mocap take, with gravity and grounded, rolling feet.
-- mannequin.glb is made on the PC by tools/blender_mannequin.py (your model, plus a take retargeted onto it).
-- Drag to turn the camera round. Tap to switch the IK and gravity on and off, to compare.
--
-- Every frame, after the animation has posed the body:
--   * gravity: the body falls until its lowest foot rests on the ground below it, and is pushed up rather than
--     let a foot sink in;
--   * each foot has two contact points, heel and toes. A ray finds the ground under each. A point the animation
--     has low and nearly still is planted: eased onto the ground and held where it touched down. So stepping off,
--     the heel rises first while the toes stay put and the foot pivots on them; landing, the heel touches first
--     and the foot rolls down flat. A point in the air follows the animation, but never goes through the ground;
--   * the foot is turned to sit on its two points, and levelled side to side when both are down;
--   * if a foot can't reach, the hips lower just enough; thigh and shin turn (two-bone IK) to put the ankle in
--     place, the knee bending the way the animation had it.

local lib = include("lib.lua")

local dummy = model.load("mannequin.glb")
local bat = model.load("bat.glb")   -- a table tennis bat duct-taped to a hand (tools/blender_bats.py), one per hand
local hands = {}
local anim = lib.animation(dummy)
local clips = model.clips(dummy)
local yaw, ik_on = 0.3, true

local PLANT_HEIGHT = 0.035  -- metres above the ground at which the animation's heel or toes count as down
local PLANT_SPEED = 0.35    -- metres a second: a heel or toe moving faster than this is in a step
local EASE = 14             -- how quickly a heel or toe settles onto or lifts off the ground (bigger is snappier)
local RELEASE = 0.25        -- a planted point this far from the animation's lets go, so the body isn't held back
local GRAVITY = 9.8         -- metres a second squared
local HEEL_BACK, TOES_FRONT = 0.045, 0.13   -- the contact points: heel, and the ball the foot pivots on (metres)
local SOLE_BACK, SOLE_FRONT, SOLE_HALF = 0.049, 0.186, 0.046   -- the sole's real corners (measured from the model),
                                                               -- which are never let through the ground
local HEEL_UP = 0.06        -- the toes stay planted until the heel has risen this far: heel first, toes last

-- The collision world the feet stand on: the floor and a low wooden step.
local STEP = { x = 0.32, y = 0.04, z = 0.05, sx = 0.42, sy = 0.08, sz = 0.8 }
local legs = {}
local hips_parent
local fall = { offset = 0, speed = 0 }   -- gravity: how far the body has dropped, and how fast it is falling
local hips_drop = 0

-- ---------------------------------------------------------------- small vector and rotation helpers

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
local function rotate(q, p) return v(lib.quat_rotate(q[1], q[2], q[3], q[4], p[1], p[2], p[3])) end

-- The turn that takes direction a onto direction b, the short way round.
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

-- Part of a turn: all of it at w = 1, none at w = 0.
local function partly(q, w)
  local p = { q[1] * w, q[2] * w, q[3] * w, 1 + (q[4] - 1) * w }
  local l = math.sqrt(p[1] ^ 2 + p[2] ^ 2 + p[3] ^ 2 + p[4] ^ 2)
  return { p[1] / l, p[2] / l, p[3] / l, p[4] / l }
end

-- A bone's place and turn in the model, as the engine has it right now.
local function joint(name)
  local x, y, z, qx, qy, qz, qw = model.joint(dummy, anim.animator, name)
  return v(x, y, z), { qx, qy, qz, qw }
end

-- Sets a bone's turn in the model by setting its turn within its parent.
local function set_turn(name, parent_turn, turn)
  local t1, t2, t3, _, _, _, _, s1, s2, s3 = model["local"](dummy, anim.animator, name)
  local q = qmul(qinv(parent_turn), turn)
  model["local"](dummy, anim.animator, name, t1, t2, t3, q[1], q[2], q[3], q[4], s1, s2, s3)
end

-- Moves the whole body up or down (through the hips, the root of the skeleton).
local function lift_body(dy)
  local _, parent_turn = joint(hips_parent)
  local shift = rotate(qinv(parent_turn), v(0, dy, 0))
  local t1, t2, t3, q1, q2, q3, q4, s1, s2, s3 = model["local"](dummy, anim.animator, "hips")
  model["local"](dummy, anim.animator, "hips", t1 + shift[1], t2 + shift[2], t3 + shift[3], q1, q2, q3, q4, s1, s2, s3)
end

-- The ground's height under a point, and which way the ground faces there.
local function ground_below(p)
  local hit, _, y, _, nx, ny, nz = phys.raycast(p[1], p[2] + 0.8, p[3], 0, -1, 0, 3)
  if not hit then return 0, v(0, 1, 0) end
  return y, v(nx, ny, nz)
end

-- The heel and toes of a foot, where the animation has them now.
local function sole_points(leg)
  local ankle, turn = joint(leg.foot)
  return ankle, turn, add(ankle, rotate(turn, leg.heel_local)), add(ankle, rotate(turn, leg.toes_local))
end

-- ---------------------------------------------------------------- one contact point: heel or toes

local function contact(point, at, dt, hold)
  local ground, normal = ground_below(at)
  local first = point.last == nil
  local speed = point.last and len(sub(at, point.last)) / math.max(dt, 1e-4) or 0
  point.last = at
  -- Down if low over the ground under it, or low over the animation's own floor (the other foot may be up on the
  -- step, which lifted the body): then the leg reaches down to the real ground and the hips drop to let it.
  local down = math.min(at[2] - ground, at[2] - fall.offset) < PLANT_HEIGHT and speed < PLANT_SPEED
  if down and not point.lock then point.lock = v(at[1], 0, at[3]) end
  if first and down then point.weight = 1 end   -- the first frame starts planted, not easing down from the air
  local drifted = point.lock and len(v(at[1] - point.lock[1], 0, at[3] - point.lock[3])) > RELEASE
  if drifted or (not down and not hold) then point.lock = nil end
  point.weight = lib.approach(point.weight, point.lock and 1 or 0, EASE, dt)
  local target = v(at[1], at[2], at[3])
  if point.lock then
    target[1] = lib.lerp(at[1], point.lock[1], point.weight)
    target[3] = lib.lerp(at[3], point.lock[3], point.weight)
  end
  target[2] = math.max(lib.lerp(at[2], ground, point.weight), ground)   -- never through the ground
  point.target, point.ground, point.normal = target, ground, normal
  return target
end

-- ---------------------------------------------------------------- gravity, then the feet

local function fall_to_ground(dt)
  -- How high the lowest heel or toe is above the ground under it, with the animation as it is.
  local lowest = math.huge
  for _, leg in ipairs(legs) do
    local ankle, turn = joint(leg.foot)
    for _, corner in ipairs(leg.corners) do
      local p = add(ankle, rotate(turn, corner))
      lowest = math.min(lowest, p[2] - ground_below(p))
    end
  end
  if not fall.started then fall.started, fall.offset = true, -lowest end   -- starts standing, not dropping in
  -- Falls while the lowest foot is above the ground; lands on it; never lets it sink in.
  fall.speed = fall.speed - GRAVITY * dt
  fall.offset = fall.offset + fall.speed * dt
  if fall.offset + lowest <= 0 then
    fall.offset, fall.speed = -lowest, 0
  end
  lift_body(fall.offset)
end

local function solve_feet(dt)
  -- Where each foot should be: its heel and toes, then the ankle that puts them there.
  local drop_needed = 0
  for _, leg in ipairs(legs) do
    local ankle, turn, heel, toes = sole_points(leg)
    local heel_t = contact(leg.heel, heel, dt)
    -- The toes hold the ground until the heel is down no longer and has risen HEEL_UP: the foot rolls off them.
    local heel_up = leg.heel.lock == nil and heel_t[2] - leg.heel.ground > HEEL_UP
    local toes_t = contact(leg.toes, toes, dt, leg.toes.lock ~= nil and not heel_up)
    -- Turn the foot so heel-to-toes runs where the targets do: this is the pivot on the toes or heel.
    local foot = qmul(from_to(sub(toes, heel), sub(toes_t, heel_t)), turn)
    -- With both down, level it side to side onto the ground.
    local both = math.min(leg.heel.weight, leg.toes.weight)
    if both > 0 then
      local along = unit(sub(toes_t, heel_t))
      local normal = unit(add(leg.heel.normal, leg.toes.normal))
      local up = rotate(foot, leg.sole_up)
      local up_flat = sub(up, mul(along, dot(up, along)))
      local normal_flat = sub(normal, mul(along, dot(normal, along)))
      if len(up_flat) > 1e-4 and len(normal_flat) > 1e-4 then
        foot = qmul(partly(from_to(up_flat, normal_flat), both), foot)
      end
    end
    leg.turn = foot
    leg.target = sub(heel_t, rotate(foot, leg.heel_local))   -- the ankle, from the heel
    -- Whatever the heel and ball do, no corner of the sole goes through the ground: lift the foot if one would.
    local lift = 0
    for _, corner in ipairs(leg.corners) do
      local p = add(leg.target, rotate(foot, corner))
      lift = math.max(lift, ground_below(p) - p[2])
    end
    leg.target[2] = leg.target[2] + lift
    local hip, knee = joint(leg.thigh), joint(leg.shin)
    local reach = (len(sub(knee, hip)) + len(sub(ankle, knee))) * 0.995
    drop_needed = math.max(drop_needed, len(sub(leg.target, hip)) - reach)
  end

  -- The first frame starts with the hips already where they need to be, so no leg floats in at the start.
  hips_drop = fall.solved and lib.approach(hips_drop, drop_needed, EASE, dt) or drop_needed
  fall.solved = true
  if hips_drop > 1e-4 then lift_body(-hips_drop) end

  -- Two-bone IK for each leg.
  local _, hips_turn = joint("hips")
  for _, leg in ipairs(legs) do
    local hip, thigh_turn = joint(leg.thigh)
    local knee, shin_turn = joint(leg.shin)
    local ankle = joint(leg.foot)
    local a, b = len(sub(knee, hip)), len(sub(ankle, knee))
    local to_target = sub(leg.target, hip)
    local d = math.min(math.max(len(to_target), math.abs(a - b) + 1e-3), a + b - 1e-3)
    local dir = unit(to_target)
    local bend = sub(sub(knee, hip), mul(dir, dot(sub(knee, hip), dir)))   -- keep the knee's own bend direction
    if len(bend) < 1e-5 then bend = rotate(hips_turn, v(0, 0, 1)) end
    bend = unit(bend)
    local cos_hip = math.max(-1, math.min(1, (a * a + d * d - b * b) / (2 * a * d)))
    local new_knee = add(add(hip, mul(dir, a * cos_hip)), mul(bend, a * math.sqrt(1 - cos_hip * cos_hip)))
    local new_ankle = add(hip, mul(dir, d))
    local turn_thigh = from_to(sub(knee, hip), sub(new_knee, hip))
    local new_thigh = qmul(turn_thigh, thigh_turn)
    local shin_dir = rotate(turn_thigh, sub(ankle, knee))
    local new_shin = qmul(from_to(shin_dir, sub(new_ankle, new_knee)), qmul(turn_thigh, shin_turn))
    set_turn(leg.thigh, hips_turn, new_thigh)
    set_turn(leg.shin, new_thigh, new_shin)
    set_turn(leg.foot, new_shin, leg.turn)
  end
end

-- ---------------------------------------------------------------- the game

local function play(i)
  if clips[i] then anim:play(clips[i].name, { loop = true, fade = 0.3 }) end
end

function init()
  phys.clear()
  phys.box(0, -0.05, 0, 10, 0.1, 10)
  phys.box(STEP.x, STEP.y, STEP.z, STEP.sx, STEP.sy, STEP.sz)
  legs = {}
  for _, side in ipairs({ "L", "R" }) do
    local leg = { thigh = "thigh." .. side, shin = "shin." .. side, foot = "foot." .. side,
                  heel = { weight = 0 }, toes = { weight = 0 } }
    -- The T-pose stands flat on the floor, so it gives the sole's points and its "up" in the foot's own terms.
    local x, y, z, qx, qy, qz, qw = model.joint(dummy, 0, leg.foot)
    local back = qinv({ qx, qy, qz, qw })
    leg.heel_local = rotate(back, v(0, -y, -HEEL_BACK))
    leg.toes_local = rotate(back, v(0, -y, TOES_FRONT))
    leg.sole_up = rotate(back, v(0, 1, 0))
    leg.corners = {}
    for _, z in ipairs({ -SOLE_BACK, TOES_FRONT, SOLE_FRONT }) do
      for _, x in ipairs({ -SOLE_HALF, SOLE_HALF }) do leg.corners[#leg.corners + 1] = rotate(back, v(x, -y, z)) end
    end
    legs[#legs + 1] = leg
  end
  for _, n in ipairs(model.nodes(dummy)) do
    if n.name == "hips" and n.parent then hips_parent = model.nodes(dummy)[n.parent].name end
  end
  -- The bat is modelled on the left hand in the T-pose; the right hand gets it turned round (half a turn about up).
  -- Its turn relative to each hand bone is worked out once from the T-pose, then it follows the hand.
  hands = {}
  for _, side in ipairs({ "L", "R" }) do
    local _, _, _, qx, qy, qz, qw = model.joint(dummy, 0, "hand." .. side)
    local facing = side == "L" and { 0, 0, 0, 1 } or { 0, 1, 0, 0 }
    hands[#hands + 1] = { bone = "hand." .. side, offset = qmul(qinv({ qx, qy, qz, qw }), facing) }
  end
  fall.offset, fall.speed, fall.started, fall.solved, hips_drop = 0, 0, false, false, 0
  play(1)
end

function update(dt)
  anim:update(dt)
  if ik_on and dt > 0 then
    fall_to_ground(dt)
    solve_feet(dt)
  end
  for _, t in ipairs(input.touches()) do
    if t.phase == "move" then yaw = yaw - t.dx * 0.01 end
    if t.phase == "up" and math.abs(t.dx) < 2 then ik_on = not ik_on end
  end
end

function draw()
  gfx.clear(0.14, 0.13, 0.16)
  gfx.camera(math.sin(yaw) * 4, 1.4, math.cos(yaw) * 4, 0, 0.95, 0, 45)
  gfx.light(-0.4, -1, -0.3, 1.0, 0.95, 0.85)
  gfx.ambient(0.35, 0.33, 0.38)
  gfx.shadows(0, 0.5, 0, 2.5, 0.6)
  gfx.plane(0, 0, 0, 8, 8, 0.32, 0.30, 0.34)
  gfx.cube(STEP.x, STEP.y, STEP.z, STEP.sx, STEP.sy, STEP.sz, 0.45, 0.32, 0.22)
  model.draw(dummy, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, anim.animator)
  for _, hand in ipairs(hands) do
    local wrist, turn = joint(hand.bone)
    local q = qmul(turn, hand.offset)
    model.draw(bat, wrist[1], wrist[2], wrist[3], q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1)
  end
  if ik_on then   -- marks under planted heels and toes
    gfx.unlit(true)
    for _, leg in ipairs(legs) do
      for _, point in ipairs({ leg.heel, leg.toes }) do
        if point.target and point.weight > 0.5 then
          gfx.sphere(point.target[1], point.ground + 0.004, point.target[3], 0.03, 0.3, 1.0, 0.5)
        end
      end
    end
    gfx.unlit(false)
  end

  local left, top = gfx.safe()
  local name = clips[1] and clips[1].name or "no clips in mannequin.glb"
  gfx.text("MANNEQUIN  -  " .. name, left + 80, top + 24, 26, 1, 1, 1)
  gfx.text("GRAVITY + FOOT IK " .. (ik_on and "ON" or "OFF") .. "   (tap to switch, drag to turn)", left + 80, top + 60, 18,
           ik_on and 0.5 or 0.9, ik_on and 1.0 or 0.6, ik_on and 0.6 or 0.5)
end
