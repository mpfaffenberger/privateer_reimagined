#!/usr/bin/env python3
# =============================================================================
# xmidi2smf.py -- convert an Origin/Miles **XMIDI** sequence (as found inside
#                 Privateer's DATA/SOUND/*.GEN and *.ADL containers) into a
#                 standard **SMF type-0** MIDI file.
#
# ASSET-TIME tool only -- NOT a runtime dependency of the game. It exists so a
# general-purpose synth (FluidSynth + a GM SoundFont, or Munt for MT-32) can
# render Privateer's *richer* General-MIDI / Roland soundtrack (the `.GEN`
# files) instead of the bleepy AdLib/OPL2 FM path (the `.ADL` files).
#
# Why this is needed: FluidSynth / Munt speak SMF, not XMIDI. XMIDI differs
# from SMF in three ways we must undo:
#   1. Delta time is a *sum of consecutive bytes < 0x80* (NOT a single VLQ).
#   2. Note-On carries its own **duration** (a trailing VLQ) instead of a
#      matching Note-Off -- we schedule the Note-Off ourselves.
#   3. Meta events use a *single-byte* length field.
# Everything else (status bytes, controllers, tempo FF51, program change) is
# already standard MIDI and passes straight through.
#
# Timebase (THE TEMPO FIX -- np-vcr): XMIDI/Miles AIL runs on a **fixed clock of
# 120 ticks per SECOND** and IGNORES the embedded FF51 tempo meta. The authentic
# OPL playback (libADLMIDI reading the .ADL directly, the game's real AdLib
# output) proves it: for ALL 27 sub-songs the reference duration equals
# span_ticks / 120 + the synth's ~1s release tail -- including the multi-tempo
# cues (OPENING has 25 FF51 events, VICTORY 6) whose tempo changes have ZERO
# effect on the wall-clock length. So the FF51 events are vestigial under AIL.
#
# The OLD converter emitted PPQN=120 and PASSED FF51 THROUGH, so FluidSynth
# OBEYED the tempo: a 60 BPM cue landed at exactly 120 ticks/s (right -- which
# is why the 60 BPM agricultural/base tunes happened to be fine), but a 140 BPM
# cue ran at 280 ticks/s = 2.33x too fast, the 142 BPM stingers faster still --
# i.e. every cue was off by BPM/60, which is why "most" tracks galloped while a
# couple were fine. (Single-track calibration on the 60 BPM BASETUNE sub-song
# 04 masked the bug.)
#
# THE FIX: reproduce the fixed XMIDI clock. Emit at **120 ticks/second for every
# track** by writing the ScummVM-canonical XMIDI division PPQN=60 with a single
# injected 120 BPM tempo (500000 us/qtr -> 1e6/500000 * 60 = 120 ticks/s) at
# tick 0, and DROPPING the embedded FF51 events (AIL never honoured them). Now
# every .GEN render matches its .ADL reference duration (see
# tools/validate_music_tempo.py).
#
# Authority for the XMIDI transform + the fixed-timebase tempo handling:
# Pentagram / Exult `xmidi.cc`, ScummVM `midiparser_xmi.cpp` ("XMIDI... 120 Hz,
# the same as a tempo of 500000 with a PPQN of 60"). Container layout authority:
# the np-xa2 XDIR/CAT parser in tools/render_music.py (REUSED here).
# =============================================================================
import struct
import sys

# XMIDI's fixed clock is 120 ticks/SECOND. Encoded as the ScummVM-canonical SMF
# pair: PPQN=60 + a forced 120 BPM tempo => 1e6/500000 * 60 = 120 ticks/s. The
# raw XMIDI tick stamps are reused unchanged; only the division + the single
# injected tempo set the wall-clock rate (the source FF51 events are dropped).
XMIDI_PPQN = 60
XMIDI_FIXED_TEMPO_US = 500000        # 120 BPM at PPQN 60 == 120 ticks/second


