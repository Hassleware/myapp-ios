-- The ball and the rules: its flight, bounces and the net (step_ball), aiming shots and serves, hits, points,
-- serving turns and new games.
-- Made by main.lua: include("rally.lua")(game) returns the functions below; the shared game table carries
-- everything it uses from elsewhere (listed at the top) and the state the files share (game.state, game.score...).
return function(game)
-- from main.lua
local BALL_R, BOUNCE, FANTASY, FEEL, GRAVITY = game.BALL_R, game.BOUNCE, game.FANTASY, game.FEEL, game.GRAVITY
local HALF_L, HALF_W, NET_TOP, REACH, SERVE = game.HALF_L, game.HALF_W, game.NET_TOP, game.REACH, game.SERVE
local TABLE_Y, ball, lib, robot, spark, you = game.TABLE_Y, game.ball, game.lib, game.robot, game.spark, game.you
-- from vec.lua
local len, sub, v = game.len, game.sub, game.v
-- from serve_meter.lua
local rate_serve = game.rate_serve
-- from planner.lua
local clear_plans, start_hint = game.clear_plans, game.start_hint
-- from view.lua
local clear_trail, show_message = game.clear_trail, game.show_message

local serve_ball, serve_bounced = false, false   -- the ball in play is a serve, and it has bounced on the server's side

-- ---------------------------------------------------------------- messages, camera

local function side_of(z) return z > 0 and "you" or "robot" end
local function other(who) return who == "you" and "robot" or "you" end

-- ---------------------------------------------------------------- shots

-- The velocity that carries the ball from p to land at (tx, tz) on the table in about T seconds,
-- raising the arc until it clears the net.
local function velocity_to_land(px, py, pz, tx, tz, T)
  local vx, vy, vz
  for _ = 1, 30 do
    vx, vz = (tx - px) / T, (tz - pz) / T
    vy = (TABLE_Y + BALL_R - py + 0.5 * GRAVITY * T * T) / T
    local tn = -pz / vz                                  -- when it reaches the net
    if py + vy * tn - 0.5 * GRAVITY * tn * tn > NET_TOP + BALL_R + 0.04 then break end
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
    vy = vy - GRAVITY * dt
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if oz * z <= 0 and oz ~= z and y - BALL_R < NET_TOP and math.abs(x) < HALF_W + 0.15 then return false end   -- the net
    if y - BALL_R <= TABLE_Y and vy < 0 and oy - BALL_R >= TABLE_Y - 0.02 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      if z * own > 0 then
        if first then return false end                   -- twice on its own side
        first = true
      else
        return first                                     -- on the other side: good only after its own side
      end
      y, vy, vx, vz = TABLE_Y + BALL_R, -vy * BOUNCE, vx * 0.97, vz * 0.97
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
      local vx, vy, vz = (lx - px) / T, (TABLE_Y + BALL_R - py + 0.5 * GRAVITY * T * T) / T, (own * land - pz) / T
      local bvx, bvy, bvz = vx * 0.97, -(vy - GRAVITY * T) * BOUNCE, vz * 0.97   -- just after the bounce on its own side
      local tn = -own * land / bvz                                          -- reaching the net
      local t2 = 2 * bvy / GRAVITY                                                -- back down on the table
      if tn > 0 then
        local over = TABLE_Y + BALL_R + bvy * tn - 0.5 * GRAVITY * tn * tn
        local z2, x2 = own * land + bvz * t2, lx + bvx * t2
        if over > NET_TOP + BALL_R + 0.03 and z2 * own < -0.2 and math.abs(z2) < HALF_L - 0.1 and math.abs(x2) < HALF_W - 0.1 then
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

local function launch_ball(by, vx, vy, vz, serving)
  ball.vx, ball.vy, ball.vz = vx, vy, vz
  ball.call, ball.kick, ball.curve = nil, nil, nil
  game.last_hitter, game.bounces = by, { you = 0, robot = 0 }
  serve_ball, serve_bounced = serving or false, false
  game.state = "rally"
  audio.play(FEEL.hit, by == "you" and 1 or 0.7, by == "you" and 1 or 1.08)
  FEEL.stop_until = sys.real_time() + (by == "you" and FEEL.stop or FEEL.stop * 0.6)
  FEEL.kick = by == "you" and FEEL.shake or FEEL.shake * 0.4
  sys.time_scale(0)
  if by == "you" then input.haptic("medium") end
  FEEL.landing = nil
end

-- Where the robot's ball will first bounce on your side, played through the ball's own physics (sidespin too).
local function predict_landing()
  FEEL.landing = nil
  do
    local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
    for _ = 1, 400 do
      vy = vy - GRAVITY / 120
      vx = vx + (ball.curve or 0) / 120
      local py = y
      x, y, z = x + vx / 120, y + vy / 120, z + vz / 120
      if y - BALL_R <= TABLE_Y and py - BALL_R >= TABLE_Y - 0.02 and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
        if z > 0 then FEEL.landing = { x, z } end
        break
      end
      if y < 0 then break end
    end
  end
