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
 (F, "Take this as collateral. I'm sure you can tell it's rare -- and "
     "valuable."),
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
 (P, "I'm looking for Sandoval. He's got fifteen thousand reasons to be "
     "happy to see me."),
 (F, "Don't bother. He's dead."),
 (P, "..."),
 (F, "Two nights ago. Word is it wasn't an accident."),
 (P, "Wonderful. He owed me fifteen thousand credits. I hauled his cargo "
     "past three patrol sweeps for the privilege of hearing this."),
 (F, "He owed a lot of people a lot of things. You're near the back of that line."),
 (P, "And you are... what, the welcoming committee for stiffed couriers?"),
 (F, "Tayla. A friend to pilots -- if they survive long enough."),
 (P, "So you tracked me down out of pure kindness. In my experience, people "
     "who find me in bars always want something."),
 (F, "Because Sandoval handed you a trinket before he died, and now the people "
     "he owed are very curious about where it went."),
 (P, "It's collateral on a debt he can now never pay. Which is perfect. "
     "It's his, not mine."),
 (F, "It's yours. He's in no position to redeem it, and his fifteen grand died "
     "with him. Keep it, like it or not."),
 (P, "I'd rather hand it to whoever's asking, collect nothing, and fly away "
     "clean. Call me old-fashioned -- I enjoy being alive and boring."),
 (F, "Hand it back to who? There's no one left to hand it to -- that's the "
     "point. You're holding it now. That's the whole story."),
 (P, "Fine. Then tell me what I'm holding, because right now it's either a "
     "paperweight or a death sentence, and I'd like to file it correctly."),
 (F, "I'll tell you this much for free. Sandoval didn't buy that thing."),
 (P, "..."),
 (F, "He got it by killing the man who owned it."),
 (P, "Better and better. So I'm carrying a murder motive with a strap. "
     "Every pawn broker in the sector will be thrilled to see me coming."),
 (F, "I'm telling you people have died for that thing, flyboy. Feel lucky?"),
 (P, "Not particularly. This week my luck has produced one dead client and "
     "one haunted paperweight."),
 (F, "Good. Lucky men die out here first."),
 (P, "You know more than you're saying. I can tell -- it's the smile."),
 (F, "Of course I do. That's the only thing I sell."),
 (P, "All right, what's the price? And don't say 'we'll discuss it later' -- "
     "that's exactly how I ended up owed fifteen grand by a corpse."),
 (F, "Work for me, $NM. I'll trade the rest out a piece at a time, and you'll earn every one."),
 (P, "That's a remarkably long way of saying you own me now. At least "
     "Sandoval had the decency to be merely in debt."),
 (F, "It's a long way of saying you're the only one I trust with it. Come back when you're ready."),
],
"tayla_m02_offer": [
 (F, "You've been staring at the artifact an hour. Point on the cargo -- what are you thinking?"),
 (P, "I'm thinking you planned this conversation three moves before I walked "
     "in. You probably ordered my drink, too."),
 (F, "Sweetheart, I don't sell to strangers. But I have an eye for pilots, and you came back."),
 (P, "Then let's skip the part where you call it a milk run. You'd be lying, "
     "I'd pretend to believe you -- it would insult us both."),
 (F, "First honest statement you've made. Ready to work?"),
 (P, "Nothing about the last week qualifies. My client died, my payday "
     "evaporated, and a woman in a bar owns my immediate future. But sure. "
     "Cargo."),
 (F, "Thirty units of plastics. Completely legal, nothing to hide."),
 (P, "Completely legal. Nothing to hide. Then why are you smiling like the "
     "paperwork is the punchline?"),
 (F, "Because officially you're running it to Newcastle."),
 (P, "And actually? There's always an 'actually' with you. It's becoming my "
     "favorite part."),
 (F, "Actually you'll divert to Oakham. Hidden pirate base out in Pentonville."),
 (P, "A falsified manifest. One week in, and I've graduated from hauling "
     "iron to light document fraud. My career is really taking off."),
 (F, "The paperwork is for anyone watching. The cargo really is plastics -- if "
     "they board you, you're clean."),
 (P, "And when someone pulls my flight recorder and asks why 'Newcastle' "
     "has an asteroid field and a pirate base?"),
 (F, "Then you're a pilot who got lost. It happens."),
 (P, "..."),
 (F, "Oakham sits in an asteroid field. Mind your speed on approach -- I've "
     "lost two pilots to those rocks and none to guns."),
 (P, "None to guns? What do the pirates do, wave? That's either reassuring "
     "or the worst safety briefing I've ever had."),
 (F, "While you fly for me, Pentonville's pirates keep their guns to "
     "themselves. That's what you're really buying."),
 (P, "Ten thousand for document fraud and a rock slalom. I notice you priced "
     "it exactly high enough that I won't say no."),
 (F, "Ten thousand on delivery. Leave the ship docked and meet me in the "
     "Oakham bar. We'll talk more there."),
 (P, "More about the artifact, you mean. You're rationing that story like "
     "it's water on a life raft."),
 (F, "More about whatever you've earned by then."),
],
"tayla_m03_offer": [
 (F, "Now the real work."),
 (P, "The milk run's over, then. Shame. I was just getting attached to "
     "honest fraud."),
 (F, "Then you'll enjoy this. Brilliance -- yes, that Brilliance -- to "
     "Hector, the mining base in Troy."),
 (P, "That's not a manifest problem, that's a prison sentence. My lawyer "
     "would object, if I could afford one."),
 (F, "Fifteen units. It'll fit where nobody looks -- I checked your "
     "dimensions personally. And you'll afford the lawyer after this one."),
 (P, "Oh really? Were you looking for length, or girth?"),
 (F, "Cargo capacity. Though your confidence is noted -- and filed."),
 (P, "Troy's crawling with militia, and my ship's top speed is best "
     "described as 'eventual.'"),
 (F, "It is. Talons, and they scan everything that moves -- even the "
     "eventual ones."),
 (P, "So the plan is me, a hold full of felony, and a checkpoint that scans "
     "everything that moves. I'm sensing a theme in your courtship."),
 (F, "If this were courtship, you'd be in far more trouble. The plan is "
     "speed: do not stop, do not fight, burn past them and dock."),
 (P, "And if they light me up? Asking for me, specifically."),
 (F, "Then you run, and you don't lead them back here. I'd hate to lose the "
     "base. I'd almost hate to lose you."),
 (P, "Charming. You nearly said something sweet just then -- careful, I'll "
     "get ideas."),
 (F, "Keep the ideas coming, flyboy -- they suit you. Fifteen thousand on "
     "delivery at Hector. Come back in one piece."),
 (P, "And another piece of the story. You still owe me the middle chapters, "
     "and I'm starting to think you like me in installments."),
 (F, "You're earning more every run, love. Even when you don't notice."),
],
"tayla_m03_debrief": [
 (F, "You made it past the militia with the goods and your hull."),
 (P, "It was close at the second nav point. Close enough to read hull "
     "numbers. Close enough to start drafting a confession."),
 (F, "I'm almost impressed."),
 (P, "'Almost' again. You keep dangling that word like it's on a string."),
 (F, "Fine -- impressed, fully. Don't let it go to your head. Catch your "
     "breath, $NM: the next run makes Troy look like a pleasure cruise."),
 (P, "You said that about the last one, and your pleasure cruises keep "
     "involving people who shoot at me."),
 (F, "The shooting is how you know it's a cruise and not a vacation. And I "
     "was wrong last time. This time I'm not."),
],
"tayla_m04_offer": [
 (F, "Bigger load this time, hotshot. Brilliance to New Constantinople itself."),
 (P, "The capital. You're not serious. That's like shoplifting from a police "
     "station."),
 (F, "Twenty-five units of shoplifting."),
 (P, "That's Confed's front porch, Tayla. They keep the porch light on and "
     "everything."),
 (F, "Before you say no -- I have friends on the route. They owe me."),
 (P, "You bribed them."),
 (F, "Generously."),
 (P, "'Generously.' Now I'm jealous of a customs officer. You'll forgive me "
     "if I keep my guns hot anyway."),
 (F, "Don't be jealous -- all they got was money. I cleared the route "
     "myself, $NM. If anyone lights you up, it wasn't my people."),
 (P, "Every time you tell me a route is clean it costs me hull plates. I've "
     "started pricing you into the repair budget. Line item: 'Tayla.'"),
 (F, "Put me under 'maintenance' -- I'm recurring. Twenty thousand on "
     "delivery, then come straight back. I get bored when you're not in "
     "danger."),
],
"tayla_m04_debrief": [
 (P, "Bribed patrols, you said. 'They owe me,' you said."),
 (F, "I did say that."),
 (P, "Stilettos and Broadswords at every jump point. Your friends have a "
     "strange way of owing you."),
 (F, "Huh. Must have been a clerical error."),
 (P, "I lost half my shields to a clerical error. The clerical error had "
     "missile lock."),
 (F, "Stop scowling. To make it up to you, my people just installed something "
     "in your ship. While you were docked."),
 (P, "You went into my ship. I feel like there are stages to this kind of "
     "thing, and you skipped several."),
 (F, "I don't do stages. A smuggler's compartment -- twenty units, "
     "invisible to any scanner ever built."),
 (P, "So that's what the noise was. I assumed the dock crew was stealing "
     "something. Silly me -- they were installing the crime."),
 (F, "Consider it a promotion, love. The contractors I trust do that on the side."),
 (P, "..."),
 (F, "You'd rather have the apology?"),
 (P, "I'd rather have been asked. Call me old-fashioned -- I like a little "
     "conversation before someone's inside my ship."),
 (F, "This is the conversation, $NM. One more run. Then we're square -- "
     "and then we'll see."),
],
"tayla_m05_offer": [
 (P, "Let me guess... I'm running a shipment of catnip to Kilrah."),
 (F, "...!"),
 (F, "You're still funny, hotshot. I save the interesting runs for the pilots I trust."),
 (P, "Generous. Most people just buy me a drink first."),
 (F, "Drinks are for marks. You're an investment. The question is whether "
     "you're done being a courier."),
 (P, "What else would I be? Careful with the job titles -- the last one you "
     "gave me came with a hidden compartment."),
 (F, "Twenty units of Brilliance to New Constantinople -- the last run. "
     "Fly it and we'll talk more, love. I'm still deciding what you're for."),
 (P, "Twenty units. Fits the new compartment exactly. Almost like somebody "
     "measured me for it."),
 (F, "Elegant, isn't it?"),
 (P, "Convenient. There's a difference. Elegance doesn't usually end in a "
     "strip search."),
 (F, "Only if they catch you, and they won't. One thing before you go. "
     "William Riordian."),
 (P, "Should that name mean something?"),
 (F, "He flew these runs before you showed up. Then he let a few friends "
     "talk him out of his nerve."),
 (P, "And now he's behind me. Wonderful. I've inherited an ex."),
 (F, "An ex with your schedule. He's telling strangers what your docking "
     "pattern looks like. Your compartment stays closed, love -- the "
     "contractors know that."),
 (F, "Ten thousand. After this I'll tell you everything I know about that "
     "trinket of yours."),
 (P, "Everything, please. I've earned the director's cut."),
 (F, "Everything I have, partner. Come back in one piece."),
],
"tayla_m05_debrief": [
 (F, "Riordian, hm?"),
 (P, "He found me at the jump point. He wasn't alone, and he wasn't there to "
     "compare compartments."),
 (F, "He always was a jealous idiot. Jealous of the runs, jealous of the "
     "pilot. You did fine."),
 (P, "You owe me a story, Tayla. I've been paid in cliffhangers for a month."),
 (F, "Here's the rest of what I owe you, first."),
 (P, "..."),
 (F, "Before Sandoval, it belonged to a spice merchant named Dieter."),
 (P, "And before him? I'm invested now. Emotionally. Possibly terminally."),
 (F, "Terminal is the usual outcome. Dieter had it from his own father -- "
     "every owner I can name died holding it."),
 (P, "That's a pattern, not a story. Patterns are what coroners find."),
 (F, "Coroners and I read the same reports. That's where my thread runs "
     "out. I've sent the holo on to someone who can actually read it."),
 (P, "And who might that be? Let me guess... Confed's very own Admiral "
     "Tolwyn?"),
 (F, "Someone with fewer medals and better manners. Roman Lynch. New "
     "Constantinople. A thug -- but an expert on exotic and valuable things."),
 (P, "You mean the famous mob boss who murders people and then bribes the "
     "authorities to stay out of prison? Gosh, Tayla, I didn't realize we'd "
     "made it this far in our relationship."),
 (F, "In my line of work, this is meeting the family. He's expecting you "
     "in the bar there -- mind your manners. Roman notices them."),
 (P, "And that squares us. Funny -- being square with you feels a lot like "
     "being in deeper."),
 (F, "That's the idea. And one more thing, partner: if you ever crack the "
     "mystery of that trinket, don't send word. Fly back here and tell me "
     "the ending in person."),
 (F, "Fair warning -- watching you fly inspired me to dust off my old F-38 "
     "Talon. I thought I was retired for good. I'm reconsidering."),
 (P, "I honestly can't tell which feeling is stronger: being flattered that "
     "you were watching my flying, or realizing you could have flown every "
     "one of these runs yourself."),
 (F, "Of course I could have. But then we'd never have gotten this close, "
     "Grayson. Try not to die -- the next drink's on me."),
],

