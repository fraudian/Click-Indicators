# t.me/gdmacros mirror

Adds the **GD Macros (ГД Макрос)** Telegram channel as a second macro source alongside
hyperbolus.net, with the channel owner's permission.

## Why this is a mirror and not a proxy

hyperbolus works as a read-through proxy because it has a public HTTP API. Telegram does not.
Verified directly across 137 preview pages and 509 file-bearing posts: every document link in
`t.me/s/gdmacros` points at the message page, and the only real CDN URLs on it are photo
thumbnails. File bytes require MTProto (a logged-in user account) or the Bot API (a bot that
administers the channel, which we are not).

So the bytes are pulled out-of-band into R2, and the Worker serves them from there behind the
licence gate it already has.

**The mod never talks to Telegram and contains no Telegram credential.** That is not incidental:
the mod originally fetched macros straight from hyperbolus with no credential, and a four-byte
patch to the licence branch turned it into a fully working free product. Putting a Telegram
fetch in the client would rebuild that hole. The credential lives on the ingest host only — not
in the Worker, not in D1, not in the binary.

## Pipeline

```
index_pass.py   →  work.db     no credential. Scrapes the public preview for message ids,
                               captions, level ids, filenames and exact sizes.
mirror_telegram.py → blobs/    the only stage that touches Telegram. Downloads one file per
                               post, decodes and validates it, content-addresses it by sha256.
sync_d1.py      →  R2 + D1     uploads blobs, emits injection-proof SQL for the index.
```

Each stage is resumable and safe to re-run; `work.db` carries the state.

## Which format gets mirrored

One file per post, `.gdr2` preferred, `.slc` as fallback — both already parsed by
`src/parsers.cpp`.

| format      | coverage | median size | mirrored |
|-------------|----------|-------------|----------|
| `.gdr2`     | newest ~5% (from 2026-06-27) | 581 B | **yes, first choice** |
| `.slc`      | 82.4% of all posts | 2.3 KB | **yes, fallback** |
| `.gdr.json` | 48% | 4.4 MB | no — per-frame physics state, up to 620 MB |
| `.tcm`      | 78% | 1.3 KB | no parser |
| `.cml`      | 42% | 64 KB | no parser |
| `.ybot`     | discontinued 2026-05-31 | 956 B | no parser |

`.gdr2` is ~7,500× smaller than the `.gdr.json` on the same post because it stores input
transitions rather than every physics frame — measured at **1.257 bytes per input** across 29,936
inputs in 47 real files. That is all a click cue needs. Whole-channel mirror is roughly **2,000
files / 7 MB**.

The channel changed its format mix three times, so nothing here is hard-coded to "the current
five extensions" — each post is scanned for the best supported format and the reason for passing
is recorded in `skipped`.

## Running it

```bash
pip install telethon
# api_id / api_hash from https://my.telegram.org → API development tools
set TG_API_ID=...
set TG_API_HASH=...

python mirror_telegram.py --login        # once, interactive
python index_pass.py --full              # ~780 pages, no credential, a few minutes
python mirror_telegram.py --limit 20     # PILOT FIRST - check the output before committing
python mirror_telegram.py                # full backfill, ~20-40 min
python sync_d1.py --upload
python sync_d1.py --sql                  # then run the printed wrangler command
```

`index_pass.py` has two modes, because the preview can only page **backwards**:

| command | does |
|---|---|
| `index_pass.py` | incremental. Starts at the top, stops after two pages with nothing new. This is the one to schedule. |
| `index_pass.py --full` | backfill. Walks to the start of the channel. |
| `index_pass.py --full --resume` | continues an interrupted backfill from its cursor. |

"Resume from the cursor" is the wrong shape for picking up new posts — the cursor points at the
oldest post seen, and new posts arrive at the other end. That is why the incremental mode always
starts from the top instead.

Rows land with `serve = 0` and are **invisible to the mod** until approved. Review, then:

```bash
npx wrangler d1 execute clickindicators --remote -c ../server/wrangler.jsonc \
  --command "UPDATE tg_macros SET serve = 1 WHERE serve = 0"
```

