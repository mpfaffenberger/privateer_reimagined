# Privateer — Bar Fixer Script

Auto-generated from `assets/data/fixers.json` by
`tools/cinematics/export_script.py`. Do not edit by hand — edit the
authoring scripts and re-run.

## ACT I - THE COURIER (Sandoval)

### Ernesto Sandoval — `sandoval_offer`

*Location:* new_detroit  
*Appears when:* blocked by `m01_active`, `m01_delivered`, `sandoval_done`

> **ERNESTO SANDOVAL:** Welcome to New Detroit. You look like a man who's hungry for work.
> **GRAYSON:** Depends on the work. And the man offering it.
> **ERNESTO SANDOVAL:** Sandoval. Ernesto Sandoval. I deal in acquisitions.
> **GRAYSON:** That's a word people use when the real one sounds worse.
> **ERNESTO SANDOVAL:** It's a word that keeps a man out of court. Sit down, Captain.
> **GRAYSON:** I'll stand. What's the run?
> **ERNESTO SANDOVAL:** Forty units of iron. Here to the Liverpool refinery, over in Newcastle.
> **GRAYSON:** Iron. That's it.
> **ERNESTO SANDOVAL:** Strictly legit. No contraband, no hassles. Just a short jump from here.
> **GRAYSON:** If it's that clean, why are you buying me a drink instead of posting it?
> **ERNESTO SANDOVAL:** Because the posted board is full of men who ask fewer questions and lose more cargo. I'd rather pay for judgment.
> **GRAYSON:** Flattery's cheap. What's the pay?
> **ERNESTO SANDOVAL:** Fifteen thousand credits on your return.
> **GRAYSON:** On my return.
> **ERNESTO SANDOVAL:** I don't have it on me tonight.
> **GRAYSON:** Then we don't have a deal tonight.
> **ERNESTO SANDOVAL:** Wait. I'll show you something better than credits.
> **GRAYSON:** *(silence)*  *[steltek_artifact]*
> **ERNESTO SANDOVAL:** Here. Hold this. Collateral, until I pay.  *[steltek_artifact]*
> **GRAYSON:** What is it?  *[steltek_artifact]*
> **ERNESTO SANDOVAL:** Don't ask me what it is.  *[steltek_artifact]*
> **GRAYSON:** It's warm.  *[steltek_artifact]*
> **ERNESTO SANDOVAL:** Just keep it safe. And keep it quiet -- inside your jacket, not on your console.  *[steltek_artifact]*
> **GRAYSON:** You're handing a stranger something you won't name, and telling him to hide it. You see how that sounds.
> **ERNESTO SANDOVAL:** I see how it sounds. Bring it back with you and there's another five thousand in it for you.
> **GRAYSON:** Twenty total, then. For hauling iron.
> **ERNESTO SANDOVAL:** For hauling iron, and for having a short memory.
> **GRAYSON:** *(silence)*
> **ERNESTO SANDOVAL:** Deliver at Liverpool. My people will meet the ship. Then come back here.
> **GRAYSON:** And if you're not here when I get back?
> **ERNESTO SANDOVAL:** I'll be here.

**> OFFER:** Haul 40 units of iron to Liverpool (Newcastle system), then return here for 15,000 credits - plus 5,000 for returning his collateral. Deal?

**— ACCEPT —**

> **ERNESTO SANDOVAL:** Good. Good! You won't regret it.
> **GRAYSON:** I regret most things eventually.
> **ERNESTO SANDOVAL:** The cargo's already aboard. It was aboard before you sat down.
> **GRAYSON:** *(silence)*
> **ERNESTO SANDOVAL:** I told you I'd rather pay for judgment. I didn't say I'd wait for it.
> **GRAYSON:** One day someone's going to shoot you, Sandoval.
> **ERNESTO SANDOVAL:** Fly safe, Captain. Come back to me.

**— REFUSE —**

> **ERNESTO SANDOVAL:** *(silence)*
> **ERNESTO SANDOVAL:** I see. A pity.
> **GRAYSON:** Find someone else to hold your warm little secret.
> **ERNESTO SANDOVAL:** There isn't anyone else. That's rather the difficulty.
> **GRAYSON:** Then your difficulty just got worse.
> **ERNESTO SANDOVAL:** It was already worse than you know. Good evening, Captain.

*Outcome:* `m01:accept`, `give_item:steltek_artifact`

---

### Tayla — `tayla_artifact_handoff`

*Location:* new_detroit  
*Appears when:* requires `m01_delivered`; blocked by `sandoval_done`

> **GRAYSON:** I'm looking for Sandoval. He's got fifteen thousand reasons to be happy to see me.
> **TAYLA:** Don't bother. He's dead.
> **GRAYSON:** *(silence)*
> **TAYLA:** Two nights ago. Word is it wasn't an accident.
> **GRAYSON:** Wonderful. He owed me fifteen thousand credits. I hauled his cargo past three patrol sweeps for the privilege of hearing this.
> **TAYLA:** He owed a lot of people a lot of things. You're near the back of that line.
> **GRAYSON:** And you are... what, the welcoming committee for stiffed couriers?
> **TAYLA:** Tayla. A friend to pilots -- if they survive long enough.
> **GRAYSON:** So you tracked me down out of pure kindness. In my experience, people who find me in bars always want something.
> **TAYLA:** Because Sandoval handed you a trinket before he died, and now the people he owed are very curious about where it went.
> **GRAYSON:** It's collateral on a debt he can now never pay. Which is perfect. It's his, not mine.
> **TAYLA:** It's yours. He's in no position to redeem it, and his fifteen grand died with him. Keep it, like it or not.
> **GRAYSON:** I'd rather hand it to whoever's asking, collect nothing, and fly away clean. Call me old-fashioned -- I enjoy being alive and boring.
> **TAYLA:** Hand it back to who? There's no one left to hand it to -- that's the point. You're holding it now. That's the whole story.
> **GRAYSON:** Fine. Then tell me what I'm holding, because right now it's either a paperweight or a death sentence, and I'd like to file it correctly.
> **TAYLA:** I'll tell you this much for free. Sandoval didn't buy that thing.
> **GRAYSON:** *(silence)*  *[steltek_artifact]*
> **TAYLA:** He got it by killing the man who owned it.  *[steltek_artifact]*
> **GRAYSON:** Better and better. So I'm carrying a murder motive with a strap. Every pawn broker in the sector will be thrilled to see me coming.  *[steltek_artifact]*
> **TAYLA:** I'm telling you people have died for that thing, flyboy. Feel lucky?
> **GRAYSON:** Not particularly. This week my luck has produced one dead client and one haunted paperweight.
> **TAYLA:** Good. Lucky men die out here first.
> **GRAYSON:** You know more than you're saying. I can tell -- it's the smile.
> **TAYLA:** Of course I do. That's the only thing I sell.
> **GRAYSON:** All right, what's the price? And don't say 'we'll discuss it later' -- that's exactly how I ended up owed fifteen grand by a corpse.
> **TAYLA:** Work for me, $NM. I'll trade the rest out a piece at a time, and you'll earn every one.
> **GRAYSON:** That's a remarkably long way of saying you own me now. At least Sandoval had the decency to be merely in debt.
> **TAYLA:** It's a long way of saying you're the only one I trust with it. Come back when you're ready.

*Outcome:* `give_item:steltek_artifact`, `set_flag:sandoval_done`

---

## ACT I - THE SMUGGLER (Tayla)

### Tayla — `tayla_m02_offer`

*Location:* new_detroit  
*Appears when:* requires `sandoval_done`; blocked by `m02_active`, `tayla_1_done`

> **TAYLA:** You've been staring at the artifact an hour. Point on the cargo -- what are you thinking?
> **GRAYSON:** I'm thinking you planned this conversation three moves before I walked in. You probably ordered my drink, too.
> **TAYLA:** Sweetheart, I don't sell to strangers. But I have an eye for pilots, and you came back.
> **GRAYSON:** Then let's skip the part where you call it a milk run. You'd be lying, I'd pretend to believe you -- it would insult us both.
> **TAYLA:** First honest statement you've made. Ready to work?
> **GRAYSON:** Nothing about the last week qualifies. My client died, my payday evaporated, and a woman in a bar owns my immediate future. But sure. Cargo.
> **TAYLA:** Thirty units of plastics. Completely legal, nothing to hide.
> **GRAYSON:** Completely legal. Nothing to hide. Then why are you smiling like the paperwork is the punchline?
> **TAYLA:** Because officially you're running it to Newcastle.
> **GRAYSON:** And actually? There's always an 'actually' with you. It's becoming my favorite part.
> **TAYLA:** Actually you'll divert to Oakham. Hidden pirate base out in Pentonville.
> **GRAYSON:** A falsified manifest. One week in, and I've graduated from hauling iron to light document fraud. My career is really taking off.
> **TAYLA:** The paperwork is for anyone watching. The cargo really is plastics -- if they board you, you're clean.
> **GRAYSON:** And when someone pulls my flight recorder and asks why 'Newcastle' has an asteroid field and a pirate base?
> **TAYLA:** Then you're a pilot who got lost. It happens.
> **GRAYSON:** *(silence)*
> **TAYLA:** Oakham sits in an asteroid field. Mind your speed on approach -- I've lost two pilots to those rocks and none to guns.
> **GRAYSON:** None to guns? What do the pirates do, wave? That's either reassuring or the worst safety briefing I've ever had.
> **TAYLA:** While you fly for me, Pentonville's pirates keep their guns to themselves. That's what you're really buying.
> **GRAYSON:** Ten thousand for document fraud and a rock slalom. I notice you priced it exactly high enough that I won't say no.
> **TAYLA:** Ten thousand on delivery. Leave the ship docked and meet me in the Oakham bar. We'll talk more there.
> **GRAYSON:** More about the artifact, you mean. You're rationing that story like it's water on a life raft.
> **TAYLA:** More about whatever you've earned by then.

