-- The charted jump: the user's own frame data, and the tables worked out from it.
-- Used as jump_chart.* by main.lua and planner.lua.
-- The charted jump (the user's paper, 17 November 2026): a straight-up jump that follows these heights exactly,
-- one a frame at 60 fps, from the button press (frame 1) to standing again. 15 is standing. No physics: the hips
-- sit at (height - 15) * CHART_UNIT metres: below standing (the jump squat, frames 1-4, and the landing squash and
-- recovery, from frame 56) the torso dips, above it it flies.
-- Read from the paper: rows 1-30, 31-50, then 22 columns (labelled 51-59, 50-62), then 63-80 (the circled L
-- column has no height and is left out). Fractions as written (34 1/2 is 34.5).
local CHART = {
  12, 12, 11, 11, 21, 26, 30, 33, 34.5, 36, 37.25, 38.5, 39.25, 40.75, 41.875, 42.875, 43.667, 44.5, 45.333, 46,
  46.25, 46.5, 46.75, 47, 47.333, 47.125, 47, 46.75, 46.5, 46.25,
  45.75, 45.25, 44.625, 44, 43.75, 42.5, 41.25, 40, 38.75, 37.5, 36, 34, 32.75, 31, 30, 28.5, 27, 26.5, 24, 23,
  21.5, 20, 19, 17.5, 16.25, 15, 13.5, 10, 9.25, 9.5, 9.25, 9.5, 10, 10.25, 10.5, 10.75, 10.875, 10.875, 10.875,
  10.75, 10.5, 10.25,
  10, 9.875, 9.75, 9.75, 10, 10.25, 10.5, 11, 11.5, 12, 12.5, 13, 13.5, 13.75, 14, 14, 14.5, 15,
}
local CHART_STAND, CHART_UNIT, CHART_FPS = 15, 0.05, 60
-- The recovery starts on the first frame back on the ground after take-off (56). From there it only plays while
-- nothing else is asked for: a walk or another jump cuts it short.
local CHART_LAND
for i = 2, #CHART do
  if not CHART_LAND and CHART[i - 1] > CHART_STAND and CHART[i] <= CHART_STAND then CHART_LAND = i end
end
-- Where the chart is in the air: AIR[frame] = height above standing (metres). BEST_FRAME[i] is the frame to meet a
-- ball needing the body i * 2 cm up, where the chart changes slowest (a frame early or late matters least).
local AIR, AIR_TOL, BEST_FRAME = {}, 0.07, {}
for f = 1, CHART_LAND - 1 do
  if CHART[f] > CHART_STAND then AIR[f] = (CHART[f] - CHART_STAND) * CHART_UNIT end
end
for i = 0, 90 do
  local best, best_c
  for f, a in pairs(AIR) do
    local miss = math.abs(a - i * 0.02)
    local c = miss + math.abs((AIR[f + 1] or a) - (AIR[f - 1] or a)) * 0.25
    if miss <= AIR_TOL and (not best_c or c < best_c) then best, best_c = f, c end
  end
  BEST_FRAME[i] = best
end
local CHART_PEAK = 1                 -- the highest frame: the flutter is from here to CHART_LAND
for i = 2, #CHART do if CHART[i] > CHART[CHART_PEAK] then CHART_PEAK = i end end

return { heights = CHART, stand = CHART_STAND, unit = CHART_UNIT, fps = CHART_FPS, land = CHART_LAND, peak = CHART_PEAK,
         air = AIR, air_tol = AIR_TOL, best_frame = BEST_FRAME }
