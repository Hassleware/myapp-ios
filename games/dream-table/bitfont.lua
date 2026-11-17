-- Words in your dot font, as a bitmap font: every letter was drawn once, with a smooth outline and a soft drop
-- shadow, by tools/make_bitmap_font.py (font.lua, font_1.png, font_2.png). Each letter is one textured square,
-- tinted by the colour given (the outline stays dark). size is the line's height in 2D units, like gfx.text.

local F = include("font.lua")
local B = {}
local G, LINE, SPACE = F.glyphs, F.line, F.space
local pages = {}
for i = 1, F.pages do pages[i] = gfx.load_texture("font_" .. i .. ".png", "clamp") end

function B.width(s, size)
  local w = 0
  for i = 1, #s do
    local g = G[s:byte(i)]
    w = w + (g and g[8] or SPACE)
  end
  return w * size / LINE
end

function B.draw(s, x, y, size, r, g, b, a)
  local k, image, inv = size / LINE, gfx.image, 1 / 256
  a = a or 1
  for i = 1, #s do
    local c = G[s:byte(i)]
    if c then
      image(pages[c[1]], x + c[6] * k, y + c[7] * k, c[4] * k, c[5] * k, r, g, b, a,
            c[2] * inv, c[3] * inv, (c[2] + c[4]) * inv, (c[3] + c[5]) * inv)
      x = x + c[8] * k
    else
      x = x + SPACE * k
    end
  end
end

return B
