-- The Dream Table shot list: what to perform for each mocap take, shown as a card in the mocap game.
-- Edit freely; saving reloads the phone. Right-handed; mirror the takes in Blender for a left-hander.
--
-- Every take has the same shape, so the game can bend it to any ball later:
--   * start still in the READY stance, on the same spot, side-on to the phone, the table to your LEFT in the picture;
--   * three counting beeps, recording starts on the last one;
--   * a high beep at `contact` seconds: hit the imaginary ball exactly on it;
--   * a low beep at `length` seconds: recording stops. Be back in READY by then.
-- So in every take the contact is at a known time and place: the game slows or speeds the swing so contact lands
-- when the ball arrives, and moves the bat hand onto the ball with IK.
--
-- Diagram units are metres from the middle of your feet: forward = toward the table (the table edge is at 0.5),
-- up = height (the table top is 0.76), side = + to your right (bat side), - to your left.
-- path = where the bat travels: start, contact (the ball), finish.
-- swipe = the finger swipe in Dream Table that picks this shot (the line's math then adjusts it).

return {
  { id = "ready", name = "Ready stance (idle)", contact = 2.0, length = 4.0,
    cues = { "Feet wider than shoulders,", "knees bent, weight forward,", "bat in front of the belly.", "Small sways only. No hit." },
    swipe = "none: played between shots",
    path = { { f = 0.25, u = 1.0, s = 0.1 }, { f = 0.27, u = 1.0, s = 0.1 }, { f = 0.25, u = 1.0, s = 0.1 } } },

  { id = "fh-drive", name = "Forehand drive", contact = 1.5, length = 3.0,
    cues = { "Turn the waist back,", "swing forward and a bit up,", "hit level with the table top,", "finish bat by the forehead." },
    swipe = "straight, upward, medium speed",
    path = { { f = -0.1, u = 0.75, s = 0.55 }, { f = 0.35, u = 0.9, s = 0.35 }, { f = 0.4, u = 1.45, s = -0.15 } } },

  { id = "fh-loop", name = "Forehand loop (topspin)", contact = 1.6, length = 3.2,
    cues = { "Drop the bat low, knee high,", "brush up the back of the ball,", "contact below table height,", "finish high over the head." },
    swipe = "starts low, sweeps up steeply",
    path = { { f = -0.15, u = 0.45, s = 0.5 }, { f = 0.3, u = 0.75, s = 0.35 }, { f = 0.3, u = 1.7, s = -0.1 } } },

  { id = "fh-smash", name = "Forehand smash", contact = 1.6, length = 3.2,
    cues = { "Bat back high,", "hit down and forward hard,", "contact above the shoulder,", "finish by the left hip." },
    swipe = "fast, straight, downward",
    path = { { f = -0.2, u = 1.6, s = 0.55 }, { f = 0.4, u = 1.5, s = 0.3 }, { f = 0.35, u = 0.8, s = -0.3 } } },

  { id = "fh-push", name = "Forehand push (backspin)", contact = 1.4, length = 2.8,
    cues = { "Step in with the right foot,", "open bat face to the sky,", "slide under the ball,", "short follow-through." },
    swipe = "short, slow, downward then forward",
    path = { { f = 0.2, u = 0.85, s = 0.3 }, { f = 0.6, u = 0.8, s = 0.2 }, { f = 0.8, u = 0.78, s = 0.1 } } },

  { id = "fh-flick", name = "Forehand flick", contact = 1.4, length = 2.8,
    cues = { "Step in, bat over the table,", "flick the wrist up and over,", "contact at the top of bounce,", "stop the bat at eye level." },
    swipe = "short, quick, curls upward",
    path = { { f = 0.45, u = 0.8, s = 0.3 }, { f = 0.65, u = 0.88, s = 0.2 }, { f = 0.7, u = 1.2, s = 0.05 } } },

  { id = "bh-drive", name = "Backhand drive", contact = 1.5, length = 3.0,
    cues = { "Bat in front of the belly,", "elbow forward, push out,", "hit in front of the body,", "finish pointing at the target." },
    swipe = "starts on the left, straight forward",
    path = { { f = 0.1, u = 0.95, s = -0.1 }, { f = 0.4, u = 1.0, s = 0.0 }, { f = 0.6, u = 1.15, s = 0.15 } } },

  { id = "bh-loop", name = "Backhand loop (topspin)", contact = 1.6, length = 3.2,
    cues = { "Wrist curled in, bat low,", "uncoil forearm and wrist up,", "brush the ball in front,", "finish up by the right ear." },
    swipe = "starts on the left, sweeps up",
    path = { { f = 0.15, u = 0.7, s = -0.15 }, { f = 0.4, u = 0.9, s = 0.0 }, { f = 0.45, u = 1.45, s = 0.3 } } },

  { id = "bh-push", name = "Backhand push (backspin)", contact = 1.4, length = 2.8,
    cues = { "Bat face open to the sky,", "push forward and under,", "contact over the table,", "keep it short and soft." },
    swipe = "on the left, short, slow, down",
    path = { { f = 0.25, u = 0.88, s = -0.1 }, { f = 0.6, u = 0.82, s = -0.05 }, { f = 0.75, u = 0.8, s = 0.0 } } },

  { id = "bh-block", name = "Backhand block", contact = 1.4, length = 2.8,
    cues = { "Bat still in front,", "angle it closed a little,", "let the ball hit it,", "tiny push, no swing." },
    swipe = "a tap or very short line on the left",
    path = { { f = 0.35, u = 1.0, s = -0.05 }, { f = 0.4, u = 1.0, s = -0.05 }, { f = 0.45, u = 1.02, s = -0.05 } } },

  { id = "serve", name = "Forehand pendulum serve", contact = 2.4, length = 4.0,
    cues = { "Free hand tosses ball up 16cm,", "bat swings across, low,", "hit as it falls, by the table,", "back to READY at once." },
    swipe = "a curve: its bend picks the spin",
    path = { { f = 0.0, u = 1.0, s = 0.45 }, { f = 0.45, u = 0.82, s = 0.2 }, { f = 0.4, u = 0.9, s = -0.25 } } },
}
