---@meta
-- Everything the engine gives to a game's Lua, described once, here.
--
-- Two things read this file:
--   * the handbook (docs/hooks.py) builds the Lua API reference from it and checks it against engine/script.cpp,
--     so a function added to or removed from the C++ without a change here shows up as a warning;
--   * the Lua extension for VS Code (see .luarc.json) uses it for autocomplete and for the text shown on hover.
--
-- Each entry: a first comment line in `backticks` with the full call as you would write it, then what it does,
-- then an empty function so editors know the name. This file is never sent to the phone or run.
--
-- Shorthand used below. <transform> is up to ten numbers: x, y, z, then a rotation quaternion qx, qy, qz, qw,
-- then scale sx, sy, sz; leave the end off for no rotation and scale 1. Colours are r, g, b [, a] from 0 to 1.
-- Things that are loaded or made come back as a number (a handle) to pass to other functions.

gfx, model, phys, audio, input, net, mocap, save, sys = {}, {}, {}, {}, {}, {}, {}, {}, {}

-- # The scene (set every frame, inside draw)

--- `gfx.clear(r, g, b)`
--- The background colour for this frame.
function gfx.clear(r, g, b) end

--- `gfx.camera(ex, ey, ez, tx, ty, tz [, fov [, near, far [, upx, upy, upz]]])`
--- Puts the eye at e, looking at t. fov is in degrees; below 0 it is a flat (orthographic) camera that many units tall.
function gfx.camera(ex, ey, ez, tx, ty, tz, fov, near, far, upx, upy, upz) end

--- `gfx.ambient(r, g, b)`
--- Light that reaches every surface from all sides.
function gfx.ambient(r, g, b) end

--- `gfx.light(x, y, z [, r, g, b])`
--- The sun: the direction its light travels, and its colour.
function gfx.light(x, y, z, r, g, b) end

--- `gfx.point_light(x, y, z, radius, r, g, b)`
--- A light that shines in all directions and fades out at radius. Up to 8 point and spot lights in all per frame.
function gfx.point_light(x, y, z, radius, r, g, b) end

--- `gfx.spot_light(x, y, z, dx, dy, dz, radius, angle, r, g, b)`
--- A cone of light from a point along a direction. angle is the width of the cone in degrees.
function gfx.spot_light(x, y, z, dx, dy, dz, radius, angle, r, g, b) end

--- `gfx.fog(r, g, b, start, finish)`
--- Fades things to a colour between two distances from the camera.
function gfx.fog(r, g, b, start, finish) end

--- `gfx.shadows(cx, cy, cz, radius [, strength])`
--- Turns on sun shadows for everything inside a ball. Keep the ball as small as the play area allows: the same shadow map is stretched over it.
function gfx.shadows(cx, cy, cz, radius, strength) end

--- `gfx.sky(texture [, r, g, b])`
--- A panorama picture wrapped round the camera, drawn behind everything.
function gfx.sky(texture, r, g, b) end

--- `gfx.target([t])`
--- Draws what follows into a texture made by gfx.new_target. Call with nothing to go back to the screen.
function gfx.target(t) end

-- # How the next things are drawn (stays until changed)

--- `gfx.shader([name])`
--- Uses shaders/name.slang from the game's folder for what follows. With no name, the engine's standard shader.
function gfx.shader(name) end

--- `gfx.texture(slot, texture | nil)`
--- Puts a texture in slot 1 to 4. Slot 1 is the colour texture, slot 2 the normal map, 3 and 4 are free for your shader.
function gfx.texture(slot, texture) end

--- `gfx.blend("opaque" | "alpha" | "add")`
--- How colours mix with what is behind: solid, see-through, or added (glows, sparks).
function gfx.blend(mode) end

--- `gfx.cull(on)`
--- false draws both sides of every triangle, for leaves, cloth and flat cards.
function gfx.cull(on) end

--- `gfx.unlit(on)`
--- true ignores lights and shows the colour as given.
function gfx.unlit(on) end

