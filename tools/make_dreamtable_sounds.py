"""Makes Dream Table's hit sounds (no recordings needed): a crisp bat "tok" and a softer table "tick".

    python tools/make_dreamtable_sounds.py

Writes games/dream-table/hit_bat.wav and bounce.wav: 22050 Hz, mono, 16-bit, a few kilobytes each.
"""
import math
import os
import random
import struct
import wave

RATE = 22050
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "games", "dream-table")


def write(name, samples):
    peak = max(abs(s) for s in samples) or 1
    with wave.open(os.path.join(OUT, name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(s / peak * 30000)) for s in samples))


def tone(length, parts, click=0.0, seed=1):
    """parts: (frequency, decay seconds, level); click: a burst of noise at the start."""
    rng = random.Random(seed)
    out = []
    for i in range(int(RATE * length)):
        t = i / RATE
        s = sum(level * math.sin(2 * math.pi * f * t) * math.exp(-t / decay) for f, decay, level in parts)
        s += click * rng.uniform(-1, 1) * math.exp(-t / 0.0025)
        out.append(s * min(1, i / 20))             # no hard start
    return out


write("hit_bat.wav", tone(0.12, [(2400, 0.018, 0.7), (1150, 0.03, 0.8), (520, 0.05, 0.5)], click=1.2, seed=1))
write("bounce.wav", tone(0.09, [(1650, 0.022, 0.6), (820, 0.03, 0.5)], click=0.6, seed=2))
print("SOUNDS written")
