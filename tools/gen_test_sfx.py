#!/usr/bin/env python3
"""Generate tiny placeholder test WAVs for the audio mixer (np-3gw.1).

Procedurally synthesized => CC0-by-construction, no licensing thoughts
required. These are STAND-INS — final SFX production is tracked as a
separate backlog issue. The gameplay set (np-3gw.2):

    blip.wav          - 100ms 880Hz sine ping (mixer test / debug button)
    burst.wav         - 120ms white-noise burst (mixer test)
    laser_fire.wav    - 80ms descending chirp (gun shot)
    impact_shield.wav - soft lowpassed thunk (shield absorbs a hit)
    impact_armor.wav  - harsher noise crack (armor takes a hit)
    explosion_small.wav / explosion_big.wav - lowpassed noise with
                        exponential tail; big = longer + deeper
    engine_hum.wav    - 1s loop-clean 2-osc hum + slow amplitude shimmer
                        (the looping engine bed; gain ridden by throttle)
    cruise_windup.wav - 1.5s rising sweep (cruise engage)
    ui_click.wav      - 5ms tick (nav/target cycle)
    hum.wav           - kept for back-compat with the np-3gw.1 debug button

All PCM16 mono 44.1kHz, written to assets/sfx/. Idempotent; rerun freely.
"""

import math
import os
import random
import struct
import wave

RATE = 44100
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "sfx")


def write_wav(name: str, samples: list[float]) -> None:
    """Clamp float [-1,1] samples to PCM16 and write a mono WAV."""
    path = os.path.join(OUT_DIR, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples
        )
        w.writeframes(frames)
    print(f"wrote {path} ({len(samples)} samples)")


def blip() -> list[float]:
    """880Hz sine, 100ms, exponential decay. Reads as a clean UI ping."""
    n = int(RATE * 0.100)
    return [
        math.sin(2 * math.pi * 880 * t / RATE) * math.exp(-t / (RATE * 0.025)) * 0.8
        for t in range(n)
    ]


def burst() -> list[float]:
    """White noise, 120ms, linear decay. Impact-ish placeholder."""
    rng = random.Random(42)  # deterministic => reruns produce identical files
    n = int(RATE * 0.120)
    return [(rng.random() * 2 - 1) * (1 - t / n) * 0.7 for t in range(n)]


def hum() -> list[float]:
    """55Hz + 110Hz blend, exactly 1s. Both frequencies divide RATE evenly
    over the duration (55 and 110 full cycles), so the loop point is
    click-free by construction — that's the property np-3gw.2's looping
    engine bed needs."""
    n = RATE  # 1.0s
    return [
        (
            0.6 * math.sin(2 * math.pi * 55 * t / RATE)
            + 0.3 * math.sin(2 * math.pi * 110 * t / RATE)
        )
        * 0.5
        for t in range(n)
    ]


def lowpass(samples: list[float], alpha: float) -> list[float]:
    """One-pole lowpass. alpha in (0,1]; smaller = darker."""
    out, acc = [], 0.0
    for s in samples:
        acc += alpha * (s - acc)
        out.append(acc)
    return out


def laser_fire() -> list[float]:
    """80ms chirp descending 1800->600Hz, fast decay. Reads as 'pew'."""
    n = int(RATE * 0.080)
    out, phase = [], 0.0
    for t in range(n):
        k = t / n
        freq = 1800 * (1 - k) + 600 * k
        phase += 2 * math.pi * freq / RATE
        out.append(math.sin(phase) * math.exp(-t / (RATE * 0.020)) * 0.7)
    return out


def impact_shield() -> list[float]:
    """Soft thunk: heavily lowpassed noise, 90ms. Energy-absorbed feel."""
    rng = random.Random(7)
    n = int(RATE * 0.090)
    noise = [(rng.random() * 2 - 1) * math.exp(-t / (RATE * 0.025)) for t in range(n)]
    return [s * 0.9 for s in lowpass(noise, 0.08)]


def impact_armor() -> list[float]:
    """Harsh crack: brighter noise, sharper envelope, 70ms. Metal pain."""
    rng = random.Random(13)
    n = int(RATE * 0.070)
    noise = [(rng.random() * 2 - 1) * math.exp(-t / (RATE * 0.012)) for t in range(n)]
    return [s * 0.8 for s in lowpass(noise, 0.45)]


def explosion(seed: int, dur_s: float, alpha: float, gain: float) -> list[float]:
    """Noise burst with exponential tail, lowpassed. dur/alpha set the size."""
    rng = random.Random(seed)
    n = int(RATE * dur_s)
    tail = dur_s * 0.25
    noise = [(rng.random() * 2 - 1) * math.exp(-t / (RATE * tail)) for t in range(n)]
    return [s * gain for s in lowpass(noise, alpha)]


def engine_hum() -> list[float]:
    """Smooth low idle rumble (np-4dr). The OLD hum stacked 55/110/165Hz
    at 0.55/0.28/0.12 — that reedy 3rd-harmonic content read as a constant
    background BUZZ (user feedback), not an engine bed. This is the
    de-buzzed replacement: a dominant 60Hz fundamental + only a GENTLE 2nd
    harmonic (120Hz, low level) for warmth, plus a slow 1Hz amplitude
    breath, at a lower master level. No reedy upper harmonic => smooth, not
    buzzy.

    Loop seam is click-free BY CONSTRUCTION: 60, 120 and 1 all complete
    INTEGER cycles over the exactly-1s loop, so the wrap from the last
    sample back to sample[0] is just the next natural step of every
    component — no discontinuity, no DC offset. (Do NOT add a seam
    crossfade: blending the tail toward sample[~5ms] would MOVE the last
    sample away from sample[0] and CREATE the very seam step it was
    meant to remove. The periodicity already guarantees seamlessness.)"""
    n = RATE
    out = []
    for t in range(n):
        base = (
            0.80 * math.sin(2 * math.pi * 60 * t / RATE)
            + 0.12 * math.sin(2 * math.pi * 120 * t / RATE)
        )
        breath = 1.0 + 0.05 * math.sin(2 * math.pi * 1 * t / RATE)
        out.append(base * breath * 0.35)
    return out


