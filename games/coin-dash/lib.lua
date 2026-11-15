-- Helpers written in Lua on top of the engine. Use them with:  local lib = include("lib.lua")
-- This file belongs to the game: change it freely. It is sent to the phone like any other game file.

local lib = {}

-- ---------------------------------------------------------------- numbers and vectors

function lib.clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end
function lib.lerp(a, b, t) return a + (b - a) * t end
-- Moves a towards b by a fraction that does not depend on the frame rate. rate: bigger is snappier.
function lib.approach(a, b, rate, dt) return b + (a - b) * math.exp(-rate * dt) end

function lib.length(x, y, z) return math.sqrt(x * x + y * y + (z or 0) ^ 2) end
function lib.normalize(x, y, z)
  local len = math.sqrt(x * x + y * y + z * z)
  if len < 1e-9 then return 0, 0, 0 end
  return x / len, y / len, z / len
end
function lib.cross(ax, ay, az, bx, by, bz) return ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx end
function lib.dot(ax, ay, az, bx, by, bz) return ax * bx + ay * by + az * bz end

-- ---------------------------------------------------------------- rotations (quaternions: x, y, z, w)

-- A turn of `degrees` about an axis.
function lib.quat_axis(ax, ay, az, degrees)
  ax, ay, az = lib.normalize(ax, ay, az)
  local half = math.rad(degrees) / 2
  local s = math.sin(half)
  return ax * s, ay * s, az * s, math.cos(half)
end

-- Yaw (turn left/right), then pitch (look up/down), then roll, in degrees.
function lib.quat_euler(yaw, pitch, roll)
  local ax, ay, az, aw = lib.quat_axis(0, 1, 0, yaw or 0)
  local bx, by, bz, bw = lib.quat_axis(1, 0, 0, pitch or 0)
  local cx, cy, cz, cw = lib.quat_axis(0, 0, 1, roll or 0)
  ax, ay, az, aw = lib.quat_mul(ax, ay, az, aw, bx, by, bz, bw)
  return lib.quat_mul(ax, ay, az, aw, cx, cy, cz, cw)
end

-- The turn b followed by the turn a.
function lib.quat_mul(ax, ay, az, aw, bx, by, bz, bw)
  return aw * bx + ax * bw + ay * bz - az * by,
         aw * by - ax * bz + ay * bw + az * bx,
         aw * bz + ax * by - ay * bx + az * bw,
         aw * bw - ax * bx - ay * by - az * bz
end

function lib.quat_rotate(qx, qy, qz, qw, x, y, z)
  local tx, ty, tz = lib.cross(qx, qy, qz, x, y, z)
  tx, ty, tz = tx * 2, ty * 2, tz * 2
  local cx, cy, cz = lib.cross(qx, qy, qz, tx, ty, tz)
  return x + qw * tx + cx, y + qw * ty + cy, z + qw * tz + cz
end

-- Part of the way from one rotation to another, the short way round.
function lib.quat_slerp(ax, ay, az, aw, bx, by, bz, bw, t)
  local d = ax * bx + ay * by + az * bz + aw * bw
  if d < 0 then bx, by, bz, bw, d = -bx, -by, -bz, -bw, -d end
  local ka, kb = 1 - t, t
  if d < 0.9995 then
    local angle = math.acos(d)
    ka, kb = math.sin((1 - t) * angle) / math.sin(angle), math.sin(t * angle) / math.sin(angle)
  end
  local x, y, z, w = ax * ka + bx * kb, ay * ka + by * kb, az * ka + bz * kb, aw * ka + bw * kb
  local len = math.sqrt(x * x + y * y + z * z + w * w)
  return x / len, y / len, z / len, w / len
end

-- The rotation that points an object's forward (-z) along a direction.
function lib.quat_look(dx, dy, dz)
  local yaw = math.deg(math.atan(-dx, -dz))
  local pitch = math.deg(math.atan(dy, math.sqrt(dx * dx + dz * dz)))
  return lib.quat_euler(yaw, pitch, 0)
end

