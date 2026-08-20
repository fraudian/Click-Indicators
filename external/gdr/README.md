# Third-party: libGDR (GDReplayFormat)

These three headers are **not** written by the Click Indicators developer. They are vendored
from maxnut's GDReplayFormat, the reference implementation of the `.gdr` / `.gdr2` replay
format used by most Geometry Dash bots.

- Upstream: <https://github.com/maxnut/GDReplayFormat>
- Files: `gdr.hpp`, `binarystream.hpp`, `physics_extension.hpp`
- Modifications: none. Vendored verbatim.

## Licence status

**The upstream repository has no LICENSE file and no copyright notice.** Under default
copyright that means all rights are reserved, so strictly speaking there is no grant of
permission to redistribute these headers — including inside this mod.

This is recorded here rather than glossed over. The upstream readme describes the format as
"meant to be usable by all bots", which suggests the omission is an oversight rather than an
intent to restrict, but intent is not a licence.

Steps being taken:

1. A request has been sent to the upstream author asking for an explicit permissive licence
   (see `docs/libgdr-license-request.md` for the text).
2. If the author declines, or does not respond, these headers will be replaced with an
   independent implementation of the format. The `.gdr2` container is documented and small
   enough that a clean-room parser is practical — `src/gdr_parse.cpp` already handles several
   other macro formats without any vendored code.

Click Indicators is a paid, closed-source mod, so this is not a theoretical concern and is
not being left unresolved.

## Why it is used at all

`.gdr2` is the most common macro format. Parsing it is what lets the mod show a player when
to click. Only the read path is used — nothing here records or replays inputs.
