#!/usr/bin/env python3
"""Generate a diverse corpus of Privateer in-flight comms + conversations.

Produces assets/speech/generated/comms.json:
  - "lines": ~250 standalone comms (greetings, taunts, low-hp, kills, demands,
    death cries) per faction, each tagged with the cloned MiniMax voice_id.
  - "conversations": multi-turn exchanges between entity pairs (merchant vs
    militia inspection, pirate vs bounty hunter, confed arrest, etc.).

Next step (synth_comms.py) renders each line/turn via t2a_v2.
"""
from __future__ import annotations

import itertools
import json
import random
from pathlib import Path

random.seed(7)

# Faction -> primary cloned voice (from clone_refs/uploads.json). Some factions
# list alternates so the corpus uses more than one actor where we have them.
VOICE = {
    "merchant": ["PrivFlightV1401", "PrivFlightV0001", "PrivFlightV0601"],
    "militia": ["PrivFlightV0101", "PrivFlightV0701"],
    "pirate": ["PrivFlightV1501", "PrivFlightV0201", "PrivFlightV1201"],
    "bounty_hunter": ["PrivFlightV0501", "PrivFlightV1101", "PrivFlightV1301"],
    "confed": ["PrivFlightV0801"],
    "kilrathi": ["PrivFlightV0901"],
    "retro": ["PrivFlightV0401"],
    "steltek": ["PrivFlightV1001"],
    "player": ["PrivBarPc01"],
}

CALLSIGN = ["friend", "Ace", "pilot", "flyboy", "Captain", "stranger", "mister",
            "hotshot", "tin can", "sport", "chum", "sunshine", "sweetheart",
            "buddy", "rookie", "old-timer", "big shot"]
PLACE = ["this sector", "my quad", "Confed space", "the Troy system",
         "this quadrant", "these lanes", "the New Detroit run", "Pleiades",
         "the Palan blockade", "the Oxford run", "this dead end of space",
         "the Humboldt quad", "the asteroid fields"]
CARGO = ["your cargo", "that contraband", "your hold", "those goods",
         "your shipment", "that hot merchandise", "your manifest",
         "those crates", "your precious freight"]
SHIP = ["that crate", "your tin can", "that flying coffin", "your bucket",
        "that scrap heap", "your pretty little ship"]
NAME = ["Lynch", "the Admiral", "the guild", "my employer", "the boss",
        "Sandoval", "command"]

