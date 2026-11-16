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
local trail = {}

-- state: "serve" (someone holds the ball), "rally", "point" (a short pause), "over"
local state, server, last, bounces = "serve", "player", nil, {}
local message, message_time, timer = "", 0, 0
local key_was_down = false

-- ---------------------------------------------------------------- the opponent: the wooden mannequin
-- It plays the shots you recorded with the mocap game. mannequin.glb is made by tools/blender_mannequin.py from
-- captures/shots-<n>-<k>-<shot>-sam3d.npz, one clip per shot, each with the hit CONTACT seconds in.
-- For each return it picks a clip by the ball (forehand or backhand side, how high), starts it so the recorded hit
-- lands when the ball arrives, and stands where its bat hand meets the ball.
-- ponytail: it slides into place (no foot IK here yet); add the mannequin game's feet when it shows.
local dummy = model.load("mannequin.glb")
local bat_model = model.load("bat.glb")
local anim = lib.animation(dummy)
local CONTACT = 2.0                  -- seconds into every shot clip that the ball is hit (tools/split_shots.py)
local moves, hands = {}, {}
local body = { x = 0, z = AI_Z - 0.5, tx = 0, tz = AI_Z - 0.5, turn = { 0, 0, 0, 1 }, move = nil, ball_seen = nil }

local function qmul(a, b) return { lib.quat_mul(a[1], a[2], a[3], a[4], b[1], b[2], b[3], b[4]) } end
local function rotate(q, x, y, z) return lib.quat_rotate(q[1], q[2], q[3], q[4], x, y, z) end
local function yaw_turn(angle) return { 0, math.sin(angle / 2), 0, math.cos(angle / 2) } end

