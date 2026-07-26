"""Expand the fixer conversations into full two-hander scenes.

The authored conversations were compressed -- 2-7 paragraphs each, mostly the
fixer monologuing with an occasional Grayson interjection. Vanilla plays very
differently: short lines, fast back-and-forth, and the player pushing back
constantly (see S0MAC2, where Grayson questions Sandoval four times in fifteen
lines). Now that conversations auto-advance and every line is voiced in the
original cast's cloned voices, length is cheap and the compression is pure loss.

This REPLACES dialogue/speaker for each scene with a longer version written in
that vanilla register:

* short lines -- one thought each, not paragraph blocks
* Grayson answers roughly every other line and has a personality: dry, wary,
  asks about money and risk, does not enjoy being handled
* every canon fact from the earlier canon pass is preserved (the artifact is
  collateral, Tayla doles out provenance, Monkhouse was kidnapped, the
  Bronte/Rondell blockade, cousin Regis and the murder trial, the Hooded Hawk)
* mission-critical instructions (destination, cargo, payment) survive verbatim
  so the offer text still matches what was said

Usage::

    python -m tools.cinematics.expand_dialogue --dry-run
    python -m tools.cinematics.expand_dialogue
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

F = ""      # fixer speaks
P = "pc"    # Grayson speaks

# id -> [(speaker, line), ...]
SCENES: dict[str, list[tuple[str, str]]] = {

# ---------------------------------------------------------------- SANDOVAL --
"sandoval_offer": [
 (F, "Welcome to New Detroit. You look like a man who's hungry for work."),
 (P, "Depends on the work. And the man offering it."),
 (F, "Sandoval. Ernesto Sandoval. I deal in acquisitions."),
 (P, "That's a word people use when the real one sounds worse."),
 (F, "It's a word that keeps a man out of court. Sit down, Captain."),
 (P, "I'll stand. What's the run?"),
 (F, "Forty units of iron. Here to the Liverpool refinery, over in Newcastle."),
 (P, "Iron. That's it."),
 (F, "Strictly legit. No contraband, no hassles. Just a short jump from here."),
 (P, "If it's that clean, why are you buying me a drink instead of posting it?"),
 (F, "Because the posted board is full of men who ask fewer questions and lose "
     "more cargo. I'd rather pay for judgment."),
 (P, "Flattery's cheap. What's the pay?"),
 (F, "Fifteen thousand credits on your return."),
 (P, "On my return."),
 (F, "I don't have it on me tonight."),
 (P, "Then we don't have a deal tonight."),
 (F, "Wait. I'll show you something better than credits."),
 (P, "..."),
 (F, "Here. Hold this. Collateral, until I pay."),
 (P, "What is it?"),
 (F, "Don't ask me what it is."),
 (P, "It's warm."),
 (F, "Just keep it safe. And keep it quiet -- inside your jacket, not on your "
     "console."),
 (P, "You're handing a stranger something you won't name, and telling him to "
     "hide it. You see how that sounds."),
 (F, "I see how it sounds. Bring it back with you and there's another five "
     "thousand in it for you."),
 (P, "Twenty total, then. For hauling iron."),
 (F, "For hauling iron, and for having a short memory."),
 (P, "..."),
 (F, "Deliver at Liverpool. My people will meet the ship. Then come back here."),
 (P, "And if you're not here when I get back?"),
 (F, "I'll be here."),
],

# ------------------------------------------------------------------- TAYLA --
"tayla_artifact_handoff": [
 (P, "I'm looking for Sandoval."),
 (F, "Don't bother. He's dead."),
 (P, "..."),
 (F, "Two nights ago. Word is it wasn't an accident."),
 (P, "He owed me fifteen thousand credits."),
 (F, "He owed a lot of people a lot of things. You're near the back of that line."),
 (P, "Who are you?"),
 (F, "Tayla. A friend to pilots -- if they survive long enough."),
 (P, "And you tracked me down why?"),
 (F, "Because Sandoval handed you a trinket before he died, and now the people "
     "he owed are very curious about where it went."),
 (P, "It's collateral. It's his."),
 (F, "It's yours. He's in no position to redeem it, and his fifteen grand died "
     "with him. Keep it, like it or not."),
 (P, "I'd rather hand it back and walk away clean."),
 (F, "Hand it back to who? There's no one left to hand it to -- that's the "
     "point. You're holding it now. That's the whole story."),
 (P, "Then tell me what I'm holding."),
 (F, "I'll tell you this much for free. Sandoval didn't buy that thing."),
 (P, "..."),
 (F, "He got it by killing the man who owned it."),
 (P, "You're telling me I'm carrying a murder motive."),
 (F, "I'm telling you people have died for that thing, flyboy. Keep it close."),
 (P, "Not particularly."),
 (F, "Good. Lucky men die out here first."),
 (P, "You know more than you're saying."),
 (F, "Of course I do. That's the only thing I sell."),
 (P, "What's the price?"),
 (F, "Work for me, $NM. I'll trade the rest out a piece at a time, and you'll earn every one."),
 (P, "That's a long way of saying you own me."),
 (F, "It's a long way of saying you're the only one I trust with it. Come back when you're ready."),
],
"tayla_m02_offer": [
 (F, "You've been staring at the artifact an hour. Point on the cargo -- what are you thinking?"),
 (P, "I'm thinking you sold me this job before you owned it."),
 (F, "Sweetheart, I don't sell to strangers. But I have an eye for pilots, and you came back."),
 (P, "Then I'm thinking the 'milk run' line would insult both of us."),
 (F, "First honest statement you've made. Ready to work?"),
 (P, "Nothing about the last week has been a milk run."),
 (F, "Thirty units of plastics. Completely legal, nothing to hide."),
 (P, "Then why are you smiling?"),
 (F, "Because officially you're running it to Newcastle."),
 (P, "And actually?"),
 (F, "Actually you'll divert to Oakham. Hidden pirate base out in Pentonville."),
 (P, "A falsified manifest. That's a jump up from hauling iron."),
 (F, "The paperwork is for anyone watching. The cargo really is plastics -- if "
     "they board you, you're clean."),
 (P, "And if they check the flight recorder?"),
 (F, "Then you're a pilot who got lost. It happens."),
 (P, "..."),
 (F, "Oakham sits in an asteroid field. Mind your speed on approach -- I've "
     "lost two pilots to those rocks and none to guns."),
 (P, "The pirates don't shoot?"),
 (F, "While you fly for me, Pentonville's pirates keep their guns to "
     "themselves. That's what you're really buying."),
 (P, "Ten thousand?"),
 (F, "Ten thousand on delivery. Leave the ship docked and meet me in the "
     "Oakham bar. We'll talk more there."),
 (P, "More about the artifact."),
 (F, "More about whatever you've earned by then."),
],
"tayla_m03_offer": [
 (F, "Now the real work."),
 (P, "The milk run's over, then."),
 (F, "Brilliance. Yes -- that Brilliance. To Hector, the mining base in Troy."),
 (P, "That's not a manifest problem. That's a prison sentence."),
 (F, "Fifteen units. It'll fit where nobody looks."),
 (P, "Troy's crawling with militia."),
 (F, "It is. Talons, and they scan everything that moves."),
 (P, "You're sending me through a checkpoint with contraband in the hold."),
 (F, "I'm sending you through it fast. Do not stop. Do not fight. Burn past "
     "them and dock."),
 (P, "And if they light me up?"),
 (F, "Then you run, and you don't lead them back here."),
 (P, "Charming."),
 (F, "Fifteen thousand on delivery at Hector. Then come back here in "
     "one piece, hotshot."),
 (P, "And another piece of the story."),
 (F, "You're earning more every run, love. Even when you don't notice."),
],
"tayla_m03_debrief": [
 (F, "You made it past the militia with the goods and your hull."),
 (P, "It was close at the second nav point."),
 (F, "I'm almost impressed."),
 (P, "Almost is bad. Almost is bait."),
 (F, "Earned, then. Catch your breath, $NM. The next run makes Troy look like a "
     "pleasure cruise."),
 (P, "You said that about the last one."),
 (F, "And I was wrong. This time I'm not."),
],
"tayla_m04_offer": [
 (F, "Bigger load this time, flyboy. Brilliance to New Constantinople itself."),
 (P, "The capital. You're not serious."),
 (F, "Twenty-five units."),
 (P, "That's Confed's front porch, Tayla."),
 (F, "Before you say no -- I have friends on the route. They owe me."),
 (P, "You bribed them."),
 (F, "Generously."),
 (P, "Bribed patrols. You'll forgive me if I keep my guns hot."),
 (F, "Trust me, $NM. I'll know inside an hour if the route wasn't clean."),
 (P, "Every time you say that it costs me hull."),
 (F, "Twenty thousand on delivery. Then come straight back."),
],
"tayla_m04_debrief": [
 (P, "Bribed patrols, you said."),
 (F, "I did say that."),
 (P, "Stilettos and Broadswords at every jump point."),
 (F, "Huh. Must have been a clerical error."),
 (P, "I lost half my shields to a clerical error."),
 (F, "Stop scowling. To make it up to you, my people just installed something "
     "in your ship. While you were docked."),
 (P, "You went into my ship."),
 (F, "A smuggler's compartment. Twenty units, invisible to any scanner ever "
     "built."),
 (P, "So that's what the noise was."),
 (F, "Consider it a promotion, flyboy. The contractors I trust do that on the side."),
 (P, "..."),
 (F, "You'd rather have the apology?"),
 (P, "I'd rather have been asked."),
 (F, "I'd rather have known you a year ago, $NM. One more run, and we're square."),
],
"tayla_m05_offer": [
 (P, "Let me guess... I'm running a shipment of catnip to Kilrah."),
 (F, "If only, hotshot. Last run. Twenty units of Brilliance to New Constantinople."),
 (P, "Fits the new compartment exactly."),
 (F, "Poetic, no?"),
 (P, "Convenient. There's a difference."),
 (F, "One thing before you go. William Riordian."),
 (P, "Should that name mean something?"),
 (F, "He flew these runs before you showed up. Then he let a few friends "
     "talk him out of his nerve."),
 (P, "And now he's behind me."),
 (F, "And now he's telling strangers what your docking pattern looks like. "
     "Your compartment stays closed, love. The contractors know that."),
 (F, "Ten thousand. After this I'll tell you everything I know about that "
     "trinket of yours."),
 (P, "Everything, please."),
 (F, "Everything I have, partner. I'm asking -- not telling, asking -- for "
     "you to come back in one piece."),
],
"tayla_m05_debrief": [
 (F, "Riordian, hm?"),
 (P, "He found me at the jump point. He wasn't alone."),
 (F, "He always was a jealous idiot. You did fine."),
 (P, "You owe me a story, Tayla."),
 (F, "Here's the rest of what I owe you, first."),
 (P, "..."),
 (F, "Before Sandoval, it belonged to a spice merchant named Deiter."),
 (P, "And before him?"),
 (F, "Deiter had it from his own father. Every owner I can name died holding it."),
 (P, "That's a pattern, not a story."),
 (F, "That's where my thread runs out. I've sent the holo on to someone who "
     "can actually read it."),
 (P, "Who?"),
 (F, "Roman Lynch. New Constantinople. A thug -- but an expert on exotic and "
     "valuable things."),
 (P, "A thug."),
 (F, "The polite kind. He's expecting you in the bar there."),
 (P, "And that squares us."),
 (F, "That squares us. It's been profitable, partner. Try not to die -- "
     "and come back when you've thought about what 'partner' means."),
],

# ------------------------------------------------------------------- LYNCH --
"lynch_m06_offer": [
 ("miggs", "You don't wanna talk to me, 'cause I don't wanna talk to you."),
 ("miggs", "And anyone that makes me do what I don't wanna do gets hurt, "
            "painwise, get me?"),
 (P, "I'm here to see Lynch."),
 ("miggs", "Mr. Lynch, sitting over there, HE'S the one you wanna talk to..."),
 ("miggs", "...so either state your bidness or take a hike, buddy."),
 (F, "Enough, Miggs."),
 (F, "Ah, Captain. I've been expecting you. I am Roman Lynch."),
 (P, "Tayla said you'd see me."),
 (F, "You may speak freely around my assistant. He is exceedingly loyal."),
 (P, "He's exceedingly something."),
 (F, "Your artifact interests me, $NM."),
 (P, "Everyone finds it interesting. Nobody will tell me what it is."),
 (F, "There is a hologram inside it. Did you know?"),
 (P, "A hologram. And you'd know that how?"),
 (F, "Because Tayla sent me an image, and my people are thorough. It is a map, "
     "or something that behaves like one."),
 (P, "A map to what?"),
 (F, "That is precisely the question I intend to answer -- while you make "
     "yourself useful."),
 (P, "There it is."),
 (F, "A certain Captain Seelig is loitering at Nav 3 in Pentonville. His ship "
     "is the Hooded Hawk."),
 (P, "And?"),
 (F, "Deliver a message. Tell him how profoundly disappointed I am in him. "
     "Verbatim, please."),
 (P, "And if I say no? Miggs alphabetizes my bones?"),
 ("miggs", "Too many little pieces. Maybe I just throw you out the airlock."),
 (F, "Miggs dislikes clerical work."),
 (P, "That's all. Words."),
 (F, "Words, Captain. I am a businessman."),
 (P, "Businessmen don't need couriers for a comm channel."),
 (F, "Some messages lose their meaning over a relay. Ten thousand when you get "
     "back."),
 (P, "And the artifact?"),
 (F, "Stays with you. I want it studied, not stolen -- and frankly it is safer "
     "in a ship than in my safe."),
 (P, "A simple courier run."),
 (F, "What could go wrong?"),
],
"lynch_m06_debrief": [
 (F, "Ah. You're breathing. How did Captain Seelig take my chastisement?"),
 (P, "He opened fire before I finished the sentence."),
 (F, "Took it badly? Shocking."),
 (P, "You knew he would."),
 (F, "I suspected. Some men simply cannot accept a severance package."),
 (P, "You used me as a trigger, Lynch."),
 (F, "I used you as a messenger. What he did with the message was his choice."),
 (P, "..."),
 (F, "Your ten thousand, as agreed."),
 (P, "And the artifact?"),
 (F, "The hologram is resisting analysis. My people need more time."),
 (P, "How much time?"),
 (F, "As much as I am willing to buy. And I have more work."),
],
"lynch_m07_offer": [
 (F, "The artifact investigation has hit complications."),
 (P, "Meaning what?"),
 (F, "People keep dying around it. Fascinating, really."),
 (P, "You say that like it's weather."),
 (F, "At my age one becomes philosophical about other people's mortality."),
 (P, "Which people?"),
 (F, "Two analysts and a records clerk. All quite unrelated, I'm sure."),
 (P, "..."),
 (F, "Meanwhile: twenty units of weaponry to Siva, the agricultural base in "
     "Rikel. Fifteen thousand on delivery."),
 (P, "So I smuggle guns onto a farm, irritate a rival mobster, and pretend this "
     "is artifact research."),
 (F, "At last, you're learning interdisciplinary work."),
 ("miggs", "Keep smiling. I can fix that."),
 (P, "Does he come with the weapons, or is the threat package complimentary?"),
 (F, "Complimentary."),
 (P, "Weapons to a farming world."),
 (F, "Farmers have enemies too."),
 (P, "There's a wrinkle. There's always a wrinkle."),
 (F, "Salman Kroiz runs guns on that route and considers it his."),
 (P, "And he'll object."),
 (F, "He flies a Demon. So do his friends. Handle it appropriately."),
],
"lynch_m08_offer": [
 (F, "Your trinket is confirmed alien."),
 (P, "..."),
 (F, "Not Kilrathi. Old. Older than the word old is usually asked to carry."),
 (P, "How old?"),
 (F, "My analysts refused to write a number down. That knowledge cost me three "
     "of them."),
 (P, "Cost them what?"),
 (F, "Their positions, Captain. What did you think I meant?"),
 (P, "..."),
 (F, "Before we discuss it further, I need a favor."),
 (P, "Of course you do."),
 (F, "My cousin Regis has been subpoenaed in a murder trial."),
 (P, "Witness relocation. Very civic-minded."),
 (F, "My family has always believed in public service."),
 (P, "Mostly the part where the public serves your family."),
 ("miggs", "Maybe I throw you out the airlock now and save us all some time."),
 (P, "Whose murder?"),
 (F, "A tedious man's. It would be best for everyone if Regis simply "
     "disappeared before he testifies."),
 (P, "You want me to disappear a witness."),
 (F, "I want you to give a relative a ride. The distinction matters to lawyers, "
     "so it should matter to you."),
 (P, "Where?"),
 (F, "He favors the Romulus mining base in Castor. Get him out of New "
     "Constantinople and take him there."),
 (P, "Thirty thousand?"),
 (F, "Thirty thousand on landing. And do avoid the Stilettos -- they are "
     "faster than you."),
],
"lynch_m09_offer": [
 (F, "One last job, Captain."),
 (P, "You've said that before."),
 (F, "This time I mean it, because this time it ends the question."),
 (P, "Go on."),
 (F, "A Mr. Smythe on Liverpool claims he found something about your artifact "
     "in the Oxford library archives."),
 (P, "Then have him transmit it."),
 (F, "He will not put it on a relay. He was quite insistent about that."),
 (P, "..."),
 (F, "Pick him up. Bring him here. Thirty thousand for a taxi run."),
 (P, "Smythe retrieval. Which crime am I committing this time?"),
 (F, "Rescue, Captain. Try to keep up."),
 (P, "Kidnapping with better branding. Got it."),
 (F, "You may notice Miggs is elsewhere tonight. Errands of his own."),
 (P, "Should that worry me?"),
 (F, "It should worry someone. Go on then. Liverpool. Newcastle system."),
 (P, "And Smythe is waiting."),
 (F, "Mr. Smythe is waiting."),
],

# --------------------------------------------------------------- MASTERSON --
"masterson_m10_offer": [
 (P, "I need access to the Oxford archive."),
 (F, "Ah. The pilot with the artifact and the trail of dead mobsters."),
 (P, "News travels."),
 (F, "Masterson. University administration. And no -- the library is CLOSED "
     "to you."),
 (P, "I can pay."),
 (F, "Everyone can pay. That is what makes payment uninteresting."),
 (P, "Then what's interesting?"),
 (F, "Service. The university needs work done, and I am nothing if not "
     "transactional."),
 (P, "As an academic, isn't the pursuit of knowledge supposed to be its own "
     "reward?"),
 (F, "The pursuit, certainly. Access to the results is billed separately."),
 (P, "How much work?"),
 (F, "Access is an endowment, paid in four installments. Complete them and I "
     "unlock the archive myself."),
 (P, "Four favors for a library card. That's quite a fee."),
 (F, "It is quite a library."),
 (P, "..."),
 (F, "First installment. Hunter Toth is inbound in a Drayman at the Saxtogue "
     "jump point."),
 (P, "Who is he?"),
 (F, "A journalist. He wrote unkind truths about the Retros, and Retros hold "
     "grudges with unusual sincerity."),
 (P, "So they'll be waiting for him."),
 (F, "Meet him there and see him to the planet. He must be ON THE GROUND "
     "before you land."),
 (P, "Before I land."),
 (F, "I will not pay for a corpse with an honor guard. Ten thousand, and one "
     "installment struck from your debt."),
],
"masterson_m11_offer": [
 (F, "Favor two."),
 (P, "Installment two, I thought."),
 (F, "Don't be smart, it doesn't suit pilots. Data pirates are draining our "
     "mainframe."),
 (P, "From where?"),
 (F, "A ship parked somewhere in-system. The Black Rhombus -- a converted "
     "Galaxy, bristling with turrets."),
 (P, "Somewhere in-system is not a location."),
 (F, "Then patrol the jump points until it becomes one, and make it stop "
     "existing."),
 (P, "Oxford's answer to stolen knowledge is to shoot the thieves?"),
 (F, "We attempted a strongly worded citation. They deleted it."),
 (P, "Peer review has gotten rough."),
 (F, "Standards have declined."),
 (P, "The escorts?"),
 (F, "Optional. The Rhombus is not."),
 (P, "Ten thousand?"),
 (F, "Ten thousand on your return. Do check your ammunition first -- I have "
     "seen your invoices."),
],
"masterson_m12_offer": [
 (F, "Favor three, and this one is genuinely unpleasant."),
 (P, "You've been saving it."),
 (F, "My book shipment arrives on the Drayman Vulcan's Forge at the Saxtogue "
     "jump point."),
 (P, "Books again."),
 (F, "A rival collector hired bounty hunters to divert it."),
 (P, "Then I fly escort. Same as Toth."),
 (F, "Not the same. Here is the wrinkle: the hunters were paid to remove the "
     "ESCORT."),
 (P, "..."),
 (F, "That would be you."),
 (P, "Rare books, hired guns, and me as bait. Academia's more exciting than "
     "the brochures."),
 (F, "The brochures omit donor relations."),
 (P, "So the freighter is bait, and I'm the target."),
 (F, "The Demons will ignore the freighter entirely. I thought you would "
     "prefer to know."),
 (P, "I'd prefer a different job."),
 (F, "Same terms. The Forge lands first, then you. Ten thousand."),
],
"masterson_m13_offer": [
 (F, "Last favor."),
 (P, "Then the archive opens."),
 (F, "Then the archive opens. One more Drayman, inbound at the XXN-1927 jump "
     "point."),
 (P, "Carrying?"),
 (F, "Let us say pirate bait. They know what is in the hold and they want it "
     "badly."),
 (P, "That's not an answer."),
 (F, "It is the only one you're getting, and I will be honest with you about "
     "the rest."),
 (P, "Go ahead."),
 (F, "This freighter is a rust bucket. Her plating would embarrass a shuttle."),
 (P, "Wonderful."),
 (F, "Keep the Talons OFF her. She lands first. Then you."),
 (P, "And then?"),
 (F, "Then the library opens. Ten thousand, and my genuine respect -- which I "
     "assure you is rarer."),
 (P, "When this is done, I want a library card and a plaque."),
 (F, "The card is possible."),
 (P, "Good. I'd hate for recognition to cheapen the pursuit of knowledge."),
],
"oxford_library_scene": [
 (F, "The archive terminal accepts Masterson's authorization."),
 (F, "You place the artifact in the scanner cradle. The cradle hums."),
 (F, "MATCH FOUND. Classification: STELTEK."),
 (F, "A precursor civilization. Starfaring while humanity was learning fire."),
 (F, "No verified contact in recorded history. No recovered vessels. No "
     "remains."),
 (F, "The artifact is a power relay of unknown function."),
 (F, "Energy signature: dormant, but not dead."),
 (F, "Cross-reference: recent anomalous Steltek-band readings reported near "
     "PALAN."),
 (F, "Appended note. Dr. Monkhouse, the sector's leading xenoarchaeologist, "
     "has been asking the same questions you are."),
 (F, "Last known location: the Palan system."),
],

# ------------------------------------------------------------------ MURPHY --
"murphy_m14_offer": [
 (F, "You tried to land on Palan, didn't you?"),
 (P, "I got as far as the orbital line."),
 (F, "Bounty hunters have the planet sewn up tight. The name's Murphy -- only "
     "my close friends call me Lynn."),
 (P, "Understood."),
 (F, "Two corporations here. Rondell and Bronte."),
 (P, "And the hunters?"),
 (F, "Bronte hired them. Choke off Rondell's food exports, ship their own in "
     "to fill the gap, take the market."),
 (P, "People are starving so a corporation can move a decimal point."),
 (F, "Now you understand Palan."),
 (P, "And you're what, the resistance?"),
 (F, "I run it for Rondell out of this refinery. Hired resistance -- I won't "
     "pretend otherwise."),
 (P, "At least you're honest about it."),
 (F, "Honesty's free. Everything else out here costs."),
 (P, "So what do you need?"),
 (F, "The blockade rotates fresh ships through the asteroid field. Cut the "
     "reinforcements and the whole thing starves."),
 (P, "How many?"),
 (F, "Three waves of Demons. Kill every last one and come back."),
 (P, "And the pay?"),
 (F, "Fifteen thousand. Most of what we have -- so try to be worth it."),
],
"murphy_m14_debrief": [
 (F, "Their whole relief wing, gone."),
 (P, "They fought like they expected to win."),
 (F, "They usually do. The hunters are already flying tighter rotations to "
     "cover the gap."),
 (P, "So it worked."),
 (F, "It worked. Here's your fifteen thousand."),
 (P, "..."),
 (F, "Don't spend it anywhere the Guild can see you. You're on Bronte's "
     "payroll list now, in the worst way."),
 (P, "And hunters hold grudges."),
 (F, "Hunters hold grudges."),
],
"murphy_m15_offer": [
 (F, "Bronte's money bought better help."),
 (P, "How much better?"),
 (F, "Ace pilots in Centurions now. Not the rabble you scattered last time."),
 (P, "Centurions. That's my hull class."),
 (F, "That's the point. They stopped sending people who lose."),
 (P, "Same job?"),
 (F, "Same job, harder targets. Ten thousand."),
 (P, "That's less than last time."),
 (F, "It's what's left in the jar. I can show you the jar if you like."),
 (P, "..."),
 (F, "That's what I thought."),
],
"murphy_m15_debrief": [
 (F, "Centurion aces, and you're still breathing."),
 (P, "Two of them nearly weren't a problem I got to solve."),
 (F, "I'd hire you permanently if we had a treasury."),
 (P, "You'd hire me permanently if I were cheaper."),
 (F, "Both things can be true. Ten thousand."),
 (P, "And after this?"),
 (F, "One more push and Palan breathes free air."),
],
"murphy_m16_offer": [
 (F, "This is it."),
 (P, "The last push."),
 (F, "Our attacks finally wore them down. They're low on fuel, food and "
     "patience."),
 (P, "Where are they?"),
 (F, "What's left is parked in orbit over Palan itself. Break it, land on the "
     "planet, and it's done."),
 (P, "Alone?"),
 (F, "Not this time. Two militia Talons launch with you."),
 (P, "Pilots or volunteers?"),
 (F, "Volunteers. They'll fight -- I won't promise more than that."),
 (P, "..."),
 (F, "Say what you're thinking."),
 (P, "I'm thinking they should stay home."),
 (F, "It's their planet. Fifteen thousand waiting on Palan."),
 (P, "And the bill afterward?"),
 (F, "You've killed a lot of bounty hunters. Militia and Confed listen to "
     "hunter gossip, and none of them will forget your transponder."),
],

# --------------------------------------------------------------- MONKHOUSE --
"monkhouse_m17_offer": [
 (P, "Doctor Monkhouse?"),
 (F, "Don't speak to me about extraterrestrial artifacts. I'm sick of them!"),
 (P, "I haven't said a word about one."),
 (F, "You have the look. I nearly got killed on this rock because of my work."),
 (P, "..."),
 (F, "Lemuel Monkhouse. Xenoarchaeology. Formerly of several institutions that "
     "no longer return my calls."),
 (P, "How did you end up on Palan?"),
 (F, "I didn't COME to Palan, young man. I was brought."),
 (P, "Brought."),
 (F, "Kidnapped. By men who wanted my Steltek fragment."),
 (P, "Kidnapped. And they're still looking for you?"),
 (F, "They are buried under their own interrogation compound. The bombing had "
     "one merciful outcome."),
 (P, "..."),
 (F, "You may take a moment. Everyone does."),
 (P, "You said fragment. You have a piece of one."),
 (F, "Thirty years I've given the Steltek. Show me your piece and I'll tell "
     "you what it is."),
 (P, "Here."),
 (F, "..."),
 (F, "Oh. Oh, my."),
 (P, "Doctor?"),
 (F, "My instruments are at the Basra refinery, and I detest cramped quarters. "
     "Fly me there."),
 (P, "That's it? A lift?"),
 (F, "Five thousand credits toward your expenses, and every answer I have."),
 (P, "Then why do you look nervous?"),
 (F, "Because someone else has been asking about me. Someone with FUR."),
 (P, "Fur. You're saying Kilrathi."),
 (F, "I am saying we should leave promptly."),
],
"monkhouse_m17_debrief": [
 (F, "Extraordinary. EXTRAORDINARY."),
 (P, "You've been at that bench for six hours."),
 (F, "My fragment -- look. Look here. It FITS yours."),
 (P, "..."),
 (F, "They were one device, split millennia ago. Deliberately, I'd wager."),
 (P, "Why split it?"),
 (F, "Because whoever did it wanted it hard to use. Together they form a map, "
     "and something like a translation aid."),
 (P, "A map to where?"),
 (F, "The marked route runs past Rygannon, out into the unexplored frontier."),
 (P, "That's a long way to fly on a hunch."),
 (F, "It is not a hunch, it is a heading. And I have a proposal."),
 (P, "Go on."),
 (F, "Join Exploratory Services at Rygannon. They run the only jump-capable "
     "survey net out there."),
 (P, "And you get?"),
 (F, "I publish the findings. You keep whatever you find. Ask for Taryn Cross."),
 (P, "Whatever I find."),
 (F, "Young man, if I am right about what is out there, you will not want to "
     "keep it either."),
],

# ------------------------------------------------------------------- CROSS --
"cross_m18_offer": [
 (F, "Taryn Cross, Exploratory Services. Monkhouse's wire said you'd come."),
 (P, "He tends to arrange things without asking."),
 (F, "He does. Sign here -- you're a survey contractor now."),
 (P, "That fast?"),
 (F, "Out here paperwork is the only thing that moves fast. First assignment: "
     "the Delta system."),
 (P, "..."),
 (F, "Four nav points, full sensor sweep at each."),
 (P, "And what a coincidence -- Delta's the first system on my map."),
 (F, "I noticed that too. I decided not to find it suspicious."),
 (P, "Generous of you."),
 (F, "Fair warning: the frontier pirates don't care for company."),
 (P, "They never do. Pay?"),
 (F, "Ten thousand on completion. And bring the survey disc back intact -- "
     "the data's worth more than the ship."),
],
"cross_m18_debrief": [
 (F, "Telemetry checks out. Four clean sweeps."),
 (P, "There were pirates at the third point."),
 (F, "They're already in my report as local color."),
 (P, "Is that what we're calling them."),
 (F, "It's what the brass calls anything they don't want to fund a response "
     "to. Ten thousand, as agreed."),
 (P, "And next?"),
 (F, "Rest up. Beta is next, and Beta has a problem."),
],
"cross_m19_offer": [
 (F, "Survey Beta. Same pattern as Delta."),
 (P, "You said Beta has a problem."),
 (F, "Captain Garrovick took a Centurion out that way three weeks ago."),
 (P, "And never reported back."),
 (F, "Never reported back."),
 (P, "Missing three weeks. You want a survey or a search party?"),
 (F, "I want both, and I can only fund one."),
 (P, "..."),
 (F, "Garrovick is -- was -- our best pilot. I've been careful with that "
     "tense for two weeks."),
 (P, "I'll look for him."),
 (F, "Ten thousand, and Services covers the recovery bonus if he's alive."),
],
"cross_m19_debrief": [
 (F, "Well? Did you find him?"),
 (P, "You should sit down."),
 (F, "..."),
 (P, "Here's the gun-camera footage."),
 (F, "That's Garrovick's ship. That's his transponder."),
 (P, "He fired first."),
 (F, "He fired FIRST?"),
 (P, "He was raving on an open channel. I don't think he knew what he was "
     "shooting at."),
 (F, "What in the void does that to a man's mind?"),
 (P, "I've been asking myself that the whole way back."),
 (F, "You did what you had to. Ten thousand."),
 (P, "I don't want it."),
 (F, "Take it anyway. Something out there broke him, pilot -- and Gamma is "
     "deeper in."),
],
"cross_m20_offer": [
 (F, "Whatever unmade Garrovick is past Beta."),
 (P, "You've decided that's a fact."),
 (F, "I've decided it's the only lead. Survey Gamma -- four navs, full sweep."),
 (P, "And find out what."),
 (F, "And find out what. There's more."),
 (P, "There always is."),
 (F, "Intelligence flagged Kilrathi drive signatures in the area. A lot of "
     "them."),
 (P, "A corvette guarding an empty system."),
 (F, "We don't know that yet. That's the job."),
 (P, "..."),
 (F, "Conserve your missiles for the last nav. Ten thousand."),
],
"cross_m20_debrief": [
 (F, "Did you complete the run?"),
 (P, "All four. And I found your Kilrathi."),
 (F, "How many?"),
 (P, "A Kamekh. They posted a CORVETTE to a system with nothing in it."),
 (F, "..."),
 (F, "The cats aren't surveying, pilot. They're guarding the road."),
 (P, "The road to what?"),
 (F, "One system left on your map. Whatever everyone is so interested in "
     "lives there."),
 (P, "Delta Prime."),
 (F, "Ten thousand. Go get some sleep first -- that's an order from your "
     "employer."),
],
"cross_m21_offer": [
 (F, "Last one. Delta Prime."),
 (P, "And then the map's finished."),
 (F, "Long-range scopes show a single anomalous return."),
 (P, "Anomalous how?"),
 (F, "Metallic. Kilometers long. Cold as the void."),
 (P, "Kilometers."),
 (F, "I had the figure checked three times. Survey it and come home."),
 (P, "Ten thousand?"),
 (F, "Ten thousand. And pilot -- one more thing."),
 (P, "Go ahead."),
 (F, "Garrovick's last logged course was Delta Prime."),
 (P, "..."),
 (F, "Whatever you find out there -- do not touch it."),
],

# ------------------------------------------------------- GOODIN / TERRELL --
"goodin_offer": [
 (F, "Sandra Goodin, attache to Admiral Terrell, Confederation Navy."),
 (P, "I'm just passing through."),
 (F, "Sit down, pilot. That was not a request."),
 (P, "..."),
 (F, "Every long-range array in the sector is tracking a green energy "
     "signature."),
 (P, "And?"),
 (F, "Every track terminates on YOUR transponder."),
 (P, "That's not possible."),
 (F, "It's on nine separate arrays. Argue with them, not with me."),
 (P, "Am I under arrest?"),
 (F, "If you were, we'd be having this conversation somewhere with worse "
     "lighting."),
 (P, "The Admiral wants a word. Do I get a choice?"),
 (F, "He commands this sector from Perry Naval Base. Get there. Alive, "
     "preferably -- the paperwork is shorter."),
],
"terrell_offer": [
 (F, "So YOU'RE the privateer who dragged a Kilrathi secret weapon across half "
     "my sector."),
 (P, "It isn't Kilrathi."),
 (F, "Don't argue -- what else could it be?"),
 (P, "Whatever it is, it's older than that. A lot older."),
 (F, "It's green, it's hostile, and it doesn't die. That's Kilrathi "
     "engineering to the rivet."),
 (P, "It's Steltek. There's a library record and a xenoarchaeologist who'll "
     "swear to it."),
 (F, "I have read your file, and I have read his. I'm not interested in "
     "archaeology, I'm interested in the fact that it follows you."),
 (P, "..."),
 (F, "You don't deny that part."),
 (P, "No. I don't."),
 (F, "Good. Then we can do business. Commodore Reismann has assembled a fleet "
     "at Blockade Point Tango."),
 (P, "How many?"),
 (F, "Two Paradigms, two Broadswords. Far enough out to keep civilian "
     "casualties down."),
 (P, "So I'm bait. Say it plainly, Admiral."),
 (F, "Your job is simple, privateer. The thing follows you -- so LEAD it to "
     "Tango and let the navy do what the navy does."),
 (P, "And if the navy doesn't do it?"),
 (F, "Then you'll have died somewhere useful, which is more than most people "
     "manage. Thirty thousand credits when it's confirmed destroyed."),
],
"terrell_debrief": [
 (F, "Reismann's after-action report is... creative."),
 (P, "I'll bet."),
 (F, "It says the fleet provided a decisive containment perimeter."),
 (P, "The fleet was scrap in ninety seconds."),
 (F, "It also says a lone privateer did all the shooting. With an alien gun. "
     "That my analysts insist cannot exist."),
 (P, "It exists. It's bolted to my ship."),
 (F, "I know it exists, son. I'm deciding what the record says it was."),
 (P, "..."),
 (F, "You're an insubordinate smart-ass and one of the best pilots I've ever "
     "seen. Say the word and an officer's commission is yours."),
 (P, "I've spent this whole run getting out from under people who owned me."),
 (F, "That's a no, then."),
 (P, "That's a no."),
 (F, "Then you're getting the Confederation Medal of Freedom whether you like "
     "it or not."),
 (P, "And officially?"),
 (F, "Officially, a Kilrathi prototype was destroyed by naval action at "
     "Blockade Point Tango."),
 (P, "Unofficially?"),
 (F, "Unofficially -- here's your thirty thousand. I'm sure you're headed for "
     "trouble, and I'm sure you probably deserve it."),
 (P, "Admiral."),
 (F, "Good luck anyway, pilot."),
],
"terrell_epilogue": [
 (P, "Admiral."),
 (F, "Still here? Fine."),
 (P, "I want to know what the official history will say."),
 (F, "Of course you do. Goodin drafted three versions."),
 (P, "Three."),
 (F, "One credits the fleet. One credits classified assets."),
 (P, "And the third?"),
 (F, "Her personal favorite. It credits an unnamed civilian contractor with an "
     "unregistered antique."),
 (P, "..."),
 (F, "I signed the first one."),
 (P, "Naturally."),
 (F, "The Confederation does not lose to a drone, privateer. And it CERTAINLY "
     "does not get rescued from one by a freelancer behind on his ship "
     "payments."),
 (P, "So none of it happened."),
 (F, "You happened. There just isn't a form for it."),
 (P, "..."),
 (F, "Now get out of my office. And -- good hunting."),
],
}

PC_PORTRAIT = "portraits/grayson/_ref.png"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    path = root / "assets" / "data" / "fixers.json"
    doc = json.loads(path.read_text())

    before = after = touched = 0
    for entry in doc["fixers"]:
        scene = SCENES.get(entry["id"])
        if not scene:
            continue
        before += len(entry.get("dialogue", []))
        after += len(scene)
        touched += 1
        entry["dialogue"] = [line for _, line in scene]
        entry["speaker"] = [who for who, _ in scene]
        if any(who == "miggs" for who, _ in scene):
            entry.setdefault("cast", {})["miggs"] = {
                "name": "Miggs",
                "portrait": "portraits/miggs/_ref.png",
            }
        # Voice clips are keyed by index; the indices just changed.
        entry.pop("voice", None)
        if any(who == "pc" for who, _ in scene):
            entry["portrait_pc"] = PC_PORTRAIT
        if args.dry_run:
            print(f"=== {entry['id']}  ({len(scene)} lines) ===")
            for who, line in scene:
                tag = "GRAYSON" if who == "pc" else "fixer  "
                print(f"  [{tag}] {line}")

    if not args.dry_run:
        path.write_text(json.dumps(doc, indent=2) + "\n")

    verb = "would expand" if args.dry_run else "expanded"
    print(f"[expand] {verb} {touched} scene(s): {before} -> {after} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