--- `gfx.depth(test, write)`
--- Whether nearer things hide what follows, and whether what follows hides later things.
function gfx.depth(test, write) end

--- `gfx.cutoff(alpha)`
--- Pixels whose texture alpha is below this are not drawn at all. For fences and foliage with hard edges.
function gfx.cutoff(alpha) end

--- `gfx.cast_shadows(on)`
--- Whether what follows appears in the sun's shadow map.
function gfx.cast_shadows(on) end

--- `gfx.param(index, x, y, z, w)`
--- Four numbers for your shader, index 1 or 2, sent with each thing drawn (push.params in Slang).
function gfx.param(index, x, y, z, w) end

--- `gfx.frame_param(index, x, y, z, w)`
--- Four numbers for your shader, index 1 to 4, the same for the whole frame (frame.user in Slang).
function gfx.frame_param(index, x, y, z, w) end

--- `gfx.font(font | nil)`
--- Uses a font from gfx.load_font for gfx.text. nil goes back to the built-in 8x8 font.
function gfx.font(font) end

--- `gfx.reset()`
--- Puts shader, textures, blend, cull, depth and the rest back to how a frame starts.
function gfx.reset() end

-- # Loading and making things

--- `gfx.load_texture(file [, options]) -> texture`
--- Loads a .png or .jpg from the game's folder. options is a string of words: "nearest" (blocky), "clamp" (no repeat), "nomip", "data" (numbers such as a normal map, not colours).
function gfx.load_texture(file, options) end

--- `gfx.new_texture(w, h, pixels [, options]) -> texture`
--- Makes a texture from a string of w * h * 4 bytes (red, green, blue, alpha).
function gfx.new_texture(w, h, pixels, options) end

--- `gfx.new_target(w, h) -> texture`
--- A texture that can be drawn into with gfx.target, then used like any other.
function gfx.new_target(w, h) end

--- `gfx.texture_size(t) -> w, h`
--- A texture's size in pixels.
function gfx.texture_size(t) end

--- `gfx.free_texture(t)`
--- Lets go of a texture made by the game. Everything is freed anyway when the game changes.
function gfx.free_texture(t) end

--- `gfx.new_mesh(numbers [, indices [, layout]]) -> mesh`
--- Builds a mesh from a flat list of numbers. layout says what each vertex holds, in order: p position (3), n normal (3), u texture coordinate (2), c colour (4). The default is "pnu". indices count from 1.
function gfx.new_mesh(numbers, indices, layout) end

--- `gfx.free_mesh(m)`
--- Lets go of a mesh made by the game.
function gfx.free_mesh(m) end

--- `gfx.load_font(file [, pixel_height]) -> font`
--- Loads a .ttf. Letters are drawn into a shared picture the first time each is used, so any language the font covers works.
function gfx.load_font(file, pixel_height) end

--- The built-in cube, one unit across. Use with gfx.draw.
gfx.CUBE = 0
--- The built-in sphere, one unit across.
gfx.SPHERE = 0
--- The built-in flat square, one unit across, facing up.
gfx.PLANE = 0

-- # Drawing in 3D

--- `gfx.draw(mesh, <transform>, r, g, b, a)`
--- Draws a mesh with the current shader, textures and settings.
function gfx.draw(mesh, ...) end

--- `gfx.cube(x, y, z, sx, sy, sz, r, g, b [, yaw [, alpha]])`
--- A box, centred on x, y, z. yaw turns it about the up axis, in degrees.
function gfx.cube(x, y, z, sx, sy, sz, r, g, b, yaw, alpha) end

--- `gfx.sphere(x, y, z, diameter, r, g, b [, a])`
--- A ball.
function gfx.sphere(x, y, z, diameter, r, g, b, a) end

--- `gfx.plane(x, y, z, sx, sz, r, g, b [, a])`
--- A flat rectangle facing up, such as a floor.
function gfx.plane(x, y, z, sx, sz, r, g, b, a) end