**> OFFER:** Haul 30 units of plastics to Oakham (Pentonville system) for 10,000 credits on landing. Deal?

**— ACCEPT —**

> **TAYLA:** Smart. The manifest's already filed.
> **GRAYSON:** Naturally. Consent is mostly paperwork to you.
> **TAYLA:** I don't waste time on people who say no. I waste it on people who say yes slowly.
> **GRAYSON:** Here I am, talking dirty with a pirate girlboss instead of collecting fifteen thousand from a dead man. The universe is interesting.
> **TAYLA:** Keep saying yes slowly and it may get more interesting. Don't scratch my plastics.

**— REFUSE —**

> **TAYLA:** Suit yourself.
> **GRAYSON:** That's it? No pitch?
> **TAYLA:** You've got a dead man's trinket in your jacket and nobody left to sell it to. You'll be back.
> **GRAYSON:** *(silence)*
> **TAYLA:** I'll still be here. That's the whole advantage of my line of work.

*Outcome:* `m02:accept`

---

### Tayla — `tayla_m03_offer`

*Location:* oakham  
*Appears when:* requires `tayla_1_done`; blocked by `m03_active`, `m03_delivered`, `tayla_2_done`

> **TAYLA:** Now the real work.
> **GRAYSON:** The milk run's over, then.
> **TAYLA:** Brilliance. Yes -- that Brilliance. To Hector, the mining base in Troy.
> **GRAYSON:** That's not a manifest problem. That's a prison sentence.
> **TAYLA:** Fifteen units. It'll fit where nobody looks.
> **GRAYSON:** Troy's crawling with militia.
> **TAYLA:** It is. Talons, and they scan everything that moves.
> **GRAYSON:** You're sending me through a checkpoint with contraband in the hold.
> **TAYLA:** I'm sending you through it fast. Do not stop. Do not fight. Burn past them and dock.
> **GRAYSON:** And if they light me up?
> **TAYLA:** Then you run, and you don't lead them back here.
> **GRAYSON:** Charming.
> **TAYLA:** Fifteen thousand on delivery at Hector. Then come back here in one piece, hotshot.
> **GRAYSON:** And another piece of the story.
> **TAYLA:** You're earning more every run, love. Even when you don't notice.

**> OFFER:** Smuggle 15 units of Brilliance to Hector (Troy system) for 15,000 credits, then return to Oakham. In?

**— ACCEPT —**

> **TAYLA:** Good. It's loaded in the false floor, not the hold.
> **GRAYSON:** You loaded it before I said yes.
> **TAYLA:** I loaded it before I asked. Burn past the militia and don't be clever.

**— REFUSE —**

> **TAYLA:** Brilliance scares you.
> **GRAYSON:** Militia scanners scare me. There's a difference.
> **TAYLA:** Not to a customs officer there isn't.
> **GRAYSON:** *(silence)*
> **TAYLA:** Come back when you've decided which kind of pilot you are.

*Outcome:* `m03:accept`

---

### Tayla — `tayla_m03_debrief`

*Location:* oakham  
*Appears when:* requires `m03_delivered`; blocked by `tayla_2_done`

> **TAYLA:** You made it past the militia with the goods and your hull.
> **GRAYSON:** It was close at the second nav point.
> **TAYLA:** I'm almost impressed.
> **GRAYSON:** Almost is bad. Almost is bait.
> **TAYLA:** Earned, then. Catch your breath, $NM. The next run makes Troy look like a pleasure cruise.
> **GRAYSON:** You said that about the last one.
> **TAYLA:** And I was wrong. This time I'm not.

*Outcome:* `set_flag:tayla_2_done`

---

### Tayla — `tayla_m04_offer`

*Location:* oakham  
*Appears when:* requires `tayla_2_done`; blocked by `m04_active`, `m04_delivered`, `tayla_3_done`

> **TAYLA:** Bigger load this time, hotshot. Brilliance to New Constantinople itself.
> **GRAYSON:** The capital. You're not serious.
> **TAYLA:** Twenty-five units.
> **GRAYSON:** That's Confed's front porch, Tayla.
> **TAYLA:** Before you say no -- I have friends on the route. They owe me.
> **GRAYSON:** You bribed them.
> **TAYLA:** Generously.
> **GRAYSON:** Bribed patrols. You'll forgive me if I keep my guns hot.
> **TAYLA:** I cleared the route myself, $NM. If anyone lights you up, it wasn't my people.
> **GRAYSON:** Every time you say that it costs me hull.
> **TAYLA:** Twenty thousand on delivery. Then come straight back.

**> OFFER:** Smuggle 25 units of Brilliance to New Constantinople for 20,000 credits, then return to Oakham. Deal?

**— ACCEPT —**

> **TAYLA:** Twenty-five units, straight to the capital. You're moving up.
> **GRAYSON:** That's not the word I'd use.
> **TAYLA:** It's the word your account balance would use. Go.

**— REFUSE —**

> **TAYLA:** The bribes are already paid, $CS.
> **GRAYSON:** Then you're out the money.
> **TAYLA:** I'm out the money either way. What I'm short of is a pilot.
> **GRAYSON:** *(silence)*
> **TAYLA:** Find your nerve and come back. The route doesn't stay bought forever.

*Outcome:* `m04:accept`

---

### Tayla — `tayla_m04_debrief`

*Location:* oakham  
*Appears when:* requires `m04_delivered`; blocked by `tayla_3_done`

> **GRAYSON:** Bribed patrols, you said.
> **TAYLA:** I did say that.
> **GRAYSON:** Stilettos and Broadswords at every jump point.
> **TAYLA:** Huh. Must have been a clerical error.
> **GRAYSON:** I lost half my shields to a clerical error.
> **TAYLA:** Stop scowling. To make it up to you, my people just installed something in your ship. While you were docked.
> **GRAYSON:** You went into my ship.
> **TAYLA:** A smuggler's compartment. Twenty units, invisible to any scanner ever built.
> **GRAYSON:** So that's what the noise was.
> **TAYLA:** Consider it a promotion, love. The contractors I trust do that on the side.
> **GRAYSON:** *(silence)*
> **TAYLA:** You'd rather have the apology?
> **GRAYSON:** I'd rather have been asked.
> **TAYLA:** One more run, $NM. Then we're square -- and then we'll see.

*Outcome:* `m04:install_compartment`, `set_flag:tayla_3_done`

---

### Tayla — `tayla_m05_offer`

*Location:* oakham  
*Appears when:* requires `tayla_3_done`; blocked by `m05_active`, `m05_delivered`, `tayla_done`

> **GRAYSON:** Let me guess... I'm running a shipment of catnip to Kilrah.
> **TAYLA:** ...!
> **TAYLA:** You're still funny, hotshot. I save the interesting runs for the pilots I trust.
> **GRAYSON:** Generous.
> **TAYLA:** I think of it as an investment. The question is whether you're done being a courier.
> **GRAYSON:** What else would I be?
> **TAYLA:** Fly my missions and we'll talk more, hotshot.
> **GRAYSON:** Fits the new compartment exactly.
> **TAYLA:** Poetic, no?
> **GRAYSON:** Convenient. There's a difference.
> **TAYLA:** One thing before you go. William Riordian.
> **GRAYSON:** Should that name mean something?
> **TAYLA:** He flew these runs before you showed up. Then he let a few friends talk him out of his nerve.
> **GRAYSON:** And now he's behind me.
> **TAYLA:** And now he's telling strangers what your docking pattern looks like. Your compartment stays closed, love. The contractors know that.
> **TAYLA:** Ten thousand. After this I'll tell you everything I know about that trinket of yours.
> **GRAYSON:** Everything, please.
> **TAYLA:** Everything I have, partner. Come back in one piece.

**> OFFER:** One final run: 20 units of Brilliance to New Constantinople for 10,000 credits. Finish the job?

**— ACCEPT —**

> **TAYLA:** Last one. Then I pay out in full -- credits and story both.
> **GRAYSON:** I'll hold you to the second half.
> **TAYLA:** You've earned it. Watch for Riordian on the way out.
> **GRAYSON:** You said he was just talking.
> **TAYLA:** Men who just talk don't fuel their ships at three in the morning.

**— REFUSE —**

> **TAYLA:** Now? One run from the end?
> **GRAYSON:** You've had me smuggling for a man I never met and a reason I never got.
> **TAYLA:** And I've kept you alive doing it.
> **GRAYSON:** *(silence)*
> **TAYLA:** Fine. Walk. But you'll never learn what you're carrying, and it'll still be in your jacket.

