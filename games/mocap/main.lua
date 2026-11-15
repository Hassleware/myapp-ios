-- Motion capture: the iPhone's back camera tracks a body (ARKit), you record a take, and SEND turns it
-- into captures/<name>.bvh on the PC (backends/mocap/server.py), ready for Blender.
-- Prop the phone up about 3 metres away at waist height, or have someone hold it, and keep the whole body in view.

local lib = include("lib.lua")

local started, why_not = false, nil
local state = "idle"          -- idle, countdown, recording, sending
local countdown, record_start = 0, 0
local status = ""
local takes = math.floor(save.get("takes", 0))
local pose, parents, tracked = {}, {}, false
local buttons

local function begin_camera()
  started, why_not = mocap.start()
  status = started and "Looking for a body..." or why_not
end

local function send()
  local text = mocap.take()
  if not text then status = "Nothing recorded yet." return end
  takes = takes + 1
  save.set("takes", takes)
  local name = "take-" .. takes
  state, status = "sending", "Sending " .. name .. " to the PC..."
  net.backend({ path = "/take?name=" .. name, method = "POST", body = text, timeout = 60 }, function(code, body, err)
    state = "idle"
    if err then status = "FAILED: " .. err return end
    local file = body:match('"file":%s*"(.-)"')
    status = file and ("Saved " .. file .. " (" .. (body:match('"seconds":%s*([%d.]+)') or "?") .. " s)") or ("FAILED: " .. body)
  end)
end

buttons = {
  { label = function() return (state == "recording" or state == "countdown") and "STOP" or "RECORD" end, run = function()
      if state == "recording" or state == "countdown" then
        mocap.record(false)
        state, status = "idle", string.format("Recorded %d frames. SEND to save it on the PC.", mocap.frames())
      elseif started then
        state, countdown = "countdown", 3
      end
    end },
  { label = function() return "SEND" end, run = function() if state == "idle" then send() end end },
  { label = function() return "CAMERA" end, run = begin_camera },
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
  local width = (w - left - right - 80) / #buttons
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
  if started and state == "idle" and status == "Looking for a body..." and tracked then status = "Tracking. Tap RECORD." end

  if state == "countdown" then
    countdown = countdown - dt
    if countdown <= 0 then
      mocap.record(true)
      state, record_start = "recording", sys.real_time()
      input.haptic("medium")
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

function draw()
  gfx.clear(0.06, 0.07, 0.10)

  -- The skeleton as the phone sees it, from the front. Positions are metres around the hips.
  gfx.camera(0, 0.1, 3.2, 0, 0.1, 0, 45)
  gfx.unlit(true)
  local r, g, b = 0.4, 0.9, 1.0
  if not tracked then r, g, b = 0.35, 0.35, 0.4 end
  if state == "recording" then r, g, b = 1.0, 0.35, 0.35 end
  for i, parent in ipairs(parents) do
    if parent > 0 then
      local a, p = (i - 1) * 3, (parent - 1) * 3
      gfx.line(pose[a + 1], pose[a + 2], pose[a + 3], pose[p + 1], pose[p + 2], pose[p + 3], r, g, b, 1, 0.025)   -- width in metres
    end
  end
  gfx.unlit(false)

  local w, h = gfx.size()
  local left, top = gfx.safe()
  gfx.text("MOCAP", left + 80, top + 20, 30, 1, 1, 1)
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

  for _, b in ipairs(buttons) do
    local label = b.label()
    local red = label == "STOP"
    gfx.rect(b.x, b.y, b.w, b.h, red and 0.75 or 0.2, red and 0.2 or 0.45, red and 0.25 or 0.85)
    gfx.text(label, b.x + (b.w - gfx.text_width(label, 26)) / 2, b.y + 37, 26, 1, 1, 1)
  end
end
