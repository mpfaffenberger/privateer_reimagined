#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# remap_sfx_originals.sh — rebuild the LOCAL-ONLY assets/sfx/original/*.wav set
# from the user's F7 ground-truth labels (docs/sound_labels.json).
#
# Source clips are the extracted SOUNDFX.PAK files in gog_extracted/sfx_wav/
# (sfx_NN.wav, already 44100/mono/s16). We re-encode each chosen clip to the
# canonical engine format (44100 Hz, mono, s16) under assets/sfx/original/ with
# a CLEAR gameplay name. Both source and destination trees are gitignored —
# this script + docs/sfx_gun_mapping.md are the committable record of WHAT maps
# to WHAT. Re-run any time the labels change.
#
# Per-gun "loud = player (2D), quiet twin = NPC": the labels show sfx_09..17 are
# the quieter versions of sfx_00..08. We keep the loud clip for the player's own
# guns and the quiet twin for NPC guns (more authentic than reusing the loud).
#
# NOTE: sfx_07 -> Tachyon is a BEST-INFERENCE (user labeled it "particle
# cannon", but sfx_04 is already particle and NO clip is labeled tachyon;
# sfx_07 sits at Tachyon's slot in gun order). AWAITING USER CONFIRMATION.
# -----------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/gog_extracted/sfx_wav"
DST="$ROOT/assets/sfx/original"
mkdir -p "$DST"

# enc <src_index> <dest_name>  — re-encode sfx_NN.wav -> <dest_name>.wav
enc() {
  local idx="$1" name="$2"
  local in="$SRC/sfx_$(printf '%02d' "$idx").wav"
  local out="$DST/$name.wav"
  if [[ ! -f "$in" ]]; then
    echo "[remap] MISSING source $in (skipping $name)" >&2
    return 0
  fi
  ffmpeg -hide_banner -loglevel error -y -i "$in" -ar 44100 -ac 1 -c:a pcm_s16le "$out"
  echo "[remap] $(basename "$in") -> $name.wav"
}

echo "[remap] per-gun firing (loud=player / quiet=NPC)"
#                          loud(player)            quiet(NPC)
enc  5 gun_laser;              enc 14 gun_laser_npc
enc  3 gun_mass_driver;        enc 12 gun_mass_driver_npc
enc  1 gun_meson_blaster;      enc 10 gun_meson_blaster_npc
enc  2 gun_neutron_gun;        enc 11 gun_neutron_gun_npc
enc  4 gun_particle_cannon;    enc 13 gun_particle_cannon_npc
enc  7 gun_tachyon_cannon;     enc 16 gun_tachyon_cannon_npc   # sfx_07 = BEST-INFERENCE
enc  0 gun_ionic_pulse_cannon; enc  9 gun_ionic_pulse_cannon_npc
enc  6 gun_plasma_gun;         enc 15 gun_plasma_gun_npc
enc  8 gun_steltek_gun;        enc 17 gun_steltek_gun_npc

echo "[remap] combat events (player/NPC damage variants)"
enc 23 impact_armor;   enc 24 impact_armor_npc     # player / NPC armor hit
enc 25 impact_shield;  enc 26 impact_shield_npc    # player / NPC shield hit
enc 27 explosion_big                               # ship destroyed
enc 28 explosion_small                             # missile explosion (smaller boom)
enc 18 missile_fire                                # missile launch

echo "[remap] flight / UI"
enc 22 cruise_windup   # afterburner / cruise spool
enc 41 jump            # jump sting (plays first)
enc 42 jump2           # follows jump (sequence)
enc 34 ui_click        # nav cycling
enc 31 lock_acquired   # target locked

# DELIBERATELY NOT regenerated (kept procedural — no canonical original):
#   engine_hum  : SOUNDFX.PAK has no idle-engine loop (sfx_22 is afterburner).
#   lock_seeking: no clean "seeking beep" clip is labeled.
# Remove any stale originals so the committed procedural placeholders win.
rm -f "$DST/engine_hum.wav" "$DST/lock_seeking.wav"
echo "[remap] removed stale engine_hum/lock_seeking originals (procedural fallback wins)"

echo "[remap] done -> $DST"
