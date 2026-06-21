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


def seam_blend(buf: list[float], fade: int) -> list[float]:
    """Make a click-free LOOP out of a buffer of length n+fade by
    crossfading the trailing `fade` samples over the head. Returns n
    samples. Pure periodic content (tones whose period divides n) is
    unaffected (the blended pairs are identical); NON-periodic content
    (filtered noise / air rush) is smoothed across the wrap so the loop
    seam has no step. This is the right tool for noise beds — unlike a
    tonal bed, you CAN'T get seamlessness from integer-cycle periodicity
    when there's noise in the mix."""
    n = len(buf) - fade
    res = list(buf[:n])
    for i in range(fade):
        w = i / fade
        res[i] = buf[i] * w + buf[n + i] * (1.0 - w)
    return res


def air_rush(n_plus_fade: int, seed: int, alpha: float) -> list[float]:
    """Band-limited noise = the 'air' of a jet. One-pole lowpass of white
    noise (alpha sets brightness: higher = hissier, lower = breathier).
    Returned UNwrapped (length == n_plus_fade) so the caller can mix it
    with tones and seam_blend the whole thing once."""
    rng = random.Random(seed)
    noise = [rng.random() * 2 - 1 for _ in range(n_plus_fade)]
    return lowpass(noise, alpha)


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
    """JET IDLE bed (np-3dp.9). Mike wanted the engine to read like a jet,
    not a smooth generator hum. A real turbofan is THREE things layered:
      1. a low spool RUMBLE (the fan),
      2. a mid TURBINE WHINE (the compressor spinning), and
      3. broadband AIR RUSH (intake/exhaust hiss).
    The old bed was just (1) — pure 60/120Hz tones — which is why it read
    as a hum. This adds (2) a gentle 760Hz whine and (3) band-limited
    noise for the air, which is what sells 'jet'.

    Loop seam: the TONES complete integer cycles over the exactly-1s loop
    (50/100/760/1 all divide RATE), so they're periodic-seamless on their
    own. The NOISE can't be, so we generate 1s + a 50ms tail and
    seam_blend() crossfades the tail over the head — the tones are
    unaffected (blended pairs identical) and the air rush wraps without a
    click."""
    n = RATE
    fade = int(RATE * 0.05)   # 50ms crossfade tail for the noise
    air = air_rush(n + fade, seed=4242, alpha=0.30)   # breathy intake hiss
    buf = []
    for t in range(n + fade):
        rumble = (0.70 * math.sin(2 * math.pi * 50  * t / RATE)
                + 0.22 * math.sin(2 * math.pi * 100 * t / RATE))
        whine  = 0.07 * math.sin(2 * math.pi * 760 * t / RATE)
        breath = 1.0 + 0.05 * math.sin(2 * math.pi * 1 * t / RATE)
        s = (rumble + whine) * breath + air[t] * 0.45
        buf.append(s * 0.34)
    return seam_blend(buf, fade)


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
    """Exactly-1s held-loop afterburner ROAR (np-3dp.9). Distinct from the
    idle bed: where the idle is a breathy whine, the afterburner is a
    LOUD, BRIGHT air roar — lots of band-limited noise (the reheat plume)
    on top of a hard turbine stack. This is the layer the menu's
    'jet engine' read comes from, so it leans on the air rush.

    Layers:
      * turbine stack: 110Hz fundamental + 55Hz sub + 220Hz 2nd harmonic
        (tones, integer-cycle => periodic-seamless),
      * REHEAT AIR: bright band-limited noise (alpha 0.55), the dominant
        voice — this is what makes it a roar, not a tone,
      * 0.7Hz tremolo sway + 2.3Hz flutter for life.
    Loop seam: tones are periodic; the noise is made seamless via a 50ms
    seam_blend() crossfade (same as the idle bed).
    """
    n = RATE
    fade = int(RATE * 0.05)
    air = air_rush(n + fade, seed=2718, alpha=0.55)   # bright reheat plume
    buf = []
    for t in range(n + fade):
        fundamental = math.sin(2 * math.pi * 110 * t / RATE)
        sub         = math.sin(2 * math.pi * 55  * t / RATE)
        harmonic    = math.sin(2 * math.pi * 220 * t / RATE)
        tremolo     = 1.0 + 0.07 * math.sin(2 * math.pi * 0.7 * t / RATE)
        flutter     = math.sin(2 * math.pi * 2.3 * t / RATE) * 0.05
        turbine = (0.55 * fundamental
                 + 0.28 * sub
                 + 0.14 * harmonic
                 + flutter)
        s = (turbine * 0.55 + air[t] * 0.85) * tremolo * 0.46
        buf.append(s)
    return seam_blend(buf, fade)


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
