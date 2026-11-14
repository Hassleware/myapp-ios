# Audio

`engine/audio.cpp` is a small mixer. SDL asks it for the next piece of sound many times a second, and it adds together every playing sound into one stereo stream at 48,000 samples a second.

## What it offers

::: file engine/audio.h

## Sounds and voices

A **sound** is a loaded file. A **voice** is one playing instance of it. Playing the same sound three times gives three voices, each with its own volume and pitch. Up to 32 voices play at once.

```lua
local jump  = audio.load("jump.wav")
local music = audio.load("theme.ogg", true)        -- true: stream it from the file

local MUSIC, SFX = 1, 2                            -- groups, named by the game

function init()
  audio.play(music, -8, 1, true, MUSIC)            -- volume, pitch, loop, group
end

function update(dt)
  if input.pressed("jump") then
    audio.play(jump, -3, 0.95 + math.random() * 0.1, false, SFX)   -- a little pitch variety
  end
end
```

| Kind | Format | How it is held |
|---|---|---|
| Short effects | `.wav`, or `.ogg` | Unpacked into memory when loaded |
| Music | `.ogg` with `stream = true` | Read from the file a little at a time |

## Decibels

Every volume is in decibels, because that is how loudness is heard: each step of 6 sounds about half as loud.

| dB | Sounds like |
|---|---|
| `0` | Full volume, as recorded |
| `-6` | About half |
| `-12` | About a quarter |
| `-80` | Silent |

The conversion to the multiplier the mixer uses:

::: source engine/audio.cpp from_db

## Groups

A group is a volume control for many voices at once. There are eight, numbered 0 to 7; a game decides what they mean. Group 0 is where voices go if you do not say.

```lua
audio.group(MUSIC, -20, 1.5)          -- fade the music down over 1.5 seconds
audio.group_pause(SFX, true)          -- freeze every effect, for a pause menu
```

`lib.duck` lowers one group while something important plays and then restores it:

::: source tools/template/lib.lua lib.duck

## Sounds in the world

```lua
local hum = audio.play(engine_sound, 0, 1, true)
audio.position(hum, car.x, car.y, car.z, 2, 30)     -- full volume within 2 units, silent beyond 30
```

A positioned voice is panned and faded by where it is relative to the listener. The listener is the camera unless the game calls `audio.listener`. For a third-person game you may prefer ears at the character rather than the camera.

Update the position every frame for anything that moves.

## Shaping a voice while it plays

| Call | Effect | Typical use |
|---|---|---|
| `audio.volume(v, db)` | Louder or quieter | An engine under load |
| `audio.pitch(v, p)` | Faster and higher, or slower and lower | Engine revs; slow motion |
| `audio.pan(v, -1..1)` | Left to right | Interface sounds |
| `audio.lowpass(v, hz)` | Muffles everything above a frequency | Behind a wall, under water, a paused game |
| `audio.stop(v, seconds)` | Stops, with an optional fade | Ending music cleanly |

## Variety without effort

The same footstep played ten times in a row sounds mechanical. `lib.cue` plays a set of takes in shuffled order with slight pitch changes:

::: source tools/template/lib.lua lib.cue

## On the phone

- **The audio session is "ambient".** The ring switch silences the game, and the player's own music keeps playing underneath. That is right for most games. A music game or one that needs sound with the switch off would need a different category, which is an engine change.
- **Sound stops when the app goes to the background** and resumes when it returns.
- **Files are loaded on the main thread.** A long `.wav` unpacked during play causes a hitch. Load everything in `init`, and stream music.

## Things that go wrong

| Symptom | Cause |
|---|---|
| Nothing plays, and `dev.py` shows a missing-file warning | The name is wrong or the file is not in the game's folder |
| No sound on the phone only | The ring switch is on silent |
| Music stutters when it starts | It was loaded without `stream = true` |
| Sounds cut off when many play | More than 32 voices. Stop long ones, or play fewer |
| An `.mp3` does not load | Only `.wav` and `.ogg` are supported. Convert it |

## Exercises

??? example "1. Fade the music out over two seconds when the level ends"
    `audio.stop(music_voice, 2)`. Keep the voice returned by `audio.play`.

??? example "2. Make everything sound muffled while the game is paused"
    Keep your voices in a table and call `audio.lowpass(v, 800)` on each, then `audio.lowpass(v, 0)` to restore. Or put a quiet pause-menu track in its own group and pause the rest with `audio.group_pause`.

??? example "3. How loud is `-18` dB compared with full volume?"
    About an eighth: three halvings.