Use `--approve` on `sync_d1.py --sql` to skip that step for a backfill you have already reviewed.

For incremental runs, `index_pass.py` with no arguments resumes from its cursor. Leave new posts
at `serve = 0` for a day or two rather than approving on sight — see below.

### Kill switch

```bash
# stop serving one macro
--command "UPDATE tg_macros SET serve = 0 WHERE msg_id = 15639"
# stop serving the whole source
--command "UPDATE tg_macros SET serve = 0"
```

Takes effect on the next list request. Note it does not delete files already cached on a
customer's disk.

## Why the paranoia in sync_d1.py

Anyone can put text into this channel through `@GDmacrosBot`, and `wrangler d1 execute --file` is
multi-statement with full account privilege against the database holding `users.paid` and
`devices.token_hash`. A level name containing a quote and a semicolon would not be a display bug —
it would be arbitrary SQL against production, and the obvious payload mints a paid device row.

So no scraped text is ever interpolated as SQL text. Strings are emitted as `CAST(x'..' AS TEXT)`
hex literals, which have no quoting to escape, every number goes through `int()`/`float()`,
`r2_key` is rebuilt from the sha256 rather than carried, and the generated file is rejected unless
every line is exactly one complete statement.

The same reasoning drives `serve = 0` by default: a brand-new post must not be able to become the
file that auto-loads on 800+ machines within the hour.

## Validation at ingest

`.gdr2` is fully decoded with the same header layout `src/gdr_parse.cpp` reads, and rejected if:

- the magic is not `GDR`, or the header does not decode
- it declares **zero inputs** — EclipseBot writes structurally valid 60–80 byte stubs, and one of
  those would satisfy the client's cache check and leave a level with a permanently blank guide
  and no error message. Three such files exist in local test data.
- the framerate is not finite and within `(1, 100000]`
- it asserts a level id that disagrees with the caption

`.slc` carries no embedded level id and no cheap input count, so it is magic-and-size checked only
and always lands with `verified = 0`. The Worker ranks `verified` first, so a self-verifying file
always beats a caption-only one.

Size caps are applied to the size the **preview** reported, before anything is downloaded:
256 KB for `.gdr2`, 1.5 MB for `.slc`. The channel really does contain a 620 MB `.gdr.json`.

## Album ids: the thing that will bite you

A post's five documents occupy **five consecutive message ids**, but the web preview renders them
as ONE bubble whose `data-post` is only the **first** document. For the MinAY post:

```
15638  caption (photo)
15639  MinAY.gdr.json   5.7 MB   <- the bubble reports THIS id
15640  MinAY.slc        2.1 KB
15641  MinAY.tcm        1.1 KB
15642  MinAY.gdr2        551 B   <- the one we actually want
15643  MinAY.cml      111.6 KB
```

Downloading by the bubble id fetches the multi-megabyte `.gdr.json` every single time, which then
fails the size cap and marks the post permanently skipped. The real id is only in the per-document
`href`, which is why `DOC_RE` captures it and `posts.doc_msg_id` exists separately from
`posts.msg_id`.

Do not collapse those two columns. `msg_id` is the post's identity and is what `tg_macros.msg_id`,
the `/api/macro?t=` route and the kill switch all key on; `doc_msg_id` is only for the download.
`mirror_telegram.py` additionally checks that the filename Telegram returns matches the one that
was indexed, so a future renumbering fails loudly on the first post instead of quietly on all of
them.

## The pairing rule, and why it is checked

One post is two message bubbles: the caption sits on a photo at id *N*, the five documents are a
separate bubble at *N+1* with empty text. Reading the caption off the bubble holding the files
gets an empty string every time.

That offset is an undocumented property of Telegram's HTML rendering. It is correct on 100% of
current-era posts, but if it ever shifts, every row would silently take the previous post's level
id — and for `.slc`, which is most of the archive, there is no embedded id to catch it. So the
index cross-checks the caption's level name against the attached filename (they agree on 99.2% of
real posts) and records `pair-unverified` rather than guessing.

A sudden rise in that skip reason is the signal that Telegram changed its markup.
