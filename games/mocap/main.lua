-- Motion capture: the iPhone's back camera tracks a body (ARKit) and records a video at the same time.
-- SEND saves ARKit's take as captures/<name>.bvh at once, and hands the video to SAM 3D Body (Meta) on the PC's
-- graphics card, a stronger model with hands and feet, which writes captures/<name>-sam3d.bvh. The phone shows the PC's progress, then replays
-- that result (REPLAY switches between that and the live camera).
-- Prop the phone up about 3 metres away at waist height, or have someone hold it, and keep the whole body in view.
-- The card at the top is the Dream Table shot to perform (shots.lua): tap its left or right side to change shot.
-- Beeps lead the take, since the screen faces away while recording: three ticks, a high beep to hit the ball on,
-- a low beep when it stops by itself. Takes are named after the shot: fh-drive-1, fh-drive-2...

local lib = include("lib.lua")
local shots = include("shots.lua")
local shot_index = math.max(1, math.min(#shots, math.floor(save.get("shot", 1))))
local sounds = { tick = audio.load("tick.wav"), hit = audio.load("hit.wav"), done = audio.load("end.wav") }
local hit_played = false
local card = { x = 0, y = 0, w = 0, h = 0 }

local started, why_not = false, nil
local state = "idle"          -- idle, countdown, recording, sending, processing
local countdown, record_start = 0, 0
local status = ""
local takes = math.floor(save.get("takes", 0))
local hands = save.get("hands", "false") == "true"   -- saved as text. HANDS: the PC's slower pass with finer fingers
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
  local shot = shots[shot_index]
  local count = math.floor(save.get("count-" .. shot.id, 0)) + 1
  save.set("count-" .. shot.id, count)
  local name = shot.id .. "-" .. count
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
    net.backend({ path = "/video?name=" .. name .. (hands and "&hands=1" or ""), method = "POST", body = video, timeout = 120 }, function(code, body, err)
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
        state, status = "idle", string.format("Recorded %d frames. SEND to save it on the PC.", mocap.frames())
      elseif started and state == "idle" then
        showing_result = false
        state, countdown = "countdown", 3
        audio.play(sounds.tick)
      end
    end },
  { label = function() return "SEND" end, run = function() if state == "idle" then send() end end },
  { label = function() return showing_result and "LIVE" or "REPLAY" end, run = function()
      if result then showing_result, result_start = not showing_result, sys.real_time() end
    end },
  { label = function() return hands and "HANDS ON" or "HANDS OFF" end, run = function()
      hands = not hands
      save.set("hands", tostring(hands))
      status = hands and "Finer fingers on the PC: about 4x slower." or "Fast: hands come from the body pass."
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
  if started and state == "idle" and status == "Looking for a body..." and tracked then status = "Tracking. Tap RECORD." end

  if state == "countdown" then
    local before = math.ceil(countdown)
    countdown = countdown - dt
    if math.ceil(countdown) < before then audio.play(sounds.tick) end   -- a tick each second; the last starts it
    if countdown <= 0 then
      mocap.record(true)
      state, record_start, hit_played = "recording", sys.real_time(), false
      input.haptic("medium")
    end
  elseif state == "recording" then
    local t, shot = sys.real_time() - record_start, shots[shot_index]
    if not hit_played and t >= shot.contact then hit_played = true audio.play(sounds.hit) end
    if t >= shot.length then   -- stops by itself, so every take of a shot is the same length
      mocap.record(false)
      audio.play(sounds.done)
      state, status = "idle", string.format("Recorded %d frames of %s. SEND to save it on the PC.", mocap.frames(), shot.id)
    end
  end

  for _, t in ipairs(input.touches()) do
    if t.phase == "down" and state == "idle" and not showing_result and t.y >= card.y and t.y <= card.y + card.h then
      local step = t.x < card.x + card.w / 2 and -1 or 1   -- left half: previous shot, right half: next
      shot_index = (shot_index - 1 + step) % #shots + 1
      save.set("shot", shot_index)
    end
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

-- A dotted line on the 2D screen.
local function dots(x1, y1, x2, y2, size, r, g, b, a)
  local steps = math.max(1, math.floor(math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2) / (size * 1.6)))
  for k = 0, steps do gfx.circle(x1 + (x2 - x1) * k / steps, y1 + (y2 - y1) * k / steps, size, r, g, b, a) end
end

-- The bat's path through start, contact and finish, in one view. map turns a path point into screen x, y.
local function draw_path(path, map)
  local x1, y1 = map(path[1])
  local x2, y2 = map(path[2])
  local x3, y3 = map(path[3])
  dots(x1, y1, x2, y2, 4, 0.55, 0.7, 1.0)
  dots(x2, y2, x3, y3, 4, 0.45, 0.95, 0.6)
  gfx.circle(x1, y1, 9, 0.55, 0.7, 1.0)          -- start
  gfx.circle(x3, y3, 7, 0.45, 0.95, 0.6)         -- finish
  gfx.circle(x2, y2, 13, 1.0, 0.6, 0.15)         -- contact: the ball
  gfx.circle(x2, y2, 7, 1.0, 0.9, 0.7)
end

-- The shot card: name, what to do, the bat's path from the side and from above, and the take's timing.
local function draw_card()
  local shot = shots[shot_index]
  local x, y, w, h = card.x, card.y, card.w, card.h
  gfx.rect(x, y, w, h, 0.09, 0.10, 0.14, 0.88)
  gfx.text("<", x + 12, y + 18, 30, 0.5, 0.55, 0.65)
  gfx.text(">", x + w - 30, y + 18, 30, 0.5, 0.55, 0.65)
  gfx.text(shot.name, x + 44, y + 16, 26, 1, 0.85, 0.4)
  gfx.text(string.format("%d/%d  %s   takes done: %d", shot_index, #shots, shot.id, math.floor(save.get("count-" .. shot.id, 0))),
           x + 44, y + 52, 16, 0.6, 0.65, 0.75)
  for i, cue in ipairs(shot.cues) do gfx.text(cue, x + 24, y + 80 + i * 28, 20, 0.9, 0.92, 0.95) end

  -- Side view: you on the left, facing the table on the right.
  local vx, vy, vw, vh = x + 20, y + 240, w * 0.56, 250
  gfx.text("SIDE", vx, vy - 4, 14, 0.5, 0.55, 0.65)
  local function side(p) return vx + (p.f + 0.45) / 1.5 * vw, vy + vh - p.u / 1.9 * vh end
  local fx, fy = side({ f = -0.45, u = 0 })
  local tx = side({ f = 1.05, u = 0 })
  dots(fx, fy, tx, fy, 2, 0.4, 0.42, 0.5)                                   -- floor
  local ax, ay = side({ f = 0.5, u = 0.76 })
  local bx = side({ f = 1.05, u = 0.76 })
  gfx.rect(ax, ay, bx - ax, 6, 0.25, 0.45, 0.85)                            -- table top
  local lx = side({ f = 0.6, u = 0 })
  gfx.rect(lx, ay + 6, 4, fy - ay - 6, 0.25, 0.3, 0.45)                     -- table leg
  local hx, hy = side({ f = 0, u = 0.95 })
  local nx, ny = side({ f = 0.05, u = 1.45 })
  dots(hx, hy, nx, ny, 3, 0.6, 0.6, 0.65)                                   -- body, leaning in
  local kx, ky = side({ f = -0.1, u = 0 })
  local k2x = side({ f = 0.15, u = 0 })
  dots(hx, hy, kx, ky, 3, 0.6, 0.6, 0.65)                                   -- legs
  dots(hx, hy, k2x, ky, 3, 0.6, 0.6, 0.65)
  local px, py = side({ f = 0.07, u = 1.62 })
  gfx.circle(px, py, 12, 0.6, 0.6, 0.65)                                    -- head
  draw_path(shot.path, side)

  -- From above: the table at the top, you below it; right of the picture is your bat side.
  local ox, oy, ow, oh = vx + vw + 20, vy + 22, w - vw - 60, vh - 22
  gfx.text("ABOVE", ox, oy - 26, 14, 0.5, 0.55, 0.65)
  local function above(p) return ox + (p.s + 0.7) / 1.4 * ow, oy + oh - (p.f + 0.45) / 1.5 * oh end
  local t1x, t1y = above({ s = -0.7, f = 1.05 })
  local t2x, t2y = above({ s = 0.7, f = 0.5 })
  gfx.rect(t1x, t1y, t2x - t1x, t2y - t1y, 0.12, 0.25, 0.5)                 -- table
  for _, foot in ipairs({ -0.22, 0.22 }) do
    local qx, qy = above({ s = foot, f = 0 })
    gfx.circle(qx, qy, 9, 0.6, 0.6, 0.65)                                   -- feet
  end
  draw_path(shot.path, above)

  -- Timing: ticks, then the take; the high beep is the hit, the low beep the end.
  local bx0, by0, bw0 = x + 24, y + h - 70, w - 48
  gfx.rect(bx0, by0, bw0, 8, 0.25, 0.27, 0.35)
  local cx = bx0 + shot.contact / shot.length * bw0
  gfx.rect(cx - 3, by0 - 10, 6, 28, 1.0, 0.6, 0.15)
  gfx.text(string.format("HIT %.1fs", shot.contact), cx - 40, by0 + 22, 16, 1.0, 0.7, 0.3)
  gfx.text(string.format("END %.1fs", shot.length), bx0 + bw0 - 110, by0 - 24, 16, 0.6, 0.65, 0.75)
  if state == "recording" then
    gfx.rect(bx0, by0, math.min(1, (sys.real_time() - record_start) / shot.length) * bw0, 8, 1, 0.35, 0.35)
  end
  gfx.text("swipe: " .. shot.swipe, x + 24, y + h - 30, 14, 0.55, 0.6, 0.7)
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

  local left, top, right = gfx.safe()
  card.x, card.y, card.w, card.h = left + 20, top + 100, w - left - right - 40, 640
  if not showing_result then draw_card() end
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
