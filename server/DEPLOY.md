# Backend + site deploy

Two Cloudflare Workers and one D1 database. No server to maintain.

| Worker | Config | Serves |
|---|---|---|
| `clickindicators-api` | `server/wrangler.jsonc` | `clickindicatorsmod.com/api/*` — accounts, Stripe, licences, the paid download |
| `autumn-paper-2321` | `wrangler.site.jsonc` | everything else — `index.html`, `account.html` |

The mod is **not on the Geode index**, so the `.geode` is sold and delivered here. It is
bundled into the API Worker and served by `/api/download`, which requires a signed-in,
paid session. It is deliberately **not** a file in `website/` — a static asset cannot be
gated, and the whole point is that it is paid for.

Only the newest build is downloadable. Older versions are listed on the dashboard with
their release notes so people can see what they missed, but never offered: supporting a
build whose bugs are already fixed costs more than it is worth.

`.gitignore` covers `*.geode`, so `server/release/jackz.click-indicators.geode` is not in
git — it is put there by `release.sh`. A fresh clone therefore cannot deploy the API
Worker until `release.sh` has run once, and the bundle fails loudly rather than quietly
deploying without a download. Release notes (`server/release.js`) *are* committed.

---

## Launch checklist

Work top to bottom. Anything marked **you** cannot be done from the agent side — it needs
a browser login or a secret that must never pass through chat.

### 1. Database — **you**
The verification migration may not be applied to the live database yet. It is safe to
re-run; both statements are idempotent apart from the `ALTER`, which errors harmlessly if
the column already exists.

    wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file=./server/migrate-verify.sql
    wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file=./server/migrate-one-device.sql

Then confirm the tables are all there:

    wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc \
      --command "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"

Expect: `device_signouts, devices, email_codes, login_attempts, purchases, sessions,
stripe_events, users`. If `email_codes` is missing, signup returns a 500 the moment a
mailer is configured. A missing `device_signouts` does not break anything — sign-in still
works and a displaced install just falls back to a generic "signed out" message instead of
being told the licence moved.

### 2. Email — **you**
Signup emails a six-digit code, and `/api/checkout` refuses an unverified account. With no
mailer configured the Worker auto-verifies instead, so buying still works — it just means
anyone can sign up with an address they do not own.

Either configure one:

    wrangler secret put RESEND_KEY -c server/wrangler.jsonc

…or accept auto-verification for launch. Decide before launch, not after: turning a mailer
on midway strands anyone who signed up in between on the verify screen. The dashboard has
the code-entry UI either way.

### 3. Stripe live mode — **you**
In the Stripe dashboard, switch from **Test** to **Live**, then:

- Recreate the $2.99 one-time product. Copy the **live** `price_...` id and put it in
  `server/wrangler.jsonc` under `vars.STRIPE_PRICE_ID` (price ids are not secret).
- Settings → Business → set the statement descriptor, e.g. `CLICKINDICATORS`. An
  unrecognisable name on a bank statement is a leading cause of chargebacks.
- Developers → Webhooks → add `https://clickindicatorsmod.com/api/stripe/webhook` with
  `checkout.session.completed`, `charge.refunded`, `charge.dispute.created`. Copy the
  **live** signing secret.

Then, without pasting either value anywhere else:

    wrangler secret put STRIPE_SECRET         -c server/wrangler.jsonc   # sk_live_...
    wrangler secret put STRIPE_WEBHOOK_SECRET -c server/wrangler.jsonc   # whsec_... (live endpoint)

**The test-mode webhook secret has been exposed and should be rotated in Stripe**, even
though a live one replaces it here.

There is no "we are live" flag in the pages any more. `/api/release` reports `live: true`
when `STRIPE_SECRET` starts with `sk_live_`, and both pages read it. A test key shows
"Launching soon" and disables the buy button, so the site cannot sell for real money
through a test key, and there is no second flag to forget.

### 4. Ship the build
`release.sh` takes the artifact from the GitHub Actions run — never a local build, which
bakes home-directory paths into the binary and is how the first index submission was
spotted as locally compiled.

    ./release.sh ~/Downloads/"Click Indicators.zip"

That unwraps the artifact, refuses it if it carries local build paths, regenerates
`server/release.js` from `changelog.md`, bundles the `.geode` into the API Worker, and
deploys both Workers.

The published version is read from the artifact's own `mod.json`, not the repo's, and the
release must have a `## vX.Y.Z` section in `changelog.md` — the dashboard shows those
notes verbatim.

### 5. Verify, before telling anyone

    curl -si https://clickindicatorsmod.com/api/download | head -1   # 401 — the gate holds
    curl -s  https://clickindicatorsmod.com/api/release              # version + live:true

Then, signed in on a paid account, download from the dashboard and check the file's
SHA-256 matches the one shown beside it.

Buy once with a real card on a spare account and confirm `paid` flips. If it does not,
`wrangler tail -c server/wrangler.jsonc` shows where the webhook stopped — every branch
logs. The usual cause is a signing secret belonging to the other mode's endpoint.

### 6. The mod itself
`CI_DEV_UNLOCK` must be OFF in `../CMakeLists.txt`; CI hard-fails if it is on, and it has
been confirmed absent from the shipped DLL.

---

## Routine release

    # bump version in mod.json, add its section to changelog.md, push, wait for CI
    ./release.sh <artifact from the Actions run>

Buyers see the new version and its notes on the dashboard immediately; nothing needs to be
sent out. There is no auto-update — the mod does not replace its own file — so announcing
it in the Discord is what actually moves people onto it.

## Refunds and chargebacks
Handled automatically: `charge.refunded` and `charge.dispute.created` clear `paid` and
delete every device row, so the mod stops working at its next licence check.

## First-time setup (already done, kept for reference)

    npm i -g wrangler
    wrangler login
    wrangler d1 create clickindicators          # id goes in server/wrangler.jsonc
    wrangler d1 execute clickindicators --remote -c server/wrangler.jsonc --file=./server/schema.sql