--- `gfx.particles(numbers [, count])`
--- Squares that always face the camera, eight numbers each: x, y, z, size, r, g, b, a.
function gfx.particles(numbers, count) end

--- `gfx.line(x1, y1, z1, x2, y2, z2, r, g, b, a [, width])`
--- A line in the world. width is in world units (metres), 0.03 if left out. Useful for debugging.
function gfx.line(x1, y1, z1, x2, y2, z2, r, g, b, a, width) end

--- `gfx.project(x, y, z) -> sx, sy`
--- Where a point in the world appears on the screen, in 2D units. Nothing if it is behind the camera.
function gfx.project(x, y, z) end

--- `gfx.unproject(sx, sy) -> ox, oy, oz, dx, dy, dz`
--- The ray into the world under a point on the screen: where it starts and which way it goes. Feed it to phys.raycast to find what a finger is on.
function gfx.unproject(sx, sy) end

-- # Drawing in 2D (the screen's shorter side is always 720 units)

--- `gfx.rect(x, y, w, h, r, g, b [, a])`
--- A filled rectangle. x, y is its top-left corner.
function gfx.rect(x, y, w, h, r, g, b, a) end

--- `gfx.circle(x, y, radius, r, g, b [, a])`
--- A filled circle.
function gfx.circle(x, y, radius, r, g, b, a) end

--- `gfx.image(texture, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]])`
--- A picture. The last four numbers pick part of the texture, from 0 to 1.
function gfx.image(texture, x, y, w, h, r, g, b, a, u0, v0, u1, v1) end

--- `gfx.text(s, x, y, size, r, g, b [, a])`
--- Text. size is the height of a line in 2D units. A line break in s starts a new line.
function gfx.text(s, x, y, size, r, g, b, a) end

--- `gfx.text_width(s, size) -> width`
--- How wide a piece of text would be, for centring and wrapping.
function gfx.text_width(s, size) end

--- `gfx.size() -> w, h`
--- The screen in 2D units. The shorter side is 720.
function gfx.size() end

--- `gfx.safe() -> left, top, right, bottom`
--- How far to stay in from each edge to clear the notch, Dynamic Island, rounded corners and home bar.
function gfx.safe() end

-- # Models (.glb files from Blender)

--- `model.load(file) -> m`
--- Loads a .glb (in Blender: File > Export > glTF 2.0, format "glTF Binary") with its meshes, textures, skeleton and animations.
function model.load(file) end

--- `model.nodes(m) -> { {name, parent, x, y, z, qx, qy, qz, qw, sx, sy, sz, mesh, extras, light, r, g, b, range, angle, fov}, ... }`
--- Every object in the file. extras holds the object's Custom Properties from Blender, which is how a level file marks spawn points and pickups.
function model.nodes(m) end

--- `model.clips(m) -> { {name, duration}, ... }`
--- The animations in the file.
function model.clips(m) end

--- `model.draw(m, <transform>, r, g, b, a [, animator [, node]])`
--- Draws the model. With an animator, in its current pose. With a node (name or number), just that part, placed at the transform.
function model.draw(m, ...) end

--- `model.animator(m) -> a`
--- Something that holds a pose for one character. Make one per character on screen.
function model.animator(m) end

--- `model.pose(m, a, clip, time, weight [, clip, time, weight ...])`
--- Sets the animator's pose from up to 8 clips mixed by weight. Mixing two clips while one weight falls and the other rises is a cross-fade.
function model.pose(m, a, ...) end

--- `model.joint(m, a, node) -> <transform>`
--- Where a bone is now, inside the model. For attaching a sword to a hand.
function model.joint(m, a, node) end

--- `model.local(m, a, node [, <transform>])`
--- Reads a bone's own position, or sets it by hand after posing (a head turning to look at something).
function model.local(m, a, node, ...) end

--- `model.sample(m, clip, node, time) -> <transform>`
--- Where an animation puts one node at a time, without an animator.
function model.sample(m, clip, node, time) end

