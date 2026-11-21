# Collision

`engine/phys.cpp` answers questions about shapes: does this line hit anything, what is inside this ball, how far can this character move before a wall stops it.

It is **not** a physics engine. Nothing falls, bounces or pushes back by itself. Games move things in Lua and ask these questions to find out what they touch. That is a decision, not a gap: simulation is written in Lua where it can be tuned on the phone, and no physics library is to be added.

## What it offers

::: file engine/phys.h

## Shapes

| Shape | From Lua | Use for |
|---|---|---|
| Sphere | `phys.sphere(x, y, z, radius)` | Balls, pickups, simple enemies |
| Capsule | `phys.capsule(x, y, z, radius, height)` | Characters. Always upright |
| Box | `phys.box(x, y, z, sx, sy, sz [, rotation])` | Crates, doors, platforms |
| Mesh | `phys.mesh(...)`, `phys.model(...)` | Level geometry. Never moves |

Each call returns a **body**, a handle. Move one with `phys.place`, remove it with `phys.remove`.

Meshes are stored in a tree of boxes (a BVH), so a question against a large level only tests the triangles near it.

## Questions

```lua
-- What is under the player's feet?
local distance, x, y, z, nx, ny, nz, body = phys.raycast(px, py, pz, 0, -1, 0, 10)
if distance then
  -- hit: distance along the ray, the point, the surface direction, and which body
end

-- What did the finger touch? Turn the screen point into a ray, then cast it.
local ox, oy, oz, dx, dy, dz = gfx.unproject(touch.x, touch.y)
local hit, hx, hy, hz = phys.raycast(ox, oy, oz, dx, dy, dz, 100)

-- Everything within 3 units of an explosion
for _, body in ipairs(phys.overlap(ex, ey, ez, 3)) do damage(body) end
```

A cast that hits nothing returns nothing, so `if distance then` is the test.

## Layers and masks

Every body has a **layer**: a set of bits. Every question has a **mask**. A question sees a body only if the two share a bit. Define the bits once at the top of a game:

```lua
local WORLD, ENEMY, PICKUP = 1, 2, 4

phys.model(level, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, WORLD)
local enemy = phys.capsule(x, y, z, 0.4, 1.8, ENEMY)

-- A bullet that hits walls and enemies but passes through pickups:
phys.raycast(ox, oy, oz, dx, dy, dz, 50, WORLD | ENEMY)
```

Leaving the mask out means "everything".

## The character mover

Walking a character through a level by hand (test, back off, slide, try again) is fiddly, so the engine provides it:

```lua
local player = phys.capsule(0, 1, 0, 0.4, 1.8)
local vy = 0

function fixed_update(dt)
  local mx, mz = input.stick("move")
  vy = vy - 25 * dt                                         -- gravity is yours to write
  local x, y, z, grounded = phys.move(player, mx * 5 * dt, vy * dt, mz * 5 * dt)
  if grounded then vy = 0 end
end
```

`phys.move` takes how far you *want* to move and returns where the body ended up. Along the way it:

- slides along walls instead of stopping dead;
- climbs steps up to `step` high (the sixth argument);
- treats slopes steeper than `slope` degrees as walls;
- reports `grounded`, and the direction of the ground.

Notice what it does not do: apply gravity, jump, or decide speed. Those are the game's.

## Writing simulation in Lua

With these questions, most game physics is a few lines:

=== "A bouncing ball"

    ```lua
    local ball = { x = 0, y = 5, z = 0, vx = 2, vy = 0, vz = 0, r = 0.5 }

    function fixed_update(dt)
      ball.vy = ball.vy - 20 * dt
      local speed = math.sqrt(ball.vx ^ 2 + ball.vy ^ 2 + ball.vz ^ 2)
      local d, hx, hy, hz, nx, ny, nz =
        phys.spherecast(ball.x, ball.y, ball.z, ball.vx / speed, ball.vy / speed, ball.vz / speed, speed * dt, ball.r)
      if d then
        -- reflect the velocity about the surface direction, losing some energy
        local dot = ball.vx * nx + ball.vy * ny + ball.vz * nz
        ball.vx, ball.vy, ball.vz = (ball.vx - 2 * dot * nx) * 0.8, (ball.vy - 2 * dot * ny) * 0.8, (ball.vz - 2 * dot * nz) * 0.8
      else
        ball.x, ball.y, ball.z = ball.x + ball.vx * dt, ball.y + ball.vy * dt, ball.z + ball.vz * dt
      end
    end
    ```

=== "A trigger zone"

    ```lua
    local PLAYER = 8
    function fixed_update(dt)
      if #phys.overlap(door.x, door.y, door.z, 2, PLAYER) > 0 then door.open = true end
    end
    ```

=== "A camera that does not go through walls"

    ```lua
    local d = phys.spherecast(tx, ty, tz, dx, dy, dz, wanted_distance, 0.3, WORLD)
    local distance = d or wanted_distance
    ```

## Seeing the shapes

Call `phys.debug()` inside `draw()` and every body is drawn in lines. When something collides wrongly, look before you reason: the shape is usually not where you thought.

## Known simplifications

Each is marked `ponytail:` in the code.

- **A capsule is tested as a column of spheres.** Fine for characters; slightly bumpy against sharp edges.
- **Sphere casts step along the path** rather than solving exactly. A very fast, very small ball could tunnel through a thin wall. Cast in shorter pieces if you see that.
- **Capsules never tilt.**

## Exercises

??? example "1. Make a pickup collectable only by the player"
    Give the player's body a layer of its own (say `8`), and test each pickup with `phys.overlap(x, y, z, radius, 8)`.

??? example "2. Why does gravity belong in `fixed_update`?"
    So the fall is the same at every frame rate. See [A frame, start to finish](lifecycle.md).

??? example "3. A character stands on a moving platform. Whose job is it to carry them?"
    The game's. Find the ground body from a downward raycast, and if it is the platform, add the platform's movement to what you pass to `phys.move`.
