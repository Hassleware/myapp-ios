-- Swing planning: where on the ball's flight a mannequin can meet it, which move, where to be and when to
-- start the clip (plan_swing), for the robot, for your drawn lines, and as hints. Plans run as coroutines
-- spread over a few fixed steps so no frame goes over the Lua budget (budget.lua).
-- Made by main.lua: include("planner.lua")(game) returns the functions below; the shared game table carries
-- everything it uses from elsewhere (listed at the top) and the state the files share (game.state, game.score...).
return function(game)
-- from main.lua
local AIR_DRIFT, BALL_R, BOUNCE, COURT_BACK = game.AIR_DRIFT, game.BALL_R, game.BOUNCE, game.COURT_BACK
local COURT_X, CROUCH, CUE, DASH, GRAVITY = game.COURT_X, game.CROUCH, game.CUE, game.DASH, game.GRAVITY
local HALF_L, HALF_W, JET, JUMP_COST, REACH = game.HALF_L, game.HALF_W, game.JET, game.JUMP_COST, game.REACH
local SERVE, TABLE_Y, TOP_SPEED, ball = game.SERVE, game.TABLE_Y, game.TOP_SPEED, game.ball
local blade_offset, dash, jump_chart, robot, you = game.blade_offset, game.dash, game.jump_chart, game.robot, game.you
-- from skeleton.lua
local BLEND_TIME, CONTACT, keep_pose, moves = game.BLEND_TIME, game.CONTACT, game.keep_pose, game.moves

-- The ball's flight from now on: the points where mannequin m may hit it (after one bounce on its side, off the end
-- of the table; or, serving, on the way down before any bounce), with how long until it gets there.
local function hittable_points(m, serving)
  local x, y, z, vx, vy, vz = ball.x, ball.y, ball.z, ball.vx, ball.vy, ball.vz
  local own, dt, out = serving and 0 or (game.bounces[m.side] or 0), 1 / 120, {}
  for step = 1, 300 do
    vy = vy - (serving and game.state == "toss" and SERVE.g or GRAVITY) * dt   -- a toss floats until it's hit
    vx = vx + (ball.curve or 0) * dt                                -- a wide ball's sidespin
    x, y, z = x + vx * dt, y + vy * dt, z + vz * dt
    if y - BALL_R <= TABLE_Y and vy < 0 and math.abs(x) <= HALF_W and math.abs(z) <= HALF_L then
      y, vy, vx, vz = TABLE_Y + BALL_R, -vy * BOUNCE, vx * 0.97, vz * 0.97
      if z * m.sign > 0 then
        own = own + 1
        if own == 1 and ball.kick and m == you then vx = vx + ball.kick end   -- a wide ball kicks out sideways
      end
    end
    if y < BALL_R or own >= 2 then break end
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
--   in the air on the charted jump: already jumping (jump_chart says how high it will be then), or a jump planned to
--   start so the chart's height at contact is the one needed (jump_chart.best_frame); no walking once it's up;
--   with a dash when walking can't get there in time (on the ground or in the air).
-- Jumps and dashes cost a little extra (JUMP_COST, DASH.cost) and are then made by the plan itself. A sky ball
-- (ball.call == "jump") may only be played in the air. The blade's offset is worked out once per move, not per
-- point: this search is the game's biggest piece of Lua work (budget.lua).
local function walk_reach(t) return TOP_SPEED * math.max(0, t - 0.1) end
local function dash_need(run, walk_time, t)    -- false: walking will do; true: a dash will; nil: neither
  if run <= walk_reach(walk_time) then return false end
  if t >= DASH.time and run <= DASH.dist + walk_reach(walk_time - DASH.time) then return true end
  return nil