-- # Collision shapes (no physics solver: you move things, these tell you what they touch)

--- `phys.sphere(x, y, z, radius [, layer]) -> body`
--- A ball. layer is a bit set (1, 2, 4, 8...) used to choose what questions see it.
function phys.sphere(x, y, z, radius, layer) end

--- `phys.capsule(x, y, z, radius, height [, layer]) -> body`
--- An upright pill: the usual shape for a character. It never tilts.
function phys.capsule(x, y, z, radius, height, layer) end

--- `phys.box(x, y, z, sx, sy, sz [, qx, qy, qz, qw [, layer]]) -> body`
--- A box of full sizes sx, sy, sz, optionally rotated.
function phys.box(x, y, z, sx, sy, sz, qx, qy, qz, qw, layer) end

--- `phys.mesh(mesh, <transform> [, layer]) -> body`
--- A mesh that does not move, such as level geometry.
function phys.mesh(mesh, ...) end

--- `phys.model(m, <transform> [, layer [, node]]) -> { bodies }`
--- Every mesh in a model as static collision, or just one node's.
function phys.model(m, ...) end

--- `phys.place(body, x, y, z [, qx, qy, qz, qw])`
--- Moves a body to a position, with no checks.
function phys.place(body, x, y, z, qx, qy, qz, qw) end

--- `phys.position(body) -> x, y, z`
--- Where a body is.
function phys.position(body) end

--- `phys.remove(body)`
--- Removes one body.
function phys.remove(body) end

--- `phys.clear()`
--- Removes every body.
function phys.clear() end

-- # Collision questions

--- `phys.raycast(ox, oy, oz, dx, dy, dz, distance [, mask [, ignore]]) -> distance, x, y, z, nx, ny, nz, body`
--- The first thing a line hits: how far, where, the surface direction there, and which body. Nothing on a miss.
function phys.raycast(ox, oy, oz, dx, dy, dz, distance, mask, ignore) end

--- `phys.spherecast(ox, oy, oz, dx, dy, dz, distance, radius [, mask [, ignore]]) -> distance, x, y, z, nx, ny, nz, body`
--- The same for a ball pushed along a line. Good for thick bullets and camera collision.
function phys.spherecast(ox, oy, oz, dx, dy, dz, distance, radius, mask, ignore) end

--- `phys.overlap(x, y, z, radius [, mask]) -> { bodies }`
--- Every body touching a ball. For pickups, explosions and triggers.
function phys.overlap(x, y, z, radius, mask) end

--- `phys.move(body, dx, dy, dz [, mask [, step, slope]]) -> x, y, z, grounded, blocked, nx, ny, nz`
--- Moves a character by an amount, sliding along walls, climbing steps up to step high and refusing slopes steeper than slope degrees.
function phys.move(body, dx, dy, dz, mask, step, slope) end

--- `phys.debug()`
--- Draws every shape in lines. Call inside draw.
function phys.debug() end

-- # Sound (.wav and .ogg; volumes are decibels: 0 full, -6 about half, -80 silent)

--- `audio.load(file [, stream]) -> sound`
--- Loads a sound. stream = true plays a long .ogg from the file a little at a time, for music.
function audio.load(file, stream) end

--- `audio.play(sound [, volume, pitch, loop, group]) -> voice`
--- Starts a sound and returns the voice playing it. group is 0 to 7.
function audio.play(sound, volume, pitch, loop, group) end

--- `audio.stop(voice [, fade_seconds])`
--- Stops a voice, at once or by fading out.
function audio.stop(voice, fade_seconds) end

--- `audio.playing(voice) -> bool`
--- Whether a voice is still sounding.
function audio.playing(voice) end

--- `audio.volume(voice, db)`
--- Changes a playing voice's volume.
function audio.volume(voice, db) end

--- `audio.pitch(voice, p)`
--- Changes its speed and pitch. 1 is normal, 2 an octave up.
function audio.pitch(voice, p) end