*Outcome:* `m05:accept`

---

### Tayla — `tayla_m05_debrief`

*Location:* oakham  
*Appears when:* requires `m05_delivered`; blocked by `tayla_done`

> **TAYLA:** Riordian, hm?
> **GRAYSON:** He found me at the jump point. He wasn't alone.
> **TAYLA:** He always was a jealous idiot. You did fine.
> **GRAYSON:** You owe me a story, Tayla.
> **TAYLA:** Here's the rest of what I owe you, first.
> **GRAYSON:** *(silence)*
> **TAYLA:** Before Sandoval, it belonged to a spice merchant named Deiter.
> **GRAYSON:** And before him?
> **TAYLA:** Deiter had it from his own father. Every owner I can name died holding it.
> **GRAYSON:** That's a pattern, not a story.
> **TAYLA:** That's where my thread runs out. I've sent the holo on to someone who can actually read it.
> **GRAYSON:** Who?
> **TAYLA:** Roman Lynch. New Constantinople. A thug -- but an expert on exotic and valuable things.
> **GRAYSON:** A thug.
> **TAYLA:** The polite kind. He's expecting you in the bar there.
> **GRAYSON:** And that squares us.
> **TAYLA:** That squares us. It's been profitable, partner. Try not to die -- the next drink's on me.

*Outcome:* `set_flag:tayla_done`, `clear_flag:tayla_employed`

---

## ACT II - THE MADE MAN (Lynch)

### Roman Lynch — `lynch_m06_offer`

*Location:* new_constantinople  
*Appears when:* requires `tayla_done`; blocked by `m06_active`, `m06_message_delivered`, `lynch_1_done`

> **MIGGS:** You don't wanna talk to me, 'cause I don't wanna talk to you.
> **MIGGS:** And anyone that makes me do what I don't wanna do gets hurt, painwise, get me?
> **GRAYSON:** I'm here to see Lynch.
> **MIGGS:** Mr. Lynch, sitting over there, HE'S the one you wanna talk to...
> **MIGGS:** ...so either state your bidness or take a hike, buddy.
> **ROMAN LYNCH:** Enough, Miggs.
> **ROMAN LYNCH:** Ah, Captain. I've been expecting you. I am Roman Lynch.
> **GRAYSON:** Tayla said you'd see me.
> **ROMAN LYNCH:** You may speak freely around my assistant. He is exceedingly loyal.
> **GRAYSON:** He's exceedingly something.
> **ROMAN LYNCH:** Your artifact interests me, $NM.
> **GRAYSON:** Everyone finds it interesting. Nobody will tell me what it is.
> **ROMAN LYNCH:** There is a hologram inside it. Did you know?
> **GRAYSON:** A hologram. And you'd know that how?
> **ROMAN LYNCH:** Because Tayla sent me an image, and my people are thorough. It is a map, or something that behaves like one.
> **GRAYSON:** A map to what?
> **ROMAN LYNCH:** That is precisely the question I intend to answer -- while you make yourself useful.
> **GRAYSON:** There it is.
> **ROMAN LYNCH:** A certain Captain Seelig is loitering at Nav 3 in Pentonville. His ship is the Hooded Hawk.
> **GRAYSON:** And?
> **ROMAN LYNCH:** Deliver a message. Tell him how profoundly disappointed I am in him. Verbatim, please.
> **GRAYSON:** And if I say no? Miggs alphabetizes my bones?
> **MIGGS:** Too many little pieces. Maybe I just throw you out the airlock.
> **ROMAN LYNCH:** Miggs dislikes clerical work.
> **GRAYSON:** That's all. Words.
> **ROMAN LYNCH:** Words, Captain. I am a businessman.
> **GRAYSON:** Businessmen don't need couriers for a comm channel.
> **ROMAN LYNCH:** Some messages lose their meaning over a relay. Ten thousand when you get back.
> **GRAYSON:** And the artifact?
> **ROMAN LYNCH:** Stays with you. I want it studied, not stolen -- and frankly it is safer in a ship than in my safe.
> **GRAYSON:** A simple courier run.
> **ROMAN LYNCH:** What could go wrong?

**> OFFER:** Deliver Lynch's message to Seelig at Pentonville Nav 3, then return to New Constantinople for 10,000 credits. Accept?

**— ACCEPT —**

> **ROMAN LYNCH:** Excellent. Miggs will see you to the door.
> **GRAYSON:** I can find a door.
> **ROMAN LYNCH:** Miggs enjoys the walk.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** One more thing, Captain. When Seelig reacts -- and he will react -- remember that I asked for words, not for what follows.
> **GRAYSON:** You're building an alibi.
> **ROMAN LYNCH:** I am building a habit of precision. Do come back.

**— REFUSE —**

> **ROMAN LYNCH:** Regrettably, there shall be no next time.
> **GRAYSON:** I'll live.
> **ROMAN LYNCH:** You will, briefly and expensively. No one else in this sector will read that hologram for you.
> **GRAYSON:** Then it stays unread.
> **ROMAN LYNCH:** *(silence)*
> **ROMAN LYNCH:** Miggs. Show the Captain out. Good day to you, $NM -- and trouble me no more.

*Outcome:* `set_flag:m06_active`

---

### Roman Lynch — `lynch_m06_debrief`

*Location:* new_constantinople  
*Appears when:* requires `m06_message_delivered`; blocked by `lynch_1_done`

> **ROMAN LYNCH:** Ah. You're breathing. How did Captain Seelig take my chastisement?
> **GRAYSON:** He opened fire before I finished the sentence.
> **ROMAN LYNCH:** Took it badly? Shocking.
> **GRAYSON:** You knew he would.
> **ROMAN LYNCH:** I suspected. Some men simply cannot accept a severance package.
> **GRAYSON:** You used me as a trigger, Lynch.
> **ROMAN LYNCH:** I used you as a messenger. What he did with the message was his choice.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Your ten thousand, as agreed.
> **GRAYSON:** And the artifact?
> **ROMAN LYNCH:** The hologram is resisting analysis. My people need more time.
> **GRAYSON:** How much time?
> **ROMAN LYNCH:** As much as I am willing to buy. And I have more work.

*Outcome:* `pay:10000`, `set_flag:lynch_1_done`, `clear_flag:m06_active`, `clear_flag:m06_message_delivered`

---

### Roman Lynch — `lynch_m07_offer`

*Location:* new_constantinople  
*Appears when:* requires `lynch_1_done`; blocked by `m07_active`, `m07_delivered`, `lynch_2_done`

> **ROMAN LYNCH:** The artifact investigation has hit complications.
> **GRAYSON:** Meaning what?
> **ROMAN LYNCH:** People keep dying around it. Fascinating, really.
> **GRAYSON:** You say that like it's weather.
> **ROMAN LYNCH:** At my age one becomes philosophical about other people's mortality.
> **GRAYSON:** Which people?
> **ROMAN LYNCH:** Two analysts and a records clerk. All quite unrelated, I'm sure.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Meanwhile: twenty units of weaponry to Siva, the agricultural base in Rikel. Fifteen thousand on delivery.
> **GRAYSON:** So I smuggle guns onto a farm, irritate a rival mobster, and pretend this is artifact research.
> **ROMAN LYNCH:** At last, you're learning interdisciplinary work.
> **MIGGS:** Keep smiling. I can fix that.
> **GRAYSON:** Does he come with the weapons, or is the threat package complimentary?
> **ROMAN LYNCH:** Complimentary.
> **GRAYSON:** Weapons to a farming world.
> **ROMAN LYNCH:** Farmers have enemies too.
> **GRAYSON:** There's a wrinkle. There's always a wrinkle.
> **ROMAN LYNCH:** Salman Kroiz runs guns on that route and considers it his.
> **GRAYSON:** And he'll object.
> **ROMAN LYNCH:** He flies a Demon. So do his friends. Handle it appropriately.

**> OFFER:** Run 20 units of weaponry to Siva (Rikel system) for 15,000 credits, past Kroiz's gang. Accept?

**— ACCEPT —**

> **ROMAN LYNCH:** Splendid. The crates are aboard.
> **GRAYSON:** Of course they are.
> **ROMAN LYNCH:** Do give Mr. Kroiz my regards, should he insist on introducing himself.

**— REFUSE —**

> **ROMAN LYNCH:** You'll find your access to the investigation... severely impeded.
> **GRAYSON:** That's a threat.
> **ROMAN LYNCH:** That is a monopoly. I have the only people in this sector who can read your artifact, and they answer to me.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Mark my words. You'll be back.

*Outcome:* `m07:accept`

---

### Roman Lynch — `lynch_m08_offer`

*Location:* new_constantinople  
*Appears when:* requires `lynch_2_done`; blocked by `m08_active`, `lynch_3_done`