# faction -> category -> templated lines ({c}=callsign {p}=place {g}=cargo)
LINES = {
    "merchant": {
        "greeting": ["Just a peaceful trader, {c}, don't get jumpy.",
                     "Good to see a friendly hull out here.",
                     "Care to do a little business, {c}?",
                     "Fly safe out in {p}, the void's hungry today."],
        "hostile": ["Back off! The guild will hear about this!",
                    "You maniac, I'm just hauling freight!",
                    "I've got nothing worth dying over, {c}!"],
        "low_hp": ["Please, I'll dump {g}, just let me live!",
                   "My poor ship! I'll be selling it for scrap!",
                   "Mayday! I'm a civilian, somebody help!"],
        "kill": ["Serves you right, attacking an honest dealer.",
                 "Another bandit cashes in his chips.",
                 "Not bad for a lowly merchant, eh?"],
    },
    "militia": {
        "greeting": ["Militia patrol, stick to your flight path, {c}.",
                     "We've got {p} bottled up tight, keep it clean.",
                     "You're clear to pass. Keep your nose clean."],
        "hostile": ["Prepare for inspection, cut your engines.",
                    "You're not leaving {p} alive, smuggler.",
                    "Contraband detected. All units, close and terminate."],
        "low_hp": ["Militia forces need assistance, we're in trouble!",
                   "Sensors show we're breaking up, falling back!"],
        "kill": ["Target eliminated. Militia to base, enemy dispatched.",
                 "That's another runner off the board."],
        "demand": ["Maintain speed and course for a contraband search.",
                   "Power down and prepare to be boarded, {c}."],
    },
    "pirate": {
        "greeting": ["Well, well, fresh meat drifts into {p}.",
                     "Looking sharp, {c}. Shame about what's coming."],
        "hostile": ["Drop {g} or I scatter you across {p}!",
                    "You're a loose end looking to be tied, {c}.",
                    "Nice ship. It'll look better in my hangar."],
        "low_hp": ["Damn, you're ripping me apart!",
                   "This crate's about to blow, I'm out!",
                   "Alright, alright, take {g}, just stop shooting!"],
        "kill": ["You're nailed, {c}. Easy money.",
                 "Should've paid the toll, {c}.",
                 "Another fool who couldn't fly. Hah!"],
        "demand": ["Jettison {g} now and maybe you live.",
                   "This is a toll road, {c}. Pay up."],
    },
    "bounty_hunter": {
        "greeting": ["You're on my list today, {c}. Nothing personal.",
                     "I only kill for a fee, {c}. Today the fee's on you."],
        "hostile": ["The advantage is mine. Now you die.",
                    "Nothing personal, but your death is my paycheck.",
                    "Oh, this is most distasteful, I must say."],
        "low_hp": ["This wasn't in the contract!",
                   "No! It can't end like this!"],
        "kill": ["Another day, another kill. One more for the resume.",
                 "Clean shot. The guild pays well for you, {c}.",
                 "Killing you put me in the black, {c}."],
    },
    "confed": {
        "greeting": ["Confederation patrol. State your business in {p}.",
                     "We have you on our screens, {c}. Continue on course."],
        "hostile": ["You've got quite a record, {c}, but it ends here.",
                    "We've accessed your file, pirate. You're done.",
                    "Halt and surrender, by order of the Confederation."],
        "low_hp": ["Mayday, we've sustained massive damage!",
                   "Confed wing breaking up, need backup now!"],
        "kill": ["One bogey terminated. Enemy target destroyed.",
                 "Criminal scum, neutralized. Confed out."],
        "demand": ["Prepare to be searched for contraband, {c}.",
                   "Cut your engines. This is a Confederation stop."],
    },
    "kilrathi": {
        "greeting": ["My claws are sheathed today, apeling. Pass.",
                     "You amuse me, hairless one. For now."],
        "hostile": ["Your pelt will adorn my hall, ape!",
                    "Crawl back to your dirtball, human filth!",
                    "I will savor the snapping of your bones."],
        "low_hp": ["The hunt turns! This cannot be!",
                   "My ship bleeds! Curse you, ape!"],
        "kill": ["Your bones are mine to mount.",
                 "Another ape returns to the void. Hrrr."],
    },
    "retro": {
        "greeting": ["Repent, sinner, the Church of Man watches {p}.",
                     "Cast off your machines and be saved."],
        "hostile": ["Die by the very weapons you adore!",
                    "Your destruction is the will of God!",
                    "Sinner, ready thyself for righteous retribution!"],
        "low_hp": ["The Lord tests me! I shall not yield!",
                   "Machines of damnable intent, you wound me!"],
        "kill": ["The purifying fire takes another heathen!",
                 "The Church of Man condemns you. Burn."],
    },
    "steltek": {
        "greeting": ["We are the guardians. Your kind is... noted.",
                     "Lifeform detected. Intent: unknown."],
        "hostile": ["You will surrender the relic, primitive.",
                    "Your weapons are dust against us."],
        "kill": ["Threat neutralized. We endure.",
                 "You were nothing. We remain."],
    },
}

