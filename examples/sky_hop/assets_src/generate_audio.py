#!/usr/bin/env python3
"""Generate original deterministic chiptune cues for Sky Hop."""
from __future__ import annotations

from pathlib import Path
import math
import struct
import wave

RATE = 24000
ROOT = Path(__file__).resolve().parent

def write(name: str, samples: list[float]) -> None:
    pcm = b"".join(struct.pack("<h", max(-32767, min(32767, round(v * 32767))))
                   for v in samples)
    with wave.open(str(ROOT / name), "wb") as out:
        out.setnchannels(1); out.setsampwidth(2); out.setframerate(RATE); out.writeframes(pcm)

def tone(freq: float, seconds: float, volume: float = .28, decay: float = 2.5) -> list[float]:
    count = round(RATE * seconds)
    return [volume * math.sin(2 * math.pi * freq * i / RATE) *
            max(0.0, 1.0 - i / count) ** decay for i in range(count)]

def sequence(notes: list[tuple[float, float]], volume: float = .18) -> list[float]:
    result: list[float] = []
    for freq, duration in notes:
        result.extend(tone(freq, duration, volume, .35))
    return result

write("jump.wav", sequence([(392, .07), (523.25, .08), (659.25, .11)], .25))
write("coin.wav", sequence([(880, .06), (1174.66, .07), (1567.98, .12)], .23))
write("stomp.wav", sequence([(180, .05), (120, .11)], .30))
write("hurt.wav", sequence([(220, .08), (174.61, .08), (130.81, .18)], .27))
write("win.wav", sequence([(523.25, .12), (659.25, .12), (783.99, .12),
                            (1046.5, .34)], .24))

melody = [(261.63,.16),(329.63,.16),(392,.16),(523.25,.16),
          (392,.16),(329.63,.16),(293.66,.16),(392,.16)]
music: list[float] = []
for _ in range(8):
    music.extend(sequence(melody, .10))
write("music.wav", music)