> **ROMAN LYNCH:** Your trinket is confirmed alien.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Not Kilrathi. Old. Older than the word old is usually asked to carry.
> **GRAYSON:** How old?
> **ROMAN LYNCH:** My analysts refused to write a number down. That knowledge cost me three of them.
> **GRAYSON:** Cost them what?
> **ROMAN LYNCH:** Their positions, Captain. What did you think I meant?
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Before we discuss it further, I need a favor.
> **GRAYSON:** Of course you do.
> **ROMAN LYNCH:** My cousin Regis has been subpoenaed in a murder trial.
> **GRAYSON:** Witness relocation. Very civic-minded.
> **ROMAN LYNCH:** My family has always believed in public service.
> **GRAYSON:** Mostly the part where the public serves your family.
> **MIGGS:** Maybe I throw you out the airlock now and save us all some time.
> **GRAYSON:** Whose murder?
> **ROMAN LYNCH:** A tedious man's. It would be best for everyone if Regis simply disappeared before he testifies.
> **GRAYSON:** You want me to disappear a witness.
> **ROMAN LYNCH:** I want you to give a relative a ride. The distinction matters to lawyers, so it should matter to you.
> **GRAYSON:** Where?
> **ROMAN LYNCH:** He favors the Romulus mining base in Castor. Get him out of New Constantinople and take him there.
> **GRAYSON:** Thirty thousand?
> **ROMAN LYNCH:** Thirty thousand on landing. And do avoid the Stilettos -- they are faster than you.

**> OFFER:** Smuggle Lynch's cousin Regis to Romulus (Castor system) for 30,000 credits on landing. Take him aboard?

**— ACCEPT —**

> **ROMAN LYNCH:** Regis will be aboard within the hour.
> **GRAYSON:** He knows he's leaving?
> **ROMAN LYNCH:** He knows a great many things. That is precisely the problem.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Do not converse with him at length, Captain. He is family, and I am fond of him, and he is not a good man.
> **GRAYSON:** That's quite an endorsement from you.

**— REFUSE —**

> **ROMAN LYNCH:** You disappoint me.
> **GRAYSON:** You wanted a witness disappeared. That's not a taxi run.
> **ROMAN LYNCH:** It is precisely a taxi run. Your objection is to the passenger.
> **GRAYSON:** My objection is to the trial.
> **ROMAN LYNCH:** *(silence)*
> **ROMAN LYNCH:** Mark my words. You'll be back -- and my price will have risen.

*Outcome:* `give_item:lynch_cousin`, `set_flag:m08_active`

---

### Roman Lynch — `lynch_m09_offer`

*Location:* new_constantinople  
*Appears when:* requires `lynch_3_done`; blocked by `m09_active`, `lynch_done`

> **ROMAN LYNCH:** One last job, Captain.
> **GRAYSON:** You've said that before.
> **ROMAN LYNCH:** This time I mean it, because this time it ends the question.
> **GRAYSON:** Go on.
> **ROMAN LYNCH:** A Mr. Smythe on Liverpool claims he found something about your artifact in the Oxford library archives.
> **GRAYSON:** Then have him transmit it.
> **ROMAN LYNCH:** He will not put it on a relay. He was quite insistent about that.
> **GRAYSON:** *(silence)*
> **ROMAN LYNCH:** Pick him up. Bring him here. Thirty thousand for a taxi run.
> **GRAYSON:** Smythe retrieval. Which crime am I committing this time?
> **ROMAN LYNCH:** Rescue, Captain. Try to keep up.
> **GRAYSON:** Kidnapping with better branding. Got it.
> **ROMAN LYNCH:** You may notice Miggs is elsewhere tonight. Errands of his own.
> **GRAYSON:** Should that worry me?
> **ROMAN LYNCH:** It should worry someone. Go on then. Liverpool. Newcastle system.
> **GRAYSON:** And Smythe is waiting.
> **ROMAN LYNCH:** Mr. Smythe is waiting.

**> OFFER:** Fly to Liverpool (Newcastle system) and collect Mr. Smythe for 30,000 credits. Accept?

**— ACCEPT —**

> **ROMAN LYNCH:** Good. Liverpool, then. Mr. Smythe knows your ship's registry.
> **GRAYSON:** He knows my registry.
> **ROMAN LYNCH:** I am thorough, Captain. It is why you are still alive.

**— REFUSE —**

> **ROMAN LYNCH:** After everything, you balk at a passenger.
> **GRAYSON:** After everything, I've stopped believing your job descriptions.
> **ROMAN LYNCH:** *(silence)*
> **ROMAN LYNCH:** Smythe has what you want. Not I -- him. Refuse me and the answer stays on a rock in Newcastle.
> **GRAYSON:** Then it stays there.
> **ROMAN LYNCH:** You'll be back, Captain. You always are.

*Outcome:* `set_flag:m09_active`

---

## ACT II - THE ARCHIVE (Masterson)

### Masterson — `masterson_m10_offer`

*Location:* oxford  
*Appears when:* requires `lynch_done`; blocked by `m10_active`, `masterson_1_done`

> **GRAYSON:** I need access to the Oxford archive.
> **MASTERSON:** Ah. The pilot with the artifact and the trail of dead mobsters.
> **GRAYSON:** News travels.
> **MASTERSON:** Masterson. University administration. And no -- the library is CLOSED to you.
> **GRAYSON:** I can pay.
> **MASTERSON:** Everyone can pay. That is what makes payment uninteresting.
> **GRAYSON:** Then what's interesting?
> **MASTERSON:** Service. The university needs work done, and I am nothing if not transactional.
> **GRAYSON:** As an academic, isn't the pursuit of knowledge supposed to be its own reward?
> **MASTERSON:** The pursuit, certainly. Access to the results is billed separately.
> **GRAYSON:** How much work?
> **MASTERSON:** Access is an endowment, paid in four installments. Complete them and I unlock the archive myself.
> **GRAYSON:** Four favors for a library card. That's quite a fee.
> **MASTERSON:** It is quite a library.
> **GRAYSON:** *(silence)*
> **MASTERSON:** First installment. Hunter Toth is inbound in a Drayman at the Saxtogue jump point.
> **GRAYSON:** Who is he?
> **MASTERSON:** A journalist. He wrote unkind truths about the Retros, and Retros hold grudges with unusual sincerity.
> **GRAYSON:** So they'll be waiting for him.
> **MASTERSON:** Meet him there and see him to the planet. He must be ON THE GROUND before you land.
> **GRAYSON:** Before I land.
> **MASTERSON:** I will not pay for a corpse with an honor guard. Ten thousand, and one installment struck from your debt.

**> OFFER:** Escort Toth's Drayman from the Saxtogue Jump to Oxford for 10,000 credits. He lands first. Accept?

**— ACCEPT —**

> **MASTERSON:** Splendid. I shall log the first installment as pending.
> **GRAYSON:** Pending.
> **MASTERSON:** Nothing is credited until Mr. Toth is on the ground. The university does not pay for effort.
> **GRAYSON:** Nobody does.
> **MASTERSON:** Then we understand one another. Do hurry -- Retros are punctual.

**— REFUSE —**

> **MASTERSON:** As you wish. The archive remains closed.
> **GRAYSON:** There are other libraries.
> **MASTERSON:** There are. None of them have what you need, which is why you came to the one run by a man you dislike.
> **GRAYSON:** *(silence)*
> **MASTERSON:** You'll be back, sooner or later. They always are.

*Outcome:* `set_flag:m10_active`

---

### Masterson — `masterson_m11_offer`

*Location:* oxford  
*Appears when:* requires `masterson_1_done`; blocked by `m11_active`, `masterson_2_done`

> **MASTERSON:** Favor two.
> **GRAYSON:** Installment two, I thought.
> **MASTERSON:** Don't be smart, it doesn't suit pilots. Data pirates are draining our mainframe.
> **GRAYSON:** From where?
> **MASTERSON:** A ship parked somewhere in-system. The Black Rhombus -- a converted Galaxy, bristling with turrets.
> **GRAYSON:** Somewhere in-system is not a location.
> **MASTERSON:** Then patrol the jump points until it becomes one, and make it stop existing.
> **GRAYSON:** Oxford's answer to stolen knowledge is to shoot the thieves?
> **MASTERSON:** We attempted a strongly worded citation. They deleted it.
> **GRAYSON:** Peer review has gotten rough.
> **MASTERSON:** Standards have declined.
> **GRAYSON:** The escorts?
> **MASTERSON:** Optional. The Rhombus is not.
> **GRAYSON:** Ten thousand?
> **MASTERSON:** Ten thousand on your return. Do check your ammunition first -- I have seen your invoices.

**> OFFER:** Hunt down and destroy the Black Rhombus somewhere at Oxford's jump points, then return. 10,000 credits. Accept?

**— ACCEPT —**

> **MASTERSON:** Second installment, pending. Do find the wretched thing.
> **GRAYSON:** It's a converted Galaxy. How hidden can it be?
> **MASTERSON:** It has been hidden for six weeks. Prove me wrong.

**— REFUSE —**

> **MASTERSON:** Then our arrangement stalls at one installment of four.
> **GRAYSON:** I noticed.
> **MASTERSON:** They are draining our mainframe as we speak. Every hour costs the university more than your fee.
> **GRAYSON:** *(silence)*
> **MASTERSON:** The offer remains. So, regrettably, does the lock on the archive.