def read_vlq(d, i):
    """Standard MIDI variable-length quantity (used by XMIDI Note-On duration
    and F0 sysex length -- this is the *normal* VLQ, distinct from the XMIDI
    delta-time encoding handled inline below)."""
    value = 0
    while True:
        b = d[i]
        i += 1
        value = (value << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return value, i


def write_vlq(value):
    """Encode an int as a standard MIDI VLQ (for the SMF delta times)."""
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def find_evnt(seg):
    """Given one XMIDI container slice (FORM..XDIR CAT..XMID FORM..XMID ...),
    return the raw bytes of its EVNT chunk -- the actual event stream. We scan
    for the `EVNT` magic and read its big-endian length, exactly as the IFF
    chunking dictates."""
    p = seg.find(b"EVNT")
    if p < 0:
        raise ValueError("no EVNT chunk -- not an XMIDI sequence?")
    length = struct.unpack_from(">I", seg, p + 4)[0]
    start = p + 8
    return seg[start:start + length]


def xmidi_to_events(evnt):
    """Parse the XMIDI EVNT stream into a flat, tick-stamped SMF event list.

    Returns a list of (abs_tick, order, payload_bytes) where payload_bytes is a
    ready-to-write SMF event (status + data). Note-Offs synthesised from
    Note-On durations are interleaved at their scheduled ticks."""
    events = []
    i = 0
    tick = 0
    order = 0
    n = len(evnt)
    while i < n:
        # --- XMIDI delta time: sum the run of bytes whose top bit is clear.
        delta = 0
        while i < n and evnt[i] < 0x80:
            delta += evnt[i]
            i += 1
        tick += delta
        if i >= n:
            break

        status = evnt[i]
        i += 1
        high = status & 0xF0

        if status == 0xFF:                          # meta event
            mtype = evnt[i]
            length = evnt[i + 1]
            i += 2
            data = evnt[i:i + length]
            i += length
            if mtype == 0x2F:                       # end-of-track: stop; we
                break                               # write our own terminator
            if mtype == 0x51:                       # tempo: DROP it. XMIDI/AIL
                continue                            # ignores FF51 (fixed clock);
                                                    # we inject one fixed tempo
                                                    # in events_to_smf instead.
            payload = bytes([0xFF, mtype, length]) + data
            events.append((tick, order, payload))
            order += 1

        elif status in (0xF0, 0xF7):                # sysex (VLQ length)
            length, i = read_vlq(evnt, i)
            data = evnt[i:i + length]
            i += length
            events.append((tick, order, bytes([status]) + write_vlq(length) + data))
            order += 1

        elif high == 0x90:                          # Note-On (+ XMIDI duration)
            note = evnt[i]
            vel = evnt[i + 1]
            i += 2
            dur, i = read_vlq(evnt, i)
            events.append((tick, order, bytes([status, note, vel])))
            order += 1
            # Schedule the matching Note-Off (velocity 0) `dur` ticks later.
            off_status = 0x80 | (status & 0x0F)
            events.append((tick + dur, order, bytes([off_status, note, 0x40])))
            order += 1

        elif high in (0x80, 0xA0, 0xB0, 0xE0):      # 2-data-byte channel msgs
            events.append((tick, order, bytes([status, evnt[i], evnt[i + 1]])))
            i += 2
            order += 1

        elif high in (0xC0, 0xD0):                  # 1-data-byte channel msgs
            events.append((tick, order, bytes([status, evnt[i]])))
            i += 1
            order += 1

        else:                                       # desync -- bail cleanly
            break

    return events


def events_to_smf(events, ppqn=XMIDI_PPQN):
    """Serialise the tick-stamped event list to an SMF type-0 byte string."""
    # Stable sort: scheduled Note-Offs land at the right tick; ties keep the
    # original emission order so Note-On precedes its later Note-Off.
    events.sort(key=lambda e: (e[0], e[1]))

    track = bytearray()
    # Inject the single fixed XMIDI tempo at tick 0 (120 ticks/second). The
    # source FF51 events were dropped in xmidi_to_events -- AIL ignores them and
    # the authentic OPL reference plays every cue on this fixed clock.
    track += b"\x00\xFF\x51\x03" + struct.pack(">I", XMIDI_FIXED_TEMPO_US)[1:]
    prev = 0
    for tick, _order, payload in events:
        track += write_vlq(tick - prev)
        track += payload
        prev = tick
    track += b"\x00\xFF\x2F\x00"                     # end of track

    out = bytearray()
    out += b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppqn)
    out += b"MTrk" + struct.pack(">I", len(track)) + track
    return bytes(out)


def convert_container(seg, ppqn=XMIDI_PPQN):
    """One XMIDI container slice -> SMF bytes."""
    return events_to_smf(xmidi_to_events(find_evnt(seg)), ppqn)


def main():
    if len(sys.argv) not in (3, 4):
        print("usage: xmidi2smf.py <in.xmi/container> <out.mid> [ppqn]",
              file=sys.stderr)
        return 1
    seg = open(sys.argv[1], "rb").read()
    ppqn = int(sys.argv[3]) if len(sys.argv) == 4 else XMIDI_PPQN
    open(sys.argv[2], "wb").write(convert_container(seg, ppqn))
    print("xmidi2smf: %s -> %s (ppqn=%d)" % (sys.argv[1], sys.argv[2], ppqn))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