# ------------------------------------------------------------------- LYNCH --
"lynch_m06_offer": [
 ("miggs", "You don't wanna talk to me, 'cause I don't wanna talk to you."),
 ("miggs", "And anyone that makes me do what I don't wanna do gets hurt, "
            "painwise, get me?"),
 (P, "Relax. I'm here to see Lynch. I'll try to remember I'm inside of "
     "Thugs R Us."),
 ("miggs", "Mr. Lynch, sitting over there, HE'S the one you wanna talk to..."),
 ("miggs", "...so either state your bidness or take a hike, buddy."),
 (F, "Enough, Miggs."),
 (F, "Ah, Captain. I've been expecting you. I am Roman Lynch."),
 (P, "Tayla said you'd see me. She called you the polite kind of thug. I "
     "can see the polish from here."),
 (F, "You may speak freely around my assistant. He is exceedingly loyal."),
 (P, "He's exceedingly something. I'm going to guess it isn't 'certified "
     "in conflict de-escalation.'"),
 (F, "Your artifact interests me, $NM."),
 (P, "Everyone finds it interesting. So far the interest has a body count "
     "and zero explanations, so you'll forgive my enthusiasm."),
 (F, "There is a hologram inside it. Did you know?"),
 (P, "A hologram. Inside the rock I've been sleeping next to for a month. "
     "And you'd know that how?"),
 (F, "Because Tayla sent me an image, and my people are thorough. It is a map, "
     "or something that behaves like one."),
 (P, "A map. Of course it's a map. Fine -- a map to what?"),
 (F, "That is precisely the question I intend to answer -- while you make "
     "yourself useful."),
 (P, "There it is. I was starting to worry this meeting was free."),
 (F, "A certain Captain Seelig is loitering at Nav 3 in Pentonville. His ship "
     "is the Hooded Hawk."),
 (P, "And? People loiter. It's a hobby. Why do I care about Captain "
     "Seelig's?"),
 (F, "Deliver a message. Tell him how profoundly disappointed I am in him. "
     "Verbatim, please."),
 (P, "And if I say no? Miggs alphabetizes my bones?"),
 ("miggs", "Too many little pieces. Maybe I just throw you out the airlock."),
 (P, "Airlock. Noted. You really are a full-service operation."),
 (F, "Miggs dislikes clerical work."),
 (P, "So, to recap: I fly to Pentonville, recite a disappointed speech at an "
     "armed stranger, and fly home. That's all. Words."),
 (F, "Words, Captain. I am a businessman."),
 (P, "Businessmen don't need couriers for a comm channel. Businessmen also "
     "don't usually come with a Miggs."),
 (F, "Some messages lose their meaning over a relay. Ten thousand when you get "
     "back."),
 (P, "Yeah... I'm not entirely sure I want to be delivering messages for "
     "the mob."),
 ("miggs", "Maybe I just break your legs instead. You can deliver it limping."),
 (F, "Miggs. We do not damage the couriers before the delivery."),
 (P, "'Before.' Very comforting. And the artifact?"),
 (F, "Stays with you. I want it studied, not stolen -- and frankly it is safer "
     "in a ship than in my safe."),
 (P, "A simple courier run. The last person who told me that is dead, and "
     "now I own a haunted paperweight and a smuggling compartment."),
 (F, "What could go wrong?"),
],
"lynch_m06_debrief": [
 (F, "Ah. You're breathing. How did Captain Seelig take my chastisement?"),
 (P, "He opened fire before I finished the sentence. Apparently "
     "'profoundly' was the trigger word."),
 (F, "Took it badly? Shocking."),
 (P, "You knew he would. You sent me to deliver a eulogy and let me think "
     "it was a memo."),
 (F, "I suspected. Some men simply cannot accept a severance package."),
 (P, "You used me as a trigger, Lynch."),
 (F, "I used you as a messenger. What he did with the message was his choice."),
 (P, "..."),
 (F, "Your ten thousand, as agreed."),
 (P, "And the artifact?"),
 (F, "The hologram is resisting analysis. My people need more time."),
 (P, "How much time? People who stand near this thing keep achieving "
     "'former employee' status."),
 (F, "As much as I am willing to buy. And I have more work."),
],
"lynch_m07_offer": [
 (F, "The artifact investigation has hit complications."),
 (P, "Meaning what? With you, 'complications' usually comes with a "
     "casualty column."),
 (F, "People keep dying around it. Fascinating, really."),
 (P, "You say that like it's weather. 'Partly fatal, with a chance of "
     "clerks.'"),
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
 ("miggs", "First one's free. Second one leaves marks."),
 (P, "I'll leave a five-star review. 'Ambiance: menacing. Staff: bitey.'"),
 (P, "Back to business. Weapons to a farming world. What are they growing "
     "out there, casualties?"),
 (F, "Farmers have enemies too."),
 (P, "There's a wrinkle. There's always a wrinkle."),
 (F, "Salman Kroiz runs guns on that route and considers it his."),
 (P, "And he'll object the way everyone in your life objects -- with guns."),
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
 (P, "Of course you do. Your favors have a way of metastasizing."),
 (F, "My cousin Regis has been subpoenaed in a murder trial."),
 (P, "Witness relocation. Very civic-minded."),
 (F, "My family has always believed in public service."),
 (P, "Mostly the part where the public serves your family."),
 ("miggs", "Maybe I throw you out the airlock now and save us all some time."),
 (P, "You really do only have the one idea. Honestly, I respect the "
     "commitment."),
 (P, "Whose murder?"),
 (F, "A tedious man's. It would be best for everyone if Regis simply "
     "disappeared before he testifies."),
 (P, "You want me to disappear a witness. I'm fairly sure the bar "
     "association has a word for that. Several. With sentencing guidelines."),
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
 (P, "You've said that before. Twice. I keep a list now. It's laminated."),
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
 (P, "Should that worry me? I've gotten used to being threatened on a "
     "schedule. It's like a hotel wake-up call."),
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