*Outcome:* `set_flag:m11_active`

---

### Masterson — `masterson_m12_offer`

*Location:* oxford  
*Appears when:* requires `masterson_2_done`; blocked by `m12_active`, `masterson_3_done`

> **MASTERSON:** Favor three, and this one is genuinely unpleasant.
> **GRAYSON:** You've been saving it.
> **MASTERSON:** My book shipment arrives on the Drayman Vulcan's Forge at the Saxtogue jump point.
> **GRAYSON:** Books again.
> **MASTERSON:** A rival collector hired bounty hunters to divert it.
> **GRAYSON:** Then I fly escort. Same as Toth.
> **MASTERSON:** Not the same. Here is the wrinkle: the hunters were paid to remove the ESCORT.
> **GRAYSON:** *(silence)*
> **MASTERSON:** That would be you.
> **GRAYSON:** Rare books, hired guns, and me as bait. Academia's more exciting than the brochures.
> **MASTERSON:** The brochures omit donor relations.
> **GRAYSON:** So the freighter is bait, and I'm the target.
> **MASTERSON:** The Demons will ignore the freighter entirely. I thought you would prefer to know.
> **GRAYSON:** I'd prefer a different job.
> **MASTERSON:** Same terms. The Forge lands first, then you. Ten thousand.

**> OFFER:** Escort Vulcan's Forge from the Saxtogue Jump to Oxford - the Demons will hunt YOU. 10,000 credits. Accept?

**— ACCEPT —**

> **MASTERSON:** Brave. Or mercenary. I have stopped distinguishing.
> **GRAYSON:** Both pay the same.
> **MASTERSON:** Indeed. The Forge lands first. Do try to as well.

**— REFUSE —**

> **MASTERSON:** You object to being the target.
> **GRAYSON:** I object to being told about it afterward. You told me first, so that's something.
> **MASTERSON:** Then object and accept, like a professional.
> **GRAYSON:** Not today.
> **MASTERSON:** *(silence)*
> **MASTERSON:** Three installments remain outstanding. Good day.

*Outcome:* `set_flag:m12_active`

---

### Masterson — `masterson_m13_offer`

*Location:* oxford  
*Appears when:* requires `masterson_3_done`; blocked by `m13_active`, `masterson_done`

> **MASTERSON:** Last favor.
> **GRAYSON:** Then the archive opens.
> **MASTERSON:** Then the archive opens. One more Drayman, inbound at the XXN-1927 jump point.
> **GRAYSON:** Carrying?
> **MASTERSON:** Let us say pirate bait. They know what is in the hold and they want it badly.
> **GRAYSON:** That's not an answer.
> **MASTERSON:** It is the only one you're getting, and I will be honest with you about the rest.
> **GRAYSON:** Go ahead.
> **MASTERSON:** This freighter is a rust bucket. Her plating would embarrass a shuttle.
> **GRAYSON:** Wonderful.
> **MASTERSON:** Keep the Talons OFF her. She lands first. Then you.
> **GRAYSON:** And then?
> **MASTERSON:** Then the library opens. Ten thousand, and my genuine respect -- which I assure you is rarer.
> **GRAYSON:** When this is done, I want a library card and a plaque.
> **MASTERSON:** The card is possible.
> **GRAYSON:** Good. I'd hate for recognition to cheapen the pursuit of knowledge.

**> OFFER:** Escort the final Drayman from the XXN-1927 Jump to Oxford - she is fragile and the pirates want her badly. 10,000 credits. Accept?

**— ACCEPT —**

> **MASTERSON:** The last one. I confess I did not expect you to reach it.
> **GRAYSON:** Neither did I.
> **MASTERSON:** Bring her home and the archive is yours. My word, for whatever you judge it to be worth.

**— REFUSE —**

> **MASTERSON:** One favor from the archive and you walk away.
> **GRAYSON:** That freighter's plating would embarrass a shuttle. Your words.
> **MASTERSON:** My words, and still true. That is why I need someone competent.
> **GRAYSON:** *(silence)*
> **MASTERSON:** Three installments paid, one owed, and nothing to show. Think about that on your way out.

*Outcome:* `set_flag:m13_active`

---

### Oxford Library — `oxford_library_scene`

*Location:* oxford  
*Appears when:* requires `masterson_done`; blocked by `library_access`

> **OXFORD LIBRARY:** The archive terminal accepts Masterson's authorization.
> **OXFORD LIBRARY:** You place the artifact in the scanner cradle. The cradle hums.  *[steltek_artifact]*
> **OXFORD LIBRARY:** MATCH FOUND. Classification: STELTEK.  *[steltek_artifact]*
> **OXFORD LIBRARY:** A precursor civilization. Starfaring while humanity was learning fire.  *[steltek_artifact]*
> **OXFORD LIBRARY:** No verified contact in recorded history. No recovered vessels. No remains.
> **OXFORD LIBRARY:** The artifact is a power relay of unknown function.  *[steltek_artifact]*
> **OXFORD LIBRARY:** Energy signature: dormant, but not dead.  *[steltek_artifact]*
> **OXFORD LIBRARY:** Cross-reference: recent anomalous Steltek-band readings reported near PALAN.
> **OXFORD LIBRARY:** Appended note. Dr. Monkhouse, the sector's leading xenoarchaeologist, has been asking the same questions you are.
> **OXFORD LIBRARY:** Last known location: the Palan system.

*Outcome:* `set_flag:library_access`

---

## ACT III - THE BLOCKADE (Murphy)

### Lynn Murphy — `murphy_m14_offer`

*Location:* basra_refinery  
*Appears when:* requires `masterson_done`; blocked by `m14_active`, `m14_cleared`, `murphy_1_done`

> **LYNN MURPHY:** You tried to land on Palan, didn't you?
> **GRAYSON:** I got as far as the orbital line.
> **LYNN MURPHY:** Bounty hunters have the planet sewn up tight. The name's Murphy -- only my close friends call me Lynn.
> **GRAYSON:** Understood.
> **LYNN MURPHY:** Two corporations here. Rondell and Bronte.
> **GRAYSON:** And the hunters?
> **LYNN MURPHY:** Bronte hired them. Choke off Rondell's food exports, ship their own in to fill the gap, take the market.
> **GRAYSON:** People are starving so a corporation can move a decimal point.
> **LYNN MURPHY:** Now you understand Palan.
> **GRAYSON:** And you're what, the resistance?
> **LYNN MURPHY:** I run it for Rondell out of this refinery. Hired resistance -- I won't pretend otherwise.
> **GRAYSON:** At least you're honest about it.
> **LYNN MURPHY:** Honesty's free. Everything else out here costs.
> **GRAYSON:** So what do you need?
> **LYNN MURPHY:** The blockade rotates fresh ships through the asteroid field. Cut the reinforcements and the whole thing starves.
> **GRAYSON:** How many?
> **LYNN MURPHY:** Three waves of Demons. Kill every last one and come back.
> **GRAYSON:** And the pay?
> **LYNN MURPHY:** Fifteen thousand. Most of what we have -- so try to be worth it.

**> OFFER:** Destroy the blockade reinforcements at the Palan asteroid field, then return to Basra. 15,000 credits.

**— ACCEPT —**

> **LYNN MURPHY:** Good. I'll log you with the wing.
> **GRAYSON:** There's a wing?
> **LYNN MURPHY:** There's a list. The wing died last month.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** Don't look like that. Everyone on that list volunteered, same as you.
> **GRAYSON:** I'm being paid.
> **LYNN MURPHY:** So was I, once. Go fly, $CS.

**— REFUSE —**

> **LYNN MURPHY:** Fair enough. It's not your fight.
> **GRAYSON:** That's not what I said.
> **LYNN MURPHY:** It's what walking away says. I stopped taking it personally about two hundred funerals ago.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** The offer stands until Bronte finishes us. After that it won't matter much either way.

*Outcome:* `set_flag:m14_active`

---

### Lynn Murphy — `murphy_m14_debrief`

*Location:* basra_refinery  
*Appears when:* requires `m14_cleared`; blocked by `murphy_1_done`

> **LYNN MURPHY:** Their whole relief wing, gone.
> **GRAYSON:** They fought like they expected to win.
> **LYNN MURPHY:** They usually do. The hunters are already flying tighter rotations to cover the gap.
> **GRAYSON:** So it worked.
> **LYNN MURPHY:** It worked. Here's your fifteen thousand.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** Don't spend it anywhere the Guild can see you. You're on Bronte's payroll list now, in the worst way.
> **GRAYSON:** And hunters hold grudges.
> **LYNN MURPHY:** Hunters hold grudges.

*Outcome:* `pay:15000`, `set_flag:murphy_1_done`, `clear_flag:m14_cleared`

---

### Lynn Murphy — `murphy_m15_offer`

*Location:* basra_refinery  
*Appears when:* requires `murphy_1_done`; blocked by `m15_active`, `m15_cleared`, `murphy_2_done`

