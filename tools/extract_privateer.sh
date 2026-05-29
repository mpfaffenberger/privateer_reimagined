#!/usr/bin/env bash
# extract_privateer.sh — mount the user's GOG copy of Privateer and extract
# PRIV.TRE + RF.TRE into ./gog_extracted/extracted/{priv,rf}/.
#
# Idempotent: every step skips if its outputs already exist. Safe to re-run.
#
# Usage:
#   tools/extract_privateer.sh               # mount + extract, leave mounted
#   tools/extract_privateer.sh --unmount     # also detach the .iso when done
#   GOG_INSTALL=/path/to/foo.app  tools/extract_privateer.sh
#
# Outputs:
#   gog_extracted/mount/             — read-only mount of GAME.GOG (macOS)
#   gog_extracted/extracted/priv/    — 832 files from PRIV.TRE
#   gog_extracted/extracted/rf/      — 857 files from RF.TRE
#   third_party/wctools/wctre/wctre  — built tool (cached)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GOG_INSTALL="${GOG_INSTALL:-/Applications/Wing Commander ® Privateer ™.app}"
GAME_GOG="$GOG_INSTALL/Contents/Resources/game/GAME.GOG"
MOUNT_DIR="$REPO/gog_extracted/mount"
OUT_DIR="$REPO/gog_extracted/extracted"
WCTRE_DIR="$REPO/third_party/wctools/wctre"
WCTRE_BIN="$WCTRE_DIR/wctre"

log() { printf '[extract] %s\n' "$*"; }
die() { printf '[extract] ERROR: %s\n' "$*" >&2; exit 1; }

[[ -f "$GAME_GOG" ]] || die "GAME.GOG not found at: $GAME_GOG
  Set GOG_INSTALL to the .app bundle path, or install Privateer from GOG."

# ─── 1. Build wctre if needed ───────────────────────────────────────────────
if [[ ! -x "$WCTRE_BIN" ]]; then
    log "building wctre (one-time)"
    (cd "$WCTRE_DIR" && make >/dev/null)
fi
[[ -x "$WCTRE_BIN" ]] || die "wctre still not built — check $WCTRE_DIR/Makefile"

# ─── 2. Mount GAME.GOG if not mounted ───────────────────────────────────────
mkdir -p "$MOUNT_DIR" "$OUT_DIR/priv" "$OUT_DIR/rf"
if mount | grep -F -q " on $MOUNT_DIR "; then
    log "already mounted at $MOUNT_DIR"
else
    log "mounting GAME.GOG → $MOUNT_DIR"
    # hdiutil refuses the .GOG extension — symlink it as .iso first.
    STAGE="$(mktemp -d)"
    ln -sf "$GAME_GOG" "$STAGE/GAME.iso"
    hdiutil attach -nobrowse -readonly -mountpoint "$MOUNT_DIR" "$STAGE/GAME.iso" >/dev/null
fi

# ─── 3. Extract each TRE if its output dir is empty ─────────────────────────
extract_tre() {
    local src="$1" dst="$2" label="$3"
    if [[ -n "$(ls -A "$dst" 2>/dev/null)" ]]; then
        local n; n=$(find "$dst" -type f | wc -l | tr -d ' ')
        log "  $label already extracted ($n files)"
        return 0
    fi
    log "  extracting $label → $(realpath --relative-to="$REPO" "$dst" 2>/dev/null || echo "$dst")"
    (cd "$dst" && yes y | "$WCTRE_BIN" "$src" >/dev/null) \
        || die "$label extraction failed"
    local n; n=$(find "$dst" -type f | wc -l | tr -d ' ')
    log "    done ($n files)"
}

extract_tre "$MOUNT_DIR/PRIV.TRE" "$OUT_DIR/priv" "PRIV.TRE"
extract_tre "$MOUNT_DIR/RF.TRE"   "$OUT_DIR/rf"   "RF.TRE"

# ─── 4. Optional unmount ────────────────────────────────────────────────────
if [[ "${1:-}" = "--unmount" ]] || [[ "${DETACH:-0}" = "1" ]]; then
    log "unmounting $MOUNT_DIR"
    hdiutil detach "$MOUNT_DIR" >/dev/null || true
fi

log "OK — ready at gog_extracted/extracted/"
log "next: python3 tools/import_privateer_db/cargo.py"
