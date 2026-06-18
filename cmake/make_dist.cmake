# -----------------------------------------------------------------------------
# make_dist.cmake — produce a portable, relocatable game folder.
#
# Run via the `dist` custom target (see CMakeLists.txt). Copies the built
# executable plus a PRUNED, real-file copy of assets/ into <build>/dist/.
# The pruning drops build-time-only trees (sprite-gen intermediates, Blender
# renders, imported OBJ meshes) that the runtime never loads — that's the
# difference between a ~1.9 GB source tree and a lean shippable folder.
#
# Required -D args:
#   NP_EXE  — full path to the built executable
#   NP_SRC  — repo root (contains assets/)
#   NP_OUT  — output dir (e.g. <build>/dist)
# -----------------------------------------------------------------------------

if(NOT NP_EXE OR NOT NP_SRC OR NOT NP_OUT)
    message(FATAL_ERROR "make_dist.cmake requires -DNP_EXE, -DNP_SRC, -DNP_OUT")
endif()

message(STATUS "[dist] cleaning ${NP_OUT}")
file(REMOVE_RECURSE "${NP_OUT}")
file(MAKE_DIRECTORY "${NP_OUT}")

# ---- executable -------------------------------------------------------------
message(STATUS "[dist] copying executable: ${NP_EXE}")
file(COPY "${NP_EXE}" DESTINATION "${NP_OUT}")

# ---- assets (pruned) --------------------------------------------------------
# REGEX ... EXCLUDE matches anywhere in each entry's full path. We keep the
# runtime-needed trees (sprites_3d/ atlas frames, manifests, sfx, music,
# skybox, bolts, data, systems, sprites, bases, cockpits) and drop:
#   - per-ship sprites/   (raw/clean/on_black generation intermediates, ~100 MB/ship)
#   - per-ship renders/   (Blender renders)
#   - per-ship *_sprites/ wcnews/ingame/regen capture dirs
#   - meshes/             (imported OBJs — only sprite-gen used them, no system loads them)
#   - *.raw.png / *_on_black.png stragglers, .DS_Store, prompts/
# The /ships/<class>/sprites/ regex deliberately does NOT match the top-level
# assets/sprites/ (no /ships/ prefix) nor sprites_3d/ (the "/" after "sprites"
# fails to match "_3d/").
message(STATUS "[dist] copying pruned assets/ -> ${NP_OUT}/assets")
file(COPY "${NP_SRC}/assets" DESTINATION "${NP_OUT}"
    REGEX "/ships/[^/]+/sprites/"        EXCLUDE
    REGEX "/ships/[^/]+/renders/"        EXCLUDE
    REGEX "/ships/[^/]+/regen_trials/"   EXCLUDE
    REGEX "/ships/[^/]+/wcnews_sprites/" EXCLUDE
    REGEX "/ships/[^/]+/sprites_wcnews/" EXCLUDE
    REGEX "/ships/[^/]+/sprites_ingame/" EXCLUDE
    REGEX "/meshes/"                     EXCLUDE
    REGEX "/shipgen/"                    EXCLUDE
    REGEX "\\.raw\\.png$"                EXCLUDE
    REGEX "_on_black\\.png$"             EXCLUDE
    REGEX "\\.DS_Store$"                 EXCLUDE
    REGEX "/\\.git"                      EXCLUDE
)

# ---- size report ------------------------------------------------------------
file(GLOB_RECURSE _dist_files "${NP_OUT}/*")
list(LENGTH _dist_files _n)
message(STATUS "[dist] done: ${_n} files in ${NP_OUT}")
message(STATUS "[dist] portable folder ready — zip it and run anywhere (no VC++ redist needed; CRT is statically linked).")
