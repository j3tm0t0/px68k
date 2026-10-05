#!/usr/bin/env python3
"""Summarize an "arec" recording: what the PSP's sound callback played.

  arec.py <arec.wav> [arec.csv]

From the callback log (CSV): callbacks, their interval, how much sound the
emulation had ready, and the callbacks that ran short and synthesized the
rest themselves (the music stretches by that much).  From the WAV: runs of
digital silence and sample-to-sample jumps (clicks).
"""
import csv
import statistics
import sys
import wave
from array import array

wav = sys.argv[1]
log = sys.argv[2] if len(sys.argv) > 2 else wav[:-4] + ".csv"

rows = [(int(r["t_us"]), int(r["avail"]), int(r["filled"])) for r in csv.DictReader(open(log))]
if len(rows) > 1:
    iv = [(b[0] - a[0]) / 1000 for a, b in zip(rows, rows[1:])]
    short = [r for r in rows if r[2]]
    filled_ms = sum(r[2] for r in rows) / 4 / 44.1
    total_ms = (rows[-1][0] - rows[0][0]) / 1000
    print(f"callbacks {len(rows)} over {total_ms / 1000:.1f} s, interval "
          f"median {statistics.median(iv):.1f} ms, max {max(iv):.1f} ms")
    print(f"ready sound: median {statistics.median(r[1] for r in rows) / 4 / 44.1:.1f} ms, "
          f"min {min(r[1] for r in rows) / 4 / 44.1:.1f} ms")
    print(f"short callbacks {len(short)} ({100 * len(short) / len(rows):.1f}%), "
          f"synthesized by the callback {filled_ms:.0f} ms = {100 * filled_ms / total_ms:.1f}% "
          f"of the time (the music runs that much slower)")

w = wave.open(wav)
pcm = array("h", w.readframes(w.getnframes()))
if sys.byteorder == "big":
    pcm.byteswap()
left = pcm[0::2]
rate = w.getframerate()
runs, run = [], 0
for x in left:
    if x == 0:
        run += 1
    else:
        if run >= rate // 200:  # 5 ms
            runs.append(run)
        run = 0
loud = [abs(x) for x in left]
level = statistics.mean(loud) if loud else 0
jumps = sum(1 for a, b in zip(left, left[1:]) if abs(b - a) > 12000)
print(f"wav {len(left) / rate:.1f} s, mean level {level:.0f}, "
      f"silent runs >= 5 ms: {len(runs)} ({sum(runs) * 1000 // rate} ms), "
      f"jumps > 12000: {jumps}")
