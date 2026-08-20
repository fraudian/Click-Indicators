# Discord posts — ready to paste

Two posts. The first is the one that matters: it answers the most common criticism in your
reviews with specifics rather than argument.

---

## POST 1 — for #announcements (or #changelog)

**Click Indicators v1.0.19 — the timing complaints were right**

Five of the nine reviews in ⭐│vouches said some version of the same thing:

> "the perfect timings are sometimes off by a frame or two" — noctenon
> "perfect clicks feel off" — drewmp4am
> "a bit of off sync" — rooksgd
> "issues with click registration ... ship or wave spam" — sterminatore__
> "calibration is kinda bad" — sx.svnjesus

You were right, and I found out why. Most of it wasn't the indicator at all — it was the macro
being read wrong before it ever got drawn.

**What was actually broken**

- **Silicate macros were running 9× too fast.** The mod threw away the tick rate the file
  declares if it was above 2000, and Silicate records at the physics rate. One real file declares
  2229, so a 59-second run was being spread across 552 seconds. Every cue but the first fifteen
  landed past the end of the level.
- **Mega Hack files hit the same ceiling** — one declaring 3500 ticks was stretched from 70
  seconds to 1027.
- **Dual Mega Hack macros were half-destroyed.** The player number sits in the byte right after
  the hold flag, and the mod read the two together as one value, so every player-two press
  registered as a release. On one real file that's 17,565 records out of 35,918.
- **Dual zBot macros were merged into one stream** — the check for player two looked for a
  character no zBot file has ever contained. 3,400 presses swallowed on one macro.
- **iCreate Pro macros were driving the guide off the wrong player entirely.**
- **Start Pos was up to 77 frames out** deep in a level, and reset itself every time you switched
  Start Pos. It now remembers, and fills in from Start Positions it has already measured.
- **Recordings were being thrown away** if you left the level instead of pressing STOP in the
  pause menu first — which is how a practice session actually ends. It asks now.

**If you use .slc, .mhr, .zbf or any dual macro, this is a different mod today than it was
yesterday.** If your timing still feels off after updating, please open a ticket with the macro
file — every one of the bugs above was found from a real file somebody sent in, and the last
report I couldn't reproduce is a `.gdr2` that someone said didn't work.

Update from your account page at clickindicatorsmod.com.

**On the outage earlier** — the API was down for about three hours. Somebody pointed two million
requests at the signup endpoint from two IPs, which exhausted the request allowance and took
logins down with it. Nothing was breached: every one of those requests was rejected by the bot
check before it touched anything, no accounts were created and no data was accessed. It's blocked
at the network edge now. Sorry to everyone who couldn't log in.

---

## POST 2 — pin this in #faq

**Settings you probably already have**

Going through the suggestions channel, six of the most-requested features already exist. That's
my fault for hiding them, not yours for asking. Here they are:

**"Can I use more than one indicator at once?"**
Yes — turn OFF **"Only use lines in Classic"** in the first settings tab. Then Highway (or any
style) draws alongside the Classic lines.

**"Can I turn the indicators off for part of a level?"**
Yes — **"Hide guide from"** and **"Hide guide to"**. Set 0 and 20 to leave the opening bare, or
20 and 50 to blank out the middle.

**"Can I remove a single click indicator I don't want?"**
Yes — you can mute individual clicks. A muted click gets no cue, no sound, and isn't graded.

**"Is there mobile support?"**
Yes, Android — the same download. Install it from inside the game: Geode's mods screen has an
"Install from file" button. No computer needed. **iOS isn't possible** and isn't planned.

**"Can I pay with PayPal?"**
Yes, on the account page, alongside card.

**"Is there macOS support?"**
Yes, since v1.0.6 — Intel and Apple Silicon, same download as everything else.

**Where do I click on the Highway style?**
At the **end** of the lane, where the notes arrive at the strike bar — not when they appear.

If something you want genuinely isn't in there, 💡│suggestions is the right place and I do read
all of it.
