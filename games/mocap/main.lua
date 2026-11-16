-- Motion capture: the iPhone's back camera tracks a body (ARKit) and records a video at the same time.
-- SEND saves ARKit's take as captures/<name>.bvh at once, and hands the video to SAM 3D Body (Meta) on the PC's
-- graphics card, a stronger model with hands and feet, which writes captures/<name>-sam3d.bvh. The phone shows the PC's progress, then replays
-- that result (REPLAY switches between that and the live camera).
-- Prop the phone up about 3 metres away at waist height, or have someone hold it, and keep the whole body in view.
-- Shots: a voice calls them. "Three, two, one, go" starts the recording; then every PERIOD seconds "ready" and
-- "hit!": play a different shot on each "hit", and "stopped" when it ends. Takes are named shots-1,
-- shots-2...; the PC cuts them into one clip per shot and guesses which shot each is (tools/split_shots.py).
-- Hands free: raise both hands over your head and hold them there for a second to start, and again to stop.

local lib = include("lib.lua")
local FIRST_HIT, PERIOD = 4.0, 4.0   -- keep the same as tools/split_shots.py
local say = {}   -- spoken words, made with Windows' own voice and trimmed to start at once
for _, word in ipairs({ "three", "two", "one", "go", "ready", "hit", "stopped" }) do say[word] = audio.load(word .. ".wav") end
local READY_BEFORE = 1.2    -- "ready" comes this many seconds before "hit"
local next_beep = 0
local HANDS_UP = 0.8        -- metres above the hips that both hands must be, held for HOLD seconds, to start or stop
local HOLD = 1.0
local held, latched = 0, false

local started, why_not = false, nil
local state = "idle"          -- idle, countdown, recording, sending, processing
local countdown, record_start = 0, 0
local status = ""
local takes = math.floor(save.get("takes", 0))
local pose, parents, tracked = {}, {}, false
local result, showing_result, result_start = nil, false, 0   -- the PC's result: { fps, parents, frames }
local buttons
local CAMERA_ALPHA = 0.2                  -- how strongly the camera picture shows behind the skeleton
local FLIP_X, FLIP_Y = false, false       -- if the picture comes out mirrored or upside down, flip it here

local function begin_camera()
  started, why_not = mocap.start()
  status = started and "Looking for a body..." or why_not
end

