
-- Dream Table self-play test, appended to main.lua by selftest.py: it swipes back at every robot shot and serves when it holds the ball.
-- It feeds fake touches through input.touches and reads only ball and game.score, with a fixed random seed,
-- so the trace hash it prints is the same on every run until gameplay changes.
;(function()
  local STEPS = tonumber(PROBE_STEPS) or 20000
  local step, clock = 0, 0
  sys.real_time = function() return clock end           -- repeatable: real time is the step count
  sys.time_scale = function() end                       -- no hit-stop
  save.set = function() end                             -- never touch the saved wins
  math.randomseed(7)
  local rnd = 12345
  local function r() rnd = (rnd * 1103515245 + 12345) % 2147483648 return rnd / 2147483648 end
  local queue, still, last_vz, toss_at = {}, 0, 0, nil
  local returns, hash, trace = 0, 0, {}
  local swipes, last_swipe = 0, 0
  local function swipe(delay)
    local w, h = gfx.size()
    local a = r() * 2 * math.pi
    local x, y = w * 0.75, h * 0.6
    local dx, dy = math.cos(a) * 60, -math.sin(a) * 60
    local id = 100 + swipes
    swipes = swipes + 1
    last_swipe = step + delay
    queue[#queue + 1] = { at = step + delay, t = { id = id, phase = "down", x = x, y = y, dx = 0, dy = 0 } }
    for k = 1, 4 do
      queue[#queue + 1] = { at = step + delay + k, t = { id = id, phase = "move", x = x + dx * k, y = y + dy * k, dx = dx, dy = dy } }
    end
    queue[#queue + 1] = { at = step + delay + 5, t = { id = id, phase = "up", x = x + dx * 4, y = y + dy * 4, dx = 0, dy = 0 } }
  end
  input.touches = function()
    local out = {}
    for i = #queue, 1, -1 do
      if queue[i].at <= step then table.insert(out, 1, queue[i].t) table.remove(queue, i) end
    end
    return out
  end
  local base_fixed = fixed_update
  function fixed_update(dt)
    step = step + 1
    clock = clock + dt
    base_fixed(dt)
    local moving = math.abs(ball.vx) + math.abs(ball.vy) + math.abs(ball.vz) > 0.01
    still = moving and 0 or still + 1
    if still == 60 then swipe(0) toss_at = step end                   -- your serve's toss (or a restart)
    if toss_at and step == toss_at + 45 then swipe(0) end
    if step - last_swipe > 400 then swipe(0) end                       -- game over: tap to play again             -- and its swing
    if last_vz <= 0 and ball.vz > 0.5 then swipe(12 + math.floor(r() * 20)) end   -- the robot hit: swipe back
    if last_vz > 0.5 and ball.vz < -0.5 then returns = returns + 1 end
    last_vz = ball.vz
    if step % 30 == 0 then
      local s = string.format("%.3f %.3f %.3f %d %d", ball.x, ball.y, ball.z, game.score.you, game.score.robot)
      for i = 1, #s do hash = (hash * 31 + s:byte(i)) % 4294967296 end
      if step % 3000 == 0 then trace[#trace + 1] = s end
    end
    if step == STEPS then
      print(string.format("PROBE steps=%d returns=%d swipes=%d hash=%08x", STEPS, returns, swipes, hash))
      for _, s in ipairs(trace) do print("PROBE trace " .. s) end
      local b = sys.budget()
      print(string.format("PROBE budget worst: see report"))
      budget.report()
    end
  end
end)()