-- Reads every shot clip once: which shot it is, which way it faces, and where the bat hand is at the hit.
local function learn_moves()
  moves = {}
  local scratch = model.animator(dummy)
  for _, clip in ipairs(model.clips(dummy)) do
    local side, kind = clip.name:match("%-(%a%a)%-(%a+)$")
    if side == "fh" or side == "bh" then
      model.pose(dummy, scratch, clip.name, 0, 1)
      local lx, _, lz = model.joint(dummy, scratch, "thigh.L")
      local rx, _, rz = model.joint(dummy, scratch, "thigh.R")
      local fx, fz = -(lz - rz), lx - rx                  -- facing, from the body's left (+x in the T-pose faces +z)
      local turn = yaw_turn(-math.atan(fx, fz))           -- turned to face +z: the player's end of the table
      model.pose(dummy, scratch, clip.name, math.min(CONTACT, clip.duration), 1)
      local hx, hy, hz = rotate(turn, model.joint(dummy, scratch, "hand.R"))
      moves[#moves + 1] = { clip = clip.name, side = side, kind = kind, turn = turn, hand = { hx, hy, hz } }
    end
  end
  hands = {}
  for _, side in ipairs({ "L", "R" }) do
    local _, _, _, qx, qy, qz, qw = model.joint(dummy, 0, "hand." .. side)
    hands[#hands + 1] = { bone = "hand." .. side, node = "bat." .. side, offset = { -qx, -qy, -qz, qw } }
  end
  if moves[1] then
    body.move, body.turn = moves[1], moves[1].turn
    anim:play(moves[1].clip, { loop = false, speed = 0, time = 0.3, fade = 0 })   -- standing ready
  end
end

-- The shot for a ball arriving at x, height y: forehand on its right (-x, as it faces +z), backhand on its left.
-- Among those, the clip whose recorded hit is nearest that height (the body can slide sideways and forward, not up),
-- nudged toward the kind of shot that suits the height. A little randomness so it doesn't always play the same.
local PREFER = { high = { "smash", "drive", "loop" }, low = { "push", "loop", "flick", "block", "drive" },
                 mid = { "drive", "loop", "block", "flick", "push", "smash" } }
local function choose(x, y)
  local side = x < body.x and "fh" or "bh"
  local height = y - TABLE_Y > 0.45 and "high" or y - TABLE_Y < 0.12 and "low" or "mid"
  local best, best_cost = moves[1], math.huge
  for _, m in ipairs(moves) do
    local cost = math.abs(m.hand[2] - y) + (m.side == side and 0 or 0.5) + math.random() * 0.06
    local rank = 0.3
    for i, kind in ipairs(PREFER[height]) do if kind == m.kind then rank = (i - 1) * 0.04 end end
    if cost + rank < best_cost then best, best_cost = m, cost + rank end
  end
  return best
end

local function side_of(z) return z > 0 and "player" or "cpu" end
local function other(who) return who == "player" and "cpu" or "player" end
local function say(text) message, message_time = text, sys.real_time() end

local function camera()
  gfx.camera(0, 1.5, 2.85, 0, 0.72, -0.35, 52)
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

local function cpu_hit()
  local tx = (math.random() * 2 - 1) * (HALF_W - 0.15)
  local tz = 0.5 + math.random() * 0.75
  local T = 0.55 + math.random() * 0.2
  if math.random() < 0.15 then T = 0.42 end              -- now and then, a fast one
  if math.random() < 0.08 then tz = HALF_L + 0.25 end    -- and now and then, an unforced error: long
  hit("cpu", shot(ball.x, ball.y, ball.z, tx, tz, T))
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
        if bounces[side] >= 2 then award(last, "two bounces") end
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

  -- The paddles. A paddle only plays a ball that has bounced once on its own side.
  if state == "rally" then
    if last == "cpu" and bounces.player == 1 and pz < PADDLE_Z and ball.z >= PADDLE_Z then
      local dx, dy = ball.x - me.x, ball.y - me.y
      if dx * dx + dy * dy <= REACH * REACH then ball.z = PADDLE_Z - 0.01 player_hit() end
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
  -- The mannequin: once a ball is on its way, commit to a shot timed and placed to meet it.
  if state == "rally" and last == "player" and ball.vz < 0 and ball.z < 0 and body.ball_seen ~= bounces and #moves > 0 then
    local bx, by, t = predict(AI_Z)
    if bx then
      body.ball_seen = bounces   -- one decision per incoming ball
      local m = choose(bx, by)
      body.move, body.turn = m, m.turn
      body.tx, body.tz = bx - m.hand[1], AI_Z - m.hand[3]
      anim.layers = {}
      anim:play(m.clip, { loop = false, fade = 0, time = math.max(0, CONTACT - t) })
    end
  end
  body.x, body.z = lib.approach(body.x, body.tx, 6, dt), lib.approach(body.z, body.tz, 6, dt)

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
  else
    for _ = 1, 2 do step_ball(dt / 2) end                -- half steps, so a fast ball can't skip the net
  end
end

function update(dt)
  lib.update(dt)
  anim:update(dt)
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
  elseif state == "serve" and server == "cpu" and timer <= 0 then
    hit("cpu", shot(ball.x, ball.y, ball.z, (math.random() * 2 - 1) * 0.55, 0.6 + math.random() * 0.6, 0.7))
  elseif state == "point" and timer <= 0 then
    start_serve()
  elseif state == "over" and pressed then
    new_game()
  end

  -- A fading trail behind the ball.
  table.insert(trail, 1, { ball.x, ball.y, ball.z })
  if #trail > 14 then table.remove(trail) end
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
    model.draw(dummy, body.x, 0, body.z, q[1], q[2], q[3], q[4], 1, 1, 1, 1, 1, 1, 1, anim.animator)
    for _, hand in ipairs(hands) do
      local x, y, z, qx, qy, qz, qw = model.joint(dummy, anim.animator, hand.bone)
      local wx, wy, wz = rotate(q, x, y, z)
      local t = qmul(qmul(q, { qx, qy, qz, qw }), hand.offset)
      model.draw(bat_model, body.x + wx, wy, body.z + wz, t[1], t[2], t[3], t[4], 1, 1, 1, 1, 1, 1, 1, nil, hand.node)
    end
  end
  gfx.blend("alpha")
  -- Paddles: the computer's in blue, yours in red and slightly see-through so it never hides the ball.
  if #moves == 0 then paddle(cpu.x, cpu.y, AI_Z, 0.2, 0.45, 1.0, 1) end   -- no mannequin clips: the old blue paddle
  paddle(me.x, me.y, PADDLE_Z, 1.0, 0.25, 0.3, 0.8)
  gfx.blend("opaque")

  -- Ball and a fading trail of smaller balls behind it.
  gfx.unlit(true)
  gfx.sphere(ball.x, ball.y, ball.z, R * 2, 1.0, 0.95, 0.85)
  gfx.blend("alpha")
  for i, p in ipairs(trail) do
    if i > 1 then
      local fade = 1 - i / (#trail + 1)
      gfx.sphere(p[1], p[2], p[3], R * 2 * fade, 1.0, 0.5 + 0.3 * fade, 0.8, 0.45 * fade)
    end
  end
  gfx.blend("opaque")
  gfx.unlit(false)

  -- Score and messages.
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
  if center then gfx.text(center, (w - gfx.text_width(center, 26)) / 2, h * 0.42, 26, 1, 1, 1) end
  if state ~= "over" and sys.real_time() - message_time < 1.6 then
    gfx.text(message, (w - gfx.text_width(message, 30)) / 2, h * 0.3, 30, 1, 0.85, 0.55)
  end
  gfx.text("HOLD THE MOUSE AND MOVE   -   WASD", left + 80, h - 60, 16, 0.55, 0.55, 0.7)
end