> **LYNN MURPHY:** Bronte's money bought better help.
> **GRAYSON:** How much better?
> **LYNN MURPHY:** Ace pilots in Centurions now. Not the rabble you scattered last time.
> **GRAYSON:** Centurions. That's my hull class.
> **LYNN MURPHY:** That's the point. They stopped sending people who lose.
> **GRAYSON:** Same job?
> **LYNN MURPHY:** Same job, harder targets. Ten thousand.
> **GRAYSON:** That's less than last time.
> **LYNN MURPHY:** It's what's left in the jar. I can show you the jar if you like.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** That's what I thought.

**> OFFER:** Destroy the blockade patrols at the asteroid field - Demons and ace Centurions. 10,000 credits.

**— ACCEPT —**

> **LYNN MURPHY:** Good. Same field, worse company.
> **GRAYSON:** I'll manage.
> **LYNN MURPHY:** They said that too, ace. Watch the Centurions -- they fly like they mean to retire.

**— REFUSE —**

> **LYNN MURPHY:** Aces put you off?
> **GRAYSON:** Ten thousand for player-grade hulls put me off.
> **LYNN MURPHY:** I told you what's in the jar. I can't conjure more by being charming.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** Come back if the arithmetic improves. It won't, but come back anyway.

*Outcome:* `set_flag:m15_active`

---

### Lynn Murphy — `murphy_m15_debrief`

*Location:* basra_refinery  
*Appears when:* requires `m15_cleared`; blocked by `murphy_2_done`

> **LYNN MURPHY:** Centurion aces, and you're still breathing.
> **GRAYSON:** Two of them nearly weren't a problem I got to solve.
> **LYNN MURPHY:** I'd hire you permanently if we had a treasury.
> **GRAYSON:** You'd hire me permanently if I were cheaper.
> **LYNN MURPHY:** Both things can be true. Ten thousand.
> **GRAYSON:** And after this?
> **LYNN MURPHY:** One more push and Palan breathes free air.

*Outcome:* `pay:10000`, `set_flag:murphy_2_done`, `clear_flag:m15_cleared`

---

### Lynn Murphy — `murphy_m16_offer`

*Location:* basra_refinery  
*Appears when:* requires `murphy_2_done`; blocked by `m16_active`, `murphy_done`

> **LYNN MURPHY:** This is it.
> **GRAYSON:** The last push.
> **LYNN MURPHY:** Our attacks finally wore them down. They're low on fuel, food and patience.
> **GRAYSON:** Where are they?
> **LYNN MURPHY:** What's left is parked in orbit over Palan itself. Break it, land on the planet, and it's done.
> **GRAYSON:** Alone?
> **LYNN MURPHY:** Not this time. Two militia Talons launch with you.
> **GRAYSON:** Pilots or volunteers?
> **LYNN MURPHY:** Volunteers. They'll fight -- I won't promise more than that.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** Say what you're thinking.
> **GRAYSON:** I'm thinking they should stay home.
> **LYNN MURPHY:** It's their planet. Fifteen thousand waiting on Palan.
> **GRAYSON:** And the bill afterward?
> **LYNN MURPHY:** You've killed a lot of bounty hunters. Militia and Confed listen to hunter gossip, and none of them will forget your transponder.

**> OFFER:** Break the Palan blockade (four waves) with two friendly Talon wingmen, then land on Palan. 15,000 credits.

**— ACCEPT —**

> **LYNN MURPHY:** Then it's tonight. I'll tell the Talons.
> **GRAYSON:** Tell them to stay behind me.
> **LYNN MURPHY:** I'll tell them. They won't listen -- it's their planet down there.

**— REFUSE —**

> **LYNN MURPHY:** *(silence)*
> **LYNN MURPHY:** We go anyway. With you or without you.
> **GRAYSON:** Two volunteers and no heavy support. That's not an assault, it's a funeral.
> **LYNN MURPHY:** It's the only one we can afford.
> **GRAYSON:** *(silence)*
> **LYNN MURPHY:** If you change your mind, we launch at the next shift change. After that there won't be anyone left to launch with.

*Outcome:* `set_flag:m16_active`

---

## ACT III - THE SCHOLAR (Monkhouse)

### Dr. Lemuel Monkhouse — `monkhouse_m17_offer`

*Location:* palan  
*Appears when:* requires `murphy_done`; blocked by `m17_active`, `m17_delivered`, `monkhouse_done`

> **GRAYSON:** Doctor Monkhouse?
> **DR. LEMUEL MONKHOUSE:** Don't speak to me about extraterrestrial artifacts. I'm sick of them!
> **GRAYSON:** I haven't said a word about one.
> **DR. LEMUEL MONKHOUSE:** You have the look. I nearly got killed on this rock because of my work.
> **GRAYSON:** *(silence)*
> **DR. LEMUEL MONKHOUSE:** Lemuel Monkhouse. Xenoarchaeology. Formerly of several institutions that no longer return my calls.
> **GRAYSON:** How did you end up on Palan?
> **DR. LEMUEL MONKHOUSE:** I didn't COME to Palan, young man. I was brought.
> **GRAYSON:** Brought.
> **DR. LEMUEL MONKHOUSE:** Kidnapped. By men who wanted my Steltek fragment.
> **GRAYSON:** Kidnapped. And they're still looking for you?
> **DR. LEMUEL MONKHOUSE:** They are buried under their own interrogation compound. The bombing had one merciful outcome.
> **GRAYSON:** *(silence)*
> **DR. LEMUEL MONKHOUSE:** You may take a moment. Everyone does.
> **GRAYSON:** You said fragment. You have a piece of one.
> **DR. LEMUEL MONKHOUSE:** Thirty years I've given the Steltek. Show me your piece and I'll tell you what it is.
> **GRAYSON:** Here.
> **DR. LEMUEL MONKHOUSE:** *(silence)*  *[steltek_artifact]*
> **DR. LEMUEL MONKHOUSE:** Oh. Oh, my.  *[steltek_artifact]*
> **GRAYSON:** Doctor?  *[steltek_artifact]*
> **DR. LEMUEL MONKHOUSE:** My instruments are at the Basra refinery, and I detest cramped quarters. Fly me there.
> **GRAYSON:** That's it? A lift?
> **DR. LEMUEL MONKHOUSE:** Five thousand credits toward your expenses, and every answer I have.
> **GRAYSON:** Then why do you look nervous?
> **DR. LEMUEL MONKHOUSE:** Because someone else has been asking about me. Someone with FUR.
> **GRAYSON:** Fur. You're saying Kilrathi.
> **DR. LEMUEL MONKHOUSE:** I am saying we should leave promptly.

**> OFFER:** Fly Dr. Monkhouse to Basra. The direct route may be watched - the Kilrathi want him. 5,000 credits.

**— ACCEPT —**

> **DR. LEMUEL MONKHOUSE:** Oh, thank heavens. Thank heavens.
> **GRAYSON:** Get your things, Doctor.
> **DR. LEMUEL MONKHOUSE:** My things are two cases and thirty years of notes.
> **GRAYSON:** Then get one case.
> **DR. LEMUEL MONKHOUSE:** *(silence)*
> **DR. LEMUEL MONKHOUSE:** Young man. When we reach Basra and I put these two pieces together -- you should be prepared for the possibility that you will not like the answer.
> **GRAYSON:** I stopped expecting to like it a while ago.

**— REFUSE —**

> **DR. LEMUEL MONKHOUSE:** You're LEAVING? After I told you what I have?
> **GRAYSON:** You've told me a lot of things, Doctor. Most of them about yourself.
> **DR. LEMUEL MONKHOUSE:** That is grossly unfair and largely accurate.
> **GRAYSON:** *(silence)*
> **DR. LEMUEL MONKHOUSE:** Go, then. I shall be here -- there is nowhere else to be. Come back when curiosity beats caution. It always does.

*Outcome:* `give_item:dr_monkhouse`, `set_flag:m17_active`

---

### Dr. Lemuel Monkhouse — `monkhouse_m17_debrief`

*Location:* basra_refinery  
*Appears when:* requires `m17_delivered`; blocked by `monkhouse_done`

> **DR. LEMUEL MONKHOUSE:** Extraordinary. EXTRAORDINARY.
> **GRAYSON:** You've been at that bench for six hours.
> **DR. LEMUEL MONKHOUSE:** My fragment -- look. Look here. It FITS yours.  *[steltek_artifact]*
> **GRAYSON:** *(silence)*  *[steltek_artifact]*
> **DR. LEMUEL MONKHOUSE:** They were one device, split millennia ago. Deliberately, I'd wager.  *[steltek_artifact]*
> **GRAYSON:** Why split it?
> **DR. LEMUEL MONKHOUSE:** Because whoever did it wanted it hard to use. Together they form a map, and something like a translation aid.
> **GRAYSON:** A map to where?
> **DR. LEMUEL MONKHOUSE:** The marked route runs past Rygannon, out into the unexplored frontier.
> **GRAYSON:** That's a long way to fly on a hunch.
> **DR. LEMUEL MONKHOUSE:** It is not a hunch, it is a heading. And I have a proposal.
> **GRAYSON:** Go on.
> **DR. LEMUEL MONKHOUSE:** Join Exploratory Services at Rygannon. They run the only jump-capable survey net out there.
> **GRAYSON:** And you get?
> **DR. LEMUEL MONKHOUSE:** I publish the findings. You keep whatever you find. Ask for Taryn Cross.
> **GRAYSON:** Whatever I find.
> **DR. LEMUEL MONKHOUSE:** Young man, if I am right about what is out there, you will not want to keep it either.

