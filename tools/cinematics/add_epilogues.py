"""Author post-decision exchanges + prop shots for the fixer conversations.

Two things this adds:

PROPS. ``prop[i]`` swaps the panel art for that line -- the object being
discussed takes the frame instead of a talking head. Used for the Steltek
artifact at the exact beats where it changes hands or gets examined, which is
the whole reason the campaign happens.

EPILOGUES. A scene should not end on a button. ``accept_dialogue`` /
``refuse_dialogue`` play AFTER the player commits and BEFORE the gameplay
actions run: the fixer reacts to the decision, the player gets a last word.
Refusals in particular were silent before -- you clicked REFUSE and the panel
vanished, which is the least interesting possible outcome of saying no to a
crime boss.

Usage::

    python -m tools.cinematics.add_epilogues --dry-run
    python -m tools.cinematics.add_epilogues
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

F = ""      # fixer speaks
P = "pc"    # Grayson speaks

ARTIFACT = "props/steltek_artifact.png"

# ---------------------------------------------------------------------------
# prop shots: fixer id -> {dialogue index: prop png}
# Indices refer to the EXPANDED dialogue in expand_dialogue.py.
# ---------------------------------------------------------------------------
PROPS: dict[str, dict[int, str]] = {
    # From "I'll show you something better" through "It's warm." -- the
    # artifact holds the frame for the whole handoff, which is the moment the
    # entire campaign hangs on.
    "sandoval_offer": {17: ARTIFACT, 18: ARTIFACT, 19: ARTIFACT,
                       20: ARTIFACT, 21: ARTIFACT, 22: ARTIFACT},
    # Tayla explaining what he really handed over.
    "tayla_artifact_handoff": {16: ARTIFACT, 17: ARTIFACT, 18: ARTIFACT},
    # Monkhouse finally seeing it.
    "monkhouse_m17_offer": {17: ARTIFACT, 18: ARTIFACT, 19: ARTIFACT},
    # The two halves fitting together.
    "monkhouse_m17_debrief": {2: ARTIFACT, 3: ARTIFACT, 4: ARTIFACT},
    # The Oxford scanner reading it.
    "oxford_library_scene": {1: ARTIFACT, 2: ARTIFACT, 3: ARTIFACT,
                             5: ARTIFACT, 6: ARTIFACT},
}

# ---------------------------------------------------------------------------
# epilogues: fixer id -> {"accept": [...], "refuse": [...]}
# ---------------------------------------------------------------------------
EPILOGUES: dict[str, dict[str, list[tuple[str, str]]]] = {

"sandoval_offer": {
 "accept": [
  (F, "Good. Good! You won't regret it."),
  (P, "I regret most things eventually."),
  (F, "The cargo's already aboard. It was aboard before you sat down."),
  (P, "..."),
  (F, "I told you I'd rather pay for judgment. I didn't say I'd wait for it."),
  (P, "One day someone's going to shoot you, Sandoval."),
  (F, "Fly safe, Captain. Come back to me."),
 ],
 "refuse": [
  (F, "..."),
  (F, "I see. A pity."),
  (P, "Find someone else to hold your warm little secret."),
  (F, "There isn't anyone else. That's rather the difficulty."),
  (P, "Then your difficulty just got worse."),
  (F, "It was already worse than you know. Good evening, Captain."),
 ],
},

"tayla_m02_offer": {
 "accept": [
  (F, "Smart. The manifest's already filed."),
  (P, "Naturally. Consent is mostly paperwork to you."),
  (F, "I don't waste time on people who say no. I waste it on people who say "
      "yes slowly."),
  (P, "Here I am, talking dirty with a pirate girlboss instead of collecting "
      "fifteen thousand from a dead man. The universe is interesting."),
  (F, "Keep saying yes slowly and it may get more interesting. Don't scratch "
      "my plastics."),
 ],
 "refuse": [
  (F, "Suit yourself."),
  (P, "That's it? No pitch?"),
  (F, "You've got a dead man's trinket in your jacket and nobody left to sell "
      "it to. You'll be back."),
  (P, "..."),
  (F, "I'll still be here. That's the whole advantage of my line of work."),
 ],
},

"tayla_m05_offer": {
 "accept": [
  (F, "Last one. Then I pay out in full -- credits and story both."),
  (P, "I'll hold you to the second half."),
  (F, "You've earned it. Watch for Riordian on the way out."),
  (P, "You said he was just talking."),
  (F, "Men who just talk don't fuel their ships at three in the morning."),
 ],
 "refuse": [
  (F, "Now? One run from the end?"),
  (P, "You've had me smuggling for a man I never met and a reason I never got."),
  (F, "And I've kept you alive doing it."),
  (P, "..."),
  (F, "Fine. Walk. But you'll never learn what you're carrying, and it'll "
      "still be in your jacket."),
 ],
},

"lynch_m06_offer": {
 "accept": [
  (F, "Excellent. Miggs will see you to the door."),
  (P, "I can find a door."),
  (F, "Miggs enjoys the walk."),
  (P, "..."),
  (F, "One more thing, Captain. When Seelig reacts -- and he will react -- "
      "remember that I asked for words, not for what follows."),
  (P, "You're building an alibi."),
  (F, "I am building a habit of precision. Do come back."),
 ],
 "refuse": [
  (F, "Regrettably, there shall be no next time."),
  (P, "I'll live."),
  (F, "You will, briefly and expensively. No one else in this sector will "
      "read that hologram for you."),
  (P, "Then it stays unread."),
  (F, "..."),
  (F, "Miggs. Show the Captain out. Good day to you, $NM -- and trouble me no "
      "more."),
 ],
},

"lynch_m08_offer": {
 "accept": [
  (F, "Regis will be aboard within the hour."),
  (P, "He knows he's leaving?"),
  (F, "He knows a great many things. That is precisely the problem."),
  (P, "..."),
  (F, "Do not converse with him at length, Captain. He is family, and I am "
      "fond of him, and he is not a good man."),
  (P, "That's quite an endorsement from you."),
 ],
 "refuse": [
  (F, "You disappoint me."),
  (P, "You wanted a witness disappeared. That's not a taxi run."),
  (F, "It is precisely a taxi run. Your objection is to the passenger."),
  (P, "My objection is to the trial."),
  (F, "..."),
  (F, "Mark my words. You'll be back -- and my price will have risen."),
 ],
},

"masterson_m10_offer": {
 "accept": [
  (F, "Splendid. I shall log the first installment as pending."),
  (P, "Pending."),
  (F, "Nothing is credited until Mr. Toth is on the ground. The university "
      "does not pay for effort."),
  (P, "Nobody does."),
  (F, "Then we understand one another. Do hurry -- Retros are punctual."),
 ],
 "refuse": [
  (F, "As you wish. The archive remains closed."),
  (P, "There are other libraries."),
  (F, "There are. None of them have what you need, which is why you came to "
      "the one run by a man you dislike."),
  (P, "..."),
  (F, "You'll be back, sooner or later. They always are."),
 ],
},

"murphy_m14_offer": {
 "accept": [
  (F, "Good. I'll log you with the wing."),
  (P, "There's a wing?"),
  (F, "There's a list. The wing died last month."),
  (P, "..."),
  (F, "Don't look like that. Everyone on that list volunteered, same as you."),
  (P, "I'm being paid."),
  (F, "So was I, once. Go fly, $CS."),
 ],
 "refuse": [
  (F, "Fair enough. It's not your fight."),
  (P, "That's not what I said."),
  (F, "It's what walking away says. I stopped taking it personally about two "
      "hundred funerals ago."),
  (P, "..."),
  (F, "The offer stands until Bronte finishes us. After that it won't matter "
      "much either way."),
 ],
},

"monkhouse_m17_offer": {
 "accept": [
  (F, "Oh, thank heavens. Thank heavens."),
  (P, "Get your things, Doctor."),
  (F, "My things are two cases and thirty years of notes."),
  (P, "Then get one case."),
  (F, "..."),
  (F, "Young man. When we reach Basra and I put these two pieces together -- "
      "you should be prepared for the possibility that you will not like the "
      "answer."),
  (P, "I stopped expecting to like it a while ago."),
 ],
 "refuse": [
  (F, "You're LEAVING? After I told you what I have?"),
  (P, "You've told me a lot of things, Doctor. Most of them about yourself."),
  (F, "That is grossly unfair and largely accurate."),
  (P, "..."),
  (F, "Go, then. I shall be here -- there is nowhere else to be. Come back "
      "when curiosity beats caution. It always does."),
 ],
},

"cross_m19_offer": {
 "accept": [
  (F, "Thank you."),
  (P, "You don't have to do that."),
  (F, "I know. I'd like the record to show somebody said it before you went."),
  (P, "..."),
  (F, "Beta's four navs. Garrovick's last beacon was near the third. Bring "
      "back the disc either way."),
 ],
 "refuse": [
  (F, "I'll find someone else."),
  (P, "You said he was your best pilot."),
  (F, "He is. That's why I'm asking a stranger instead of my own people -- "
      "they'd fly it angry."),
  (P, "..."),
  (F, "Think about it. He's been out there three weeks and I'm running out of "
      "ways to say 'overdue' in a report."),
 ],
},

"terrell_offer": {
 "accept": [
  (F, "Good man."),
  (P, "I want it in writing that this was your idea."),
  (F, "Goodin will draft something. It'll be a lie, but it'll be filed."),
  (P, "..."),
  (F, "Blockade Point Tango, Nav 1. Get there, sit still, and let it come to "
      "you."),
  (P, "And if the fleet can't kill it?"),
  (F, "Then you'll have learned something no one else in this sector knows, "
      "and you'll have about four seconds to enjoy it."),
 ],
 "refuse": [
  (F, "I could order this, you understand."),
  (P, "I'm a civilian."),
  (F, "You're a civilian with a Confederation problem welded to his "
      "transponder."),
  (P, "..."),
  (F, "I won't force you. But that thing is going to find you eventually, and "
      "when it does you'll be alone instead of standing inside a fleet."),
  (P, "I'll take my chances."),
  (F, "Then take them. And when you change your mind -- Tango. Nav 1."),
 ],
},

"tayla_m03_offer": {
 "accept": [
  (F, "Good. It's loaded in the false floor, not the hold."),
  (P, "You loaded it before I said yes."),
  (F, "I loaded it before I asked. Burn past the militia and don't be clever."),
 ],
 "refuse": [
  (F, "Brilliance scares you."),
  (P, "Militia scanners scare me. There's a difference."),
  (F, "Not to a customs officer there isn't."),
  (P, "..."),
  (F, "Come back when you've decided which kind of pilot you are."),
 ],
},

"tayla_m04_offer": {
 "accept": [
  (F, "Twenty-five units, straight to the capital. You're moving up."),
  (P, "That's not the word I'd use."),
  (F, "It's the word your account balance would use. Go."),
 ],
 "refuse": [
  (F, "The bribes are already paid, $CS."),
  (P, "Then you're out the money."),
  (F, "I'm out the money either way. What I'm short of is a pilot."),
  (P, "..."),
  (F, "Find your nerve and come back. The route doesn't stay bought forever."),
 ],
},

"lynch_m07_offer": {
 "accept": [
  (F, "Splendid. The crates are aboard."),
  (P, "Of course they are."),
  (F, "Do give Mr. Kroiz my regards, should he insist on introducing himself."),
 ],
 "refuse": [
  (F, "You'll find your access to the investigation... severely impeded."),
  (P, "That's a threat."),
  (F, "That is a monopoly. I have the only people in this sector who can read "
      "your artifact, and they answer to me."),
  (P, "..."),
  (F, "Mark my words. You'll be back."),
 ],
},

"lynch_m09_offer": {
 "accept": [
  (F, "Good. Liverpool, then. Mr. Smythe knows your ship's registry."),
  (P, "He knows my registry."),
  (F, "I am thorough, Captain. It is why you are still alive."),
 ],
 "refuse": [
  (F, "After everything, you balk at a passenger."),
  (P, "After everything, I've stopped believing your job descriptions."),
  (F, "..."),
  (F, "Smythe has what you want. Not I -- him. Refuse me and the answer stays "
      "on a rock in Newcastle."),
  (P, "Then it stays there."),
  (F, "You'll be back, Captain. You always are."),
 ],
},

"masterson_m11_offer": {
 "accept": [
  (F, "Second installment, pending. Do find the wretched thing."),
  (P, "It's a converted Galaxy. How hidden can it be?"),
  (F, "It has been hidden for six weeks. Prove me wrong."),
 ],
 "refuse": [
  (F, "Then our arrangement stalls at one installment of four."),
  (P, "I noticed."),
  (F, "They are draining our mainframe as we speak. Every hour costs the "
      "university more than your fee."),
  (P, "..."),
  (F, "The offer remains. So, regrettably, does the lock on the archive."),
 ],
},

"masterson_m12_offer": {
 "accept": [
  (F, "Brave. Or mercenary. I have stopped distinguishing."),
  (P, "Both pay the same."),
  (F, "Indeed. The Forge lands first. Do try to as well."),
 ],
 "refuse": [
  (F, "You object to being the target."),
  (P, "I object to being told about it afterward. You told me first, so "
      "that's something."),
  (F, "Then object and accept, like a professional."),
  (P, "Not today."),
  (F, "..."),
  (F, "Three installments remain outstanding. Good day."),
 ],
},

"masterson_m13_offer": {
 "accept": [
  (F, "The last one. I confess I did not expect you to reach it."),
  (P, "Neither did I."),
  (F, "Bring her home and the archive is yours. My word, for whatever you "
      "judge it to be worth."),
 ],
 "refuse": [
  (F, "One favor from the archive and you walk away."),
  (P, "That freighter's plating would embarrass a shuttle. Your words."),
  (F, "My words, and still true. That is why I need someone competent."),
  (P, "..."),
  (F, "Three installments paid, one owed, and nothing to show. Think about "
      "that on your way out."),
 ],
},

"murphy_m15_offer": {
 "accept": [
  (F, "Good. Same field, worse company."),
  (P, "I'll manage."),
  (F, "They said that too, ace. Watch the Centurions -- they fly like they "
      "mean to retire."),
 ],
 "refuse": [
  (F, "Aces put you off?"),
  (P, "Ten thousand for player-grade hulls put me off."),
  (F, "I told you what's in the jar. I can't conjure more by being charming."),
  (P, "..."),
  (F, "Come back if the arithmetic improves. It won't, but come back anyway."),
 ],
},

"murphy_m16_offer": {
 "accept": [
  (F, "Then it's tonight. I'll tell the Talons."),
  (P, "Tell them to stay behind me."),
  (F, "I'll tell them. They won't listen -- it's their planet down there."),
 ],
 "refuse": [
  (F, "..."),
  (F, "We go anyway. With you or without you."),
  (P, "Two volunteers and no heavy support. That's not an assault, it's a "
      "funeral."),
  (F, "It's the only one we can afford."),
  (P, "..."),
  (F, "If you change your mind, we launch at the next shift change. After "
      "that there won't be anyone left to launch with."),
 ],
},

"cross_m18_offer": {
 "accept": [
  (F, "Welcome to Exploratory Services. Try not to make me regret the "
      "paperwork."),
  (P, "No promises."),
  (F, "Four navs, full sweep. Bring the disc back and we'll talk about Beta."),
 ],
 "refuse": [
  (F, "Second thoughts already?"),
  (P, "It's the first system on a map that's gotten everyone who held it "
      "killed."),
  (F, "..."),
  (F, "That's fair. It's also the only way to find out why."),
  (P, "..."),
  (F, "I'll keep the contract open. Frontier work doesn't attract a queue."),
 ],
},

"cross_m20_offer": {
 "accept": [
  (F, "Thank you. I'll flag the Kilrathi signatures to Fleet, for all the "
      "good it does."),
  (P, "They won't act on it."),
  (F, "They will not. Conserve your missiles for the last nav."),
 ],
 "refuse": [
  (F, "Kilrathi put you off."),
  (P, "A lot of Kilrathi put me off."),
  (F, "Then we never learn what broke Garrovick, and the next pilot who flies "
      "that route finds out the same way he did."),
  (P, "..."),
  (F, "That was unfair of me. The contract's open when you want it."),
 ],
},

"cross_m21_offer": {
 "accept": [
  (F, "Last one. Then this sector's charted and we can all go home."),
  (P, "You don't sound convinced."),
  (F, "I'm not. Survey it and come home, $CS. Don't touch anything."),
 ],
 "refuse": [
  (F, "One nav point. That's all that's left."),
  (P, "One nav point with something kilometers long sitting cold in it, on "
      "the heading that took your best pilot's mind."),
  (F, "..."),
  (F, "When you put it that way I'd refuse it myself."),
  (P, "..."),
  (F, "But somebody's going to fly it eventually. I'd rather it were someone "
      "who's read the file."),
 ],
},

"goodin_offer": {
 "accept": [
  (F, "Sensible. I'll signal ahead."),
  (P, "Do I get an escort?"),
  (F, "You get a heading and my personal assurance that nobody shoots you on "
      "approach."),
  (P, "That's not nothing."),
  (F, "Around here it's practically a gift. Perry Naval Base, pilot. Don't "
      "sightsee."),
 ],
 "refuse": [
  (F, "That's the wrong answer."),
  (P, "It's still an answer."),
  (F, "Nine arrays, pilot. Nine. You can outrun me -- you cannot outrun "
      "arithmetic."),
  (P, "..."),
  (F, "Perry Naval Base. I'll be here when you reconsider, and I will be less "
      "pleasant about it."),
 ],
},
}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    path = root / "assets" / "data" / "fixers.json"
    doc = json.loads(path.read_text())

    art = root / "assets" / "cinematics" / ARTIFACT
    if not art.is_file():
        print(f"[epilogues] WARNING: {ARTIFACT} missing -- prop shots skipped")

    n_prop = n_acc = n_ref = 0
    for entry in doc["fixers"]:
        fid = entry["id"]

        # ---- prop shots (sparse array, index-aligned with dialogue) --------
        shots = PROPS.get(fid)
        if shots and art.is_file():
            dlg = entry.get("dialogue", [])
            props = [""] * len(dlg)
            for i, png in shots.items():
                if i < len(props):
                    props[i] = png
                    n_prop += 1
            entry["prop"] = props

        # ---- post-decision exchanges --------------------------------------
        ep = EPILOGUES.get(fid)
        if ep:
            if ep.get("accept"):
                entry["accept_dialogue"] = [t for _, t in ep["accept"]]
                entry["accept_speaker"] = [s for s, _ in ep["accept"]]
                entry.pop("accept_voice", None)
                n_acc += len(ep["accept"])
            if ep.get("refuse"):
                entry["refuse_dialogue"] = [t for _, t in ep["refuse"]]
                entry["refuse_speaker"] = [s for s, _ in ep["refuse"]]
                entry.pop("refuse_voice", None)
                n_ref += len(ep["refuse"])

        if args.dry_run and (shots or ep):
            print(f"=== {fid} ===")
            if shots:
                print(f"  prop shots: {sorted(shots)}")
            for key in ("accept", "refuse"):
                for who, line in (ep or {}).get(key, []):
                    tag = "GRAYSON" if who == "pc" else "fixer  "
                    print(f"  [{key}][{tag}] {line}")

    if not args.dry_run:
        path.write_text(json.dumps(doc, indent=2) + "\n")

    verb = "would add" if args.dry_run else "added"
    print(f"[epilogues] {verb} {n_prop} prop shot(s), "
          f"{n_acc} accept line(s), {n_ref} refuse line(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
