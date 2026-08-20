"""What can a cracked build actually read?

The licence used to be a branch. Forcing it true was the whole crack, which is why it survived every
release: the check was a question the binary asked itself, and a patched binary answers however it
likes.

It is now a dependency. Reading a macro needs a key; the key is issued to an account by the server;
there is no branch that produces one. A patched build answers "yes, licensed" and then finds it
cannot open a single file.

This is that rule as a table, because it is the kind of rule that is easy to state and easy to get
subtly wrong - and one wrong cell either hands the product to a pirate or takes it away from the
1,222 people who paid for it.
"""

ok = True


def check(name, good, detail=''):
    global ok
    print('%-62s %s %s' % (name, 'PASS' if good else '** FAIL **', detail))
    ok = ok and good


def readable(have_key, sealed_with, past_date):
    """Port of the rule in parseMacroFileInner.

    have_key    : the vault key this install holds, or None
    sealed_with : the key the file was sealed with, or None for a plain file
    past_date   : whether the migration deadline has passed
    """
    if sealed_with is not None:
        # Sealed: it opens with the right key and with nothing else. There is no third answer.
        return have_key is not None and have_key == sealed_with
    # Plain. Refused outright once this install has a key, and refused for everyone after the date.
    if have_key is not None or past_date:
        return False
    return True


MINE, THEIRS = 'account-A', 'account-B'

# --- a paying customer ----------------------------------------------------------------------------
check('signed in, own sealed macro: reads', readable(MINE, MINE, False))
check('  and still reads after the migration date', readable(MINE, MINE, True))
check('signed in, a macro sealed to someone else: refused', not readable(MINE, THEIRS, False))

# --- the upgrade path: nobody loses a library they paid for ----------------------------------------
# An install that has not been online since the update has a folder of plain files and no key yet.
# It has to keep working, or the change takes the product away from the people it is for.
check('offline upgrader, plain cache, before the date: reads',
      readable(None, None, False), 'this is the whole reason the grace exists')
check('  once it signs in, its cache is converted and the plain rule never bites',
      readable(MINE, MINE, False),
      'licResealCache seals the folder on the first launch with a key')

# --- a cracked build --------------------------------------------------------------------------------
# It can force the licence check true. That is now worth nothing: it has no key, so every sealed file
# is noise, and the only thing left is a plain pack someone handed it.
check('cracked build, sealed macros: refused', not readable(None, MINE, False),
      'no key, and forcing the check true does not make one')
check('cracked build, sealed macros, after the date: refused', not readable(None, MINE, True))
check('cracked build, a plaintext pack, after the date: refused',
      not readable(None, None, True), 'the last thing it had')
check('cracked build, a plaintext pack, before the date: READS - this is the open window',
      readable(None, None, False),
      'closed on the migration date, and the branch is deleted then')

# --- the rule that closes it today for anyone who has ever signed in --------------------------------
# The important cell. An install holding a key refuses plain files immediately, without waiting for
# the date - so a pirate cannot get a working product by handing a real customer's PC a plain pack
# either.
check('a key in hand refuses plaintext immediately, date or no date',
      not readable(MINE, None, False) and not readable(MINE, None, True),
      'no waiting for September')

# --- and the invariant underneath all of it ---------------------------------------------------------
states = [(k, s, d) for k in (None, MINE, THEIRS) for s in (None, MINE, THEIRS)
          for d in (False, True)]
without_key = [st for st in states if st[0] is None and readable(*st)]
check('with no key, the ONLY thing readable is a plain file before the date',
      all(st[1] is None and st[2] is False for st in without_key),
      '%d readable states out of %d' % (len(without_key), len(states)))
after = [st for st in states if st[2] and st[0] is None and readable(*st)]
check('  and after the date, nothing at all', not after,
      'a build with no account reads nothing, whatever it patches')

print()
print('ALL CHECKS PASS' if ok else 'SOMETHING IS WRONG - do not ship')