--- `audio.pan(voice, p)`
--- -1 is fully left, 1 fully right.
function audio.pan(voice, p) end

--- `audio.lowpass(voice, hz)`
--- Muffles everything above a frequency, as behind a wall or under water.
function audio.lowpass(voice, hz) end

--- `audio.position(voice, x, y, z [, min_radius, max_radius])`
--- Places a voice in the world. It is heard from the camera: full volume inside min_radius, silent beyond max_radius.
function audio.position(voice, x, y, z, min_radius, max_radius) end

--- `audio.listener(x, y, z, rightx, righty, rightz)`
--- Where the ears are and which way is right. Only needed when the ears are not the camera.
function audio.listener(x, y, z, rightx, righty, rightz) end

--- `audio.group(group, db [, fade_seconds])`
--- The volume of a whole group, 0 to 7: music, effects, voices.
function audio.group(group, db, fade_seconds) end

--- `audio.group_pause(group, paused)`
--- Stops and resumes everything in a group, for a pause menu.
function audio.group_pause(group, paused) end

-- # Touch controls

--- `input.controls{ {type = "stick" | "button", id =, x =, y =, size =, label =, key =}, ... }`
--- Describes the on-screen sticks and buttons. x and y are 0 to 1 across the screen; key is a PC key that presses the button.
function input.controls(list) end

--- `input.stick(id) -> x, y`
--- A stick's position, each from -1 to 1. On the PC, WASD or the arrow keys drive the first stick.
function input.stick(id) end

--- `input.down(id) -> bool`
--- Whether a button is being held.
function input.down(id) end

--- `input.pressed(id) -> bool`
--- True only on the frame a button went down.
function input.pressed(id) end

--- `input.touches() -> { {id, x, y, dx, dy, phase = "down" | "move" | "up", claimed}, ... }`
--- Every finger on the screen (the mouse on the PC), in 2D units. claimed means a stick or button took it.
function input.touches() end

-- # Sensors, pads and feedback

--- `input.key(name) -> bool`
--- A PC keyboard key by name ("space", "a", "left"). For testing.
function input.key(name) end

--- `input.tilt() -> x, y, z`
--- Which way is down, in units of gravity. Zero on the PC.
function input.tilt() end

--- `input.gyro() -> x, y, z`
--- How fast the phone is turning, in radians a second.
function input.gyro() end

--- `input.pad(name) -> number`
--- A game controller's axis (-1 to 1) or button (0 or 1): "leftx", "lefty", "rightx", "a", "b", "start"...
function input.pad(name) end

--- `input.haptic("light" | "medium" | "heavy" | "success" | "warning" | "error")`
--- A tap from the phone's motor.
function input.haptic(kind) end

-- # Keyboard and clipboard

--- `input.keyboard([show [, kind]]) -> showing`
--- Shows or hides the phone's keyboard; with no arguments, only says whether it is up. kind: "text", "email", "number", "password", or "code" (digits; iOS offers a code that just arrived by SMS). Typed text arrives through on_text and on_key.
function input.keyboard(show, kind) end

--- `input.paste() -> text`
--- What is on the clipboard. On the iPhone, iOS asks the player first.
function input.paste() end

--- `input.copy(text)`
--- Puts text on the clipboard.
function input.copy(text) end

-- # Web calls

--- `net.request({ url =, method = "GET", headers = {}, body =, timeout = 15 }, function(status, body, err) ... end)`
--- Calls a web API. Returns at once; the function is called later with status and body, or with nil, nil and a message. Only https, and only hosts named in the game's game.cfg.
function net.request(options, callback) end

--- `net.backend({ path = "/scores", method = "GET", headers = {}, body =, timeout = 15 }, function(status, body, err) ... end)`
--- Calls this game's own backend. While developing, that is backends/<game>/server.py, run by dev.py on your PC; in a standalone release, the address in game.cfg's backend line. The game never names a host.
function net.backend(options, callback) end

-- # Motion capture (iPhone only: ARKit body tracking through the back camera)