-- Places a child that sits at (x, y, z, rotation) inside a parent. Returns the child's world position and rotation.
-- Use it to attach a sword to a hand: the parent is the character times the hand joint from model.joint.
function lib.attach(px, py, pz, pqx, pqy, pqz, pqw, pscale, x, y, z, qx, qy, qz, qw)
  local wx, wy, wz = lib.quat_rotate(pqx, pqy, pqz, pqw, x * pscale, y * pscale, z * pscale)
  local rx, ry, rz, rw = lib.quat_mul(pqx, pqy, pqz, pqw, qx or 0, qy or 0, qz or 0, qw or 1)
  return px + wx, py + wy, pz + wz, rx, ry, rz, rw
end

-- ---------------------------------------------------------------- paths

-- A smooth curve through a list of points {{x, y, z}, ...}. t runs from 0 to 1 along the whole path.
function lib.spline(points, t)
  local n = #points
  local f = lib.clamp(t, 0, 1) * (n - 1)
  local i = math.min(math.floor(f), n - 2)
  local k = f - i
  local function p(index) return points[lib.clamp(index, 1, n)] end
  local a, b, c, d = p(i), p(i + 1), p(i + 2), p(i + 3)
  local out = {}
  for axis = 1, 3 do
    local p0, p1, p2, p3 = a[axis], b[axis], c[axis], d[axis]
    out[axis] = 0.5 * ((2 * p1) + (-p0 + p2) * k + (2 * p0 - 5 * p1 + 4 * p2 - p3) * k * k + (-p0 + 3 * p1 - 3 * p2 + p3) * k * k * k)
  end
  return out[1], out[2], out[3]
end

-- ---------------------------------------------------------------- time: timers and scripts that wait

local tasks = {}

