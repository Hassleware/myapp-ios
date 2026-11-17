-- Holds Dream Table to a PS1's budget. sys.budget() measures; this keeps the worst seen of each and draws a
-- meter (B on the PC, or set budget.show). budget.report() prints it (also from dev.py: lua budget.report()).
--
-- The limits, and why:
--   triangles 4000 a frame   what PS1 games drew in practice at 30 fps
--   texels    512K           1 MB of video memory, less the screen, at 8 bits a texel; none bigger than 256
--   ram       2048 KB        main memory: Lua heap, plus meshes (12 bytes a vertex, 8 a triangle) and
--                            animations (16-bit numbers), as a PS1 game would store them
--   lua_ops   50000 a frame  about what a 33 MHz processor affords at 30 fps, at roughly 20 machine
--                            instructions per Lua instruction; the engine's C++ stands in for the PS1's
--                            graphics chips (GTE and GPU)

local B = { show = false }
B.LIMITS = { triangles = 4000, texels = 512 * 1024, largest_texture = 256, ram_kb = 2048, lua_ops = 50000 }
local worst, b_was, frames = {}, false, 0
local LOADING = 60   -- frames: the first second is loading, as a PS1 game's loading screen, and isn't counted

local function measure()
  local u = sys.budget()
  u.ram_kb = u.lua_kb + (u.mesh_vertices * 12 + u.mesh_triangles * 8 + u.animation_bytes) / 1024
  return u
end

function B.update()
  local u = measure()
  frames = frames + 1
  if frames > LOADING then
    for k in pairs(B.LIMITS) do worst[k] = math.max(worst[k] or 0, u[k]) end
  end
  local b = input.key("b")
  if b and not b_was then B.show = not B.show end
  b_was = b
  return u
end

local ORDER = { "triangles", "texels", "largest_texture", "ram_kb", "lua_ops" }

function B.report()
  local u = measure()
  local parts = {}
  for _, k in ipairs(ORDER) do
    local over = (worst[k] or 0) > B.LIMITS[k] and " OVER" or ""
    parts[#parts + 1] = string.format("%s %d (worst %d) / %d%s", k, math.floor(u[k]), math.floor(worst[k] or 0), B.LIMITS[k], over)
  end
  print("BUDGET " .. table.concat(parts, " | "))
end

function B.draw(x, y)
  if not B.show then return end
  local u = measure()
  for i, k in ipairs(ORDER) do
    local f = u[k] / B.LIMITS[k]
    local yy = y + (i - 1) * 18
    gfx.rect(x, yy, 200, 12, 0, 0, 0, 0.5)
    gfx.rect(x, yy, 200 * math.min(1, f), 12, f > 1 and 1 or 0.3, f > 1 and 0.2 or 0.9, 0.3, 0.9)
    gfx.text(string.format("%s %d%%", k, math.floor(f * 100 + 0.5)), x + 206, yy - 1, 14, 1, 1, 1)
  end
end

return B