*Outcome:* `give_item:steltek_map`, `set_flag:monkhouse_done`, `clear_flag:m17_delivered`

---

## ACT IV - THE FRONTIER (Cross)

### Taryn Cross — `cross_m18_offer`

*Location:* rygannon  
*Appears when:* requires `monkhouse_done`; blocked by `m18_active`, `cross_1_done`

> **TARYN CROSS:** Taryn Cross, Exploratory Services. Monkhouse's wire said you'd come.
> **GRAYSON:** He tends to arrange things without asking.
> **TARYN CROSS:** He does. Sign here -- you're a survey contractor now.
> **GRAYSON:** That fast?
> **TARYN CROSS:** Out here paperwork is the only thing that moves fast. First assignment: the Delta system.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Four nav points, full sensor sweep at each.
> **GRAYSON:** And what a coincidence -- Delta's the first system on my map.
> **TARYN CROSS:** I noticed that too. I decided not to find it suspicious.
> **GRAYSON:** Generous of you.
> **TARYN CROSS:** Fair warning: the frontier pirates don't care for company.
> **GRAYSON:** They never do. Pay?
> **TARYN CROSS:** Ten thousand on completion. And bring the survey disc back intact -- the data's worth more than the ship.

**> OFFER:** Survey all four nav points in Delta, then report back. 10,000 credits.

**— ACCEPT —**

> **TARYN CROSS:** Welcome to Exploratory Services. Try not to make me regret the paperwork.
> **GRAYSON:** No promises.
> **TARYN CROSS:** Four navs, full sweep. Bring the disc back and we'll talk about Beta.

**— REFUSE —**

> **TARYN CROSS:** Second thoughts already?
> **GRAYSON:** It's the first system on a map that's gotten everyone who held it killed.
> **TARYN CROSS:** *(silence)*
> **TARYN CROSS:** That's fair. It's also the only way to find out why.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** I'll keep the contract open. Frontier work doesn't attract a queue.

*Outcome:* `set_flag:m18_active`

---

### Taryn Cross — `cross_m18_debrief`

*Location:* rygannon  
*Appears when:* requires `m18_active`, `m18_nav1`, `m18_nav2`, `m18_nav3`, `m18_nav4`; blocked by `cross_1_done`

> **TARYN CROSS:** Telemetry checks out. Four clean sweeps.
> **GRAYSON:** There were pirates at the third point.
> **TARYN CROSS:** They're already in my report as local color.
> **GRAYSON:** Is that what we're calling them.
> **TARYN CROSS:** It's what the brass calls anything they don't want to fund a response to. Ten thousand, as agreed.
> **GRAYSON:** And next?
> **TARYN CROSS:** Rest up. Beta is next, and Beta has a problem.

*Outcome:* `pay:10000`, `set_flag:cross_1_done`, `clear_flag:m18_active`, `clear_flag:m18_nav1`, `clear_flag:m18_nav2`, `clear_flag:m18_nav3`, `clear_flag:m18_nav4`

---

### Taryn Cross — `cross_m19_offer`

*Location:* rygannon  
*Appears when:* requires `cross_1_done`; blocked by `m19_active`, `cross_2_done`

> **TARYN CROSS:** Survey Beta. Same pattern as Delta.
> **GRAYSON:** You said Beta has a problem.
> **TARYN CROSS:** Captain Garrovick took a Centurion out that way three weeks ago.
> **GRAYSON:** And never reported back.
> **TARYN CROSS:** Never reported back.
> **GRAYSON:** Missing three weeks. You want a survey or a search party?
> **TARYN CROSS:** I want both, and I can only fund one.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Garrovick is -- was -- our best pilot. I've been careful with that tense for two weeks.
> **GRAYSON:** I'll look for him.
> **TARYN CROSS:** Ten thousand, and Services covers the recovery bonus if he's alive.

**> OFFER:** Survey Beta and find the overdue Captain Garrovick. 10,000 credits.

**— ACCEPT —**

> **TARYN CROSS:** Thank you.
> **GRAYSON:** You don't have to do that.
> **TARYN CROSS:** I know. I'd like the record to show somebody said it before you went.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Beta's four navs. Garrovick's last beacon was near the third. Bring back the disc either way.

**— REFUSE —**

> **TARYN CROSS:** I'll find someone else.
> **GRAYSON:** You said he was your best pilot.
> **TARYN CROSS:** He is. That's why I'm asking a stranger instead of my own people -- they'd fly it angry.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Think about it. He's been out there three weeks and I'm running out of ways to say 'overdue' in a report.

*Outcome:* `set_flag:m19_active`

---

### Taryn Cross — `cross_m19_debrief`

*Location:* rygannon  
*Appears when:* requires `m19_active`, `killed:garrovick`; blocked by `cross_2_done`

> **TARYN CROSS:** Well? Did you find him?
> **GRAYSON:** You should sit down.
> **TARYN CROSS:** *(silence)*
> **GRAYSON:** Here's the gun-camera footage.
> **TARYN CROSS:** That's Garrovick's ship. That's his transponder.
> **GRAYSON:** He fired first.
> **TARYN CROSS:** He fired FIRST?
> **GRAYSON:** He was raving on an open channel. I don't think he knew what he was shooting at.
> **TARYN CROSS:** What in the void does that to a man's mind?
> **GRAYSON:** I've been asking myself that the whole way back.
> **TARYN CROSS:** You did what you had to. Ten thousand.
> **GRAYSON:** I don't want it.
> **TARYN CROSS:** Take it anyway. Something out there broke him, pilot -- and Gamma is deeper in.

*Outcome:* `pay:10000`, `set_flag:cross_2_done`, `clear_flag:m19_active`

---

### Taryn Cross — `cross_m20_offer`

*Location:* rygannon  
*Appears when:* requires `cross_2_done`; blocked by `m20_active`, `cross_3_done`

> **TARYN CROSS:** Whatever unmade Garrovick is past Beta.
> **GRAYSON:** You've decided that's a fact.
> **TARYN CROSS:** I've decided it's the only lead. Survey Gamma -- four navs, full sweep.
> **GRAYSON:** And find out what.
> **TARYN CROSS:** And find out what. There's more.
> **GRAYSON:** There always is.
> **TARYN CROSS:** Intelligence flagged Kilrathi drive signatures in the area. A lot of them.
> **GRAYSON:** A corvette guarding an empty system.
> **TARYN CROSS:** We don't know that yet. That's the job.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Conserve your missiles for the last nav. Ten thousand.

**> OFFER:** Survey Gamma's four nav points. Expect heavy Kilrathi resistance. 10,000 credits.

**— ACCEPT —**

> **TARYN CROSS:** Thank you. I'll flag the Kilrathi signatures to Fleet, for all the good it does.
> **GRAYSON:** They won't act on it.
> **TARYN CROSS:** They will not. Conserve your missiles for the last nav.

**— REFUSE —**

> **TARYN CROSS:** Kilrathi put you off.
> **GRAYSON:** A lot of Kilrathi put me off.
> **TARYN CROSS:** Then we never learn what broke Garrovick, and the next pilot who flies that route finds out the same way he did.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** That was unfair of me. The contract's open when you want it.

*Outcome:* `set_flag:m20_active`

---

### Taryn Cross — `cross_m20_debrief`

*Location:* rygannon  
*Appears when:* requires `m20_active`, `m20_nav1`, `m20_nav2`, `m20_nav3`, `m20_nav4`; blocked by `cross_3_done`

> **TARYN CROSS:** Did you complete the run?
> **GRAYSON:** All four. And I found your Kilrathi.
> **TARYN CROSS:** How many?
> **GRAYSON:** A Kamekh. They posted a CORVETTE to a system with nothing in it.
> **TARYN CROSS:** *(silence)*
> **TARYN CROSS:** The cats aren't surveying, pilot. They're guarding the road.
> **GRAYSON:** The road to what?
> **TARYN CROSS:** One system left on your map. Whatever everyone is so interested in lives there.
> **GRAYSON:** Delta Prime.
> **TARYN CROSS:** Ten thousand. Go get some sleep first -- that's an order from your employer.

*Outcome:* `pay:10000`, `set_flag:cross_3_done`, `clear_flag:m20_active`, `clear_flag:m20_nav1`, `clear_flag:m20_nav2`, `clear_flag:m20_nav3`, `clear_flag:m20_nav4`

---

### Taryn Cross — `cross_m21_offer`

*Location:* rygannon  
*Appears when:* requires `cross_3_done`; blocked by `m21_active`, `cross_done`

> **TARYN CROSS:** Last one. Delta Prime.
> **GRAYSON:** And then the map's finished.
> **TARYN CROSS:** Long-range scopes show a single anomalous return.
> **GRAYSON:** Anomalous how?
> **TARYN CROSS:** Metallic. Kilometers long. Cold as the void.
> **GRAYSON:** Kilometers.
> **TARYN CROSS:** I had the figure checked three times. Survey it and come home.
> **GRAYSON:** Ten thousand?
> **TARYN CROSS:** Ten thousand. And pilot -- one more thing.
> **GRAYSON:** Go ahead.
> **TARYN CROSS:** Garrovick's last logged course was Delta Prime.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** Whatever you find out there -- do not touch it.

