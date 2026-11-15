-- Typing, pasting, calling a web API, and signing in with an emailed code (like Supercell ID).
-- game.cfg lists the hosts net.request may call. The sign-in talks to backends/net-demo/server.py
-- through dev.py; with no mail set up, the code appears in the dev.py window.

local auth = include("auth.lua")

local text, kind, email = "", "email", nil
local reply = auth.signed_in() and "Signed in. Tap WHO AM I." or "Tap EMAIL, type your address, then SEND CODE."

local function show(ok_text)
  return function(ok, err) reply = ok and ok_text or ("FAILED: " .. tostring(err)) end
end

local buttons = {
  { label = "EMAIL", run = function() kind, text = "email", "" input.keyboard(true, "email") end },
  { label = "SEND CODE", run = function()
      email = text
      reply = "sending..."
      auth.send_code(email, function(ok, err)
        if not ok then reply = "FAILED: " .. tostring(err) return end
        reply = "Code sent to " .. email .. ". Type it in, then SIGN IN."
        kind, text = "code", ""
        input.keyboard(true, "code")   -- iOS offers a code that just arrived in Mail above the keys
      end)
    end },
  { label = "PASTE", run = function() on_text(input.paste()) end },
  { label = "SIGN IN", run = function()
      reply = "checking..."
      auth.verify(email or "", text, function(ok, err)
        reply = ok and "Signed in. The phone stays signed in until LOG OUT." or ("FAILED: " .. tostring(err))
        if ok then input.keyboard(false) text = "" end
      end)
    end },
  { label = "WHO AM I", run = function()
      auth.me(function(user, err) reply = user and ("You are " .. tostring(user.email) .. " (account " .. tostring(user.id) .. ")") or ("FAILED: " .. tostring(err)) end)
    end },
  { label = "LOG OUT", run = function() auth.logout(function() reply = "Logged out." end) end },
  { label = "HIDE KEYS", run = function() input.keyboard(false) end },
  { label = "CALL API", run = function()
      reply = "asking..."
      net.request({ url = "https://api.github.com/zen" }, function(status, body, err)
        reply = err and ("FAILED: " .. err) or (status .. ": " .. body)
      end)
    end },
}

function on_text(s)
  if kind == "code" then s = s:gsub("%D", "") end   -- a pasted "Your code is 123456" keeps only the digits
  text = (text .. s):sub(1, 60)
end

function on_key(name)
  if name == "backspace" then text = text:sub(1, (utf8.offset(text, -1) or 1) - 1) end
  if name == "return" then input.keyboard(false) end
end

-- Everything sits in the top half, clear of the keyboard.
local function layout()
  local w = gfx.size()
  local left, top, right = gfx.safe()
  local x, y, width = left + 30, top + 30, w - left - right - 60
  for i, b in ipairs(buttons) do
    b.x, b.y, b.w, b.h = x + ((i - 1) % 2) * (width / 2), y + 110 + math.floor((i - 1) / 2) * 90, width / 2 - 10, 76
  end
  return x, y, width
end

local last_reply
function update()
  if reply ~= last_reply then print("net-demo: " .. reply) last_reply = reply end   -- also shown in the dev.py window
  layout()
  for _, t in ipairs(input.touches()) do
    if t.phase == "down" then
      for _, b in ipairs(buttons) do
        if t.x >= b.x and t.x <= b.x + b.w and t.y >= b.y and t.y <= b.y + b.h then b.run() end
      end
    end
  end
end

function draw()
  gfx.clear(0.1, 0.2, 0.4)
  local x, y, width = layout()
  gfx.rect(x, y, width, 90, 0.16, 0.18, 0.25)
  local cursor = input.keyboard() and sys.real_time() % 1 < 0.5 and "_" or ""
  local hint = text == "" and not input.keyboard() and (kind == "code" and "6-digit code" or "email") or ""
  gfx.text(text .. cursor .. hint, x + 16, y + 32, 26, text == "" and 0.5 or 1, text == "" and 0.55 or 1, text == "" and 0.65 or 1)
  for _, b in ipairs(buttons) do
    gfx.rect(b.x, b.y, b.w, b.h, 0.20, 0.45, 0.85)
    gfx.text(b.label, b.x + 16, b.y + 26, 24, 1, 1, 1)
  end
  -- The reply, wrapped by hand: the built-in font has no word wrap.
  local line, top = 0, y + 110 + math.ceil(#buttons / 2) * 90 + 20
  for i = 1, #reply, 28 do
    gfx.text(reply:sub(i, i + 27), x, top + line * 34, 24, 0.7, 0.9, 0.7)
    line = line + 1
  end
end