# More short/medium lines, merged into LINES at runtime.
EXTRA = {
    "merchant": {
        "greeting": ["Don't mind me, {c}, just making an honest living.",
                     "Cargo hauler coming through {p}, no trouble here.",
                     "You buying or selling, {c}? I deal fair.",
                     "Lovely day for a milk run, wouldn't you say, {c}?"],
        "hostile": ["Hey! Watch the paint, you lunatic!",
                    "I'm unarmed, {c}, this is piracy!",
                    "You'll never haul in this guild again, you hear me?"],
        "low_hp": ["My engine's on fire! I'm punching out!",
                   "Take {g}, take it all, just spare the lifeboat!",
                   "Twenty years building this business, gone in {p}..."],
        "kill": ["And stay down, you space-faring leech.",
                 "Turns out the merchant had teeth after all."],
    },
    "militia": {
        "greeting": ["Welcome to {p}, {c}. We run a tight ship here.",
                     "Keep your transponder lit and we'll get along fine.",
                     "Eyes open out there, {c}, pirates have been busy."],
        "hostile": ["That's a restricted lane, {c}. Turn back or be fired on.",
                    "You've ignored three hails. That's enough.",
                    "Hostile confirmed. Militia weapons free."],
        "demand": ["Reduce speed to dock velocity and stand by for boarding.",
                   "Open {g} for inspection, {c}, this won't take long."],
        "kill": ["Threat neutralized. {p} is secure once more."],
    },
    "pirate": {
        "greeting": ["Lost, little {c}? Let me take that burden off your hands.",
                     "Pretty {s} you got there. Be a shame if I kept it."],
        "hostile": ["Hand over {g} or I paint {p} with your guts!",
                    "You picked the wrong shortcut through {p}, {c}.",
                    "Nothing personal, {c}. Okay, maybe a little personal."],
        "low_hp": ["Okay okay, you win! I'm jettisoning the loot!",
                   "Not like this! {n} promised me an easy mark!"],
        "kill": ["Ha! {n}'ll pay extra for that {s} of yours.",
                 "Sleep tight in the void, {c}."],
        "demand": ["This is {n}'s space now. Toll's everything you got.",
                   "Cut engines and prepare to be relieved of {g}."],
    },
    "bounty_hunter": {
        "greeting": ["{n} put a price on your head, {c}. I came to collect.",
                     "Don't run. They always run. It's so undignified."],
        "hostile": ["Hold still, {c}, this is just business.",
                    "Your bounty just covered my fuel for a year."],
        "kill": ["Contract fulfilled. {n} will be pleased.",
                 "Tag and bag. Another name off the board."],
    },
    "confed": {
        "greeting": ["This is a Confederation checkpoint, {c}. Slow and steady.",
                     "Your file's clean, {c}. Move along."],
        "hostile": ["Last warning, {c}. Stand down or be destroyed.",
                    "You're a wanted criminal in {p}. Surrender now."],
        "kill": ["Fugitive neutralized. Logging the kill, {p}, Confed out."],
        "demand": ["Heave to and prepare for a Confederation contraband sweep."],
    },
    "kilrathi": {
        "hostile": ["You stink of fear, ape. It excites me.",
                    "Run, little human. It makes the kill sweeter.",
                    "Your ancestors fled us. You will only die tired."],
        "greeting": ["Today I do not hunger, ape. Be gone before that changes."],
        "kill": ["Hrraaa! A worthy pelt for my wall."],
    },
    "retro": {
        "hostile": ["Your engines are blasphemy! I will silence them!",
                    "The machine-spirit in you must be purged, heathen!"],
        "greeting": ["Turn from your wicked circuitry, {c}, and live."],
        "kill": ["Cleansed. The Church of Man rejoices."],
    },
    "steltek": {
        "hostile": ["Your tools are crude. Your end is certain.",
                    "You touch what is not yours, fledgling species."],
        "greeting": ["We have watched your kind crawl from the mud. Curious."],
    },
}

# Longer, multi-sentence comms (category 'long'): monologues, sermons, briefs.
LONG = {
    "merchant": [
        "Look, {c}, I've been hauling this route for thirty years. I've paid "
        "the pirates, paid the militia, paid the guild dues. I just want to "
        "reach port with my hold intact and maybe see my kids again. So "
        "whatever you're planning, let's both be reasonable about it.",
        "Friendly ship, am I glad to see you. The whole run through {p} has "
        "been nothing but trouble. Lost my escort two jumps back, scanners "
        "keep ghosting contacts. If you're headed my way, mind flying "
        "cover? I'll make it worth your while at the next base, I swear it.",
    ],
    "militia": [
        "Attention all craft in {p}. This is system militia. We have reports "
        "of pirate activity along the main shipping lane. Maintain escort "
        "formation, keep your comms open, and report any unidentified "
        "contacts immediately. We're stretched thin out here, so watch each "
        "other's backs. Militia out.",
        "Listen up, {c}. Your transponder's flagged, your manifest doesn't "
        "add up, and you've been loitering near a restricted jump point. "
        "Now, I can do this the easy way, where you power down and let us "
        "aboard, or the hard way, where I call in a strike wing. Your call. "
        "You've got ten seconds.",
    ],
    "pirate": [
        "Well now, what have we here. A fat little hauler, all alone, miles "
        "from any patrol. You know how this goes, {c}. You dump {g}, I let "
        "you limp home to tell everyone how generous {n} was. Or you can be "
        "a hero, and I sell {s} for scrap with you still in it. Tick tock.",
        "You Confed types always think you're untouchable out here. But this "
        "is the frontier, {c}. No badges, no backup, no rules. Out here the "
        "only law is who's got the bigger guns, and pal, I've been "
        "upgrading. Let's find out whose toll the void collects today.",
    ],
    "bounty_hunter": [
        "You want to know the secret, {c}? It's not about the killing. Any "
        "fool with a gun can kill. It's about the patience. I've tracked you "
        "across four systems, learned your jump patterns, your fuel stops, "
        "your habits. By the time you saw me, it was already over. Nothing "
        "personal. It never is. It's just the only thing I'm good at.",
    ],
    "confed": [
        "This is Admiral's wing, all units, listen close. The target jumps "
        "in at these coordinates in under a minute. We hit it hard and fast "
        "the moment it arrives, no hesitation. Hold your positions until "
        "then, keep formation tight, and watch your fire. Any privateers in "
        "the area, feel free to join in. There's bounty enough to share.",
    ],
    "kilrathi": [
        "You fly well, ape. For a hairless thing born in the mud, you have "
        "surprised me. So I will grant you the honor my kind reserves for "
        "worthy prey. I will tell of this hunt in my hall, and your name, "
        "if you have one, will be spoken with something near respect. Now. "
        "Let us finish this as warriors. Claws out.",
    ],
    "retro": [
        "Sinner! Look upon your works and despair! You wrap yourself in "
        "metal and circuitry, you trust your soul to soulless machines, and "
        "you call it progress. It is rot. It is the great lie that doomed "
        "the old world. But the Church of Man offers you one mercy: repent, "
        "cast out your devices, and embrace a clean death. Refuse, and the "
        "purifying fire shall do for you what you would not do for yourself.",
    ],
    "steltek": [
        "Lifeform. You carry a relic of the makers, a fragment of a power "
        "your kind cannot comprehend and must not wield. We were left to "
        "watch. To wait. To ensure the old weapons never woke. You have "
        "woken one. This cannot be permitted. Surrender it, and you may yet "
        "return to your mud. Refuse, and you will simply cease.",
    ],
}