**> OFFER:** Survey the Delta Prime anomaly and return. 10,000 credits.

**— ACCEPT —**

> **TARYN CROSS:** Last one. Then this sector's charted and we can all go home.
> **GRAYSON:** You don't sound convinced.
> **TARYN CROSS:** I'm not. Survey it and come home, $CS. Don't touch anything.

**— REFUSE —**

> **TARYN CROSS:** One nav point. That's all that's left.
> **GRAYSON:** One nav point with something kilometers long sitting cold in it, on the heading that took your best pilot's mind.
> **TARYN CROSS:** *(silence)*
> **TARYN CROSS:** When you put it that way I'd refuse it myself.
> **GRAYSON:** *(silence)*
> **TARYN CROSS:** But somebody's going to fly it eventually. I'd rather it were someone who's read the file.

*Outcome:* `set_flag:m21_active`

---

## ACT IV - THE NAVY (Goodin / Terrell)

### Sandra Goodin — `goodin_offer`

*Location:* mining  
*Appears when:* requires `cross_done`; blocked by `m22_active`, `goodin_done`

> **SANDRA GOODIN:** Sandra Goodin, attache to Admiral Terrell, Confederation Navy.
> **GRAYSON:** I'm just passing through.
> **SANDRA GOODIN:** Sit down, pilot. That was not a request.
> **GRAYSON:** *(silence)*
> **SANDRA GOODIN:** Every long-range array in the sector is tracking a green energy signature.
> **GRAYSON:** And?
> **SANDRA GOODIN:** Every track terminates on YOUR transponder.
> **GRAYSON:** That's not possible.
> **SANDRA GOODIN:** It's on nine separate arrays. Argue with them, not with me.
> **GRAYSON:** Am I under arrest?
> **SANDRA GOODIN:** If you were, we'd be having this conversation somewhere with worse lighting.
> **GRAYSON:** The Admiral wants a word. Do I get a choice?
> **SANDRA GOODIN:** He commands this sector from Perry Naval Base. Get there. Alive, preferably -- the paperwork is shorter.

**> OFFER:** Report to Admiral Terrell at Perry Naval Base. No pay - serving the Confederation is its own reward, citizen.

**— ACCEPT —**

> **SANDRA GOODIN:** Sensible. I'll signal ahead.
> **GRAYSON:** Do I get an escort?
> **SANDRA GOODIN:** You get a heading and my personal assurance that nobody shoots you on approach.
> **GRAYSON:** That's not nothing.
> **SANDRA GOODIN:** Around here it's practically a gift. Perry Naval Base, pilot. Don't sightsee.

**— REFUSE —**

> **SANDRA GOODIN:** That's the wrong answer.
> **GRAYSON:** It's still an answer.
> **SANDRA GOODIN:** Nine arrays, pilot. Nine. You can outrun me -- you cannot outrun arithmetic.
> **GRAYSON:** *(silence)*
> **SANDRA GOODIN:** Perry Naval Base. I'll be here when you reconsider, and I will be less pleasant about it.

*Outcome:* `set_flag:m22_active`

---

### Admiral Terrell — `terrell_offer`

*Location:* perry_naval  
*Appears when:* requires `goodin_done`; blocked by `m23_active`, `terrell_done`

> **ADMIRAL TERRELL:** So YOU'RE the privateer who dragged a Kilrathi secret weapon across half my sector.
> **GRAYSON:** It isn't Kilrathi.
> **ADMIRAL TERRELL:** Don't argue -- what else could it be?
> **GRAYSON:** Whatever it is, it's older than that. A lot older.
> **ADMIRAL TERRELL:** It's green, it's hostile, and it doesn't die. That's Kilrathi engineering to the rivet.
> **GRAYSON:** It's Steltek. There's a library record and a xenoarchaeologist who'll swear to it.
> **ADMIRAL TERRELL:** I have read your file, and I have read his. I'm not interested in archaeology, I'm interested in the fact that it follows you.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** You don't deny that part.
> **GRAYSON:** No. I don't.
> **ADMIRAL TERRELL:** Good. Then we can do business. Commodore Reismann has assembled a fleet at Blockade Point Tango.
> **GRAYSON:** How many?
> **ADMIRAL TERRELL:** Two Paradigms, two Broadswords. Far enough out to keep civilian casualties down.
> **GRAYSON:** So I'm bait. Say it plainly, Admiral.
> **ADMIRAL TERRELL:** Your job is simple, privateer. The thing follows you -- so LEAD it to Tango and let the navy do what the navy does.
> **GRAYSON:** And if the navy doesn't do it?
> **ADMIRAL TERRELL:** Then you'll have died somewhere useful, which is more than most people manage. Thirty thousand credits when it's confirmed destroyed.

**> OFFER:** Lead the drone to the fleet at Blockade Point Tango. 30,000 credits on its confirmed destruction.

**— ACCEPT —**

> **ADMIRAL TERRELL:** Good man.
> **GRAYSON:** I want it in writing that this was your idea.
> **ADMIRAL TERRELL:** Goodin will draft something. It'll be a lie, but it'll be filed.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** Blockade Point Tango, Nav 1. Get there, sit still, and let it come to you.
> **GRAYSON:** And if the fleet can't kill it?
> **ADMIRAL TERRELL:** Then you'll have learned something no one else in this sector knows, and you'll have about four seconds to enjoy it.

**— REFUSE —**

> **ADMIRAL TERRELL:** I could order this, you understand.
> **GRAYSON:** I'm a civilian.
> **ADMIRAL TERRELL:** You're a civilian with a Confederation problem welded to his transponder.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** I won't force you. But that thing is going to find you eventually, and when it does you'll be alone instead of standing inside a fleet.
> **GRAYSON:** I'll take my chances.
> **ADMIRAL TERRELL:** Then take them. And when you change your mind -- Tango. Nav 1.

*Outcome:* `set_flag:m23_active`

---

### Admiral Terrell — `terrell_debrief`

*Location:* perry_naval  
*Appears when:* requires `m23_active`, `killed:steltek_drone`; blocked by `terrell_done`

> **ADMIRAL TERRELL:** Reismann's after-action report is... creative.
> **GRAYSON:** I'll bet.
> **ADMIRAL TERRELL:** It says the fleet provided a decisive containment perimeter.
> **GRAYSON:** The fleet was scrap in ninety seconds.
> **ADMIRAL TERRELL:** It also says a lone privateer did all the shooting. With an alien gun. That my analysts insist cannot exist.
> **GRAYSON:** It exists. It's bolted to my ship.
> **ADMIRAL TERRELL:** I know it exists, son. I'm deciding what the record says it was.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** You're an insubordinate smart-ass and one of the best pilots I've ever seen. Say the word and an officer's commission is yours.
> **GRAYSON:** I've spent this whole run getting out from under people who owned me.
> **ADMIRAL TERRELL:** That's a no, then.
> **GRAYSON:** That's a no.
> **ADMIRAL TERRELL:** Then you're getting the Confederation Medal of Freedom whether you like it or not.
> **GRAYSON:** And officially?
> **ADMIRAL TERRELL:** Officially, a Kilrathi prototype was destroyed by naval action at Blockade Point Tango.
> **GRAYSON:** Unofficially?
> **ADMIRAL TERRELL:** Unofficially -- here's your thirty thousand. I'm sure you're headed for trouble, and I'm sure you probably deserve it.
> **GRAYSON:** Admiral.
> **ADMIRAL TERRELL:** Good luck anyway, pilot.

*Outcome:* `pay:30000`, `set_flag:terrell_done`, `set_flag:campaign_complete`, `clear_flag:m23_active`, `clear_flag:drone_active`

---

### Admiral Terrell — `terrell_epilogue`

*Location:* perry_naval  
*Appears when:* requires `terrell_done`; blocked by `terrell_epilogue_seen`

> **GRAYSON:** Admiral.
> **ADMIRAL TERRELL:** Still here? Fine.
> **GRAYSON:** I want to know what the official history will say.
> **ADMIRAL TERRELL:** Of course you do. Goodin drafted three versions.
> **GRAYSON:** Three.
> **ADMIRAL TERRELL:** One credits the fleet. One credits classified assets.
> **GRAYSON:** And the third?
> **ADMIRAL TERRELL:** Her personal favorite. It credits an unnamed civilian contractor with an unregistered antique.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** I signed the first one.
> **GRAYSON:** Naturally.
> **ADMIRAL TERRELL:** The Confederation does not lose to a drone, privateer. And it CERTAINLY does not get rescued from one by a freelancer behind on his ship payments.
> **GRAYSON:** So none of it happened.
> **ADMIRAL TERRELL:** You happened. There just isn't a form for it.
> **GRAYSON:** *(silence)*
> **ADMIRAL TERRELL:** Now get out of my office. And -- good hunting.

*Outcome:* `set_flag:terrell_epilogue_seen`

---