--- `mocap.start() -> true | false, why`
--- Starts tracking a body. On the PC, or an iPhone older than the XS / XR, it answers false and why.
function mocap.start() end

--- `mocap.stop()`
--- Turns the camera off. It also goes off when the game stops.
function mocap.stop() end

--- `mocap.pose() -> tracked, positions, parents, error`
--- The latest skeleton: positions holds x, y, z for each joint in metres around the hips (y up); parents gives each joint's parent, counting from 1, with 0 for the root. Draw a line from each joint to its parent to see the body.
function mocap.pose() end

--- `mocap.camera() -> texture, width, height`
--- The back camera's picture, upright and small (270 x 360), refreshed every frame; draw it with gfx.image. Nothing until the camera is running. On the PC, GAMELAB_FAKE_CAMERA=1 gives a moving test picture.
function mocap.camera() end

--- `mocap.screen() -> { x, y, x, y, ... }`
--- Where each joint appears on that picture, from 0 to 1 across and down, in the same order as mocap.pose. Nothing while no body is tracked.
function mocap.screen() end

--- `mocap.record(on)`
--- true starts a new take, false ends it.
function mocap.record(on) end

--- `mocap.frames() -> count`
--- Frames in the current take.
function mocap.frames() end

--- `mocap.take() -> text | nil`
--- The take as text, for sending to the game's backend with net.backend, which turns it into a .bvh file.
function mocap.take() end

-- # Saved values

--- `save.get(key, default) -> value`
--- A value kept between runs of the game, or the default if it was never set.
function save.get(key, default) end

--- `save.set(key, value)`
--- Keeps a number, string or boolean. Written to a plain file, so not the place for a login token.
function save.set(key, value) end

-- # Time and the app

--- `sys.time() -> seconds`
--- Game time since the game started. Slowed or stopped by sys.time_scale.
function sys.time() end

--- `sys.real_time() -> seconds`
--- Time that keeps running at normal speed whatever the time scale. For menus shown while the game is stopped.
function sys.real_time() end

--- `sys.time_scale([scale]) -> scale`
--- 1 is normal, 0.3 slow motion, 0 stops the game. With no argument, returns the current scale.
function sys.time_scale(scale) end

--- `sys.step()`
--- While stopped, moves the game on by exactly one tick.
function sys.step() end

--- `sys.hud(on)`
--- Shows the frame time, draw calls and memory in a corner (F3 on the PC).
function sys.hud(on) end

--- `sys.stats() -> fps, draws, vertices, sounds`
--- The numbers behind the HUD.
function sys.stats() end

--- `sys.platform() -> "ios" | "pc"`
--- Where the game is running.
function sys.platform() end

-- # Files and printing

--- `print(...)`
--- Shows in the dev.py window, from the phone as well as the PC.
function print(...) end

--- `include(file) -> whatever the file returns`
--- Runs another Lua file from the game's folder. This replaces require, dofile and loadfile.
function include(file) end

-- # Called by the engine, if the game defines them

--- `function init() ... end`
--- Once, when the game starts and after every reload.
-- callback init

--- `function fixed_update(dt) ... end`
--- A steady 60 times a second whatever the frame rate: movement and collision go here.
-- callback fixed_update

--- `function update(dt) ... end`
--- Once a frame, before drawing. dt is the seconds since the last frame, already scaled by sys.time_scale.
-- callback update

--- `function late_update(dt) ... end`
--- Once a frame after update: cameras, and things attached to other things.
-- callback late_update

--- `function draw() ... end`
--- Once a frame. All gfx drawing happens here.
-- callback draw

--- `function on_pause() ... end`
--- The app is going to the background. Save now.
-- callback on_pause

--- `function on_resume() ... end`
--- The app is back.
-- callback on_resume

--- `function on_text(text) ... end`
--- While the keyboard is up: the player typed or pasted this.
-- callback on_text

--- `function on_key(name) ... end`
--- While the keyboard is up: "backspace" or "return".
-- callback on_key
