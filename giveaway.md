# Giveaway — everything needed to run it

25 keys. Awarded for contribution, not raffled. ~15 named now, the rest held back for the next
fortnight so it doesn't look like a one-off stunt.

---

## 1. The post — #announcements

**25 free keys, for the people who actually fixed this mod**

Five of the nine reviews in ⭐│vouches said the same thing in different words — the timing felt a
frame or two off. You were right. Over the last two days I went and found out why, and almost none
of it was the indicator itself:

- Silicate macros were running **9× too fast** — the mod was throwing away the tick rate the file
  declares. One real file says 2229; a 59-second run was being spread over 552 seconds.
- Mega Hack duals were **half-destroyed** — 17,565 player-two presses out of 35,918 were being read
  as releases.
- zBot duals were **merged into one stream**, swallowing 3,400 presses on one macro.
- iCreate Pro macros were driving the guide off **the wrong player entirely**.
- Start Pos was up to **77 frames out** deep in a level, and forgot the correction every time you
  switched.
- Recordings were **thrown away** if you left the level instead of pressing STOP first.

Every single one of those was found from something a person in this server sent me — a log, a macro
file, or a report precise enough to chase. So:

**Keys are going to the people who did that.** Not a raffle. If your name is below, open a ticket in
🎫│help with the email on your clickindicatorsmod.com account and I'll flip it to paid.

- **JugBongo** — debug logs from his Mac, then wrote his own test script to help. The Start Pos work
  is his.
- **omg_at_il_a** — worked out that recordings were lost when you exit instead of pressing STOP.
  Fixed in v1.0.19.
- **theodore1646** — reported the delay on Start Pos copies. Fixed.
- **capybaraobssesed** — macro search failing on first use.
- **cz4h** and **drewmp4am** — both independently said single presses are hard to see on bright and
  fast levels. Two people, same problem, so it's real and it's next.
- **sterminatore__** — the most useful review anyone has written, and the reason 💡│suggestions is a
  forum now.
- **noctenon**, **rooksgd**, **sx.svnjesus**, **n1hy**, **supernova0652**, **mapgao**,
  **smilerfromstatefarm** — for honest reviews, including the unflattering ones. The 5.5/10 was more
  useful than the 10/10.
- **918c** — the idea for the high-CPS ship and wave display.
- **race8248** — found a Mac install failure. Sorry it sat for three days; check your ticket.

**Ten more are unclaimed.** They go to: anyone who sends a macro file that reproduces a bug, anyone
who posts an honest review in ⭐│vouches (honest, not positive — the 5.5 got a key), and one
specifically for whoever can send me a **.gdr2 file that fails to load** — that's the last bug I
know about and can't reproduce.

If you already bought it, you're not getting a refund out of this, and I'm not pretending that's
generous. What you get is a mod that reads your macros correctly now.

---

## 2. Claim flow

Grants are by **email**, not Discord name, so a winner has to give you one.

1. Winner opens a ticket in 🎫│help
2. They post the email on their clickindicatorsmod.com account (tell them to sign up first if they
   haven't — free, no card)
3. Run the grant (below)
4. Reply "done, sign out and back in inside the mod"

Their Discord role is applied automatically if they've linked Discord — `grantAccess` does it.

---

## 3. The command

```
python server/grant-licence.py grant their@email.com "giveaway: <what they did>"
```

It prints a wrangler command; read it, then run it. It records the key as a deliberate **0** with a
reason attached rather than just flipping `paid = 1` — so six months from now these are still
distinguishable from a payment that half-failed.

Put the real reason in, not "giveaway". `"giveaway: reported recordings lost on level exit"` is what
you want to be reading later.

To take one back: `python server/grant-licence.py revoke their@email.com`

---

## 4. DM template for each winner

> You're getting a free Click Indicators key — you earned it for <the specific thing>.
>
> Open a ticket in 🎫│help with the email on your clickindicatorsmod.com account (sign up first if
> you haven't, it's free) and I'll switch it on. If you've linked Discord you'll get the role
> automatically.

---

## 5. Two judgement calls left to you

**20stu.** does more support work than anyone and isn't on the list. He's also called customers
"retarded" and "slow" in #general today. Rewarding him publicly right now endorses that in front of
2,500 people. My suggestion: give him one privately, and have the other conversation separately.

**race8248** is on the list but he already paid. A key is not what he's owed — a working install or
a refund is. Fix his ticket first, then the key is a goodwill extra rather than a substitute.