-- "fps 30 joints 22", then the parents, then x y z for every joint, one frame a line.
local function read_result(text)
  local lines = {}
  for line in text:gmatch("[^\n]+") do lines[#lines + 1] = line end
  local motion = { fps = tonumber(lines[1]:match("fps (%d+)")) or 30, parents = {}, frames = {} }
  for p in lines[2]:gmatch("%S+") do motion.parents[#motion.parents + 1] = tonumber(p) + 1 end   -- from 1; 0 is none
  for i = 3, #lines do
    local frame = {}
    for v in lines[i]:gmatch("%S+") do frame[#frame + 1] = tonumber(v) end
    motion.frames[#motion.frames + 1] = frame
  end
  return motion
end

-- Asks the PC how the model is getting on, every 1.5 seconds, until it is done or has failed.
local function watch(name)
  lib.run(function()
    while state == "processing" do
      local answered = false
      net.backend({ path = "/job?name=" .. name }, function(code, body, err)
        answered = true
        if err or code ~= 200 then status = "PC: " .. tostring(err or body) return end
        local job, stage = body:match('"state":%s*"(.-)"'), body:match('"stage":%s*"(.-)"') or ""
        status = "PC (SAM 3D Body): " .. stage
        if job == "failed" then state = "idle" status = "SAM 3D Body failed: " .. stage .. " (details in the dev.py window)" end
        if job == "done" then
          net.backend({ path = "/result?name=" .. name, timeout = 30 }, function(c, text, e)
            state = "idle"
            if e or c ~= 200 then status = "Couldn't fetch the result: " .. tostring(e or text) return end
            result, showing_result, result_start = read_result(text), true, sys.real_time()
            status = "Done: captures/" .. name .. "-sam3d.bvh. Replaying it. REPLAY switches back to live."
            input.haptic("success")
          end)
        end
      end)
      lib.wait_until(function() return answered end)
      lib.wait(1.5)
    end
  end)
end

local function send()
  local text = mocap.take()
  if not text then status = "Nothing recorded yet." return end
  takes = takes + 1
  save.set("takes", takes)
  local name = "shots-" .. takes
  state, status = "sending", "Sending " .. name .. " (ARKit) to the PC..."
  lib.run(function()
    local done = false
    net.backend({ path = "/take?name=" .. name, method = "POST", body = text, timeout = 60 }, function(code, body, err)
      done = true
      status = err and ("ARKit take FAILED: " .. err) or ("ARKit take saved. Sending the video...")
    end)
    lib.wait_until(function() return done end)
    -- The video finishes writing a moment after STOP.
    local video
    for _ = 1, 50 do
      video = mocap.video()
      if video then break end
      lib.wait(0.1)
    end
    if not video then state, status = "idle", "No video for this take, so only the ARKit .bvh is saved." return end
    status = string.format("Sending the video (%.1f MB)...", #video / 1e6)
    net.backend({ path = "/video?name=" .. name, method = "POST", body = video, timeout = 120 }, function(code, body, err)
      if err or code ~= 202 then state, status = "idle", "Video FAILED: " .. tostring(err or body) return end
      state, status = "processing", "Video received. SAM 3D Body is starting on the PC..."
      watch(name)
    end)
  end)
end

buttons = {
  { label = function() return (state == "recording" or state == "countdown") and "STOP" or "RECORD" end, run = function()
      if state == "recording" or state == "countdown" then
        mocap.record(false)
        audio.play(say.stopped)
        state, status = "idle", string.format("Recorded %d frames. SEND to save it on the PC.", mocap.frames())
      elseif started and state == "idle" then
        showing_result = false
        state, countdown = "countdown", 3
        audio.play(say.three)
      end
    end },
  { label = function() return "SEND" end, run = function() if state == "idle" then send() end end },
  { label = function() return showing_result and "LIVE" or "REPLAY" end, run = function()
      if result then showing_result, result_start = not showing_result, sys.real_time() end
    end },
}

function init()
  begin_camera()
end

function on_resume()
  if started then begin_camera() end   -- iOS stops the camera while the app is away
end

local function layout()
  local w, h = gfx.size()
  local left, top, right, bottom = gfx.safe()
  local width = (w - left - right - 90) / #buttons
  for i, b in ipairs(buttons) do
    b.x, b.y, b.w, b.h = left + 30 + (i - 1) * (width + 10), h - bottom - 140, width, 100
  end
end

function update(dt)
  lib.update(dt)
  layout()
  local err
  tracked, pose, parents, err = mocap.pose()
  if err then status = "Camera problem: " .. err end
  if started and state == "idle" and status == "Looking for a body..." and tracked then
    status = "Tracking. Raise both hands over your head for a second to start (or tap RECORD)."
  end

  -- Hands free: two joints high over the hips and apart from each other (one in each hand) for HOLD seconds
  -- presses RECORD or STOP. It has to be let go before it can fire again.
  local high = {}
  if tracked then
    for i = 1, #pose - 2, 3 do
      if pose[i + 1] > HANDS_UP then high[#high + 1] = i end
    end
  end
  local both = false
  for a = 1, #high do
    for b = a + 1, #high do
      local i, j = high[a], high[b]
      if (pose[i] - pose[j]) ^ 2 + (pose[i + 2] - pose[j + 2]) ^ 2 > 0.25 ^ 2 then both = true end
    end
  end
  held = both and held + dt or 0
  if not both then latched = false end
  if held >= HOLD and not latched and (state == "idle" or state == "recording") then
    latched = true
    buttons[1].run()
  end

  if state == "countdown" then
    local before = math.ceil(countdown)
    countdown = countdown - dt
    local now = math.ceil(countdown)
    if now < before then audio.play(now == 2 and say.two or now == 1 and say.one or say.go) end
    if countdown <= 0 then
      mocap.record(true)
      state, record_start, next_beep = "recording", sys.real_time(), 0
      input.haptic("medium")
    end
  elseif state == "recording" then
    -- Calls number 2k ("ready") and 2k+1 ("hit!") belong to shot k.
    local k, part = next_beep // 2, next_beep % 2
    local at = FIRST_HIT + k * PERIOD - (part == 0 and READY_BEFORE or 0)
    if sys.real_time() - record_start >= at then
      audio.play(part == 1 and say.hit or say.ready)
      next_beep = next_beep + 1
    end
  end

  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then
      for _, b in ipairs(buttons) do
        if t.x >= b.x and t.x <= b.x + b.w and t.y >= b.y and t.y <= b.y + b.h then b.run() end
      end
    end
  end
end

-- SAM 3D Body's motion, looping, seen from the front and a little above, with a floor.
local function draw_result()
  local frames = result.frames
  local f = frames[math.floor((sys.real_time() - result_start) * result.fps) % #frames + 1]
  local cx, cz = f[1], f[3]
  gfx.camera(cx, 1.1, cz + 3.6, cx, 0.85, cz, 45)
  gfx.unlit(true)
  for k = -3, 3 do
    gfx.line(cx + k * 0.5, 0, cz - 1.5, cx + k * 0.5, 0, cz + 1.5, 0.25, 0.28, 0.35, 1, 0.01)
    gfx.line(cx - 1.5, 0, cz + k * 0.5, cx + 1.5, 0, cz + k * 0.5, 0.25, 0.28, 0.35, 1, 0.01)
  end
  for i, parent in ipairs(result.parents) do
    if parent > 0 then
      local a, p = (i - 1) * 3, (parent - 1) * 3
      gfx.line(f[a + 1], f[a + 2], f[a + 3], f[p + 1], f[p + 2], f[p + 3], 1.0, 0.75, 0.3, 1, 0.03)
    end
  end
  gfx.unlit(false)
end

function draw()
  gfx.clear(0.06, 0.07, 0.10)
  local w, h = gfx.size()
  local r, g, b = 0.4, 0.9, 1.0
  if not tracked then r, g, b = 0.35, 0.35, 0.4 end
  if state == "recording" then r, g, b = 1.0, 0.35, 0.35 end

  if showing_result and result then
    draw_result()
  else
    -- The camera picture, faint, filling the width. Where it is on screen, so the joints can be placed on it.
    local picture, pw, ph = mocap.camera()
    local iw, ih = w, w * 4 / 3
    if picture then iw, ih = w, w * ph / pw end
    local ix, iy = 0, (h - ih) / 2
    if picture then
      local u0, u1, v0, v1 = 0, 1, 0, 1
      if FLIP_X then u0, u1 = 1, 0 end
      if FLIP_Y then v0, v1 = 1, 0 end
      gfx.image(picture, ix, iy, iw, ih, 1, 1, 1, CAMERA_ALPHA, u0, v0, u1, v1)
    end

    local flat = mocap.screen()
    if flat then
      -- The skeleton on top of the person: dots along each bone, a bigger dot at each joint.
      local function at(j)
        local u, v = flat[(j - 1) * 2 + 1], flat[(j - 1) * 2 + 2]
        if FLIP_X then u = 1 - u end
        if FLIP_Y then v = 1 - v end
        return ix + u * iw, iy + v * ih
      end
      for j, parent in ipairs(parents) do
        if parent > 0 and flat[(j - 1) * 2 + 1] then
          local x1, y1 = at(j)
          local x2, y2 = at(parent)
          local steps = math.max(1, math.floor(math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2) / 7))
          for k = 0, steps do
            gfx.circle(x1 + (x2 - x1) * k / steps, y1 + (y2 - y1) * k / steps, 3, r, g, b)
          end
          gfx.circle(x1, y1, 5, r, g, b)
        end
      end
    else
      -- No camera (the PC): the skeleton from the front, in metres around the hips.
      gfx.camera(0, 0.1, 3.2, 0, 0.1, 0, 45)
      gfx.unlit(true)
      for i, parent in ipairs(parents) do
        if parent > 0 then
          local a, p = (i - 1) * 3, (parent - 1) * 3
          gfx.line(pose[a + 1], pose[a + 2], pose[a + 3], pose[p + 1], pose[p + 2], pose[p + 3], r, g, b, 1, 0.025)   -- width in metres
        end
      end
      gfx.unlit(false)
    end
  end

  local left, top = gfx.safe()
  gfx.text(showing_result and "MOCAP  -  SAM 3D REPLAY" or "MOCAP", left + 80, top + 20, 30, 1, 1, 1)
  gfx.text("takes saved: " .. takes, left + 80, top + 62, 18, 0.6, 0.65, 0.75)

  if state == "countdown" then
    local n = tostring(math.ceil(countdown))
    gfx.text(n, (w - gfx.text_width(n, 160)) / 2, h * 0.35, 160, 1, 0.8, 0.3)
  elseif state == "recording" then
    if sys.real_time() % 1 < 0.6 then gfx.circle(left + 60, top + 120, 14, 1, 0.2, 0.2) end
    gfx.text(string.format("REC %.1f s   %d frames", sys.real_time() - record_start, mocap.frames()), left + 84, top + 108, 24, 1, 0.4, 0.4)
  end

  -- Status, wrapped by hand.
  local line = 0
  for i = 1, #status, 30 do
    gfx.text(status:sub(i, i + 29), left + 30, h - 330 + line * 32, 22, 0.75, 0.9, 0.75)
    line = line + 1
  end

  for _, bt in ipairs(buttons) do
    local label = bt.label()
    local red = label == "STOP"
    gfx.rect(bt.x, bt.y, bt.w, bt.h, red and 0.75 or 0.2, red and 0.2 or 0.45, red and 0.25 or 0.85)
    gfx.text(label, bt.x + (bt.w - gfx.text_width(label, 24)) / 2, bt.y + 38, 24, 1, 1, 1)
  end
end
