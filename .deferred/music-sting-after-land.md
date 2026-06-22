# Deferred: landing sting doesn't stop when the Landed bed starts

Mike noticed: when afterburning into a base and the auto-land kicks in,
the "now entering an automatic landing zone" landing-zone sting (combat_09)
keeps playing over the top of the Landed-mode base music bed that takes over
once the player docks.

Diagnosis direction: `music::landing_approach()` fires the sting at the
auto-land threshold; once we Landed, `update()` switches the LOOP target to
the base bed, but the sting is a one-shot SFX sample that runs to its own
end (no override cuts it off). Likely fix: pass a handle / voice id back
to the caller so the base-music switch in `Landed` mode can stop the sting
sample, OR route the sting through the same channel as the jump sting so
mode transitions get a "fade stings" beat.

Where to wire: `src/music.cpp` ~line 350 (`case GameMode::Landed: ...`).
