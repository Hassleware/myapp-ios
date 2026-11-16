# Models and animation

`engine/model.cpp` loads `.glb` files, the binary form of glTF, which Blender exports directly. One file can hold meshes, their textures, a skeleton, animations, lights, cameras and empty marker objects. The engine treats a level and a character the same way: both are models.

## From Blender to the game

1. In Blender: **File > Export > glTF 2.0**, format **glTF Binary (.glb)**.
2. Save it into the game's folder.
3. In Lua:

```lua
local hero = model.load("hero.glb")

function draw()
  gfx.clear(0.1, 0.1, 0.15)
  gfx.camera(0, 2, 5, 0, 1, 0, 50)
  model.draw(hero, 0, 0, 0)
end
```

Saving the `.glb` again reloads it in the running game, on the PC and on the phone.

## What the engine offers

::: file engine/model.h

## Nodes: a level is a list of named things

Every object in the Blender scene becomes a **node** with a name, a parent, a position and optionally a mesh. `model.nodes(m)` returns them all. That turns Blender into the level editor:

```lua
local level = model.load("level.glb")
local spawn

for _, node in ipairs(model.nodes(level)) do
  if node.name == "PlayerStart" then
    spawn = { node.x, node.y, node.z }
  elseif node.extras and node.extras.pickup then
    add_pickup(node.x, node.y, node.z, node.extras.pickup)
  end
end
```

`extras` are the object's **Custom Properties** in Blender. A designer adds a property `pickup = "coin"` to an empty object, exports, and the game places a coin there. No level format of our own is needed.

Lights and cameras in the file come through as nodes too, with `light`, `r`, `g`, `b`, `range`, `angle` and `fov` fields.

`phys.model(level, ...)` turns the same file's meshes into collision. See [Collision](collision.md).

## Animation

A **clip** is one named animation in the file ("walk", "jump"). An **animator** holds the current pose of one character. Each frame, you tell the animator which clips to mix:

```lua
local a = model.animator(hero)

function draw()
  local t = sys.time()
  model.pose(hero, a, "walk", t % 1.2, 1.0)          -- one clip at full weight
  model.draw(hero, 0, 0, 0,  0, 0, 0, 1,  1, 1, 1,  1, 1, 1, 1,  a)
end
```

`model.pose` takes up to eight `clip, time, weight` triples and blends them. A cross-fade is two clips whose weights move in opposite directions:

```lua
model.pose(hero, a, "idle", t, 1 - blend, "walk", t, blend)
```

You rarely call `model.pose` by hand. `lib.animation` in `lib.lua` wraps it with cross-fades, looping and timed events:

::: source tools/template/lib.lua lib.animation

## Skinning

Bending the mesh to follow the skeleton is done on the processor, into the vertex buffer for that frame. The consequence is useful: a skinned character is ordinary triangles by the time it reaches the shader, so **every shader works on characters** with no special version.

The cost is processor time per character. A handful is fine. A crowd would need skinning moved to the graphics chip; that is a marked simplification.

## Attaching things to bones

`model.joint(m, a, node)` returns where a bone is, inside the model, in its current pose. Combine that with the character's own position to put a sword in a hand. `lib.attach` does the arithmetic:

::: source tools/template/lib.lua lib.attach

## Changing a bone by hand

`model["local"](m, a, node, ...)` reads or sets one bone after posing (written with brackets because `local` is a Lua keyword). That is how a head turns to look at something, or a hand reaches for a handle, on top of whatever animation is playing.

## Things that go wrong

| Symptom | Cause |
|---|---|
| The model is pink or missing, with a warning in `dev.py` | The file name is wrong or the file is not in the game's folder |
| The model is tiny or huge | Blender's scene scale. Apply the scale before exporting (**Object > Apply > Scale**) |
| It is inside out | Normals are flipped. In Blender: **Mesh > Normals > Recalculate Outside** |
| Animations are missing | In the export options, make sure **Animation** is ticked, and that each action is stashed or pushed down to the NLA |
| Textures are missing | They must be packed into the `.glb` (the default for glTF Binary) |

## Exercises

??? example "1. List every clip in a model"
    ```lua
    for i, clip in ipairs(model.clips(hero)) do print(i, clip.name, clip.duration) end
    ```

??? example "2. Draw only the node called `Wheel`, somewhere else"
    `model.draw(car, x, y, z, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, nil, "Wheel")`. The last two arguments are the animator (none) and the node.

??? example "3. Why can a custom toon shader be used on an animated character without changes?"
    Skinning has already happened on the processor. The shader receives final positions and normals.