end

-- Your shot, aimed by the line you drew: it goes the way the line went across, longer lines go deeper and faster.
local function your_hit(serving)
  local line = you.aim or { right = 0, up = 0, length = 200 }
  local power = serving and you.serve_power or lib.clamp(line.length / 380, 0.25, 1)   -- a serve: the meter's charge
  local tx = lib.clamp(line.right * 0.65, -(HALF_W - 0.1), HALF_W - 0.1)
  local tz = -(0.45 + 0.8 * power)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.5 + 0.6 * power)
  if serving then rate_serve(you.serve_rating or "okay") end
  game.hint = nil
  if served then launch_ball("you", served[1], served[2], served[3], true)
  else launch_ball("you", velocity_to_land(ball.x, ball.y, ball.z, tx, tz, 0.85 - 0.35 * power)) end
  you.swing_planned, you.body.sink, you.body.goal = false, 0, nil
  if power > 0.85 and line.up < -0.3 then show_message("SMASH!") end
end

-- The robot is polite and can't lose: most returns (and every serve) are a gentle ball landing mid-table on your
-- half, on your side of it. But it's dream table tennis, so some returns call for your fantasy moves:
--   a sky ball: a tall lob landing deep that bounces up over your head, to be played in the air (jump!);
--   a wide ball: landing by the side line away from you, it kicks out sideways off its bounce (dash!).
local function robot_hit(serving)
  local tx = lib.clamp(you.body.x * 0.6 + (math.random() - 0.5) * 0.2, -(HALF_W - 0.25), HALF_W - 0.25)
  local served = serving and serve_shot(ball.x, ball.y, ball.z, tx, 0.8)
  local roll = math.random()
  if served then launch_ball("robot", served[1], served[2], served[3], true)
  elseif roll < FANTASY.sky_chance then
    launch_ball("robot", velocity_to_land(ball.x, ball.y, ball.z, tx, 1.15, 1.7))
    ball.call = "jump"
    show_message("SKY BALL - JUMP!")
  elseif roll < FANTASY.sky_chance + FANTASY.wide_chance then
    -- A banana shot: aimed in from the side line by as far as the sidespin will carry it, so it bends out
    -- through the air, lands by the side line away from you, and keeps curving after the bounce.
    local side = you.body.x > 0 and -1 or 1                      -- away from you
    local land, T = side * (HALF_W - 0.12), 0.7
    for _ = 1, 2 do                                              -- velocity_to_land() may lengthen the flight to clear the net
      local vx, vy, vz = velocity_to_land(ball.x, ball.y, ball.z, land - side * 0.5 * FANTASY.wide_curve * T * T, 0.85, T)
      T = (0.85 - ball.z) / vz
    end
    launch_ball("robot", velocity_to_land(ball.x, ball.y, ball.z, land - side * 0.5 * FANTASY.wide_curve * T * T, 0.85, T))
    ball.curve, ball.kick, ball.call = side * FANTASY.wide_curve, side * FANTASY.wide_kick, "dash"   -- a call only: any way that reaches it will do
    show_message("WIDE BALL - DASH!")
  else launch_ball("robot", velocity_to_land(ball.x, ball.y, ball.z, tx, 0.7 + math.random() * 0.2, 0.8)) end
  robot.swing_planned, robot.body.sink, robot.body.goal = false, 0, nil
  if not serving then predict_landing() end
  start_hint()
end

-- ---------------------------------------------------------------- points and serving

local function serve_turn()
  local total = game.score.you + game.score.robot
  if game.score.you >= 10 and game.score.robot >= 10 then return total % 2 == 0 and "you" or "robot" end
  return math.floor(total / 2) % 2 == 0 and "you" or "robot"
end

local function award(who, why)
  if game.state ~= "rally" then return end
  game.score[who] = game.score[who] + 1
  show_message((who == "you" and "YOUR POINT" or "ROBOT POINT") .. "  -  " .. why)
  if who == "you" then input.haptic("success") end
  local a, b = game.score[who], game.score[other(who)]
  if a >= 11 and a - b >= 2 then
    game.state = "over"
    if who == "you" then game.wins = game.wins + 1 save.set("wins", game.wins) end
    show_message(who == "you" and "YOU WIN!" or "THE ROBOT WINS")
  else
    game.state, game.timer = "point", 1.2
  end
end

local function start_serve()
  game.server, game.state, game.timer, game.last_hitter = serve_turn(), "serve", 1.0, nil
  ball.vx, ball.vy, ball.vz = 0, 0, 0
  ball.call, ball.kick, ball.curve = nil, nil, nil      -- a sky or wide ball's call ends with its point
  FEEL.landing = nil
  game.hint = nil
  you.swing_planned, robot.swing_planned, you.waiting_line = false, false, nil
  for _, m in ipairs({ you, robot }) do                 -- back to their spots behind the table to serve or receive
    m.body.tx, m.body.tz, m.body.goal = lib.clamp(m.body.x, -0.6, 0.6), m.sign * (HALF_L + 0.6), nil
  end
  clear_plans()
  clear_trail()
