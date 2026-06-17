# Combat AI: Privateer Gemini Gold / Vega Strike vs vanilla Privateer (MNVR) vs new_privateer

Derived comparative analysis. No game bytes; all Vega Strike facts are cited to
the cloned engine source at `/tmp/vegastrike_ai` (read-only study), all vanilla
facts to `docs/ai_model.md` (the decoded MNVR model), all new_privateer facts to
`src/ship_ai.cpp` / `src/ship_ai.h`.

Privateer Gemini Gold (PGG) is a total-conversion mod that runs on the Vega
Strike engine, so "PGG combat AI" == "Vega Strike's `AggressiveAI`" driven by
PGG's data tree. The engine source ships the brain (`engine/src/cmd/ai/`); the
*tuning* (`*.agg.xml`, `VegaPersonalities.csv`) lives in the PGG/WCU **data**
tree, which is NOT in this engine clone (see §4).

---

## 1. How PGG / Vega Strike structures combat AI

### 1.1 Class stack

```
Order                      (order.h)            base: queued sub-orders, bit-coded parallelism
  CommunicatingAI                                comms / contraband / target-switching
    FireAt                 (fire.cpp/.h)         target selection + firing decision
      AggressiveAI         (aggressive.cpp/.h)   the combat brain (condition->script)
```

