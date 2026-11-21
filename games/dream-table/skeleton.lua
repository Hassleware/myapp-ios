-- The mannequin's bones: reading and setting their turns, mirroring and blending poses, where the bat's
-- blade is, the serving hand's flat bat and lifted arm, and the moves read from the recorded clips. Knows
-- nothing about the ball or the rules.
-- Made by main.lua: include("skeleton.lua")(game) returns the functions below; the shared game table carries
-- everything it uses from elsewhere (listed at the top) and the state the files share (game.state, game.score...).
return function(game)
local bone_local = model.bone_local or model["local"]   -- a bone's own transform (older apps only have the second)
-- from main.lua
local SERVE, lib = game.SERVE, game.lib
-- from vec.lua
local add, axes_turn, cross, mirror_turn, mul = game.add, game.axes_turn, game.cross, game.mirror_turn, game.mul
local qinv, qmul, rot, sub, toward, v = game.qinv, game.qmul, game.rot, game.sub, game.toward, game.v
local yaw_turn = game.yaw_turn

-- ---------------------------------------------------------------- the mannequins
-- Both play the swings you recorded with the mocap game: mannequin.glb (tools/blender_mannequin.py) holds one clip per
-- recorded swing, each with its hit CONTACT seconds in. Every clip also plays mirror-imaged with the bat in the other
-- hand, so 11 recordings give 22 moves. A swing is planned onto the ball's flight (plan_swing): which move, where to
-- be for its bat to meet the ball, and when to start the clip so the recorded hit lands as the ball gets there.
-- The ball is only hit if it really touches the bat (REACH).
local mannequin_model = model.load("mannequin.glb")
local bat_model = model.load("bat.glb")
local CONTACT = 2.0                  -- seconds into every shot clip that the ball is hit (tools/split_shots.py)
-- The middle of each bat's blade, in the bat's own frame (tools/blender_bats.py: along the hand, palm side).
local BLADE = { L = { 0.273, -0.003, -0.033 }, R = { -0.273, -0.003, -0.033 } }
local BLEND_TIME = 0.18              -- switching moves: every bone blends from the old pose over this long
local moves, hands, bones = {}, {}, {}   -- filled once by load_moves
local rest = {}                      -- every bone's turn in the T-pose
local hips_parent

-- A bone's place and turn inside the model, as animator a has it now (0: the T-pose).
local function joint(name, a)
  local x, y, z, qx, qy, qz, qw = model.joint(mannequin_model, a, name)
  return v(x, y, z), { qx, qy, qz, qw }
end

-- Sets a bone's turn inside the model, through its turn within its parent.
local function set_turn(name, parent_turn, turn, a)
  local t1, t2, t3, _, _, _, _, s1, s2, s3 = bone_local(mannequin_model, a, name)
  local q = qmul(qinv(parent_turn), turn)
  bone_local(mannequin_model, a, name, t1, t2, t3, q[1], q[2], q[3], q[4], s1, s2, s3)
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
  local _, _, _, q1, q2, q3, q4, s1, s2, s3 = bone_local(mannequin_model, a, "hips")
  bone_local(mannequin_model, a, "hips", at[1], at[2], at[3], q1, q2, q3, q4, s1, s2, s3)
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
local function load_moves()
  local nodes = model.nodes(mannequin_model)
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

  local scratch = model.animator(mannequin_model)
  for _, clip in ipairs(model.clips(mannequin_model)) do
    local side, kind = clip.name:match("%-(%a%a)%-(%a+)$")
    if side == "fh" or side == "bh" then
      model.pose(mannequin_model, scratch, clip.name, 0, 1)
      local left, right = joint("thigh.L", scratch), joint("thigh.R", scratch)
      local fx, fz = -(left[3] - right[3]), left[1] - right[1]   -- facing, from the body's left (+x faces +z)
      model.pose(mannequin_model, scratch, clip.name, CONTACT - 0.15, 1)
      local before = blade_in_model("R", scratch)
      model.pose(mannequin_model, scratch, clip.name, CONTACT + 0.15, 1)
      local after = blade_in_model("R", scratch)
      model.pose(mannequin_model, scratch, clip.name, CONTACT, 1)
      local blade = blade_in_model("R", scratch)
      for _, mirrored in ipairs({ false, true }) do
        local turn = yaw_turn(-math.atan(mirrored and -fx or fx, fz))   -- turned to face +z
        local flip = mirrored and -1 or 1
        local b = v(blade[1] * flip, blade[2], blade[3])
        local swing = rot(turn, v((after[1] - before[1]) * flip, after[2] - before[2], after[3] - before[3]))
        -- The swing as you see it from behind: across (to the body's right is +) and up.
        local s_right, s_up = -swing[1], swing[2]
        local l = math.sqrt(s_right * s_right + s_up * s_up)
        moves[#moves + 1] = { clip = clip.name, kind = kind, side = side, mirrored = mirrored, turn = turn,
                              hand = mirrored and "L" or "R", blade = rot(turn, b),
                              swing = { s_right / math.max(l, 1e-6), s_up / math.max(l, 1e-6) } }
      end
    end
  end
end

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

-- The turn of the bone the hips hang from (the armature), for turning the hips alone.
local function hips_parent_turn(a) return select(2, joint(hips_parent, a)) end

return {
  hips_parent_turn = hips_parent_turn,
  moves = moves, hands = hands, mannequin_model = mannequin_model, bat_model = bat_model,
  CONTACT = CONTACT, BLEND_TIME = BLEND_TIME, joint = joint, set_turn = set_turn,
  write_pose = write_pose, mirror_pose = mirror_pose, blade_in_model = blade_in_model, load_moves = load_moves,
  keep_pose = keep_pose, blend_pose = blend_pose, flat_bat = flat_bat, lift_arm = lift_arm,
}
end