def cruise_windup() -> list[float]:
    """1.5s rising sweep 80->480Hz with a slow attack — spool-up feel."""
    n = int(RATE * 1.5)
    out, phase = [], 0.0
    for t in range(n):
        k = t / n
        freq = 80 + 400 * k * k          # quadratic ramp: lazy start, urgent end
        phase += 2 * math.pi * freq / RATE
        env = min(k * 4, 1.0) * (1.0 - max(0.0, k - 0.9) * 10)  # fade-in + last-10% fade-out
        out.append(math.sin(phase) * env * 0.55)
    return out


def afterburner_loop() -> list[float]:
    """Exactly 1s held-loop afterburner roar. Same periodicity trick as
    engine_hum: every component completes an integer number of cycles
    over the loop length, so the seam is click-free by construction.

    Recipe: dominant 110Hz fundamental (the afterburner turbine)
    + 55Hz subharmonic (deep rumble) at 0.4 weight
    + 220Hz 2nd harmonic at 0.15 weight (turbine whine) for 'bite'
    + 0.7Hz LFO tremolo (1 cycle/sec, sway) for life
    + 2.3Hz harmonic for a subtle Doppler-style flutter, deeply attenuated
    Master gain 0.42 so the held loop sits solidly under the engine bed.
    """
    n = RATE
    out = []
    for t in range(n):
        fundamental = math.sin(2 * math.pi * 110 * t / RATE)
        sub         = math.sin(2 * math.pi * 55 * t / RATE)
        harmonic    = math.sin(2 * math.pi * 220 * t / RATE)
        tremolo     = 1.0 + 0.07 * math.sin(2 * math.pi * 0.7 * t / RATE)
        flutter     = math.sin(2 * math.pi * 2.3 * t / RATE) * 0.06
        s = (0.85 * fundamental
             + 0.40 * sub
             + 0.15 * harmonic
             + flutter) * tremolo * 0.42
        out.append(s)
    return out


def ui_click() -> list[float]:
    """5ms 2kHz tick. Barely a sound; exactly a click."""
    n = int(RATE * 0.005)
    return [
        math.sin(2 * math.pi * 2000 * t / RATE) * (1 - t / n) * 0.5 for t in range(n)
    ]


def missile_fire() -> list[float]:
    """250ms missile launch: a low whoosh (noise swelling then decaying)
    under a short descending tone. Reads as 'fwoomp' — distinct from the
    laser 'pew' so the player can tell a missile left the rail by ear."""
    rng = random.Random(99)
    n = int(RATE * 0.250)
    out, phase = [], 0.0
    for t in range(n):
        k = t / n
        # Noise body: swell in over the first 30%, decay after.
        env = (min(k / 0.3, 1.0)) * math.exp(-max(0.0, k - 0.3) * 4)
        noise = (rng.random() * 2 - 1) * env
        # Descending tone 420 -> 160 Hz for the 'launch' pitch drop.
        freq = 420 * (1 - k) + 160 * k
        phase += 2 * math.pi * freq / RATE
        tone = math.sin(phase) * env * 0.5
        out.append((0.6 * noise + tone) * 0.7)
    return lowpass(out, 0.5)


def lock_seeking() -> list[float]:
    """Short 90ms 1200Hz square-ish beep — the 'searching' chirp the HUD
    re-triggers on a cadence while acquiring an IR lock. Hard envelope so a
    train of them reads as discrete 'bip... bip... bip'."""
    n = int(RATE * 0.090)
    out = []
    for t in range(n):
        s = 1.0 if math.sin(2 * math.pi * 1200 * t / RATE) >= 0 else -1.0
        env = min(t / (RATE * 0.005), 1.0) * min((n - t) / (RATE * 0.010), 1.0)
        out.append(s * env * 0.4)
    return out


def lock_acquired() -> list[float]:
    """350ms solid two-tone (1600Hz over 800Hz) — the 'LOCKED' confirmation.
    Steady (no decay) so it reads as a held tone vs the seeking blips."""
    n = int(RATE * 0.350)
    out = []
    for t in range(n):
        a = math.sin(2 * math.pi * 1600 * t / RATE)
        b = math.sin(2 * math.pi * 800 * t / RATE)
        env = min(t / (RATE * 0.008), 1.0) * min((n - t) / (RATE * 0.020), 1.0)
        out.append((0.55 * a + 0.45 * b) * env * 0.45)
    return out


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    write_wav("blip.wav", blip())
    write_wav("burst.wav", burst())
    write_wav("hum.wav", hum())
    write_wav("laser_fire.wav", laser_fire())
    write_wav("impact_shield.wav", impact_shield())
    write_wav("impact_armor.wav", impact_armor())
    write_wav("explosion_small.wav", explosion(seed=21, dur_s=0.5, alpha=0.30, gain=0.85))
    write_wav("explosion_big.wav",   explosion(seed=22, dur_s=1.2, alpha=0.12, gain=0.95))
    write_wav("engine_hum.wav", engine_hum())
    write_wav("cruise_windup.wav", cruise_windup())
    write_wav("afterburner_loop.wav", afterburner_loop())
    write_wav("ui_click.wav", ui_click())
    write_wav("missile_fire.wav", missile_fire())
    write_wav("lock_seeking.wav", lock_seeking())
    write_wav("lock_acquired.wav", lock_acquired())


if __name__ == "__main__":
    main()
