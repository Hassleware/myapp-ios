-- Your serve's meter: the capsule that fills with the toss, the falling star to time the swing by, and
-- the Perfect / Good / Okay / Fail rating.
-- Made by main.lua: include("serve_meter.lua")(game) returns the functions below; the shared game table carries
-- everything it uses from elsewhere (listed at the top) and the state the files share (game.state, game.score...).
return function(game)
-- from main.lua
local SERVE, ball, bitfont, lib, you = game.SERVE, game.ball, game.bitfont, game.lib, game.you

-- Your serve's charge, 0 to 1: how far up the toss has gone (full at the top, and it stays full on the way down).
local function serve_charge()
  if game.state ~= "toss" or game.server ~= "you" then return 0 end
  return ball.vy > 0 and 1 - ball.vy / SERVE.toss or 1
end

-- How far the star has fallen, 0 to 1: from the top of the toss to SERVE.last, the last moment to swing.
local function serve_star()
  if game.state ~= "toss" or game.server ~= "you" or ball.vy > 0 or not you.toss_y then return 0 end
  local apex = you.toss_y + SERVE.toss * SERVE.toss / (2 * SERVE.g)
  return math.min(1, math.max(0, (apex - ball.y) / (apex - you.toss_y - SERVE.last)))
end

-- The serve meter: a long capsule on the right edge that slides in for your serve and out once it's hit. It fills
-- from the bottom with the charge, a smooth gradient from cool blue through gold to hot pink, shimmering when full;
-- once the ball is falling a star with a sparkle trail drops down inside it, reaching the bottom at the last moment
-- to swing (SERVE.last). Drawn as thin horizontal slices, each as wide as the capsule is at that height.
local METER_H, METER_W, METER_SLICES = 340, 30, 48
local meter = { show = 0 }
local GRADIENT = { { 0.25, 0.65, 1 }, { 0.3, 0.95, 0.85 }, { 1, 0.85, 0.25 }, { 1, 0.45, 0.2 }, { 1, 0.35, 0.7 } }
local function gradient(t)                          -- 0 at the bottom, 1 at the top
  local f = t * (#GRADIENT - 1)
  local i = math.min(#GRADIENT - 1, math.floor(f) + 1)
  local a, b, u = GRADIENT[i], GRADIENT[i + 1], f - (i - 1)
  return a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u, a[3] + (b[3] - a[3]) * u
end
local function capsule_width(y, h, w)               -- y from the top; rounded at both ends
  local r = w / 2
  local d = y < r and r - y or (y > h - r and y - (h - r) or 0)
  return d >= r and 0 or 2 * math.sqrt(r * r - d * d)
end
local function draw_meter(tex, w, h, right, now, dt)
  local wanted = game.server == "you" and (game.state == "serve" or game.state == "toss") and not (game.state == "toss" and you.swing_planned and ball.vy < -3)
  meter.show = lib.approach(meter.show, wanted and 1 or 0, 9, dt)
  if meter.show < 0.01 then return end
  local slide = 1 - meter.show
  slide = slide * slide * (3 - 2 * slide)
  local x = w - right - 46 + slide * 140               -- the capsule's middle; slides in from beyond the right edge
  local y0 = h * 0.5 - METER_H / 2                     -- its top
  local charge = serve_charge()
  local full = charge >= 0.999
  local a = meter.show
  local step = METER_H / METER_SLICES
  for i = 0, METER_SLICES - 1 do
    local y = i * step
    local mid = y + step / 2
    local rim = capsule_width(mid, METER_H, METER_W + 8)    -- a dark rim
    if rim > 0 then gfx.rect(x - rim / 2, y0 - 4 + y * (METER_H + 8) / METER_H, rim, step * (METER_H + 8) / METER_H + 0.5, 0.05, 0.06, 0.16, 0.7 * a) end
  end
  for i = 0, METER_SLICES - 1 do
    local y = i * step
    local cw = capsule_width(y + step / 2, METER_H, METER_W)
    if cw > 0 then
      local t = 1 - (y + step / 2) / METER_H           -- 0 at the bottom
      local r, g, b = gradient(t)
      if t <= charge then
        if full then                                   -- full: a bright shimmer running up the capsule
          local pulse = 0.5 + 0.5 * math.sin(now * 10 - t * 9)
          r, g, b = r + (1 - r) * pulse * 0.45, g + (1 - g) * pulse * 0.45, b + (1 - b) * pulse * 0.45
        end
        gfx.rect(x - cw / 2, y0 + y, cw, step + 0.5, r, g, b, a)
      else
        gfx.rect(x - cw / 2, y0 + y, cw, step + 0.5, r * 0.3, g * 0.3, b * 0.35, 0.55 * a)   -- not yet filled
      end
    end
  end
  gfx.rect(x - METER_W * 0.28, y0 + METER_W * 0.4, METER_W * 0.14, METER_H - METER_W * 0.8, 1, 1, 1, 0.22 * a)   -- a shine
  if full then
    local glow = METER_W * 3.2
    gfx.image(tex, x - glow / 2, y0 - glow * 0.25, glow, glow * 0.6, 1, 0.5, 0.8, (0.25 + 0.15 * math.sin(now * 10)) * a)
  end
  -- The perfect zone: a pink bracket beside the stretch where the star's last SERVE.perfect falls.
  local zy = y0 + METER_W / 2 + (1 - SERVE.perfect) * (METER_H - METER_W)
  local zb = y0 + METER_H - METER_W / 2
  local bx = x - METER_W / 2 - 14
  gfx.rect(bx, zy - 3, 6, zb - zy + 6, 0.12, 0.05, 0.12, 0.6 * a)                -- a dark edge, so it reads on cloud
  gfx.rect(bx + 1, zy - 2, 4, zb - zy + 4, 1, 0.4, 0.8, a)
  gfx.rect(bx + 1, zy - 2, 12, 4, 1, 0.4, 0.8, a)
  gfx.rect(bx + 1, zb - 2, 12, 4, 1, 0.4, 0.8, a)
  -- The star: from the top once the ball turns over, down to the bottom at the last moment to swing.
  if game.state == "toss" and ball.vy <= 0 and you.toss_y then
    local sy = y0 + METER_W / 2 + serve_star() * (METER_H - METER_W)
    for k = 1, 4 do                                    -- the sparkle trail above it
      local ty = sy - k * 14
      if ty > y0 then
        local sz = 10 - k * 2
        gfx.image(tex, x - sz / 2 + math.sin(now * 20 + k) * 3, ty - sz / 2, sz, sz, 1, 1, 0.8, (0.8 - k * 0.15) * a)
      end
    end
    local spin = 1 + 0.15 * math.sin(now * 18)
    gfx.image(tex, x - 30 * spin, sy - 30 * spin, 60 * spin, 60 * spin, 1, 0.95, 0.6, 0.6 * a)   -- its glow
    bitfont.draw("*", x - 24 * spin, sy - 34 * spin, 72 * spin, 1, 0.97, 0.6, a)
  end
end

-- The serve's rating, popping up in your font when the serve is hit (or missed). It's the timing only: the charge
-- is the serve's speed and distance, not its rating. Perfect (pink): swung while the star is in the lower
-- SERVE.perfect of its fall (marked on the capsule); Good (gold): after it's fallen SERVE.good; Okay (green): earlier;
-- Fail (dark grey): a missed serve.
local RATINGS = { perfect = { "Perfect!", 1, 0.45, 0.8 }, good = { "Good", 1, 0.8, 0.2 },
                  okay = { "Okay", 0.35, 0.85, 0.35 }, fail = { "Fail", 0.3, 0.3, 0.33 } }
local rating = nil                                   -- { kind, t }
local function rate_serve(kind) rating = { kind = kind, at = sys.time() } end
local function draw_rating(w, h)
  if not rating then return end
  rating.t = sys.time() - rating.at                    -- game time: it pauses with the game
  if rating.t > 1.4 then rating = nil return end
  local r = RATINGS[rating.kind]
  local pop = rating.t < 0.15 and rating.t / 0.15 * 1.25 or (rating.t < 0.3 and 1.25 - (rating.t - 0.15) / 0.15 * 0.25 or 1)
  local fade = rating.t > 1.1 and 1 - (rating.t - 1.1) / 0.3 or 1
  local size = 56 * pop
  local word = r[1]
  bitfont.draw(word, (w - bitfont.width(word, size)) / 2, h * 0.3 - size / 2, size, r[2], r[3], r[4], fade)
end

return {
  serve_charge = serve_charge, serve_star = serve_star, draw_meter = draw_meter, rate_serve = rate_serve,
  draw_rating = draw_rating,
}
end
