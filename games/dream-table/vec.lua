-- Vector and quaternion helpers: plain { x, y, z } and { x, y, z, w } tables. A turn is a quaternion.
-- Made by main.lua: include("vec.lua")(lib) returns the functions below; they are pure.
return function(lib)
local function v(x, y, z) return { x, y, z } end
local function add(a, b) return v(a[1] + b[1], a[2] + b[2], a[3] + b[3]) end
local function sub(a, b) return v(a[1] - b[1], a[2] - b[2], a[3] - b[3]) end
local function mul(a, s) return v(a[1] * s, a[2] * s, a[3] * s) end
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
local function len(a) return math.sqrt(dot(a, a)) end
local function unit(a) local l = len(a) return l > 1e-9 and mul(a, 1 / l) or v(0, 1, 0) end
local function cross(a, b) return v(lib.cross(a[1], a[2], a[3], b[1], b[2], b[3])) end
local function qmul(a, b) return { lib.quat_mul(a[1], a[2], a[3], a[4], b[1], b[2], b[3], b[4]) } end
local function qinv(q) return { -q[1], -q[2], -q[3], q[4] } end
local function rot(q, p) return v(lib.quat_rotate(q[1], q[2], q[3], q[4], p[1], p[2], p[3])) end
local function mirror_turn(q) return { q[1], -q[2], -q[3], q[4] } end   -- the turn seen in a mirror across x = 0
local function yaw_turn(angle) return { 0, math.sin(angle / 2), 0, math.cos(angle / 2) } end

-- From turn a part of the way to turn b, the short way round.
local function toward(a, b, t)
  if a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] < 0 then b = { -b[1], -b[2], -b[3], -b[4] } end
  local q = { lib.lerp(a[1], b[1], t), lib.lerp(a[2], b[2], t), lib.lerp(a[3], b[3], t), lib.lerp(a[4], b[4], t) }
  local l = math.sqrt(q[1] ^ 2 + q[2] ^ 2 + q[3] ^ 2 + q[4] ^ 2)
  return { q[1] / l, q[2] / l, q[3] / l, q[4] / l }
end

-- A turn from three axes (where the turn sends x, y and z), as a quaternion.
local function axes_turn(x, y, z)
  local tr = x[1] + y[2] + z[3]
  if tr > 0 then
    local s4 = math.sqrt(tr + 1) * 2
    return { (y[3] - z[2]) / s4, (z[1] - x[3]) / s4, (x[2] - y[1]) / s4, s4 / 4 }
  elseif x[1] > y[2] and x[1] > z[3] then
    local s4 = math.sqrt(1 + x[1] - y[2] - z[3]) * 2
    return { s4 / 4, (y[1] + x[2]) / s4, (z[1] + x[3]) / s4, (y[3] - z[2]) / s4 }
  elseif y[2] > z[3] then
    local s4 = math.sqrt(1 + y[2] - x[1] - z[3]) * 2
    return { (y[1] + x[2]) / s4, s4 / 4, (z[2] + y[3]) / s4, (z[1] - x[3]) / s4 }
  end
  local s4 = math.sqrt(1 + z[3] - x[1] - y[2]) * 2
  return { (z[1] + x[3]) / s4, (z[2] + y[3]) / s4, s4 / 4, (x[2] - y[1]) / s4 }
end

return {
  v = v, add = add, sub = sub, mul = mul,
  dot = dot, len = len, unit = unit, cross = cross,
  qmul = qmul, qinv = qinv, rot = rot, mirror_turn = mirror_turn,
  yaw_turn = yaw_turn, toward = toward, axes_turn = axes_turn,
}
end