`AggressiveAI` inherits `FireAt`'s firing and target logic and adds the
data-driven maneuver selection. Orders are bit-coded `MOVEMENT|FACING|WEAPON|
CLOAKING` (`order.h:114`) so a FACING order and a WEAPON order can run in
parallel, but two MOVEMENT orders cannot (one engine can't go two ways).

### 1.2 The data-driven condition -> script model (the core)

The brain is **not** a hardcoded state machine. It loads an XML "logic" file
into an `ElemAttrMap` (`event_xml.h:75`) and evaluates it every tick.

**The condition vocabulary** is an `EnumMap` (`aggressive.cpp:65-92`):
`Distance, MeterDistance, Threat, FShield/LShield/RShield/BShield, Hull,
Facing, Movement, FShield_Heal_Rate` (+ B/L/R and Armor + Hull heal rates),
`Target_Faces_You, Target_In_Front_Of_You, Rand, Target_Going_Your_Direction`.

**A logic item** (`event_xml.h:43`, `AIEvresult`) is:
`{ type (which condition, negative = "not"), min, max, timetofinish,
timetointerrupt, priority, script (a hard-coded maneuver name) }`.

**The XML format** (parsed in `event_xml.cpp:103-167`): the root element sets
`time` (the loop `maxtime`) and `obedience`; each nested element is a condition
named from the EnumMap, with attributes `min`, `max`, `not`, `Script`, `time`
(timetofinish), `timetointerrupt`, `priority`. Nesting expresses logical AND
(the parser duplicates the parent list so a child appends an extra AND-ed
clause, `event_xml.cpp:143-160`). Conceptually:

```xml
<AggressiveAI time="10" obedience="0.5">
  <Distance min="1000" max="20000" Script="AfterburnTurnTowards"
            time="4" priority="4"/>
  <Distance min="0" max="1000">
     <Hull min="0.0" max="0.3" Script="AfterburnVeerAndTurnAway" time="3"
           priority="6" timetointerrupt="1"/>
  </Distance>
  <Rand min="0" max="0.1" Script="BarrelRoll" time="2" priority="5"/>
</AggressiveAI>
```

**Per-tick evaluation** (`AggressiveAI::ProcessLogicItem` `aggressive.cpp:309`):
a big `switch(abs(item.type))` computes a scalar `value` for the requested
metric — e.g. `DISTANCE` -> `distance`, `METERDISTANCE` -> a retro-burn-adjusted
closing distance (`aggressive.cpp:314-330`), `THREAT` -> `computer.threatlevel`,
`FSHIELD` -> `shield.Percent(front)`, `HULL` -> `hull.Percent()`,
`TARGET_FACES_YOU` / `TARGET_IN_FRONT_OF_YOU` -> orientation dot products,
`FACING`/`MOVEMENT` -> "is there no FACING/MOVEMENT order queued?", `RANDOMIZ`
-> a fresh random float. It then returns `item.Eval(value)`.

`Eval` (`event_xml.h:60`) is just an interval test: true when
`min <= value < max` (or, for a negated `type`, true when value is *outside*
`[min,max)`). So a condition is a **named metric in a numeric window**.

**Selection + dispatch** (`AggressiveAI::ProcessLogic` `aggressive.cpp:494`):
walk the list of logic items; for each, AND all its conditions; the first item
whose conditions all pass "wins". Its script(s) are enqueued via
`ExecuteLogicItem` (`aggressive.cpp:299`) as
`new ExecuteFor(new AIScript(script), timetofinish)` — i.e. run that maneuver
for `timetofinish` seconds. The chosen item sets `logiccurtime` /
`interruptcurtime` from `timetofinish` / `timetointerrupt`.

**Logic vs interrupt** (`AggressiveAI::Execute` `aggressive.cpp:1592`,
specifically 1660-1700): two passes.
- **Interrupt pass** — `ProcessLogic(*logic, true)` runs when
  `interruptcurtime <= 0` (`aggressive.cpp:1660`). An interrupt only fires if
  its `priority > currentpriority` (`aggressive.cpp:516`), and when it does it
  *erases* the current FACING and MOVEMENT orders (`aggressive.cpp:518-519`) and
  preempts. This is the "drop what you're doing and evade the incoming missile"
  channel.
- **Logic pass** — `ProcessLogic(*logic, false)` runs when the current maneuver
  finished (`logiccurtime <= 0`, `aggressive.cpp:1691`) or no FACING/MOVEMENT
  order is queued (`aggressive.cpp:1670-1676`). This is the normal "pick my next
  maneuver" channel; non-interrupt selections ignore priority.

So distance/hull/facing thresholds **gate** maneuvers: a maneuver is reachable
only while its metric sits inside the authored window, and is held for its
`timetofinish` before reselection.

### 1.3 The script loading path + personality

`SignalChosenTarget` (`aggressive.cpp:292`) reloads the logic on target change:
`logic = getProperScript(parent, parent->Target(), false, personalityseed)`.

`getProperScript` (`aggressive.cpp:208`) builds the filename from the
**attacker role x target role** (`ROLES::getRoleEvents(...)`), the faction, and
the unit type, then calls `getLogicOrInterrupt` (`aggressive.cpp:171`). That
function picks a *personality append* from `VegaPersonalities.csv`
(`getAITypes`, `aggressive.cpp:135`) keyed by `faction%unittype` or `faction`,
and selects one of several space-separated options with
`select_from_space_list(value, personalityseed)` (`aggressive.cpp:138`,
`personalityseed % count`). The final file is `"<roleevents>.<append>.xml"`,
defaulting to `*.agg.xml`. `LoadAI` (`event_xml.cpp:169`) parses it with a
multi-level fallback to `default.agg.xml`.

`personalityseed` is a per-pilot random int seeded at construction
(`aggressive.cpp:230-234`, `GenRandInt31()`). It does double duty: it picks
*which* script file a pilot uses, and it is reused inside several maneuvers
(`LoopAround`, `FacePerpendicular`, etc.) as the RNG seed so each pilot's jink
geometry is stable-but-distinct.

### 1.4 Firing logic (`fire.cpp`)

`FireAt::Execute` (`fire.cpp:774`) each tick: if the target is visible and not a
jump point, `shouldfire |= ShouldFire(targ, missilelock)` (`fire.cpp:815`), then
`FireWeapons(shouldfire, missilelock)` (`fire.cpp:828`).

`ShouldFire` (`fire.cpp:643`):
- `parent->getAverageGunSpeed(gunspeed, gunrange, missilerange)` (`fire.cpp:655`).
- `angle = parent->cosAngleTo(targ, dist, itts ? gunspeed : FLT_MAX, gunrange,
  false)` (`fire.cpp:656`) — **ITTS lead**: if the ship has an ITTS computer it
  aims at the lead point computed from `gunspeed`; otherwise it aims straight.
- `targ->Threaten(parent, angle/dist)` (`fire.cpp:658`) — feeds the target's
  `threatlevel` (which is itself a condition metric, closing the loop).
- **Firing arc scales with aggressivity** (`fire.cpp:663-671`):
  `fangle = (fireangle_minagg + fireangle_maxagg*agg)/(1+agg)`, where
  `minagg ~= cos(10deg)` and `maxagg ~= cos(18deg)` from config. Higher `agg`
  widens the cone the AI will fire in.
- Fire when `dist < in_weapon_range AND (angle > fangle OR (tracking guns AND
  angle > tracking-threshold) OR (missilelock AND angle > 0))`, and target is
  not a jump point (`fire.cpp:670-672`).
- A "too many attackers" throttle (`fire.cpp:677-697`) randomly suppresses fire
  on the player when too many AIs gang up.

`FireWeapons` (`fire.cpp:728`): a **reaction-time delay** gate
(`delay < pilot->getReactionTime()`, `fire.cpp:736`), a per-atom **missile
probability** roll (`missileprobability`, `fire.cpp:730`), and a **missile
gun-delay** (`fire.cpp:741`) before `parent->Fire(FireBitmask(...))`. Missiles
suppress guns that frame (`FireBitmask`, `fire.cpp:710-725`).

---

## 2. The maneuver-script catalog (`hard_coded_scripts.cpp`)

These are the named scripts an `agg.xml` `Script="..."` can call. Each is a
`CCScript(Order*, Unit*)` (`hard_coded_scripts.h`) that enqueues sub-orders
(MatchLinearVelocity / FaceTarget / ChangeHeading / FlyByWire) onto the AI.

| Script | What it does |
|--------|--------------|
| `AfterburnTurnTowards` | match velocity toward `(0,0,10000)` (forward) + FaceTarget; afterburn if `use_afterburner_to_follow`. The bread-and-butter close (`:225`). |
| `AfterburnTurnTowardsITTS` | same but FaceTargetITTS (lead the target) (`:235`). |
| `TurnTowards` / `TurnTowardsITTS` | non-afterburner versions (`:935`, `:990`). |
| `FlyStraight` / `FlyStraightAfterburner` | hold heading, zero angular velocity (`:920`, ` :927`). |
| `BarrelRoll` | FlyByWire roll left/right (random sign) + random up/right deflection, match max speed, afterburn (`:245`). |
| `EvadeLeftRight` / `EvadeUpDown` | `EvadeLeftRightC` FlyByWire that flips deflection sign whenever heading drifts past `evasion_angle` (`:128`,`:280-310`). |
| `AfterburnEvadeLeftRight` / `AfterburnEvadeUpDown` | same, afterburner on (`:290-300`). |
| `LoopAround` / `LoopAroundFast` / `LoopAroundSlow` | overshoot-and-curl-back: if past the target (`r.Dot(relloc)<0`) face+ITTS and close; else fly to an offset point beside the target. Uses `loop_around_*` config dists + `personalityseed` RNG (`:315-430`). |
| `AggressiveLoopAround[Fast/Slow]` | loop variant that keeps facing the target (MoveToParent) for a gun-on pass (`:445-520`). |
| `FacePerpendicular[Fast/Slow]` / `RollFacePerpendicular*` | fly perpendicular to the target's facing (a deflection-shot setup) (`:525-640`). |
| `Roll{Left,Right}` / `Roll{Left,Right}Hard` | timed `MatchRoll` at max roll rate (`:660-700`). |
| `VeerAway` / `VeerAwayITTS` | thrust sideways (cross product of relpos) then face/ITTS (`:1050-1075`). |
| `VeerAndVectorAway` / `AfterburnVeerAndVectorAway` / `AfterburnVeerAndTurnAway` | escape variants combining sideways thrust + heading change (`:1080-1120`). |
| `SheltonSlide` / `AfterburnerSlide` / `SkilledABSlide` | "Shelton slide" inertial drift: kill rotation while keeping lateral velocity so guns stay on while the hull slips sideways (`:1135-1200`). |
| `TurnAway` / `AfterburnTurnAway` | point/run directly away (`200*(self-target)`) (`:905-918`). |
| `Kickstop` / `MatchVelocity` / `CoastToStop` / `Stop` / `DoNothing` | velocity-management: hard stop, match target velocity, coast, full stop, hold current velocity (`:830-900`). |
| `MoveTo` | MoveTo the target's position (`:825`). |
| `CloakForScript` | close + face + cloak for a while (`:970`). |
| `DropCargo` / `DropOneCargo` / `DropHalfCargo` | eject cargo + stop (merchant "take my stuff, spare me") (`:1000-1045`). |
| `Takeoff` / `TakeoffEveryZig` | launch sequences for carriers/bases (`:1210-1290`). |
| `SelfDestruct` | explode/split (`:725`). |

Config-driven numeric constants used by these live under `configuration().ai.*`
(`loop_around_distance`, `evasion_angle`, `roll_order_duration`,
`gun_range_percent_ok`, `use_afterburner*`), i.e. the maneuvers are tunable
without recompiling.

---

## 3. Three-way comparison

| Axis | Vanilla Privateer (decoded MNVR, `docs/ai_model.md`) | PGG / Vega Strike (`/tmp/vegastrike_ai`) | new_privateer (`src/ship_ai.cpp`) |
|------|------------------------------------------------------|------------------------------------------|------------------------------------|
| **Condition model** | Maneuver-object tree: per-tick dispatch matches target by 8-byte key, runs the maneuver's `vtable +0x0c` **condition**, and on match runs `+0x14` **update** (§9.3). Leaf objects (ORIENT/APPROACH/EVADE) carry their own phase/timer state. | XML logic list; each item = named metric (Distance/Hull/Facing/Threat/Rand/...) in a `[min,max)` window, AND-able by nesting; first passing item wins and its `Script` runs for `timetofinish`s. Two channels: logic + priority-gated interrupt (`aggressive.cpp:494,1660`). | Hardcoded `enum AIState {Idle,Patrol,Engage,BreakOff,Flee}` switch; transitions are inline `if (has_hostile && hp<=flee_hp)`-style guards (`ship_ai.cpp:~163-260`), act blocks per state. |
| **Maneuver catalog** | ~35 unique MNVR scripts across 47 pilots x 3 morale tiers; class IDs `0x06/0x07` (approach/attack pair), `0x11/0x12/0x13` (steer/throttle/fire), `0x12` evade, `0x19` terminator (§7.5, §2.3). Three decoded leaves: ORIENT, APPROACH/ATTACK-RUN, EVADE/JINK (§9.4). | ~40 named C++ scripts (table in §2): turn-towards, barrel roll, loops, evades, slides, veers, perpendicular, rolls, cloak, drop-cargo, takeoff, etc. | 3 real maneuvers: lead-pursuit + timed attack run (Engage), perpendicular+away afterburn extend (BreakOff), away+sinusoidal-jink (Flee). No barrel roll / loop / slide. |
| **Distance / range tiers** | Sensor/awareness **15000**; comms/taunt `f1` **1500**; pursue->attack-run **1000**; break-off `f0` **600 + hull radii**; gun = per-gun range + arc (§0.1). | `in_weapon_range` (config) for fire; `gun_range_percent_ok` for loop closing; condition windows author all other tiers in the XML (`Distance`/`MeterDistance`). | Same tiers as vanilla, adopted 1:1 via `kPvtScale=1.0`: `k_pursue_attack_switch=1000`, break `f0`=`engage_f0` (def 600)+radii, `comms_f1` (def 1500) for barks (`ship_ai.cpp:~70-110`). No 15000 sensor gate in this file (perception owns detection). |
| **Firing decision** | Attack-run leaf: inside 1000u, fire from tick **76** to **153** (guns-hot window), then run complete (§9.4.2). Arc/accuracy from `f2`/`f3` skill tier (not fully decoded). | `dist < in_weapon_range` AND lead-corrected `angle > fangle`, where `fangle` interpolates between ~10deg and ~18deg by `aggressivity`; ITTS lead via `cosAngleTo(gunspeed)`; reaction-time delay; missile probability roll (`fire.cpp:643-744`). | 15deg fixed cone (`cos=0.966`) on the lead vector, gated by the timed attack-run window (tick76..153 -> seconds via `kAiHz=10`), `weapons_range` check, throttle to 0.5 on the run (`ship_ai.cpp:Engage act`). No reaction-time, no missiles, fixed cone. |
| **Morale / experience / personality** | Two-axis (FAQ §5.1): experience = `f3` evade gain + `f2` accuracy; morale = `f6` caution (low=fanatical 64, high=timid 128). 3 morale tiers each with own MNVR script (§0.2, §9.3). | `personalityseed` (per-pilot RNG) selects script file via `VegaPersonalities.csv` and seeds maneuver RNG (`aggressive.cpp:171-188,230`); `aggressivity` widens firing cone; `obedience` gates flightgroup orders. | Per-class CNST proxies: `morale_f6` -> flee HP threshold (`flee_threshold_for`), `maneuver_jitter_f3` -> break duration/character, `personality==Coward` -> flee-when-hunted. RNG desync off `(t_now, ship.id)`. No per-pilot script selection. |
| **Evasion** | EVADE/JINK leaf: pick 1 of 4 cardinal rotation axes + roll in {-1,0,+1}; turn rate full if angular mag < 50, else `(f3 + rng*30)/mag` (§9.4.3). | `BarrelRoll`, `EvadeLeftRight/UpDown` (sign-flip on heading drift past `evasion_angle`), slides, veers, loops — all selectable as interrupt scripts. | BreakOff: 1-of-4 cached axis + roll {-1,0,+1} blended with away-vector, afterburn extend (mirrors vanilla §9.4.3 directly). Flee: sinusoidal perpendicular jink (~25deg, ~3s period). |
| **Data-driven vs hardcoded** | **Data-driven** (serialized object tree in MNVR bytes per pilot, 3 tiers). | **Data-driven** (XML per role/faction/personality; maneuvers are the only C++ part). | **Hardcoded** C++ state machine; constants in code (sourced from the vanilla decode but baked, not authored). |

---

## 4. AI tunables / the `agg.xml` files

`find` for `*.agg.xml` / `*aggressiv*` / `interpolate*` in the clone returns
**only** `aggressive.cpp/.h` — the engine repo does **not** ship PGG's data
tree, exactly as expected (PGG is engine + separate data). The tuning surface is:

- **`*.agg.xml`** (and `*.int.xml` interrupt files): the per-role/faction logic
  lists. Default is `default.agg.xml` / `default.int.xml` (`aggressive.cpp:253-258`).
  Format fully recoverable from the loader (`event_xml.cpp`, §1.2 above) even
  without a sample: root `<AggressiveAI time=.. obedience=..>` + nested
  condition elements with `min/max/not/Script/time/timetointerrupt/priority`.
- **`VegaPersonalities.csv`**: maps `faction` / `faction%unittype` to a
  space-separated list of personality appends; `personalityseed % count` picks
  one (`aggressive.cpp:113-138,180-188`).
- **`configuration().ai.*`** and `configuration().unit.default_aggressivity`:
  global dials — `firing.aggressivity`, `firing.maximum_firing_angle.min/maxagg`,
  `firing.in_weapon_range`, `firing.missile_probability`, `loop_around_*`,
  `evasion_angle`, `roll_order_duration`, `gun_range_percent_ok`,
  `use_afterburner[_to_run/_to_follow]`, `targeting.obedience`.

The public PGG/WCU data trees (sourceforge "Privateer Gemini Gold",
github "wcuniverse") carry the actual `agg.xml` values; not cloned here to avoid
a rabbit-hole. The format + defaults above are enough to author against.

---

## 5. KEY INSIGHT: vanilla's `+0x0c`/`+0x14` tree IS PGG's condition->script list

The two originals are the **same architecture** wearing different clothes:

| Vanilla MNVR (decoded) | PGG `agg.xml` |
|------------------------|---------------|
| per-tick dispatch matches target, calls maneuver `vtable +0x0c` **condition** (§9.3) | `ProcessLogicItem` evaluates a logic item's **condition window** (`aggressive.cpp:309`) |
| on match, runs `vtable +0x14` **update/tick** that drives the ship | on match, `ExecuteLogicItem` runs the named **Script** for `timetofinish` (`aggressive.cpp:299`) |
| leaf object carries phase flags + timer (`leaf+0x14`) (§9.1b) | `ExecuteFor(AIScript, timetofinish)` carries the run timer (`aggressive.cpp:301`) |
| 3 morale-tier slots, target-key matched (§9.3) | per-role/faction/**personality** script files, `personalityseed`-selected (§1.3) |
| `0x19 0x19 0x19` terminator / loop | `maxtime` loop + reselection each `logiccurtime` expiry |

Both are **"evaluate a condition table each tick; the first/active match selects
a maneuver object that runs for a bounded time, carrying its own phase/timer
state."** Vanilla serializes that table as an object tree in MNVR bytes; PGG
serializes it as XML. Vanilla varies it by 3 morale tiers; PGG by
personality-selected files. Our `AIState` switch is the **collapsed, hardcoded**
form of the same idea.

**Likely script <-> class-ID correspondence** (both `[I]`, structural):

| Vanilla class ID (§7.5/§9.4) | Vanilla decoded role | PGG named script analog |
|------------------------------|----------------------|-------------------------|
| `0x06`, `0x07` (paired, mid-script) | approach/attack sub-block | `AfterburnTurnTowards` / `AfterburnTurnTowardsITTS` (close + face/lead) |
| `0x11` / `0x13` (dominant body) | steer/throttle toward heading = ORIENT (`0x3966f`) / APPROACH (`0x397c8`) | `TurnTowards` / `FaceTarget` / `MatchLinearVelocity`-driven closes; `LoopAround`/`FacePerpendicular` for the curl-back |
| `0x12` | EVADE/JINK leaf (`0x39981`, the `f3` consumer) | `EvadeLeftRight` / `EvadeUpDown` / `BarrelRoll` |
| `0x0e/0x0d/0x0f/0x09` (roots) | maneuver-list container | the `<AggressiveAI>` root + its logic list |
| `0x19` | terminator/loop | `maxtime` reselection loop |

I.e. APPROACH (vanilla 0x397c8, the 1000u lead-pursuit / 76..153 attack run)
is the structural twin of PGG's `AfterburnTurnTowardsITTS` gated by a
`Distance min=0 max=1000` logic item; EVADE (0x39981) is `Evade*`/`BarrelRoll`
gated by a `Threat`/`Hull`/`Rand` interrupt item.

---

## 6. WHAT TO BORROW (ranked, concrete)

### 6.1 Adopt a data-driven condition->maneuver table (highest value)
Both originals are data-driven; we are a hardcoded switch. Replace the `AIState`
cascade with a small table of `{condition, maneuver, min, max, timetofinish,
priority}` rows, evaluated each tick exactly like `ProcessLogic`. This is the
single biggest architectural alignment and it directly matches the decoded
vanilla `+0x0c`/`+0x14` model (`docs/ai_model.md` §7.8 already flags this as the
"next" step). Concretely:
- Keep our existing maneuver *implementations* (Engage/BreakOff/Flee acts) as
  the "scripts"; add a couple more (below).
- Condition metrics to support first: `Distance`, `Hull`, `Threat` (incoming
  fire), `Facing`/in-arc, `Rand`. These cover ~all of vanilla's used set.
- Split into a **logic** channel (timed reselection) and a **priority interrupt**
  channel (preempt on incoming fire / critical hull) — this is PGG's two-pass
  structure (`aggressive.cpp:1660-1700`) and gives us missile-evasion-style
  reactivity our single-pass machine can't express (`ship_ai.h` even notes "no
  hysteresis yet"; a priority+timer table subsumes that).
- Author the table in JSON (we already load ship/AI from JSON) so designers tune
  without recompiling — PGG's whole point.

### 6.2 Add the missing named maneuvers
We have 3 maneuvers; vanilla has ~35 scripts, PGG ~40. Highest-impact additions,
in order:
1. **BarrelRoll** (`hard_coded_scripts.cpp:245`) — cheap, hugely improves the
   "ship under fire" read; maps to vanilla `0x12`.
2. **LoopAround / AggressiveLoopAround** (`:315`) — the overshoot-and-curl-back.
   This is what makes dogfights look like dogfights instead of jousting; it is
   the natural replacement for our BreakOff straight-line extend.
3. **EvadeLeftRight/UpDown** (`:280`) — sign-flipping weave; a better Flee jink
   than our fixed sinusoid (theirs is reactive to heading drift).
4. **Shelton/AB slide** (`:1135`) — keep guns on target while drifting sideways;
   a high-skill maneuver that reads as "ace pilot".
5. **DropCargo** (`:1000`) for merchants — narrative-correct surrender behaviour.

### 6.3 Improve the firing decision toward PGG's model
Ours is a fixed 15deg cone + timed window. Borrow:
- **Aggressivity-scaled firing cone** (`fire.cpp:663-671`): widen/narrow the
  cone per-pilot from a skill/aggressivity value instead of a global `0.966`.
  Maps cleanly onto our per-class `f2` (accuracy) which is currently unused for
  arc (`docs/ai_model.md` §3 flags `k_engage_cos_threshold` as a candidate).
- **ITTS lead toggle** (`fire.cpp:656`): we already compute a lead vector; make
  *whether* we lead a per-pilot capability (rookies fire straight, aces lead),
  mirroring `computer.itts`.
- **Reaction-time delay** (`fire.cpp:736`): gate first-shot on a per-pilot
  reaction time so pilots don't snap-fire the instant the cone aligns. Cheap,
  big feel improvement, and maps to the experience axis.
- **Missile probability + gun-delay** (`fire.cpp:730,741`): we fire no missiles
  yet; when we add them, copy the per-atom probability + cooldown structure
  rather than inventing one.

### 6.4 Personality variation: lean toward `personalityseed`, keep per-faction CNST
PGG's `personalityseed` (`aggressive.cpp:171-188,230`) gives **per-pilot**
variety two ways: it selects *which script file* a pilot runs and seeds its
maneuver RNG. We currently derive everything from per-**class** CNST
(`morale_f6`, `maneuver_jitter_f3`) plus an `(t_now, ship.id)` RNG. Recommended:
- Keep per-faction/class CNST as the **baseline** dial (it is the vanilla model;
  `f6` morale tier, `f3` evade gain) — do NOT throw this away for `personalityseed`.
- Add a stable **per-pilot seed** (stamped at spawn, like vanilla's
  `personalityseed`) and use it to (a) jitter CNST values +-a few percent so two
  Talons of the same faction don't fly identically, and (b) seed jink geometry
  deterministically (so a given pilot's weave is stable across frames — ours
  recomputes off `t_now` which is fine but not reproducible).
- Once §6.1 lands, let the seed pick among 2-3 authored tables per
  faction/morale tier — that is exactly PGG's file-selection, and vanilla's
  3-morale-tier array (§9.3), unified.

### 6.5 Wire the morale tiers explicitly
Vanilla has **3 morale-tier MNVR scripts per pilot** (§9.3); PGG approximates via
files. We collapse morale to a single flee-HP number. Borrow the *tier* idea:
seed a low/normal/high morale tier from `f6` and from current HP/incoming-fire,
and let the tier select a different table (fanatical = head-on/no-break,
timid = early-break/flee). This is the structural piece both originals have and
we don't.

---

## 7. What DIFFERS / what NOT to copy

- **Vega Strike is a much heavier sim.** `AggressiveAI` carries shield/armor
  *heal-rate* conditions (8 facings, `aggressive.cpp:78-86,360-470`), flightgroup
  obedience + comms FSM (`ProcessCurrentFgDirective`, `aggressive.cpp:540-700`),
  jump-drive AI, turret sub-unit AI (`TurretFAW`), contraband scans, and
  warp-to-enemy. Privateer (and our game) has none of that. **Do not** port the
  heal-rate conditions, turret recursion, jump/warp AI, or the comms-FSM
  obedience system — they target a fundamentally bigger ship/economy sim.
- **Order/sub-order bit-coding** (`order.h:114`, parallel MOVEMENT|FACING|WEAPON
  via a suborder queue) is more machinery than we need; our controller already
  separates rotation from throttle. Borrow the *concept* (FACING and WEAPON run
  in parallel) but not the `Order` class hierarchy.
- **`ExecuteFor`/`AIScript` order objects** (heap-allocated per maneuver every
  reselection) are fine for VS but we should keep maneuvers as plain functions /
  table rows, not allocate order objects per tick.
- **Aggressivity default `2.01F` sentinel** (`aggressive.cpp:227`) and the
  config-soup of ~30 `configuration().ai.*` knobs is more surface than we want;
  pick the handful in §6.3 and keep the rest baked from the vanilla decode.
- **The 15000 sensor/awareness gate** is vanilla-canonical (`docs/ai_model.md`
  §0.1) and already lives in our `perception` layer — keep it there; do NOT move
  detection into the firing/maneuver code the way VS blends it into `FireAt`.
- **Vanilla wins on authority for *values*.** Where VS and the decoded MNVR
  disagree on a number (ranges, the 76/153 attack-run window, the 1000u switch),
  trust the decode (`docs/ai_model.md` §0) — it is *this game's* AI; VS is a
  cousin we borrow *structure* from, not numbers.

---

## 8. One-paragraph summary

PGG/Vega Strike runs a **data-driven condition->script** combat AI:
`AggressiveAI` evaluates an XML table of `{metric-in-a-window -> named maneuver
for N seconds}` items each tick across a normal "logic" channel and a
priority-gated "interrupt" channel (`aggressive.cpp:494,1660`), selecting from
~40 hard-coded maneuver scripts (`hard_coded_scripts.cpp`), with per-pilot
variety from `personalityseed` (file selection + RNG) and aggressivity-scaled,
ITTS-led, reaction-time-gated firing (`fire.cpp:643-744`). This is structurally
**identical** to vanilla Privateer's decoded MNVR maneuver-object tree (per-tick
`+0x0c` condition / `+0x14` update over 3 morale tiers, `docs/ai_model.md`
§9.3-9.4): both are "condition table -> bounded-time maneuver object with its own
timer." Our `new_privateer` AI is the collapsed hardcoded `AIState` form. The
ranked borrow list: (1) adopt a JSON condition->maneuver table with a
priority-interrupt channel, (2) add BarrelRoll / LoopAround / EvadeLeftRight /
slides, (3) take PGG's aggressivity-cone + ITTS-toggle + reaction-time firing,
(4) add a per-pilot seed on top of (not instead of) per-faction CNST, (5) make
morale tiers explicit. Skip VS's heavyweight sim machinery (heal-rate conditions,
turret/jump/warp AI, comms-FSM obedience, per-tick order allocation).