end

local function new_game()
  game.score = { you = 0, robot = 0 }
  start_serve()
  show_message("FIRST TO 11")
end

-- ---------------------------------------------------------------- the ball

local function step_ball(dt)
  local px, py, pz = ball.x, ball.y, ball.z
  ball.vy = ball.vy - (game.state == "toss" and SERVE.g or GRAVITY) * dt   -- a tossed ball floats until it's hit
  ball.vx = ball.vx + (ball.curve or 0) * dt                      -- a wide ball's sidespin
  ball.x, ball.y, ball.z = ball.x + ball.vx * dt, ball.y + ball.vy * dt, ball.z + ball.vz * dt

  -- The net: below its top, the ball stops and drops back.
  if pz * ball.z <= 0 and pz ~= ball.z and ball.y - BALL_R < NET_TOP and math.abs(ball.x) < HALF_W + 0.15 then
    ball.z = pz > 0 and BALL_R or -BALL_R
    ball.vz, ball.vx = -ball.vz * 0.15, ball.vx * 0.4
    if game.state == "rally" then show_message("NET") end
  end

  -- The table.
  if ball.y - BALL_R <= TABLE_Y and ball.vy < 0 and py - BALL_R >= TABLE_Y - 0.02 and math.abs(ball.x) <= HALF_W and math.abs(ball.z) <= HALF_L then
    audio.play(FEEL.bounce, math.min(1, 0.3 + math.abs(ball.vy) * 0.15), 1)
    if ball.z > 0 then FEEL.landing = nil end
    ball.y, ball.vy = TABLE_Y + BALL_R, -ball.vy * BOUNCE
    ball.vx, ball.vz = ball.vx * 0.97, ball.vz * 0.97
    local side = side_of(ball.z)
    if ball.kick and side == "you" then ball.vx, ball.kick = ball.vx + ball.kick, nil end   -- a wide ball kicks
    if game.state == "rally" then
      local own_side = side == side_of(game.last_hitter == "you" and 1 or -1)
      if serve_ball and not serve_bounced then
        if own_side then serve_bounced = true                       -- a serve: first on the server's own side
        else award(other(game.last_hitter), "a serve must bounce on your own side first") end
      elseif own_side then
        award(other(game.last_hitter), "it bounced on the hitter's own side")
      else
        serve_ball = false
        game.bounces[side] = game.bounces[side] + 1
        if game.bounces[side] >= 2 and side == "robot" then
          spark(ball.x, ball.y, ball.z) robot_hit()    -- the robot can't lose: a short ball is still returned
        elseif game.bounces[side] >= 2 then award(game.last_hitter, "two bounces") end
      end
    end
  end

  -- The floor, or gone for good.
  if ball.y - BALL_R <= 0 then
    ball.y, ball.vy = BALL_R, -ball.vy * 0.5
    if game.state == "rally" then
      if game.bounces[other(game.last_hitter)] >= 1 then award(game.last_hitter, "not returned") else award(other(game.last_hitter), "out") end
    end
  end

  local function touching(m) return len(sub(v(ball.x, ball.y, ball.z), m.body.blade)) <= (m == you and REACH.you or REACH.robot) end

  -- Serves: the tossed ball is hit when it meets the server's bat on the way down; dropped, it's served again.
  if game.state == "toss" then
    local m = game.server == "robot" and robot or you
    if ball.vy < 0 and touching(m) and (m == robot or you.swing_planned) then   -- yours only once you've swung
      spark(ball.x, ball.y, ball.z)
      if m == robot then
        robot_hit(true)
        robot.serve_hand = robot.serve_hand == "R" and "L" or "R"   -- next serve with the other hand
      else
        your_hit(true)
      end
    elseif ball.y - BALL_R <= 0.02 then
      if game.server == "you" then                                     -- a missed serve is the other side's point
        game.state, game.last_hitter = "rally", "you"
        award("robot", "missed serve")
        rate_serve("fail")
      else
        start_serve()
      end
    end
  end

  -- Returns: a bat only plays a ball that has bounced once on its own side.
  if game.state == "rally" then
    if game.last_hitter == "robot" and game.bounces.you == 1 and you.swing_planned and touching(you) then
      spark(ball.x, ball.y, ball.z) your_hit()
    elseif game.last_hitter == "you" and game.bounces.robot == 1 then
      -- The robot's bat should meet the ball; if its swing ever misses, it still returns it before the floor.
      if touching(robot) or (ball.vy < 0 and ball.y < 0.35) then spark(ball.x, ball.y, ball.z) robot_hit() end
    end
  end
end

return {
  launch_ball = launch_ball, your_hit = your_hit, robot_hit = robot_hit, serve_turn = serve_turn,
  award = award, start_serve = start_serve, new_game = new_game, step_ball = step_ball,
  side_of = side_of, other = other,
}
end