-- Runs a function as a script that can pause itself:
--   lib.run(function() door.open = true  lib.wait(2)  door.open = false end)
function lib.run(fn)
  local task = { co = coroutine.create(fn), wake = 0 }
  tasks[#tasks + 1] = task
  return task
end

function lib.wait(seconds) coroutine.yield(seconds or 0) end
function lib.wait_until(test) while not test() do coroutine.yield(0) end end
function lib.after(seconds, fn) return lib.run(function() lib.wait(seconds) fn() end) end
function lib.cancel(task) task.dead = true end

-- Call once a frame from update(dt).
function lib.update(dt)
  for i = #tasks, 1, -1 do
    local task = tasks[i]
    task.wake = task.wake - dt
    if task.dead then
      table.remove(tasks, i)
    elseif task.wake <= 0 then
      local ok, wait = coroutine.resume(task.co)
      if not ok then error(wait, 0) end
      if coroutine.status(task.co) == "dead" then table.remove(tasks, i) else task.wake = wait or 0 end
    end
  end
end

-- ---------------------------------------------------------------- events

local listeners = {}

function lib.on(name, fn)
  listeners[name] = listeners[name] or {}
  table.insert(listeners[name], fn)
end

-- lib.emit("coin", {value = 5}). Every listener gets the table. One that returns true stops the rest.
function lib.emit(name, args)
  for _, fn in ipairs(listeners[name] or {}) do
    if fn(args or {}) then return end
  end
end

function lib.emit_later(seconds, name, args) lib.after(seconds, function() lib.emit(name, args) end) end

-- ---------------------------------------------------------------- state machines

-- local m = lib.machine({ idle = {enter = f, update = f, exit = f}, run = {...} }, "idle")
-- m:go("run")   m:update(dt)   m.state
function lib.machine(states, first)
  local m = { states = states, time = 0 }
  function m:go(name, ...)
    local old = self.states[self.state]
    if old and old.exit then old.exit(self, ...) end
    self.state, self.time = name, 0
    local new = self.states[name]
    if new and new.enter then new.enter(self, ...) end
  end
  function m:update(dt)
    self.time = self.time + dt
    local s = self.states[self.state]
    if s and s.update then s.update(self, dt) end
  end
  m:go(first)
  return m
end

-- ---------------------------------------------------------------- animation player

-- Plays a model's clips with cross-fades, timed events and (optionally) movement taken from the animation.
--   local a = lib.animation(m)          a:play("walk", {fade = 0.2, loop = true, speed = 1})
--   a:event("walk", 0.3, function() ... end)     a:update(dt)     model.draw(m, x,y,z, qx,qy,qz,qw, s,s,s, 1,1,1,1, a.animator)
function lib.animation(m)
  local a = { model = m, animator = model.animator(m), layers = {}, events = {}, durations = {} }
  for i, clip in ipairs(model.clips(m)) do a.durations[clip.name] = clip.duration; a.durations[i] = clip.duration end

  function a:play(clip, options)
    options = options or {}
    local top = self.layers[#self.layers]
    if top and top.clip == clip and not options.restart then return end
    local fade = options.fade or 0.2
    self.layers[#self.layers + 1] = { clip = clip, time = 0, weight = (fade <= 0 or not top) and 1 or 0, fade = fade,
                                      loop = options.loop ~= false, speed = options.speed or 1 }
  end

  function a:event(clip, time, fn)
    self.events[clip] = self.events[clip] or {}
    table.insert(self.events[clip], { time = time, fn = fn })
  end

  -- True once a clip that does not loop has reached its end.
  function a:finished()
    local top = self.layers[#self.layers]
    return top ~= nil and not top.loop and top.time >= (self.durations[top.clip] or 0)
  end

  function a:update(dt)
    local count = #self.layers
    local args = {}
    for i, layer in ipairs(self.layers) do
      local duration = math.max(self.durations[layer.clip] or 0, 0.0001)
      local before = layer.time
      layer.time = layer.time + dt * layer.speed
      if i == count then
        for _, e in ipairs(self.events[layer.clip] or {}) do
          local t = layer.loop and layer.time % duration or layer.time
          local b = layer.loop and before % duration or before
          if (b < e.time and t >= e.time) or (t < b and (e.time > b or e.time <= t)) then e.fn() end
        end
        layer.weight = layer.fade > 0 and math.min(1, layer.weight + dt / layer.fade) or 1
      end
      local t = layer.loop and layer.time % duration or math.min(layer.time, duration)
      args[#args + 1] = layer.clip; args[#args + 1] = t; args[#args + 1] = layer.weight
    end
    -- Once the newest clip has fully faded in, the older ones are no longer needed.
    if count > 1 and self.layers[count].weight >= 1 then self.layers = { self.layers[count] } end
    if #args > 0 then model.pose(self.model, self.animator, table.unpack(args)) end
  end

  -- How far a node (the hips) moves in the clip between two times. Add it to the character's own position
  -- and pin the node with model.local so the feet do not slide.
  function a:root_motion(clip, node, from, to)
    local ax, ay, az = model.sample(self.model, clip, node, from)
    local bx, by, bz = model.sample(self.model, clip, node, to)
    return bx - ax, by - ay, bz - az
  end

  return a
end

-- ---------------------------------------------------------------- sound

lib.MUSIC, lib.SFX, lib.VOICE = 1, 2, 3   -- group numbers for audio.play and audio.group

-- A set of takes of one sound, played in shuffled order so the same one never comes twice in a row.
--   local step = lib.cue({"step1.wav", "step2.wav", "step3.wav"}, {volume = -6, pitch_spread = 0.08})     step:play()
function lib.cue(files, options)
  options = options or {}
  local cue = { sounds = {}, bag = {} }
  for i, name in ipairs(files) do cue.sounds[i] = audio.load(name) end
  function cue:play(x, y, z)
    if #self.bag == 0 then
      for i = 1, #self.sounds do if i ~= self.last or #self.sounds == 1 then self.bag[#self.bag + 1] = i end end
    end
    local pick = table.remove(self.bag, math.random(#self.bag))
    self.last = pick
    local spread = options.pitch_spread or 0
    local voice = audio.play(self.sounds[pick], options.volume or 0, 1 + (math.random() * 2 - 1) * spread, false, options.group or lib.SFX)
    if x then audio.position(voice, x, y, z, options.min or 2, options.max or 40) end
    return voice
  end
  return cue
end

-- Lowers one group while something important plays, then brings it back:  lib.duck(lib.MUSIC, -12, 2.5)
function lib.duck(group, db, seconds)
  audio.group(group, db, 0.25)
  lib.after(seconds, function() audio.group(group, 0, 0.6) end)
end

-- ---------------------------------------------------------------- words in more than one language

-- lib.words({ en = {start = "START"}, fr = {start = "JOUER"} }, "en")     lib.tr("start")
local strings, language = {}, "en"
function lib.words(tables, lang) strings, language = tables, lang or language end
function lib.language(lang) language = lang end
function lib.tr(id)
  local text = (strings[language] or {})[id] or (strings.en or {})[id]
  return text or ("?" .. id)   -- a missing string shows up on screen instead of hiding
end

-- ---------------------------------------------------------------- particles

-- A simple emitter. e:burst(n, x, y, z)   e:update(dt)   e:draw()
function lib.emitter(options)
  options = options or {}
  local e = { list = {}, flat = {} }
  local function spread(v) return (math.random() * 2 - 1) * v end
  function e:burst(n, x, y, z)
    local speed, c = options.speed or 3, options.color or { 1, 1, 1 }
    for _ = 1, n do
      self.list[#self.list + 1] = { x = x, y = y, z = z, vx = spread(speed), vy = math.random() * speed, vz = spread(speed),
                                    life = options.life or 1, age = 0, size = options.size or 0.2, r = c[1], g = c[2], b = c[3] }
    end
  end
  function e:update(dt)
    local gravity = options.gravity or -6
    for i = #self.list, 1, -1 do
      local p = self.list[i]
      p.age = p.age + dt
      if p.age >= p.life then
        table.remove(self.list, i)
      else
        p.vy = p.vy + gravity * dt
        p.x, p.y, p.z = p.x + p.vx * dt, p.y + p.vy * dt, p.z + p.vz * dt
      end
    end
  end
  function e:draw()
    local flat, n = self.flat, 0
    for _, p in ipairs(self.list) do
      local fade = 1 - p.age / p.life
      flat[n + 1], flat[n + 2], flat[n + 3], flat[n + 4] = p.x, p.y, p.z, p.size * (0.5 + 0.5 * fade)
      flat[n + 5], flat[n + 6], flat[n + 7], flat[n + 8] = p.r, p.g, p.b, fade
      n = n + 8
    end
    if n > 0 then
      if options.texture then gfx.texture(1, options.texture) end
      gfx.blend(options.blend or "alpha")
      gfx.particles(flat, n // 8)
      gfx.blend("opaque")
      if options.texture then gfx.texture(1, nil) end
    end
  end
  return e
end

-- ---------------------------------------------------------------- loading with a progress bar

-- Loads a list of things one per frame, so draw() can show progress instead of the screen freezing.
--   local loading = lib.loader({ {"tex", gfx.load_texture, "grass.png"}, {"hero", model.load, "hero.glb"} }, assets)
--   in update: if loading then loading = loading:step() end        in draw: loading.done / loading.total
function lib.loader(list, into)
  local l = { done = 0, total = #list }
  function l:step()
    local item = list[self.done + 1]
    if not item then return nil end
    into[item[1]] = item[2](table.unpack(item, 3))
    self.done = self.done + 1
    return self.done < self.total and self or nil
  end
  return l
end

-- ---------------------------------------------------------------- debugging

-- A box drawn in lines: centre and full sizes.
function lib.wire_box(x, y, z, sx, sy, sz, r, g, b)
  local hx, hy, hz = sx / 2, sy / 2, sz / 2
  for i = 0, 3 do
    local a, c = (i % 2 == 0) and -1 or 1, (i < 2) and -1 or 1
    gfx.line(x - hx, y + a * hy, z + c * hz, x + hx, y + a * hy, z + c * hz, r, g, b, 1)
    gfx.line(x + a * hx, y - hy, z + c * hz, x + a * hx, y + hy, z + c * hz, r, g, b, 1)
    gfx.line(x + a * hx, y + c * hy, z - hz, x + a * hx, y + c * hy, z + hz, r, g, b, 1)
  end
end

-- Text that floats at a point in the world.
function lib.label(text, x, y, z, size, r, g, b)
  local sx, sy = gfx.project(x, y, z)
  if sx then gfx.text(text, sx - gfx.text_width(text, size or 16) / 2, sy, size or 16, r or 1, g or 1, b or 1) end
end

-- After this, using a global that was never set is an error instead of a silent nil. Catches typos.
function lib.strict()
  setmetatable(_G, {
    __newindex = function(t, k, v) rawset(t, k, v) end,
    __index = function(_, k) error("the global '" .. tostring(k) .. "' was never set (a typo?)", 2) end,
  })
end

return lib