PAIRS = [
    ("Inspection gone wrong", [
        ("militia", "Trader, this is the militia. Cut engines for a contraband check."),
        ("merchant", "Of course, officer, I'm clean, just hauling grain to New Detroit."),
        ("militia", "Our scanners say otherwise. Those crates read as Brilliance."),
        ("merchant", "That's-- that's a sensor glitch, I swear on the guild!"),
        ("militia", "Power down. You're impounded, smuggler."),
        ("merchant", "No, please, I'll lose everything! Can't we work something out?"),
        ("militia", "Bribery now? That's another charge. Prepare to be boarded."),
    ]),
    ("Turf war", [
        ("pirate", "Back off, hunter. This merchant's cargo is mine."),
        ("bounty_hunter", "I'm not here for the cargo. I'm here for you."),
        ("pirate", "You've got a contract on me? Who's paying?"),
        ("bounty_hunter", "Does it matter? Nothing personal. It's just good business."),
        ("pirate", "Then come and collect, you cold-blooded snake!"),
        ("bounty_hunter", "Gladly. One more for the resume."),
    ]),
    ("The arrest", [
        ("confed", "Pirate vessel, this is Confed patrol. You're under arrest."),
        ("pirate", "Arrest? Out here? You Confed boys are a long way from home."),
        ("confed", "We've accessed your file. Murder, smuggling, piracy. It ends now."),
        ("pirate", "It ends when I say it ends, badge."),
        ("confed", "Have it your way. All units, weapons free."),
    ]),
    ("Zealot and trader", [
        ("retro", "Heathen! Your hold is full of the devil's machines!"),
        ("merchant", "It's mining equipment, you lunatic! Leave me be!"),
        ("retro", "Technology is the rot of mankind. Repent, or burn!"),
        ("merchant", "Somebody, anybody, there's a retro on my tail!"),
        ("retro", "No one hears you out here. Only God. And He approves."),
    ]),
    ("Predator's mercy", [
        ("kilrathi", "Halt, apeling. Your cargo smells of fear."),
        ("player", "I'm just passing through. I don't want a fight."),
        ("kilrathi", "Wise. Perhaps I let you live, to tell the others of me."),
        ("player", "...Generous of you."),
        ("kilrathi", "Do not mistake boredom for mercy. Fly, before I change my mind."),
    ]),
    ("Jurisdiction", [
        ("militia", "Confed wing, this is system militia. We've got this one."),
        ("confed", "Negative, militia. That's a Confederation fugitive."),
        ("militia", "He's in our space, breaking our laws. He's ours."),
        ("confed", "Stand down. You're outgunned and out-ranked."),
        ("militia", "...Acknowledged. He's all yours, flyboy. Don't lose him."),
    ]),
    ("The contract", [
        ("bounty_hunter", "There you are. You're worth a lot of credits, Captain."),
        ("player", "Whoever hired you, I can pay double. Name it."),
        ("bounty_hunter", "Tempting. But a hunter who breaks contract never works again."),
        ("player", "Then you leave me no choice."),
        ("bounty_hunter", "None of us ever had one, friend. Now hold still."),
    ]),
    ("Old war, new fight", [
        ("kilrathi", "Confederation ship. We have unfinished business, your kind and mine."),
        ("confed", "The war's over, cat. Your empire lost."),
        ("kilrathi", "Empires fall. Hatred endures. Defend yourself."),
        ("confed", "All wings, Kilrathi raider, weapons hot!"),
    ]),
    ("Two traders", [
        ("merchant", "Friendly hull, good to see one out here. Slow run today?"),
        ("merchant", "Brutal. Pirates thick as flies past the jump point."),
        ("merchant", "Tell me about it. Lost a cousin on the Palan route last week."),
        ("merchant", "Stay sharp, friend. Fair winds and full holds."),
    ]),
    ("Thieves and law", [
        ("pirate", "Militia patrol, eh? Bit far from your little checkpoint."),
        ("militia", "Far enough to put you down with no witnesses, scum."),
        ("pirate", "Big talk for a rent-a-cop. Let's see if your aim matches."),
        ("militia", "Gladly. Militia to base, engaging a hostile."),
    ]),
    ("The guardians", [
        ("steltek", "Primitive vessel. You carry a fragment of the old ones."),
        ("player", "The... what? I don't understand you."),
        ("steltek", "You would not. Surrender the relic, or be unmade."),
        ("player", "That's not an answer I can live with."),
        ("steltek", "Then you have chosen. We are sorry. We are thorough."),
    ]),
]


