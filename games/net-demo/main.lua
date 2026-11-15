-- Typing, pasting and calling a web API. The smallest thing that shows all three.
-- game.cfg lists the hosts this game may call; the engine refuses every other address.

local text, kind, reply = "", "text", "Tap CALL API"
local buttons = {
  { label = "TEXT",     run = function() kind = "text" input.keyboard(true, "text") end },
  { label = "CODE",     run = function() kind = "code" input.keyboard(true, "code") end },   -- digits; iOS offers an SMS code
  { label = "PASTE",    run = function() on_text(input.paste()) end },
  { label = "HIDE",     run = function() input.keyboard(false) end },
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

function update()
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
  gfx.text(text .. cursor, x + 16, y + 32, 26, 1, 1, 1)
  for _, b in ipairs(buttons) do
    gfx.rect(b.x, b.y, b.w, b.h, 0.20, 0.45, 0.85)
    gfx.text(b.label, b.x + 16, b.y + 26, 24, 1, 1, 1)
  end
  -- The reply, wrapped by hand: the built-in font has no word wrap.
  local line = 0
  for i = 1, #reply, 28 do
    gfx.text(reply:sub(i, i + 27), x, y + 400 + line * 34, 24, 0.7, 0.9, 0.7)
    line = line + 1
  end
end
