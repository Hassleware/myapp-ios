-- The wooden mannequin playing a mocap take, with its feet grounded by IK.
-- mannequin.glb is made on the PC by tools/blender_mannequin.py (your model, plus a take retargeted onto it).
-- Drag to turn the camera round. Tap to switch the foot IK on and off, to compare.
--
-- Foot IK, every frame, after the animation has posed the body:
--   * a ray from above each ankle finds the ground below it (the floor, or the wooden step under one foot);
--   * a foot the animation has low and nearly still is planted: eased onto the ground and held where it landed,
--     so it can't slide; a foot in mid-step follows the animation, but never goes through the ground;
--   * if a foot can't reach its spot, the hips lower just enough;
--   * thigh and shin turn (two-bone IK) to put the ankle there, the knee bending the way the animation had it.

local lib = include("lib.lua")

local dummy = model.load("mannequin.glb")
local anim = lib.animation(dummy)
local clips = model.clips(dummy)
local yaw, ik_on = 0.3, true

local PLANT_HEIGHT = 0.05   -- metres above its resting height at which the animation's ankle counts as down
local PLANT_SPEED = 0.35    -- metres a second: an ankle moving faster than this is stepping
local EASE = 12             -- how quickly a foot settles onto or lifts off the ground (bigger is snappier)
local RELEASE = 0.25        -- a planted foot this far from the animation's lets go, so the body isn't held back

-- The collision world the feet stand on: the floor and a low wooden step.
local STEP = { x = 0.32, y = 0.04, z = 0.05, sx = 0.42, sy = 0.08, sz = 0.8 }
local legs = {}
local ankle_rest, hips_parent
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

local function ground_below(p)
  local hit, _, y = phys.raycast(p[1], p[2] + 0.8, p[3], 0, -1, 0, 3)
  return hit and y or 0
end

-- ---------------------------------------------------------------- the IK

local function solve_feet(dt)
  -- Where each foot should go.
  local drop_needed = 0
  for _, leg in ipairs(legs) do
    local ankle = joint(leg.foot)
    local hip = joint(leg.thigh)
    local floor = ground_below(ankle) + ankle_rest
    local speed = leg.last and len(sub(ankle, leg.last)) / math.max(dt, 1e-4) or 0
    leg.last = ankle
    local down = ankle[2] - ankle_rest < PLANT_HEIGHT and speed < PLANT_SPEED
    leg.weight = lib.approach(leg.weight, down and 1 or 0, EASE, dt)
    if down and not leg.lock then leg.lock = v(ankle[1], 0, ankle[3]) end
    if not down or (leg.lock and len(v(ankle[1] - leg.lock[1], 0, ankle[3] - leg.lock[3])) > RELEASE) then leg.lock = nil end

    local target = v(ankle[1], ankle[2], ankle[3])
    if leg.lock then
      target[1] = lib.lerp(ankle[1], leg.lock[1], leg.weight)
      target[3] = lib.lerp(ankle[3], leg.lock[3], leg.weight)
    end
    target[2] = math.max(lib.lerp(ankle[2], floor, leg.weight), floor)   -- never through the ground
    leg.target, leg.floor = target, floor

    local knee = joint(leg.shin)
    local reach = (len(sub(knee, hip)) + len(sub(ankle, knee))) * 0.995
    drop_needed = math.max(drop_needed, len(sub(target, hip)) - reach)
  end

  -- Lower the hips when a foot can't reach.
  hips_drop = lib.approach(hips_drop, drop_needed, EASE, dt)
  if hips_drop > 1e-4 then
    local _, parent_turn = joint(hips_parent)
    local shift = rotate(qinv(parent_turn), v(0, -hips_drop, 0))
    local t1, t2, t3, q1, q2, q3, q4, s1, s2, s3 = model["local"](dummy, anim.animator, "hips")
    model["local"](dummy, anim.animator, "hips", t1 + shift[1], t2 + shift[2], t3 + shift[3], q1, q2, q3, q4, s1, s2, s3)
  end

  -- Two-bone IK for each leg.
  local _, hips_turn = joint("hips")
  for _, leg in ipairs(legs) do
    local hip, thigh_turn = joint(leg.thigh)
    local knee, shin_turn = joint(leg.shin)
    local ankle, foot_turn = joint(leg.foot)
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
    set_turn(leg.foot, new_shin, foot_turn)   -- the foot keeps the angle the animation gave it
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
    legs[#legs + 1] = { thigh = "thigh." .. side, shin = "shin." .. side, foot = "foot." .. side, weight = 0 }
  end
  local _, rest_y = model.joint(dummy, 0, "foot.L")   -- the ankle's height above the floor in the T-pose
  ankle_rest = rest_y or 0.1
  for _, n in ipairs(model.nodes(dummy)) do
    if n.name == "hips" and n.parent then hips_parent = model.nodes(dummy)[n.parent].name end
  end
  hips_drop = 0
  play(1)
end

function update(dt)
  anim:update(dt)
  if ik_on and dt > 0 then solve_feet(dt) end
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
  if ik_on then   -- a mark under each planted foot
    for _, leg in ipairs(legs) do
      if leg.target and leg.weight > 0.5 then
        gfx.unlit(true)
        gfx.sphere(leg.target[1], leg.floor - ankle_rest + 0.005, leg.target[3], 0.05, 0.3, 1.0, 0.5)
        gfx.unlit(false)
      end
    end
  end

  local left, top = gfx.safe()
  local name = clips[1] and clips[1].name or "no clips in mannequin.glb"
  gfx.text("MANNEQUIN  -  " .. name, left + 80, top + 24, 26, 1, 1, 1)
  gfx.text("FOOT IK " .. (ik_on and "ON" or "OFF") .. "   (tap to switch, drag to turn)", left + 80, top + 60, 18,
           ik_on and 0.5 or 0.9, ik_on and 1.0 or 0.6, ik_on and 0.6 or 0.5)
end