def expand(text):
    return (text.replace("{c}", random.choice(CALLSIGN))
                .replace("{p}", random.choice(PLACE))
                .replace("{g}", random.choice(CARGO))
                .replace("{s}", random.choice(SHIP))
                .replace("{n}", random.choice(NAME)))


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--variants", type=int, default=10,
                    help="max distinct fills per templated line (variety knob)")
    args = ap.parse_args()
    K = args.variants
    out = Path("assets/speech/generated")
    out.mkdir(parents=True, exist_ok=True)
    # Merge the extra short/medium pools and the long monologues into LINES.
    for fac, cats in EXTRA.items():
        for cat, pool in cats.items():
            LINES[fac].setdefault(cat, []).extend(pool)
    for fac, pool in LONG.items():
        LINES[fac].setdefault("long", []).extend(pool)

    lines = []
    seen = set()
    counter = itertools.count()
    # Authored lines; templated ones get up to K distinct fills for variety.
    for fac, cats in LINES.items():
        voices = VOICE[fac]
        for cat, pool in cats.items():
            for base in pool:
                if cat == "long":
                    want = 2 if "{" in base else 1
                else:
                    want = K if "{" in base else 1
                tries = 0
                made = 0
                while made < want and tries < max(60, K * 10):
                    tries += 1
                    t = expand(base)
                    if t in seen:
                        continue
                    seen.add(t)
                    made += 1
                    lines.append({
                        "id": f"comm_{next(counter):04d}",
                        "faction": fac, "category": cat,
                        "voice_id": random.choice(voices), "text": t,
                    })
    conversations = []
    for ci, (title, turns) in enumerate(PAIRS):
        distinct = {f for f, _ in turns}
        if len(distinct) == 1:
            # self-conversation (e.g. two traders): alternate two speakers.
            fac = turns[0][0]
            vl = VOICE[fac]
            tobjs = [{"faction": fac,
                      "voice_id": vl[i % 2 % len(vl)],
                      "text": txt} for i, (f, txt) in enumerate(turns)]
        else:
            # one consistent voice per faction for the whole exchange.
            vmap = {f: random.choice(VOICE[f]) for f in distinct}
            tobjs = [{"faction": f, "voice_id": vmap[f], "text": txt}
                     for f, txt in turns]
        conversations.append({
            "id": f"conv_{ci:02d}", "title": title,
            "participants": sorted(distinct), "turns": tobjs,
        })
    corpus = {"lines": lines, "conversations": conversations}
    (out / "comms.json").write_text(json.dumps(corpus, indent=1))

    import collections
    bycat = collections.Counter((l["faction"], l["category"]) for l in lines)
    print(f"[comms] {len(lines)} standalone lines, "
          f"{len(conversations)} conversations -> {out}/comms.json")
    for fac in LINES:
        n = sum(v for (f, _), v in bycat.items() if f == fac)
        print(f"  {fac:14} {n:3d} lines")
    print(f"  conversation turns: {sum(len(c['turns']) for c in conversations)}")


if __name__ == "__main__":
    main()