end
local function plan_swing(m, points, cost_of, hand, lead, peek)
  lead = lead or 0.3                                             -- the least time to swing in (a serve: less)
  local b = m.body
  local f_now = b.jump_t and math.floor(b.jump_t * jump_chart.fps + 0.5) + 1
  local flying = f_now and f_now < jump_chart.land
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
        local still = b.jet.hovering and math.max(0, JET.hover_max - (b.jet.hover_t or 0)) or 0   -- hovering: up there till let go
        local fc = f_now and f_now + math.floor(math.max(0, t - still) * jump_chart.fps + 0.5)
        if flying and jump_chart.air[fc] then                                 -- already jumping, still up then
          local dash = false                                        -- in the air the jet drifts AIR_DRIFT to line up
          if run > AIR_DRIFT then dash = assist and dash_need(run, 0, t) or nil end
          local miss = math.abs(jump_chart.air[fc] - need)
          if dash ~= nil and miss <= (assist and jump_chart.air_tol or REACH.your_air_tol) then
            local cost = cost_of(move, p, miss, run, off) + (dash and DASH.cost or 0)
            if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "air", 0, nil, dash end
          end
        elseif not sky and not (flying and fc < jump_chart.land) then   -- on the ground
          local sink = math.max(-CROUCH, math.min(0, need))
          local dash = dash_need(run, t - busy, t)
          if dash and not assist then dash = nil end                -- it needs a dash you haven't made
          if dash == nil and game.state == "toss" and m == you and run <= SERVE.reach then dash = true end   -- your serve: glide into place
          if dash ~= nil then
            local cost = cost_of(move, p, math.abs(need - sink) - sink * 0.1, run, off) + (dash and DASH.cost or 0)
            if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "ground", sink, nil, dash end
          end
        end
        if assist and not flying and need > 0.2 then               -- a jump, timed to meet it (the robot's)
          local f = jump_chart.best_frame[math.floor(need / 0.02 + 0.5)]
          local delay = f and t - (f - 1) / jump_chart.fps
          if delay and delay >= 0 then
            local dash = dash_need(run, delay, t)
            if dash ~= nil then
              local cost = cost_of(move, p, math.abs(jump_chart.air[f] - need), run, off) + JUMP_COST + (dash and DASH.cost or 0)
              if cost < best_cost then best, at, best_cost, how, best_sink, best_delay, best_dash = move, p, cost, "jump", 0, delay, dash end
            end
          end
        end
      end
    end
    ::next_move::
  end
  if not best then return false end
  if peek then                                                   -- only looking (the hints): report what it would do
    local off = blade_offset(m, best)
    return { x = at.x - off[1], z = at.z - off[3], how = how, dash = best_dash, delay = best_delay, contact = at.t - waited }
  end
  m.used[best] = (m.used[best] or 0) + 1
  keep_pose(m)
  b.move, m.swing_planned = best, true
  local off = blade_offset(m, best)
  local sx, sz = at.x - off[1], at.z - off[3]
  b.tx, b.tz, b.sink, b.goal = sx, sz, best_sink, { sx, sz }
  if best_dash then                                              -- dash toward the spot (then walk the rest)
    local dx, dz = sx - b.x, sz - b.z
    local run = math.sqrt(dx * dx + dz * dz)
    dash(m, dx / run, dz / run, math.min(run, DASH.dist), math.min(DASH.time, (at.t - waited) * 0.7))
  end
  if how == "jump" then b.jump_in = best_delay end
  m.anim.layers = {}
  local t = at.t - waited                                         -- from now, after the time spent planning
  play_swing(m, best.clip, t)
  return true
end

-- The robot's choice: the height should suit the shot, it shouldn't run far, and it likes variety.
local function plan_robot_swing(m, serve_hand)
  return plan_swing(m, hittable_points(m, serve_hand ~= nil), function(move, p, fit, run, off)
    return fit * 3 + run * (serve_hand and 1.5 or 0.3)
           + (m.used[move] or 0) * 0.35 + math.random() * 0.05
  end, serve_hand)
end

-- Hints, once the robot has hit: where to stand for the cleanest return, and whether it calls for a jump (and
-- when) or a dash (and which way). Worked out like your own swing would be, without making it.
local function start_hint()
  game.hint = nil
  start_plan("hint", function()
    return plan_swing(you, hittable_points(you, false), function(move, p, fit, run) return fit * 3 + run * 0.8 end, nil, nil, true)
  end, function(r)
    if not r then return end
    local now = sys.time()
    game.hint = { x = r.x, z = r.z, how = r.how, dash = r.dash, jump_at = r.delay and now + r.delay, contact_at = now + r.contact }
    -- The moments to act, in order: dash soon (a beat to react), then the jump when the plan would. There is no
    -- swing cue: draw your line any time after the robot's shot (it waits, and is planned again when you jump).
    game.hint.moves = {}
    if r.dash then game.hint.moves[#game.hint.moves + 1] = { at = now + CUE.react, kind = "dash" } end
    if r.how == "jump" and r.delay then game.hint.moves[#game.hint.moves + 1] = { at = now + r.delay, kind = "jump" } end
    table.sort(game.hint.moves, function(a, b) return a.at < b.at end)
  end)
end

-- Your choice, from the line you drew: the move whose recorded swing runs most like the line, that can meet the
-- ball without much running.
local function plan_your_swing(m, line, serving)
  return plan_swing(m, hittable_points(m, serving), function(move, p, fit, run, off)
    local likeness = move.swing[1] * line.right + move.swing[2] * line.up   -- 1: the same direction
    return (1 - likeness) * 2.5 + fit * 3 + run * 0.8
  end, nil, serving and SERVE.lead or REACH.your_lead)
end

-- What the rest of the game asks about plans in progress.
local function clear_plans() plans = {} end
local function is_planning(key) return plans[key] ~= nil end
local function cancel_plan(key) plans[key] = nil end

return {
  hittable_points = hittable_points, start_plan = start_plan, run_plans = run_plans, clear_plans = clear_plans,
  is_planning = is_planning, cancel_plan = cancel_plan, play_swing = play_swing, plan_swing = plan_swing,
  plan_robot_swing = plan_robot_swing, plan_your_swing = plan_your_swing, start_hint = start_hint,
}
end
