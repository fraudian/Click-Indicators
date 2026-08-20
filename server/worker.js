/**
 * Click Indicators backend - Cloudflare Worker + D1.
 *
 * Buy on the website with Stripe, then log in inside Geometry Dash with the same
 * email and password. The mod trades those credentials once for a device token and
 * never stores the password.
 *
 * Secrets (wrangler secret put ...):
 *   STRIPE_SECRET         sk_live_... / sk_test_...
 *   STRIPE_WEBHOOK_SECRET whsec_...
 *   STRIPE_PRICE_ID       price_...
 * Binding: DB -> the D1 database.
 */

import { LATEST, HISTORY } from "./release.js";
import GEODE_BYTES from "./release/jackz.click-indicators.geode";

const JSON_HEADERS = { "content-type": "application/json; charset=utf-8" };
// Workers caps PBKDF2 at 100k iterations per call, which is below what you'd want on
// a server you control. Two chained rounds gets 200k of effective work while staying
// inside the platform limit.
const PBKDF2_ITERS  = 100000;
const PBKDF2_ROUNDS = 2;
// Sign-in hashes against this when the address has no account, so a miss costs the same 200k
// iterations as a hit. Without it the "no such user" path short-circuited and answered in a
// fraction of the time, which told anyone who asked whether a given person is a customer -
// and that list is exactly what an attacker needs before guessing passwords at it.
const DUMMY_SALT = "Q2xpY2tJbmRpY2F0b3JzRHVtbXk=";
const SESSION_DAYS = 30;
const DEVICE_DAYS  = 365;
// One device at a time. Signing in inside the game takes the licence off whatever
// machine held it rather than claiming another slot. The old five-slot rule meant a
// buyer could hand their email and password to four friends and everyone played.
const MAX_DEVICES  = 1;
// A displaced install is told why at its next check, so it is kept for a while after
// the device row itself is gone. Long enough to outlast a holiday, short enough that
// the table stays small.
const SIGNOUT_KEEP_DAYS = 90;

const enc = new TextEncoder();
const now = () => Math.floor(Date.now() / 1000);

function json(obj, status = 200, extra = {}) {
  return new Response(JSON.stringify(obj), { status, headers: { ...JSON_HEADERS, ...extra } });
}
function b64(buf) {
  return btoa(String.fromCharCode(...new Uint8Array(buf)));
}
function randB64(n = 32) {
  const a = new Uint8Array(n);
  crypto.getRandomValues(a);
  return b64(a).replace(/[+/=]/g, (c) => ({ "+": "-", "/": "_", "=": "" }[c]));
}
// --- signed licence grants ---------------------------------------------------------------------
//
// The mod used to decide for itself whether it was licensed: licGate() recomputed an FNV checksum
// over the saved values, using a seed compiled into the binary. Anything the binary can compute,
// a cracker can compute - so the crack was four values in a save file, no patch anywhere, and it
// survived every release untouched. That is why cracks kept reappearing the day each build shipped.
//
// A grant is signed here with a key that exists only in this Worker's secret store. The mod carries
// the matching PUBLIC key, which verifies a grant and cannot produce one. There is no local edit
// that yields a valid grant any more; forging one needs this private key, and patching the verifier
// out is a code change that has to be redone against every new binary.
//
// The grant binds to the device token, so it is not transferable on its own: a copied grant without
// the matching token fails, and copying both is sharing an account that only works on one device at
// a time.
const GRANT_DAYS = 21;                       // outer bound; the mod also keeps its own 14-day grace
let LIC_KEY = null;

async function licSignKey(env) {
  if (LIC_KEY) return LIC_KEY;
  if (!env.LIC_SIGN_KEY) return null;        // not configured yet - endpoints still work, unsigned
  try {
    const raw = atob(env.LIC_SIGN_KEY.trim());
    const der = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) der[i] = raw.charCodeAt(i);
    LIC_KEY = await crypto.subtle.importKey("pkcs8", der, { name: "Ed25519" }, false, ["sign"]);
    return LIC_KEY;
  } catch (e) {
    // A malformed secret, or a runtime without Ed25519. Either way this must not become a 500:
    // sign-in and the half-hourly check both run through here, and a 500 on those is every
    // customer's licence failing to revalidate until the offline grace runs out.
    console.log("LIC signing key unusable:", e && e.message);
    return null;
  }
}

function b64u(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

// 56 bytes: "CIG1" | exp i64le | SHA-512(token)[0..32] | issued i64le | version u32le
async function grantPayload(token, seconds, at) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-512", enc.encode(token)));
  const p = new Uint8Array(56);
  p.set([0x43, 0x49, 0x47, 0x31], 0);
  const dv = new DataView(p.buffer);
  dv.setBigInt64(4, BigInt(at + seconds), true);
  p.set(digest.slice(0, 32), 12);
  dv.setBigInt64(44, BigInt(at), true);
  dv.setUint32(52, 1, true);
  return p;
}

// The macro vault key: one per ACCOUNT, generated on first use.
//
// Per account rather than per device, deliberately. The device token is regenerated on every
// sign-in, and signing in on another PC and back is routine now that it is one device at a time -
// keying the cache on the token would wipe a customer's downloaded macros every time they did it.
//
// Handing this out is what makes a leaked macro pack worthless: reading one needs a key that only
// an account gets, and an account plays on one device at a time and can be revoked.
async function vaultKeyFor(env, userId) {
  // EVERYTHING in here is inside the try, including the first read.
  //
  // It was not, and that was a live hazard: on a database where migrate-vault.sql has not run,
  // "SELECT vault_key" throws, the throw walks up through modLogin and modCheck to the top-level
  // handler, and every one of them answers 500. The mod treats a 500 as an outage rather than a
  // revocation, so nobody is signed out on the spot - but no licence revalidates either, and
  // fourteen days later the grace runs out for all of them at once. Sign-in would have failed
  // outright from the first minute.
  //
  // Deploy order should be migration first regardless. This is so that getting it wrong costs
  // nothing rather than costing every customer.
  try {
    const row = await env.DB.prepare("SELECT vault_key FROM users WHERE id = ?")
      .bind(userId).first();
    if (row && row.vault_key) return row.vault_key;
    const k = randB64(32);
    await env.DB.prepare("UPDATE users SET vault_key = ? WHERE id = ? AND vault_key IS NULL")
      .bind(k, userId).run();
    const back = await env.DB.prepare("SELECT vault_key FROM users WHERE id = ?")
      .bind(userId).first();
    return (back && back.vault_key) || k;
  } catch (e) {
    // No column yet. Sending no key means the mod writes plain files exactly as it does today,
    // so a half-applied deploy degrades instead of breaking.
    console.log("VAULT unavailable (has migrate-vault.sql run?):", e && e.message);
    return null;
  }
}

async function makeGrant(env, token) {
  try {
    const key = await licSignKey(env);
    if (!key) return null;
    const p = await grantPayload(token, GRANT_DAYS * 86400, now());
    const sig = new Uint8Array(await crypto.subtle.sign({ name: "Ed25519" }, key, p));
    return b64u(p) + "." + b64u(sig);
  } catch (e) {
    // No grant is a licence that keeps working on the path it already used. A thrown exception is
    // a 500, and a 500 here is 1,348 licences that stop revalidating.
    console.log("LIC grant not issued:", e && e.message);
    return null;
  }
}

async function sha256b64(s) {
  return b64(await crypto.subtle.digest("SHA-256", enc.encode(s)));
}
/** Compare two strings without leaking where they diverge. */
function timingSafeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

async function hashPassword(password, saltB64) {
  const salt = Uint8Array.from(atob(saltB64), (c) => c.charCodeAt(0));
  let material = enc.encode(password);
  for (let i = 0; i < PBKDF2_ROUNDS; i++) {
    const key = await crypto.subtle.importKey("raw", material, "PBKDF2", false, ["deriveBits"]);
    const bits = await crypto.subtle.deriveBits(
      { name: "PBKDF2", salt, iterations: PBKDF2_ITERS, hash: "SHA-256" }, key, 256);
    material = new Uint8Array(bits);
  }
  return b64(material);
}

function validEmail(e) {
  return typeof e === "string" && e.length <= 254 && /^[^@\s]+@[^@\s.]+\.[^@\s]+$/.test(e);
}

/**
 * Reject a password the game cannot accept.
 *
 * Sign-in inside Geometry Dash goes through a cocos text field that only produces printable
 * ASCII. A password containing an accented letter or any other non-ASCII character can be set
 * happily on this website and then never typed in the game - which is the only place it is
 * needed. Several paying customers hit precisely that: a working website session, and a
 * string of failed in-game attempts they had no way to explain.
 */
function typeablePassword(p) {
  return typeof p === "string" && /^[\x20-\x7E]+$/.test(p);
}
const PW_HINT = "Password can only use ordinary keyboard characters - no accents or emoji, " +
                "because the sign-in box inside Geometry Dash cannot type them.";

/* ------------------------------------------------------------------ *
 * rate limiting
 * ------------------------------------------------------------------ */
async function checkLock(db, ident) {
  const r = await db.prepare("SELECT fails, until FROM login_attempts WHERE ident = ?")
    .bind(ident).first();
  if (r && r.until > now()) return r.until - now();
  return 0;
}
async function noteFail(db, ident) {
  const r = await db.prepare("SELECT fails, until FROM login_attempts WHERE ident = ?")
    .bind(ident).first();
  // Forget an old run of failures. Without this the count only ever rose, so a handful of
  // mistyped passwords spread over months eventually put an account or an address into the
  // maximum backoff permanently.
  const stale = r && r.until > 0 && now() - r.until > 3600;
  const fails = (r && !stale ? r.fails : 0) + 1;
  // 5 free tries, then back off: 30s, 2m, 8m, 30m ...
  const until = fails > 5 ? now() + Math.min(1800, 30 * Math.pow(4, fails - 6)) : 0;
  await db.prepare(
    "INSERT INTO login_attempts (ident, fails, until) VALUES (?, ?, ?) " +
    "ON CONFLICT(ident) DO UPDATE SET fails = ?, until = ?"
  ).bind(ident, fails, until, fails, until).run();
}
async function clearFails(db, ident) {
  await db.prepare("DELETE FROM login_attempts WHERE ident = ?").bind(ident).run();
}

/* ------------------------------------------------------------------ *
 * website sessions (cookie)
 * ------------------------------------------------------------------ */
function cookieOf(req, name) {
  const raw = req.headers.get("cookie") || "";
  for (const part of raw.split(";")) {
    const [k, ...v] = part.trim().split("=");
    if (k === name) return v.join("=");
  }
  return null;
}
async function userFromSession(env, req) {
  const tok = cookieOf(req, "ci_session");
  if (!tok) return null;
  const row = await env.DB.prepare(
    "SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id " +
    "WHERE s.token_hash = ? AND s.expires > ?"
  ).bind(await sha256b64(tok), now()).first();
  return row || null;
}
async function newSession(env, userId) {
  const tok = randB64(32);
  await env.DB.prepare("INSERT INTO sessions (token_hash, user_id, expires) VALUES (?, ?, ?)")
    .bind(await sha256b64(tok), userId, now() + SESSION_DAYS * 86400).run();
  return tok;
}
function sessionCookie(tok) {
  return `ci_session=${tok}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=${SESSION_DAYS * 86400}`;
}

/* ------------------------------------------------------------------ *
 * Stripe
 * ------------------------------------------------------------------ */
async function stripe(env, path, params) {
  const body = new URLSearchParams(params).toString();
  const r = await fetch("https://api.stripe.com/v1/" + path, {
    method: "POST",
    headers: {
      authorization: "Bearer " + env.STRIPE_SECRET,
      "content-type": "application/x-www-form-urlencoded",
    },
    body,
  });
  return { ok: r.ok, data: await r.json() };
}

/**
 * Verify Stripe's signature header. Without this anyone could POST a fake
 * "payment succeeded" and grant themselves the mod.
 */
async function stripeSigOK(env, payload, header) {
  if (!header) return false;
  let t = null; const sigs = [];
  for (const part of header.split(",")) {
    const [k, v] = part.split("=");
    if (k === "t") t = v;
    else if (k === "v1") sigs.push(v);
  }
  if (!t || !sigs.length) return false;
  if (Math.abs(now() - parseInt(t, 10)) > 300) return false;      // replay window
  const key = await crypto.subtle.importKey(
    "raw", enc.encode(env.STRIPE_WEBHOOK_SECRET), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, enc.encode(`${t}.${payload}`));
  const hex = [...new Uint8Array(mac)].map((b) => b.toString(16).padStart(2, "0")).join("");
  return sigs.some((s) => timingSafeEqual(s, hex));
}



/* ------------------------------------------------------------------ *
 * Turnstile
 *
 * Runs before the real handler and fails CLOSED: a siteverify that errors,
 * times out or returns the wrong action/hostname is treated as a failure.
 * The hostname allowlist is production only - accepting localhost here would
 * let anyone mint tokens on a local page and replay them against production.
 * ------------------------------------------------------------------ */
// The site's own hostnames. Both are ours; the second exists because consumer ISPs have been
// seen resetting any connection whose TLS SNI names clickindicatorsmod.com - two unrelated home
// networks so far - which leaves those customers able to run the mod (it fails over on its own)
// but unable to reach the site to buy it. A name those networks do not inspect is the only
// remedy available from our side, since the filtering is on theirs.
//
// This list is a replay guard, not an access control: it stops a token minted on somebody else's
// page being spent here. Adding a hostname we serve ourselves does not weaken it. Adding one we
// do not control would.
const TURNSTILE_HOSTS = new Set([
  "clickindicatorsmod.com",
  "clickindicators.com",
  "www.clickindicators.com",
  // The failover proxy's own address, which serves the whole site: static pages from the site
  // Worker and /api from this one. The site Worker's workers.dev name is NOT here on purpose -
  // it carries no API, so the pages load there and every call from them 404s, which is worse
  // than not offering it.
  "clickindicators-failover-production.up.railway.app",
]);

/* --------------------------------------------------------------------
 * Reaching us through the failover proxy.
 *
 * While Cloudflare's edge refuses to serve the clickindicatorsmod.com zone, the hostname
 * resolves to a proxy outside Cloudflare that forwards here over workers.dev. That changes two
 * things this Worker reads straight off the request, and both matter:
 *
 *   cf-connecting-ip  - Cloudflare sets this at its own edge from whoever opened the connection,
 *                       which through the proxy is the proxy. Left alone, every customer would
 *                       share one address and the per-IP login limiter would lock out all of
 *                       them together. The proxy sends the real address separately.
 *   the request origin - every URL we hand back (macro links, OAuth redirects, Stripe returns,
 *                       password-reset links) is built from it, and would otherwise point at
 *                       workers.dev, where the session cookie and the Turnstile allowlist do
 *                       not apply.
 *
 * Both are only believed when the caller proves it is the proxy. This Worker is publicly
 * reachable on workers.dev, so an unauthenticated x-ci-client-ip would let anyone claim any
 * address and walk past the rate limits, and an unauthenticated forwarded host would be an open
 * redirect through the OAuth callback. No secret configured means neither is trusted, which is
 * exactly the behaviour from before the outage.
 * ------------------------------------------------------------------ */
const PUBLIC_HOSTS = new Set(["clickindicatorsmod.com", "www.clickindicatorsmod.com"]);

function fromProxy(req, env) {
  const want = env && env.CI_PROXY_SECRET;
  if (!want) return false;
  const got = req.headers.get("x-ci-proxy-secret") || "";
  // Constant-time. The length check leaks only the length, which is fixed and not secret; the
  // comparison itself accumulates every byte instead of stopping at the first mismatch, so the
  // time it takes says nothing about how much of the secret was right. Cheap enough to simply
  // do rather than argue about whether a remote timing attack is practical here.
  if (got.length !== want.length) return false;
  let diff = 0;
  for (let i = 0; i < want.length; i++) diff |= got.charCodeAt(i) ^ want.charCodeAt(i);
  return diff === 0;
}

function clientIP(req, env) {
  if (fromProxy(req, env)) {
    const fwd = (req.headers.get("x-ci-client-ip") || "").trim();
    if (fwd) return fwd;
  }
  return req.headers.get("cf-connecting-ip") || "unknown";
}

// The hostname to build outward-facing URLs from. Only ever one of ours, or whatever the
// request actually arrived on.
function publicHost(req, env) {
  if (!fromProxy(req, env)) return null;
  const h = (req.headers.get("x-forwarded-host") || "").toLowerCase();
  return PUBLIC_HOSTS.has(h) ? h : null;
}

async function turnstileOK(env, token, expectedAction, ip) {
  if (typeof token !== "string" || token.length === 0 || token.length > 2048) {
    console.log("TURNSTILE missing or malformed token for " + expectedAction);
    return false;
  }
  let result;
  try {
    const r = await fetch("https://challenges.cloudflare.com/turnstile/v0/siteverify", {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      signal: AbortSignal.timeout(10000),
      body: new URLSearchParams({
        secret: env.TURNSTILE_SECRET || "",
        response: token,
        remoteip: ip || "",
      }),
    });
    if (!r.ok) throw new Error("siteverify HTTP " + r.status);
    result = await r.json();
  } catch (e) {
    console.log("TURNSTILE siteverify failed:", e && e.message);
    return false;
  }
  if (!result.success || result.action !== expectedAction || !TURNSTILE_HOSTS.has(result.hostname)) {
    console.log("TURNSTILE rejected", JSON.stringify({
      success: result.success, action: result.action,
      hostname: result.hostname, errors: result["error-codes"],
    }));
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------ *
 * email
 *
 * Tries the Cloudflare Email binding first, then Resend if a key is set.
 * With neither configured, sendMail reports false and signup falls back to
 * auto-verifying - an unconfigured mailer must not lock people out of buying.
 * ------------------------------------------------------------------ */
// Sent from the SUBDOMAIN, which is the domain actually verified with Resend. The root is
// deliberately not used: its SPF belongs to the registrar's forwarding for inbound support
// mail, and a from-address on an unverified domain is rejected outright.
const MAIL_FROM = "noreply@send.clickindicatorsmod.com";
// Replies go somewhere a person reads. Someone answering a password-reset email with "it
// still doesn't work" should reach support, not a black hole called noreply.
const MAIL_REPLY_TO = "support@clickindicatorsmod.com";

async function sendMail(env, to, subject, text, html) {
  try {
    if (env.EMAIL && typeof env.EMAIL.send === "function") {
      await env.EMAIL.send({
        to,
        from: { email: MAIL_FROM, name: "Click Indicators" },
        replyTo: MAIL_REPLY_TO,
        subject, text, html,
      });
      return true;
    }
    if (env.RESEND_KEY) {
      const r = await fetch("https://api.resend.com/emails", {
        method: "POST",
        headers: { authorization: "Bearer " + env.RESEND_KEY, "content-type": "application/json" },
        body: JSON.stringify({
          from: "Click Indicators <" + MAIL_FROM + ">",
          reply_to: MAIL_REPLY_TO,
          to: [to], subject, text, html,
        }),
      });
      if (!r.ok) console.log("MAIL resend failed", r.status, await r.text());
      return r.ok;
    }
  } catch (e) {
    console.log("MAIL error", e && e.message);
  }
  return false;
}

function sixDigit() {
  const a = new Uint32Array(1);
  crypto.getRandomValues(a);
  return String(100000 + (a[0] % 900000));
}

async function issueCode(env, email) {
  const code = sixDigit();
  try {
    await env.DB.prepare(
      "INSERT INTO email_codes (email, code_hash, expires, tries, sent) VALUES (?, ?, ?, 0, ?) " +
      "ON CONFLICT(email) DO UPDATE SET code_hash = ?, expires = ?, tries = 0, sent = ?"
    ).bind(email, await sha256b64(code), now() + 900, now(),
           await sha256b64(code), now() + 900, now()).run();
  } catch (e) {
    // Almost certainly the email_codes migration has not been run. Reporting "not sent" makes
    // signup fall back to auto-verifying, which is the same path as having no mailer at all.
    // Letting this throw instead left the users row already inserted with verified = 0: the
    // address was taken, no code existed to verify it with, and signing up again returned
    // "that email already has an account". An unrecoverable account, created by our own 500.
    console.log("ISSUECODE store failed (is email_codes migrated?):", e && e.message);
    return false;
  }

  const sent = await sendMail(env, email, "Your Click Indicators code",
    `Your verification code is ${code}\n\nIt expires in 15 minutes.\n\n` +
    `If you didn't sign up at clickindicators.com, ignore this email.`,
    `<p>Your verification code is</p><p style="font-size:28px;letter-spacing:4px;` +
    `font-family:monospace"><b>${code}</b></p><p>It expires in 15 minutes.</p>` +
    `<p style="color:#888;font-size:13px">If you didn't sign up at clickindicators.com, ignore this email.</p>`);
  return sent;
}

async function verifyCode(env, req) {
  const { email, code } = await req.json().catch(() => ({}));
  const mail = (email || "").trim().toLowerCase();
  const row = await env.DB.prepare("SELECT * FROM email_codes WHERE email = ?").bind(mail).first();
  if (!row) return json({ ok: false, error: "No code was sent to that address." }, 400);
  if (row.expires < now()) return json({ ok: false, error: "That code expired. Send a new one." }, 400);
  if (row.tries >= 6) return json({ ok: false, error: "Too many attempts. Send a new code." }, 429);

  const ok = timingSafeEqual(await sha256b64(String(code || "")), row.code_hash);
  if (!ok) {
    await env.DB.prepare("UPDATE email_codes SET tries = tries + 1 WHERE email = ?").bind(mail).run();
    return json({ ok: false, error: "Wrong code." }, 401);
  }
  await env.DB.prepare("UPDATE users SET verified = 1 WHERE email = ?").bind(mail).run();
  await env.DB.prepare("DELETE FROM email_codes WHERE email = ?").bind(mail).run();
  return json({ ok: true });
}

async function resendCode(env, req) {
  const { email } = await req.json().catch(() => ({}));
  const mail = (email || "").trim().toLowerCase();
  // One reply for every address, always. The old version answered three different ways - 429 if a
  // code had been sent in the last minute, {ok:true} for an unknown address, {ok:true,sent:...}
  // for a known one - and only an address with an account could ever produce the first or third.
  // That let anyone test whether a person is a customer, unauthenticated and unthrottled, which is
  // the reconnaissance step before guessing passwords at them. The page already says "if that
  // address has an account, a code is on its way", so nothing legitimate needs the difference.
  const prev = await env.DB.prepare("SELECT sent FROM email_codes WHERE email = ?").bind(mail).first();
  const tooSoon = prev && now() - prev.sent < 60;
  if (!tooSoon) {
    const u = await env.DB.prepare("SELECT id FROM users WHERE email = ?").bind(mail).first();
    if (u) await issueCode(env, mail);
  }
  return json({ ok: true });
}

/* ------------------------------------------------------------------ *
 * routes
 * ------------------------------------------------------------------ */
async function signup(env, req) {
  const body = await req.json().catch(() => ({}));
  const { email, password } = body;
  const mail = (email || "").trim().toLowerCase();
  // Signup was the one unrated endpoint - an easy way to fill the database.
  const ip = clientIP(req, env);
  if (!(await turnstileOK(env, body["cf-turnstile-response"], "signup", ip)))
    return json({ ok: false, error: "Bot check failed. Reload the page and try again." }, 403);
  const wait = await checkLock(env.DB, "signup:" + ip);
  if (wait) return json({ ok: false, error: `Slow down. Try again in ${wait}s.` }, 429);
  await noteFail(env.DB, "signup:" + ip);
  if (!validEmail(mail)) return json({ ok: false, error: "Enter a valid email address." }, 400);
  if (typeof password !== "string" || password.length < 8)
    return json({ ok: false, error: "Password must be at least 8 characters." }, 400);
  if (!typeablePassword(password)) return json({ ok: false, error: PW_HINT }, 400);

  const existing = await env.DB.prepare("SELECT id FROM users WHERE email = ?").bind(mail).first();
  if (existing) return json({ ok: false, error: "That email already has an account." }, 409);

  const salt = randB64(16).replace(/-/g, "+").replace(/_/g, "/") + "==";
  const hash = await hashPassword(password, salt);
  const id = crypto.randomUUID();
  await env.DB.prepare(
    "INSERT INTO users (id, email, pw_hash, pw_salt, paid, created) VALUES (?, ?, ?, ?, 0, ?)"
  ).bind(id, mail, hash, salt, now()).run();

  // Verified on creation, deliberately. Email verification is not wanted here: it puts a step
  // between deciding to buy and being able to, and the address gets proved in practice by the
  // Stripe receipt and by password reset. This used to auto-verify only because no mailer was
  // configured - once one existed it silently started gating signup behind a code, which is
  // the opposite of the intent and burns the sending quota that password reset depends on.
  await env.DB.prepare("UPDATE users SET verified = 1 WHERE id = ?").bind(id).run();
  const tok = await newSession(env, id);
  return json({ ok: true, email: mail, paid: false, verified: true },
               200, { "set-cookie": sessionCookie(tok) });
}

async function login(env, req) {
  const body = await req.json().catch(() => ({}));
  const { email, password } = body;
  const mail = (email || "").trim().toLowerCase();
  const ip = clientIP(req, env);
  if (!(await turnstileOK(env, body["cf-turnstile-response"], "login", ip)))
    return json({ ok: false, error: "Bot check failed. Reload the page and try again." }, 403);
  // Keyed on the caller, not the account. The email-keyed version let anyone who knew a
  // customer's address hold them out of their own dashboard, and a correct password should
  // never be refused - the same fix already applied to in-game sign-in.
  const wait = await checkLock(env.DB, "webip:" + ip);
  if (wait) return json({ ok: false, error: `Too many attempts. Try again in ${wait}s.` }, 429);

  const u = await env.DB.prepare("SELECT * FROM users WHERE email = ?").bind(mail).first();
  // Constant work whether or not the address exists - see DUMMY_SALT.
  const attempt = await hashPassword(password || "", u ? u.pw_salt : DUMMY_SALT);
  if (!u || !timingSafeEqual(attempt, u.pw_hash)) {
    await noteFail(env.DB, "webip:" + ip);
    await noteFail(env.DB, "web:" + mail);
    return json({ ok: false, error: "Wrong email or password." }, 401);
  }
  await clearFails(env.DB, "web:" + mail);
  await clearFails(env.DB, "webip:" + ip);
  const tok = await newSession(env, u.id);
  return json({ ok: true, email: u.email, paid: !!u.paid, verified: !!u.verified }, 200, { "set-cookie": sessionCookie(tok) });
}

/* ------------------------------------------------------------------ *
 * Discord role
 *
 * Buyers get the Pro role automatically. The link is proved by OAuth rather than by asking
 * people to type their tag, because nothing else establishes that the person holding this
 * licence is that Discord user.
 *
 * Everything here is best effort on the licence's terms: a Discord outage, a missing bot
 * permission or someone who has not joined the server must never break the dashboard or
 * affect whether the mod works.
 * ------------------------------------------------------------------ */
const DISCORD_API = "https://discord.com/api/v10";
const OAUTH_TTL = 600;

function discordConfigured(env) {
  return !!(env.DISCORD_CLIENT_ID && env.DISCORD_CLIENT_SECRET &&
            env.DISCORD_BOT_TOKEN && env.DISCORD_GUILD_ID && env.DISCORD_ROLE_ID);
}

/**
 * Give or take the Pro role. Returns a short reason string rather than throwing, because
 * every caller wants to carry on regardless.
 */
async function discordRole(env, discordId, grant) {
  if (!discordConfigured(env) || !discordId) return "not-configured";
  const url = `${DISCORD_API}/guilds/${env.DISCORD_GUILD_ID}/members/${discordId}/roles/${env.DISCORD_ROLE_ID}`;
  try {
    const r = await fetch(url, {
      method: grant ? "PUT" : "DELETE",
      headers: { authorization: "Bot " + env.DISCORD_BOT_TOKEN },
      signal: AbortSignal.timeout(10000),
    });
    if (r.status === 204 || r.status === 200) return "ok";
    // 404 here almost always means the person is not in the server, not that the role or
    // guild is wrong - worth distinguishing, because the fix is "go join" not "fix the bot".
    if (r.status === 404) return "not-in-server";
    // 403 means the bot's own role sits below the one it is trying to assign. Discord gives
    // no other signal for this and it is the single most common way this setup fails.
    if (r.status === 403) return "bot-role-too-low";
    console.log("DISCORD role " + (grant ? "grant" : "remove") + " failed", r.status, await r.text());
    return "error-" + r.status;
  } catch (e) {
    console.log("DISCORD role call threw:", e && e.message);
    return "unreachable";
  }
}

async function discordStart(env, req, url) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Sign in first." }, 401);
  if (!discordConfigured(env)) return json({ ok: false, error: "Discord linking isn't set up yet." }, 503);

  const state = randB64(24);
  await env.DB.batch([
    env.DB.prepare("DELETE FROM discord_states WHERE user_id = ? OR expires < ?").bind(u.id, now()),
    env.DB.prepare("INSERT INTO discord_states (state_hash, user_id, expires) VALUES (?, ?, ?)")
      .bind(await sha256b64(state), u.id, now() + OAUTH_TTL),
  ]);
  const redirect = `${url.origin}/api/discord/callback`;
  const auth = `${DISCORD_API}/oauth2/authorize?client_id=${encodeURIComponent(env.DISCORD_CLIENT_ID)}` +
               `&redirect_uri=${encodeURIComponent(redirect)}&response_type=code&scope=identify` +
               `&state=${encodeURIComponent(state)}&prompt=none`;
  return Response.redirect(auth, 302);
}

async function discordCallback(env, req, url) {
  const back = (q) => Response.redirect(`${url.origin}/account.html?discord=${q}`, 302);
  const code = url.searchParams.get("code");
  const state = url.searchParams.get("state");
  if (!code || !state) return back("cancelled");
  if (!discordConfigured(env)) return back("off");

  const row = await env.DB.prepare(
    "SELECT user_id FROM discord_states WHERE state_hash = ? AND expires > ?"
  ).bind(await sha256b64(state), now()).first();
  // Single use. A replayed callback must not be able to rebind an account.
  await env.DB.prepare("DELETE FROM discord_states WHERE state_hash = ?")
    .bind(await sha256b64(state)).run();
  if (!row) return back("expired");

  let discordId = null;
  try {
    const tokRes = await fetch(`${DISCORD_API}/oauth2/token`, {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      signal: AbortSignal.timeout(10000),
      body: new URLSearchParams({
        client_id: env.DISCORD_CLIENT_ID,
        client_secret: env.DISCORD_CLIENT_SECRET,
        grant_type: "authorization_code",
        code,
        redirect_uri: `${url.origin}/api/discord/callback`,
      }),
    });
    if (!tokRes.ok) { console.log("DISCORD token exchange", tokRes.status, await tokRes.text()); return back("failed"); }
    const tok = await tokRes.json();
    const meRes = await fetch(`${DISCORD_API}/users/@me`, {
      headers: { authorization: "Bearer " + tok.access_token },
      signal: AbortSignal.timeout(10000),
    });
    if (!meRes.ok) return back("failed");
    discordId = (await meRes.json()).id;
  } catch (e) {
    console.log("DISCORD oauth threw:", e && e.message);
    return back("failed");
  }
  if (!discordId) return back("failed");

  // One Discord account per licence, and one licence per Discord account - otherwise a single
  // Discord user could collect the role from several accounts, or two buyers could both point
  // at one Discord user and confuse revocation.
  const taken = await env.DB.prepare("SELECT id FROM users WHERE discord_id = ? AND id != ?")
    .bind(discordId, row.user_id).first();
  if (taken) return back("taken");

  await env.DB.prepare("UPDATE users SET discord_id = ? WHERE id = ?").bind(discordId, row.user_id).run();

  const owner = await env.DB.prepare("SELECT paid FROM users WHERE id = ?").bind(row.user_id).first();
  if (owner && owner.paid) {
    const res = await discordRole(env, discordId, true);
    if (res !== "ok") return back(res);
  }
  return back("ok");
}

async function discordUnlink(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Not signed in." }, 401);
  if (u.discord_id) await discordRole(env, u.discord_id, false);
  await env.DB.prepare("UPDATE users SET discord_id = NULL WHERE id = ?").bind(u.id).run();
  return json({ ok: true });
}

/* ------------------------------------------------------------------ *
 * password reset
 *
 * An hour after launch, four paying customers were locked out of accounts they had bought,
 * because there was no recovery path of any kind. A reset link is emailed; the token is
 * stored hashed, single-use, and short-lived.
 * ------------------------------------------------------------------ */
const RESET_TTL = 3600;

async function forgot(env, req, url) {
  const body = await req.json().catch(() => ({}));
  const mail = (body.email || "").trim().toLowerCase();
  const ip = clientIP(req, env);
  if (!(await turnstileOK(env, body["cf-turnstile-response"], "forgot", ip)))
    return json({ ok: false, error: "Bot check failed. Reload the page and try again." }, 403);
  const wait = await checkLock(env.DB, "forgot:" + ip);
  if (wait) return json({ ok: false, error: `Too many requests. Try again in ${wait}s.` }, 429);
  await noteFail(env.DB, "forgot:" + ip);

  const u = await env.DB.prepare("SELECT id, email FROM users WHERE email = ?").bind(mail).first();
  if (u) {
    const tok = randB64(32);
    await env.DB.batch([
      // One live link per account: asking again invalidates the previous one, so a link
      // forwarded or left in an old inbox stops working.
      env.DB.prepare("DELETE FROM password_resets WHERE user_id = ?").bind(u.id),
      env.DB.prepare(
        "INSERT INTO password_resets (token_hash, user_id, expires, created) VALUES (?, ?, ?, ?)"
      ).bind(await sha256b64(tok), u.id, now() + RESET_TTL, now()),
    ]);
    const link = `${url.origin}/reset.html?t=${encodeURIComponent(tok)}`;
    const sent = await sendMail(env, u.email, "Reset your Click Indicators password",
      `Open this link to set a new password:\n\n${link}\n\nIt expires in an hour.\n\n` +
      `If you didn't ask for this you can ignore it - nothing has changed.`,
      `<p>Open this link to set a new password:</p>` +
      `<p><a href="${link}">${link}</a></p><p>It expires in an hour.</p>` +
      `<p style="color:#888;font-size:13px">If you didn't ask for this you can ignore it - ` +
      `nothing has changed.</p>`);
    if (!sent) console.log("FORGOT no mailer configured - reset link could not be sent to " + u.email);
  }
  // Identical answer either way. Anything else turns this into a free tool for checking
  // which addresses have accounts here.
  return json({ ok: true });
}

async function resetPassword(env, req) {
  const { token, password } = await req.json().catch(() => ({}));
  if (!token) return json({ ok: false, error: "That link isn't valid." }, 400);
  if (typeof password !== "string" || password.length < 8)
    return json({ ok: false, error: "Password must be at least 8 characters." }, 400);
  if (!typeablePassword(password)) return json({ ok: false, error: PW_HINT }, 400);

  const row = await env.DB.prepare(
    "SELECT r.user_id, u.email FROM password_resets r JOIN users u ON u.id = r.user_id " +
    "WHERE r.token_hash = ? AND r.expires > ?"
  ).bind(await sha256b64(token), now()).first();
  if (!row)
    return json({ ok: false, error: "That link has expired or has already been used. Ask for a new one." }, 400);

  const salt = randB64(16).replace(/-/g, "+").replace(/_/g, "/") + "==";
  const hash = await hashPassword(password, salt);
  await env.DB.batch([
    env.DB.prepare("UPDATE users SET pw_hash = ?, pw_salt = ? WHERE id = ?")
      .bind(hash, salt, row.user_id),
    // You reset a password when you may have lost control of the account, so everything that
    // derived its authority from the old one goes too - website sessions and the game's device
    // token alike. They sign in again with the new password.
    env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(row.user_id),
    env.DB.prepare("DELETE FROM devices WHERE user_id = ?").bind(row.user_id),
    env.DB.prepare("DELETE FROM password_resets WHERE user_id = ?").bind(row.user_id),
    // Someone resetting has usually been guessing first, and would otherwise hit the backoff
    // immediately after finally setting a password that works.
    env.DB.prepare("DELETE FROM login_attempts WHERE ident IN (?, ?)")
      .bind("web:" + row.email, "mod:" + row.email),
  ]);
  console.log("RESET password changed for " + row.email);
  return json({ ok: true });
}

async function changePassword(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Not signed in." }, 401);
  const { current, password } = await req.json().catch(() => ({}));
  if (typeof password !== "string" || password.length < 8)
    return json({ ok: false, error: "New password must be at least 8 characters." }, 400);
  if (!typeablePassword(password)) return json({ ok: false, error: PW_HINT }, 400);
  if (!timingSafeEqual(await hashPassword(current || "", u.pw_salt), u.pw_hash))
    return json({ ok: false, error: "That isn't your current password." }, 401);

  const salt = randB64(16).replace(/-/g, "+").replace(/_/g, "/") + "==";
  const hash = await hashPassword(password, salt);
  // Other sessions go, but the device token stays: they proved they know the current
  // password, so this is a deliberate change rather than a recovery, and signing them out of
  // the game mid-level to reward good security hygiene is a poor trade.
  await env.DB.batch([
    env.DB.prepare("UPDATE users SET pw_hash = ?, pw_salt = ? WHERE id = ?").bind(hash, salt, u.id),
    env.DB.prepare("DELETE FROM sessions WHERE user_id = ?").bind(u.id),
  ]);
  const tok = await newSession(env, u.id);
  return json({ ok: true }, 200, { "set-cookie": sessionCookie(tok) });
}

async function me(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Not signed in." }, 401);
  const devs = await env.DB.prepare(
    "SELECT id, name, created, last_seen FROM devices WHERE user_id = ? ORDER BY last_seen DESC"
  ).bind(u.id).all();
  return json({ ok: true, email: u.email, paid: !!u.paid, verified: !!u.verified,
                devices: devs.results || [], max_devices: MAX_DEVICES,
                discord_linked: !!u.discord_id,
                discord_available: discordConfigured(env) });
}

async function checkout(env, req, url) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Sign in first." }, 401);
  if (u.paid) return json({ ok: false, error: "You already own Click Indicators." }, 400);
  if (!u.verified) return json({ ok: false, error: "Verify your email address first." }, 403);
  // The "Launching soon" state was enforced only in the pages. Anyone signed in could POST
  // here, get a test-mode Checkout session and pay with card 4242 - a free licence. The same
  // rule the pages read, applied where it is actually load-bearing.
  if (!String(env.STRIPE_SECRET || "").startsWith("sk_live_"))
    return json({ ok: false, error: "Click Indicators isn't on sale yet." }, 403);
  const origin = url.origin;
  const { ok, data } = await stripe(env, "checkout/sessions", {
    mode: "payment",
    "line_items[0][price]": env.STRIPE_PRICE_ID,
    "line_items[0][quantity]": "1",
    // Straight to the dashboard, which is where the download and the install
    // steps are - the front page has neither.
    success_url: `${origin}/account.html?paid=1`,
    cancel_url: `${origin}/account.html?cancelled=1`,
    customer_email: u.email,
    "metadata[user_id]": u.id,
    // Stamped on the PaymentIntent as well as the session. Session metadata is not copied down,
    // and the PaymentIntent is the only object that is searchable after the fact and the only one
    // a refund or dispute event points back at. Without it, a purchase whose webhook never landed
    // can only be matched by email - which is the buyer's email at the WALLET, not the one they
    // signed up with, so Amazon Pay and PayPal buyers could not be matched at all.
    "payment_intent_data[metadata][user_id]": u.id,
    allow_promotion_codes: "true",
  });
  if (!ok) return json({ ok: false, error: data?.error?.message || "Stripe error." }, 502);
  return json({ ok: true, url: data.url });
}

/**
 * The one place access is ever granted, whichever processor took the money.
 *
 * There are now two ways to pay, and the reason nobody can get in without paying is that every
 * one of them ends here after the payment has been verified against the processor's own API or
 * signature. Two payment routes is fine. Two grant implementations would not be - the second
 * one is where the mistake would live, and it would undo the rest.
 *
 * Callers MUST have verified the money server-side before calling. Nothing here re-checks it.
 */
async function grantAccess(env, uid, purchaseId, amount, currency, extra = []) {
  await env.DB.batch([
    env.DB.prepare("UPDATE users SET paid = 1 WHERE id = ?").bind(uid),
    env.DB.prepare(
      "INSERT OR REPLACE INTO purchases (id, user_id, amount, currency, status, created) " +
      "VALUES (?, ?, ?, ?, 'paid', ?)"
    ).bind(purchaseId, uid, amount || 0, currency || "usd", now()),
    ...extra,
  ]);
  console.log("GRANTED access to user " + uid + " via " + purchaseId);
  // Someone who linked Discord before buying gets the role the moment they pay, without
  // having to come back and link again.
  const buyer = await env.DB.prepare("SELECT discord_id FROM users WHERE id = ?").bind(uid).first();
  if (buyer && buyer.discord_id) {
    console.log("DISCORD role on purchase for " + uid + ": " + await discordRole(env, buyer.discord_id, true));
  }
}

/**
 * Revoke, whichever processor reversed the money. Mirrors grantAccess for the same reason.
 */
async function revokeAccess(env, uid, why) {
  const u = await env.DB.prepare("SELECT discord_id FROM users WHERE id = ?").bind(uid).first();
  if (u && u.discord_id) console.log("DISCORD role on revoke: " + await discordRole(env, u.discord_id, false));
  await env.DB.batch([
    env.DB.prepare("UPDATE users SET paid = 0 WHERE id = ?").bind(uid),
    env.DB.prepare("DELETE FROM devices WHERE user_id = ?").bind(uid),
  ]);
  console.log("REVOKED access for " + uid + " (" + why + ")");
}

/**
 * "I paid but I do not have access."
 *
 * Access is granted by a webhook, and a webhook is a delivery promise rather than a guarantee: an
 * event type that was never subscribed to, an endpoint that was down, a retry window that expired.
 * Any of those leaves someone who has genuinely paid sitting on an unpaid account with no way out
 * but to email support and wait. That has already happened here at least once.
 *
 * So this asks about the money directly. Stripe is the authority on whether this account paid, and
 * the PaymentIntent carries the user id stamped on it at checkout, so nothing is trusted from the
 * caller beyond who is signed in. It cannot grant access to an account that did not pay, because
 * the search is by that id and the amount has to have actually been received.
 */
async function reclaim(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Sign in first." }, 401);
  if (u.paid) return json({ ok: true, already: true });
  // The id is a server-generated UUID, but it is going into a query string, so prove it.
  if (!/^[A-Za-z0-9-]{8,64}$/.test(String(u.id))) return json({ ok: false, error: "Bad request." }, 400);

  const wait = await checkLock(env.DB, "reclaim:" + u.id);
  if (wait) return json({ ok: false, error: `Try again in ${wait}s.` }, 429);
  await noteFail(env.DB, "reclaim:" + u.id);   // cheap ceiling; cleared on success below

  const q = encodeURIComponent(`status:'succeeded' AND metadata['user_id']:'${u.id}'`);
  let d = null, ok = false;
  try {
    const r = await fetch("https://api.stripe.com/v1/payment_intents/search?limit=5&query=" + q, {
      headers: { authorization: "Bearer " + env.STRIPE_SECRET },
    });
    ok = r.ok;
    d = await r.json().catch(() => null);
  } catch (e) { ok = false; }
  if (!ok || !d || !Array.isArray(d.data)) {
    console.log("RECLAIM search failed for " + u.id + ": " + JSON.stringify(d || {}).slice(0, 200));
    return json({ ok: false, error: "Couldn't reach the payment processor. Try again shortly." }, 502);
  }

  const pi = d.data.find((x) => x.status === "succeeded" && (x.amount_received || 0) > 0);
  if (!pi) {
    // Stripe's search index lags new payments by up to a minute, so a buyer clicking this the
    // instant they land back from checkout can legitimately find nothing. Say so.
    return json({ ok: false, error: "No completed payment found yet for this account. If you have " +
                  "just paid, wait a minute and try again." }, 404);
  }

  await grantAccess(env, u.id, pi.id, pi.amount_received || pi.amount || 0, pi.currency || "usd");
  await clearFails(env.DB, "reclaim:" + u.id);
  console.log("RECLAIM granted " + u.id + " from " + pi.id);
  return json({ ok: true, recovered: true });
}

async function webhook(env, req) {
  const payload = await req.text();
  const sigHeader = req.headers.get("stripe-signature");
  console.log("WEBHOOK received, bytes=" + payload.length + " sig=" + (sigHeader ? "present" : "MISSING"));
  if (!(await stripeSigOK(env, payload, sigHeader))) {
    console.log("WEBHOOK REJECTED: signature did not verify. Is STRIPE_WEBHOOK_SECRET " +
                "the one from THIS mode's endpoint?");
    return json({ ok: false, error: "bad signature" }, 400);
  }

  const ev = JSON.parse(payload);
  console.log("WEBHOOK verified: type=" + ev.type + " id=" + ev.id);
  // Stripe retries; make sure a retry cannot grant a second time.
  const seen = await env.DB.prepare("SELECT id FROM stripe_events WHERE id = ?").bind(ev.id).first();
  if (seen) return json({ ok: true, duplicate: true });
  // Prepared now, committed WITH the work below rather than before it. Marking the event
  // handled first meant that a transient D1 error in between left Stripe believing it had
  // delivered, the retry short-circuiting as a duplicate above, and someone who paid never
  // being granted anything - with no purchases row to reconcile against either, and Stripe's
  // dashboard showing success, so nothing would ever point at it. Every statement below is
  // idempotent, so a retry that does re-run the work is harmless.
  const mark = env.DB.prepare("INSERT INTO stripe_events (id, created) VALUES (?, ?)")
    .bind(ev.id, now());

  // A completed checkout is not the same as a completed payment. Stripe reports how far the
  // money actually got:
  //   paid                 - settled, the normal card case
  //   no_payment_required  - a 100% promotion code covered it, still a legitimate grant
  //   unpaid               - a delayed-notification method where the funds have NOT arrived.
  //                          checkout.session.async_payment_succeeded follows if they ever do.
  // Granting on "completed" alone means a payment method that settles later hands out the
  // licence the moment checkout is finished, whether or not the money turns up. Cards do not
  // behave that way, which is exactly why this would go unnoticed until the day another
  // method is enabled.
  if (ev.type === "checkout.session.completed" || ev.type === "checkout.session.async_payment_succeeded") {
    const s = ev.data.object;
    const uid = s.metadata?.user_id;
    const settled = s.payment_status === "paid" || s.payment_status === "no_payment_required" ||
                    ev.type === "checkout.session.async_payment_succeeded";
    console.log(ev.type + " user_id=" + (uid || "MISSING FROM METADATA") +
                " payment_status=" + s.payment_status + " settled=" + settled);
    if (uid && !settled) {
      // Nothing granted. Marked handled so Stripe stops retrying this particular event; the
      // async success event is what grants later.
      console.log("NOT granting yet - payment not settled for " + uid);
      await mark.run();
      return json({ ok: true, pending: true });
    }
    if (uid) {
      await grantAccess(env, uid, s.id, s.amount_total, s.currency, [mark]);
      return json({ ok: true });
    }
  } else if (ev.type === "charge.refunded" || ev.type === "charge.dispute.created") {
    // Refunded or charged back: revoke access and log every device out.
    // A Dispute object has no billing_details - that lives on the Charge - so fall back
    // to the evidence block, which is where Stripe puts the customer's address.
    const o = ev.data.object;
    // Prefer the user id stamped on the PaymentIntent. Matching by email was the only way before
    // and it is unreliable for exactly the methods that hide the buyer's real address: with Amazon
    // Pay or PayPal, billing_details holds the address on the WALLET, not the one they signed up
    // with, so a refund on one of those purchases matched nobody and quietly failed to revoke.
    let uidFromPI = null;
    const piId = typeof o.payment_intent === "string" ? o.payment_intent : null;
    if (piId) {
      try {
        const pr = await fetch("https://api.stripe.com/v1/payment_intents/" + piId, {
          headers: { authorization: "Bearer " + env.STRIPE_SECRET },
        });
        if (pr.ok) {
          const pi = await pr.json();
          uidFromPI = (pi && pi.metadata && pi.metadata.user_id) || null;
        }
      } catch (e) { console.log("revocation PI lookup failed: " + (e && e.message)); }
    }
    if (uidFromPI) {
      console.log("revocation event " + ev.type + " matched user " + uidFromPI + " via PaymentIntent");
      await revokeAccess(env, uidFromPI, ev.type);
      await mark.run();
      return json({ ok: true });
    }
    const email = (o.billing_details?.email
                || o.receipt_email
                || o.evidence?.customer_email_address
                || "").toLowerCase();
    console.log("revocation event " + ev.type + " email=" + (email || "NOT FOUND"));
    if (email) {
      const u = await env.DB.prepare("SELECT id, discord_id FROM users WHERE email = ?")
        .bind(email).first();
      if (u) {
        // Access is gone, so the Pro role goes with it.
        if (u.discord_id) console.log("DISCORD role on revoke: " + await discordRole(env, u.discord_id, false));
        await env.DB.batch([
          env.DB.prepare("UPDATE users SET paid = 0 WHERE id = ?").bind(u.id),
          env.DB.prepare("DELETE FROM devices WHERE user_id = ?").bind(u.id),
          mark,
        ]);
        return json({ ok: true });
      }
    }
  }
  // Nothing was done for this event - an type we do not act on, or a refund whose customer
  // could not be matched to an account. Mark it handled anyway so Stripe stops redelivering
  // it; the console line above is the record that it arrived.
  await mark.run();
  return json({ ok: true });
}

/**
 * Remember why a device's token stopped working, so that install can say so instead
 * of reporting a blank "signed out" that reads as the mod being broken.
 *
 * Best effort on purpose, and deliberately outside the caller's transaction: this is a
 * nicety, and it must never be what stops a paying customer signing in. If the
 * device_signouts migration has not been run, sign-in still works and the message just
 * falls back to the generic one.
 */
/* ------------------------------------------------------------------ *
 * PayPal
 *
 * Stripe cannot process PayPal for a US merchant - that payment method is only offered to
 * merchants in the EU, UK, Switzerland, Norway and Liechtenstein - so this is a direct
 * integration rather than another Stripe payment method.
 *
 * The browser is never believed. It asks this Worker to create an order, PayPal takes over,
 * and then the browser says "done" - at which point this Worker calls PayPal itself and reads
 * the order back. Access is granted on what PayPal's API says, not on what the page reports.
 * A caller who fakes the "done" call gets nothing, because the order will not read COMPLETED.
 * ------------------------------------------------------------------ */

const PP_API = "https://api-m.paypal.com";

function paypalConfigured(env) {
  return !!(env.PAYPAL_CLIENT_ID && env.PAYPAL_SECRET);
}

async function paypalToken(env) {
  const r = await fetch(PP_API + "/v1/oauth2/token", {
    method: "POST",
    headers: {
      authorization: "Basic " + btoa(env.PAYPAL_CLIENT_ID + ":" + env.PAYPAL_SECRET),
      "content-type": "application/x-www-form-urlencoded",
    },
    body: "grant_type=client_credentials",
  });
  if (!r.ok) { console.log("PAYPAL token failed " + r.status); return null; }
  const j = await r.json().catch(() => null);
  return (j && j.access_token) || null;
}

async function paypalApi(env, path, method = "GET", body = null) {
  const t = await paypalToken(env);
  if (!t) return { ok: false, status: 0, data: null };
  const r = await fetch(PP_API + path, {
    method,
    headers: { authorization: "Bearer " + t, "content-type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  return { ok: r.ok, status: r.status, data: await r.json().catch(() => null) };
}

/**
 * One source of truth for the price, read from the Stripe price object rather than kept as a
 * second copy here. Two hardcoded prices would eventually disagree, and the day they did, one
 * set of buyers would quietly be charged the wrong amount for the same thing.
 */
async function listedPrice(env) {
  const r = await fetch("https://api.stripe.com/v1/prices/" + env.STRIPE_PRICE_ID, {
    headers: { authorization: "Bearer " + env.STRIPE_SECRET },
  });
  if (!r.ok) return null;
  const p = await r.json().catch(() => null);
  if (!p || typeof p.unit_amount !== "number" || !p.currency) return null;
  return { amount: p.unit_amount, currency: String(p.currency).toUpperCase() };
}

async function paypalOrder(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Sign in first." }, 401);
  if (u.paid) return json({ ok: false, error: "You already own Click Indicators." }, 400);
  if (!u.verified) return json({ ok: false, error: "Verify your email address first." }, 403);
  if (!paypalConfigured(env)) return json({ ok: false, error: "PayPal isn't available yet." }, 503);
  // The same live-mode gate the Stripe route carries, for the same reason: without it anyone
  // signed in could pay through a sandbox and walk away with a real licence.
  if (!String(env.STRIPE_SECRET || "").startsWith("sk_live_"))
    return json({ ok: false, error: "Click Indicators isn't on sale yet." }, 403);

  const price = await listedPrice(env);
  if (!price) return json({ ok: false, error: "Couldn't read the price." }, 502);

  const { ok, data } = await paypalApi(env, "/v2/checkout/orders", "POST", {
    intent: "CAPTURE",
    purchase_units: [{
      // The amount is decided here and never accepted from the caller.
      amount: { currency_code: price.currency, value: (price.amount / 100).toFixed(2) },
      description: "Click Indicators",
      custom_id: u.id,
    }],
    application_context: {
      brand_name: "Click Indicators",
      shipping_preference: "NO_SHIPPING",
      user_action: "PAY_NOW",
    },
  });
  if (!ok || !data || !data.id) {
    console.log("PAYPAL create order failed " + (data && JSON.stringify(data).slice(0, 300)));
    return json({ ok: false, error: "PayPal wouldn't start the order." }, 502);
  }

  // The order is bound to the buyer HERE, server-side. Capture reads the owner from this row
  // and never from the request, so presenting somebody else's order id cannot land a licence
  // on the wrong account - and the expected amount is recorded now, to be checked after.
  await env.DB.prepare(
    "INSERT INTO paypal_orders (id, user_id, amount, currency, status, created) VALUES (?,?,?,?,'created',?)"
  ).bind(data.id, u.id, price.amount, price.currency, now()).run();

  return json({ ok: true, id: data.id });
}

async function paypalCapture(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Sign in first." }, 401);
  if (!paypalConfigured(env)) return json({ ok: false, error: "PayPal isn't available yet." }, 503);

  const { orderID } = await req.json().catch(() => ({}));
  if (typeof orderID !== "string" || !/^[A-Z0-9-]{5,64}$/i.test(orderID))
    return json({ ok: false, error: "Bad request." }, 400);

  const row = await env.DB.prepare("SELECT * FROM paypal_orders WHERE id = ?").bind(orderID).first();
  // Same answer for "no such order" and "not yours", so this cannot be used to test which
  // order ids exist.
  if (!row || row.user_id !== u.id) return json({ ok: false, error: "Unknown order." }, 404);
  if (row.status === "captured") return json({ ok: true, already: true });

  // Capture, then read the order back and believe only that. A capture call can legitimately
  // fail with ORDER_ALREADY_CAPTURED - a double click, a retry - and refusing a buyer who has
  // actually paid is worse than the duplicate, so the read below is what decides.
  await paypalApi(env, "/v2/checkout/orders/" + orderID + "/capture", "POST", {});

  const check = await paypalApi(env, "/v2/checkout/orders/" + orderID, "GET");
  const o = check.data;
  if (!check.ok || !o || o.status !== "COMPLETED") {
    console.log("PAYPAL order " + orderID + " not completed: " + (o && o.status));
    return json({ ok: false, error: "PayPal didn't complete that payment." }, 402);
  }

  // Verify the money that actually moved matches what we asked for. Without this an order
  // could be approved for a different amount or currency than the one recorded at creation.
  const cap = o.purchase_units?.[0]?.payments?.captures?.[0];
  const paidCents = cap && cap.amount ? Math.round(parseFloat(cap.amount.value) * 100) : -1;
  const paidCur = cap && cap.amount ? String(cap.amount.currency_code).toUpperCase() : "";
  if (cap?.status !== "COMPLETED" || paidCents !== row.amount || paidCur !== row.currency) {
    console.log("PAYPAL amount mismatch on " + orderID + ": got " + paidCents + paidCur +
                " expected " + row.amount + row.currency + " status=" + cap?.status);
    return json({ ok: false, error: "That payment didn't match the order." }, 402);
  }

  await grantAccess(env, row.user_id, "pp_" + orderID, row.amount, row.currency.toLowerCase(), [
    env.DB.prepare("UPDATE paypal_orders SET status = 'captured', capture = ? WHERE id = ?")
      .bind(cap.id || "", orderID),
  ]);
  return json({ ok: true });
}

async function paypalWebhook(env, req) {
  const raw = await req.text();
  if (!paypalConfigured(env) || !env.PAYPAL_WEBHOOK_ID) return json({ ok: true, skipped: true });

  const certUrl = req.headers.get("paypal-cert-url") || "";
  // The cert url is echoed straight back to PayPal's verifier, so pin the host rather than
  // forwarding whatever a caller supplies.
  if (!/^https:\/\/[a-z0-9.-]*\.paypal\.com\//i.test(certUrl))
    return json({ ok: false, error: "Bad request." }, 400);

  let ev;
  try { ev = JSON.parse(raw); } catch (e) { return json({ ok: false }, 400); }

  const v = await paypalApi(env, "/v1/notifications/verify-webhook-signature", "POST", {
    transmission_id: req.headers.get("paypal-transmission-id") || "",
    transmission_time: req.headers.get("paypal-transmission-time") || "",
    cert_url: certUrl,
    auth_algo: req.headers.get("paypal-auth-algo") || "",
    transmission_sig: req.headers.get("paypal-transmission-sig") || "",
    webhook_id: env.PAYPAL_WEBHOOK_ID,
    webhook_event: ev,
  });
  if (!v.ok || !v.data || v.data.verification_status !== "SUCCESS") {
    // Unverified means anyone could POST a refund and strip a paying customer of their licence.
    console.log("PAYPAL webhook signature FAILED for " + (ev && ev.event_type));
    return json({ ok: false, error: "Bad signature." }, 400);
  }

  const t = ev.event_type || "";
  console.log("PAYPAL webhook " + t);

  // An eCheck does not complete when it is captured - it clears days later, and PayPal announces
  // that with this event. paypalCapture is right to refuse access at checkout time, because the
  // capture comes back PENDING and the money has not moved yet. But nothing then granted it when
  // the money did land, so an eCheck buyer paid, saw an error, and stayed locked out forever.
  // A real customer hit exactly that: paid on the 3rd, funds due between the 7th and the 12th.
  if (t === "PAYMENT.CAPTURE.COMPLETED") {
    const r = ev.resource || {};
    const capId = r.id || null;
    const ordId = r.supplementary_data?.related_ids?.order_id || null;
    let row = null;
    if (ordId)
      row = await env.DB.prepare("SELECT * FROM paypal_orders WHERE id = ?").bind(ordId).first();
    if (!row && capId)
      row = await env.DB.prepare("SELECT * FROM paypal_orders WHERE capture = ?").bind(capId).first();
    if (!row) {
      console.log("PAYPAL completed: no order matches " + ordId + " / " + capId);
      return json({ ok: true });
    }
    // The normal card path already granted at checkout; this event arrives for those too.
    if (row.status === "captured") return json({ ok: true });

    // Same check the checkout path makes: believe the money that actually moved, not the order.
    const paidCents = r.amount ? Math.round(parseFloat(r.amount.value) * 100) : -1;
    const paidCur = r.amount ? String(r.amount.currency_code).toUpperCase() : "";
    if (paidCents !== row.amount || paidCur !== row.currency) {
      console.log("PAYPAL completed amount mismatch on " + row.id + ": got " + paidCents + paidCur +
                  " expected " + row.amount + row.currency);
      return json({ ok: true });
    }
    await grantAccess(env, row.user_id, "pp_" + row.id, row.amount, row.currency.toLowerCase(), [
      env.DB.prepare("UPDATE paypal_orders SET status = 'captured', capture = ? WHERE id = ?")
        .bind(capId || "", row.id),
    ]);
    console.log("PAYPAL eCheck cleared - access granted for order " + row.id);
    return json({ ok: true });
  }

  if (t === "PAYMENT.CAPTURE.REFUNDED" || t === "PAYMENT.CAPTURE.REVERSED" ||
      t === "CUSTOMER.DISPUTE.CREATED") {
    // custom_id carries the user id we set at order creation; fall back to matching the
    // capture id we recorded, since a dispute object does not carry custom_id.
    const r = ev.resource || {};
    let uid = r.custom_id || null;
    const capId = r.id || r.disputed_transactions?.[0]?.seller_transaction_id || null;
    if (!uid && capId) {
      const row = await env.DB.prepare("SELECT user_id FROM paypal_orders WHERE capture = ?")
        .bind(capId).first();
      uid = row && row.user_id;
    }
    if (uid) await revokeAccess(env, uid, "paypal " + t);
    else console.log("PAYPAL revoke: could not resolve a user for " + t);
  }
  return json({ ok: true });
}

async function noteSignout(env, tokenHashes, reason) {
  if (!tokenHashes.length) return;
  try {
    await env.DB.batch([
      ...tokenHashes.map((th) => env.DB.prepare(
        "INSERT OR REPLACE INTO device_signouts (token_hash, reason, created) VALUES (?, ?, ?)"
      ).bind(th, reason, now())),
      env.DB.prepare("DELETE FROM device_signouts WHERE created < ?")
        .bind(now() - SIGNOUT_KEEP_DAYS * 86400),
    ]);
  } catch (e) {
    console.log("SIGNOUT note failed (is device_signouts migrated?):", e && e.message);
  }
}

/* ---------------- the mod talks to these two ---------------- */

async function modLogin(env, req) {
  const { email, password, device } = await req.json().catch(() => ({}));
  const mail = (email || "").trim().toLowerCase();
  const ip = clientIP(req, env);
  // signup validates the address; this did not, so any string a caller sent became a
  // login_attempts primary key by way of noteFail below.
  if (!validEmail(mail)) return json({ ok: false, error: "Wrong email or password." }, 401);

  // The cheap gate is on the CALLER, not on the account. This one can fail fast, before any
  // password hashing, because it throttles whoever is actually making the requests.
  const ipWait = await checkLock(env.DB, "modip:" + ip);
  if (ipWait) return json({ ok: false, error:
    `Too many attempts - wait ${ipWait}s, or reset your password at clickindicators.com.` }, 429);

  const u = await env.DB.prepare("SELECT * FROM users WHERE email = ?").bind(mail).first();
  // Always pay the hash - see DUMMY_SALT. `!!u &&` short-circuited, so a miss skipped 200k
  // iterations and came back fast enough to enumerate customers.
  const attempt = await hashPassword(password || "", u ? u.pw_salt : DUMMY_SALT);
  const good = !!u && timingSafeEqual(attempt, u.pw_hash);
  if (!good) {
    await noteFail(env.DB, "modip:" + ip);
    await noteFail(env.DB, "mod:" + mail);
    return json({ ok: false, error: "Wrong email or password." }, 401);
  }
  // A correct password always gets through. This endpoint has no bot check - it is called
  // from inside the game - and the lock used to be keyed on the email, which anyone can
  // read off Discord. Since the backoff saturates at half an hour and never decayed, one
  // wrong-password request every 25 minutes locked a paying customer out of their own
  // licence indefinitely, with no self-service way back. Signing in on another PC is now
  // routine, so that is a licence someone bought and cannot use.
  //
  // The IP counter is deliberately NOT cleared here any more. Clearing it turned a correct
  // password into a reset button for the only throttle this endpoint has, and the reset was
  // free: signup costs nothing, and this clear used to run BEFORE the paid check below. So an
  // attacker registered an account of their own, guessed five times at a real customer, signed
  // in to their own unpaid account to wipe the counter, and repeated - unlimited online password
  // guessing from a single address, against an endpoint that cannot show a captcha because it is
  // called from inside the game. A hit yields a token good for a year that downloads the paid
  // binary. Five free tries still apply per address, and noteFail's stale rule forgets an old run
  // an hour after it expires, so a customer who mistypes and then gets it right is not carrying
  // the count around.
  if (!u.paid)
    return json({ ok: false, error: "This account hasn't bought Click Indicators yet." }, 402);
  // Clearing the caller's throttle is safe HERE and was not safe where it used to be. It used to
  // run before the paid check, so any correct password cleared it - and signup is free, so an
  // attacker registered an account of their own, guessed at a customer, signed in to their own
  // account to wipe the counter, and repeated forever. Below the check, only a PAID account can
  // clear it, so the same attack now costs a purchase and leaves a traceable account behind.
  //
  // Restored because removing it outright punished the people it was never aimed at: a customer
  // who mistypes six times and then gets it right was left locked out for half an hour with the
  // correct password in hand. One did, within a day.
  await clearFails(env.DB, "mod:" + mail);
  await clearFails(env.DB, "modip:" + ip);

  const devName = (device || "PC").slice(0, 60);

  // Signing in claims the licence: every other device loses it. That also means
  // re-installing, or signing in twice on the same PC, can no longer strand a slot -
  // there are no slots left to strand.
  const prev = await env.DB.prepare("SELECT token_hash FROM devices WHERE user_id = ?")
    .bind(u.id).all();
  const displaced = (prev.results || []).map((r) => r.token_hash);

  const tok = randB64(32);
  // One transaction: a crash between the delete and the insert would otherwise leave
  // a paying customer holding a licence that is signed in nowhere.
  await env.DB.batch([
    env.DB.prepare("DELETE FROM devices WHERE user_id = ?").bind(u.id),
    env.DB.prepare(
      "INSERT INTO devices (id, user_id, token_hash, name, created, last_seen) VALUES (?, ?, ?, ?, ?, ?)"
    ).bind(crypto.randomUUID(), u.id, await sha256b64(tok), devName, now(), now()),
  ]);
  await noteSignout(env, displaced, "displaced");

  // The grant is what the mod verifies offline. Handing it out here as well as from /api/mod/check
  // means a fresh sign-in works immediately rather than waiting for the first revalidation.
  return json({ ok: true, token: tok, email: u.email, expires: now() + DEVICE_DAYS * 86400,
                grant: await makeGrant(env, tok), vault: await vaultKeyFor(env, u.id),
                displaced: displaced.length > 0 });
}

async function modCheck(env, req) {
  const { token } = await req.json().catch(() => ({}));
  if (!token) return json({ ok: false, valid: false, error: "No token." }, 400);
  const th = await sha256b64(token);
  const row = await env.DB.prepare(
    "SELECT d.id, u.id AS uid, u.paid FROM devices d JOIN users u ON u.id = d.user_id " +
    "WHERE d.token_hash = ?"
  ).bind(th).first();
  if (!row) {
    // Say which of the two it was. A bare "signed out" reads as the mod breaking, and
    // sends the buyer to you; "in use on another device" tells them what their own
    // licence is doing and what to do about it. Reported once, then forgotten.
    let s = null;
    try {
      s = await env.DB.prepare("SELECT reason FROM device_signouts WHERE token_hash = ?")
        .bind(th).first();
      if (s) await env.DB.prepare("DELETE FROM device_signouts WHERE token_hash = ?").bind(th).run();
    } catch (e) {
      console.log("SIGNOUT lookup failed (is device_signouts migrated?):", e && e.message);
    }
    const why = s && s.reason === "displaced"
      ? "Signed in on another device. Click Indicators works on one device at a time - " +
        "sign in again here to move it back."
      : s && s.reason === "removed"
      ? "This device was removed from your account on the website."
      : "This device was signed out.";
    return json({ ok: true, valid: false, error: why });
  }
  if (!row.paid) return json({ ok: true, valid: false, error: "This account no longer has access." });
  await env.DB.prepare("UPDATE devices SET last_seen = ? WHERE id = ?").bind(now(), row.id).run();
  // A fresh grant on every check. The mod checks every half hour while online, so a licence that
  // is still good never comes close to its 21-day expiry - and one that has been refunded, charged
  // back or signed out stops getting new ones and dies at the end of the last one it holds.
  return json({ ok: true, valid: true, grant: await makeGrant(env, token),
                vault: await vaultKeyFor(env, row.uid) });
}

async function modLogout(env, req) {
  const { token } = await req.json().catch(() => ({}));
  if (token) await env.DB.prepare("DELETE FROM devices WHERE token_hash = ?")
    .bind(await sha256b64(token)).run();
  return json({ ok: true });
}

/* ------------------------------------------------------------------ *
 * macro proxy
 *
 * The mod used to fetch macro lists and macro files straight from
 * hyperbolus.net with no credential at all, so patching the licence branch
 * out of the binary left a completely working product - which is exactly
 * what happened. The licence check gated the UI; it never gated the thing
 * people pay for.
 *
 * A boolean in the client is one instruction away from being flipped, and no
 * amount of obfuscation changes that. So the macros move behind the licence
 * instead: a build with the check removed still has to present a paid device
 * token to get a list or a file, and it cannot mint one. Cracking the binary
 * now buys an empty macro list.
 * ------------------------------------------------------------------ */

const MACRO_UA  = "ClickIndicators-Proxy/1.0";
const MACRO_MAX = 64 << 20;   // same cap the client enforces on its own buffer
const RL_WINDOW = 60;         // seconds
const RL_MAX    = 30;         // requests per device per minute
// A minute ceiling alone is not a ceiling. 90/min is 129,600/day from ONE device, which is more
// than the whole account's daily Workers allowance - so a single shared token could take the site
// down, sign-in and checkout included. A real session is a handful of requests per level, so a
// daily cap costs nobody anything and bounds the worst case at ~1.5% of the day.
const RL_DAY    = 1500;

function bearer(req) {
  const m = /^Bearer\s+(\S+)$/i.exec(req.headers.get("authorization") || "");
  return m ? m[1] : null;
}

// A paid, currently-signed-in device, or null. Deliberately the same devices row
// modCheck reads, so a licence moved to another machine loses macro access on the
// old one the moment it is displaced - no separate lifetime to drift out of sync.
async function paidDevice(env, req) {
  const tok = bearer(req);
  if (!tok) return null;
  const row = await env.DB.prepare(
    "SELECT d.id, d.user_id, u.paid FROM devices d JOIN users u ON u.id = d.user_id " +
    "WHERE d.token_hash = ?"
  ).bind(await sha256b64(tok)).first();
  return row && row.paid ? row : null;
}

// Cheap per-device ceiling. One device at a time is already enforced, so this is
// aimed at a shared token being hammered rather than at ordinary play - a level's
// worth of macros is a handful of requests.
async function rateOK(env, deviceId) {
  const w = Math.floor(now() / RL_WINDOW);
  try {
    // ONE round trip and one row written per request, not two. Writing a second row for the daily
    // bucket on every request would double the D1 write cost of the whole macro path - and D1
    // rows-written, not Workers requests, is the ceiling this account reaches first. The daily
    // counter is instead derived from the minute rows that already exist, and only consulted once
    // a device has been busy enough in the current minute to be worth asking about.
    const minute = await env.DB.prepare(
      "INSERT INTO macro_rl (device_id, window, hits) VALUES (?, ?, 1) " +
      "ON CONFLICT(device_id, window) DO UPDATE SET hits = hits + 1 RETURNING hits"
    ).bind(deviceId, w).first();
    if (minute && minute.hits > RL_MAX) return false;

    // Cheap: a read over rows we already wrote, and only for a device that is actually hammering.
    if (minute && minute.hits >= 5) {
      const since = Math.floor((now() - 86400) / RL_WINDOW);
      const r = await env.DB.prepare(
        "SELECT COALESCE(SUM(hits), 0) AS n FROM macro_rl WHERE device_id = ? AND window >= ?"
      ).bind(deviceId, since).first();
      if (r && r.n > RL_DAY) return false;
      // The ceiling is 1,500 a day and the whole library is 1,510 files, so a scrape fits inside
      // the limit that exists. It does not fit inside ordinary play: someone practising downloads
      // macros for the levels they are playing, a few dozen at the very most. This does not block
      // anyone - a false positive would cost a real customer their product - it just makes the
      // pattern visible in `wrangler tail` while the pack is still being built rather than after
      // it has been posted.
      if (r && r.n === 200) console.log("SCRAPE? device", deviceId, "has pulled", r.n, "in 24h");
      if (r && r.n === 600) console.log("SCRAPE!! device", deviceId, "has pulled", r.n, "in 24h");
    }
    return true;
  } catch (e) {
    // Never lock a paying customer out of the product because a table is missing.
    console.log("RL skipped (is macro_rl migrated?):", e && e.message);
    return true;
  }
}

/* ------------------------------------------------------------------ *
 * telegram-sourced macros
 *
 * Mirrored from t.me/gdmacros with the channel owner's permission, because Telegram exposes no
 * file bytes over HTTP - see server/migrate-tg-source.sql. Everything below reads our own R2 and
 * our own index; there is no outbound fetch on this path at all, which makes it strictly tighter
 * than the hyperbolus branch it sits beside.
 * ------------------------------------------------------------------ */

const TG_LABEL = "t.me/gdmacros";

// Servable mirrored macros for one level, best first. A file that asserted its own level id beats
// a caption-only one; after that, newest wins. Note sha256/r2_key never appear in a response.
async function tgItems(env, levelId, origin) {
  const rs = await env.DB.prepare(
    "SELECT msg_id, format, bytes, filename, level_name, credit, fps FROM tg_macros " +
    "WHERE level_id = ? AND serve = 1 ORDER BY verified DESC, msg_id DESC LIMIT 25"
  ).bind(levelId).all();
  return ((rs && rs.results) || []).map((r) => ({
    // Namespaced so it cannot collide with a hyperbolus id, which is always bare digits. The
    // client uses this as the cache filename stem, so it must stay path-safe.
    id: "tg-" + r.msg_id,
    format: r.format,
    fps: r.fps ? String(r.fps) : "",
    source: TG_LABEL,
    author: { name: r.credit || "GD Macros" },
    files: [{
      url: `${origin}/api/macro?t=${r.msg_id}`,
      filename: r.filename || "",
      bytes: r.bytes,
    }],
  }));
}

// last_seen is telemetry. It used to be an unwrapped write on the download path, so once D1's
// daily row budget ran out a paying customer got a 500 on a macro they were entitled to - the
// exact failure rateOK() already goes out of its way to avoid.
async function touchDevice(env, deviceId) {
  try {
    await env.DB.prepare("UPDATE devices SET last_seen = ? WHERE id = ?").bind(now(), deviceId).run();
  } catch (e) {
    console.log("last_seen skipped:", e && e.message);
  }
}

// Only ever a hyperbolus download slug, never a caller-supplied URL.
function slugOf(u) {
  const m = /^https?:\/\/hyperbolus\.net\/download\/([A-Za-z0-9_-]{1,64})$/.exec(String(u || ""));
  return m ? m[1] : null;
}

async function macroList(env, req, url) {
  const d = await paidDevice(env, req);
  if (!d) return json({ ok: false, error: "Sign in to Click Indicators to browse macros." }, 401);
  if (!(await rateOK(env, d.id)))
    return json({ ok: false, error: "Too many requests - give it a moment." }, 429);

  const lvl  = String(url.searchParams.get("level_id") || "");
  const page = String(url.searchParams.get("page") || "1");
  if (!/^\d{1,12}$/.test(lvl) || !/^\d{1,4}$/.test(page))
    return json({ ok: false, error: "Bad request." }, 400);

  // From the caller's url, not req.url: the macro links in this response are fetched by the mod
  // afterwards, so they have to name the host the mod can actually reach.
  const origin = url.origin;

  // Two independent sources. Either can fail without taking the other down - the channel mirror
  // is the only macro source for a lot of levels hyperbolus has never had, so a hyperbolus outage
  // must not empty those lists, and vice versa.
  let data = null, upErr = null;
  try {
    // BOUNDED, because an unbounded wait here is not paid by this Worker - it is paid by the
    // player looking at "Searching for macros...".
    //
    // hyperbolus is usually slow rather than dead - about a second on a good day, two and a half
    // on a normal one - but it does sometimes accept the connection and then never answer at all.
    // With no timeout this fetch waits through that, the mod's own 15s expires, and the mod reads
    // a request that never came back as "this host is unreachable" and moves to the next name on
    // its list. One stalled upstream therefore became three fifteen-second waits and then
    // "Offline".
    //
    // Nine seconds, chosen against the mod's fifteen rather than against the upstream: what
    // matters is that an answer of SOME kind always arrives before the client gives up, because a
    // 502 costs one round trip and a client-side timeout costs three. It is also several times
    // the slowest healthy response measured, so it does not truncate a working request.
    const up = await fetch(`https://hyperbolus.net/api/macros?level_id=${lvl}&page=${page}`, {
      headers: { "user-agent": MACRO_UA, accept: "application/json" },
      signal: AbortSignal.timeout(9000),
    });
    if (!up.ok) upErr = `Macro server returned ${up.status}.`;
    else data = await up.json();
  } catch (e) {
    // A timeout lands here too, and is reported as what it is rather than as a dead server: the
    // other source may still have this level, and the client must not treat a slow upstream as a
    // reason to go looking for a different one of our hostnames.
    upErr = (e && e.name === "TimeoutError")
      ? "The macro server did not answer in time."
      : "Could not reach the macro server.";
    console.log("MACROS upstream failed", lvl, page, String(e && e.name));
  }
  if (!data || typeof data !== "object" || !Array.isArray(data.data)) {
    data = { data: [], next_page_url: "" };
  }

  // GATHER THE REST OF THE PAGES HERE, NOT ONE ROUND TRIP AT A TIME FROM THE GAME.
  //
  // Upstream pages hold fifteen macros, and a popular level has two or three. The mod walked them
  // sequentially, so every extra page was another full trip out of the player's machine, through
  // here, out to a source that takes one to three seconds, and back - and all of it behind a
  // status line that just says "Searching for macros..." with nothing to show it is working.
  //
  // Fetched in parallel from here instead, where the trip is short and they overlap, and returned
  // as one list. The wait stops being (pages x slow) and becomes (slowest page), and it improves
  // for every install that already exists, including the ones that will never be updated, because
  // nothing about the client has to change: it asks for page one and gets everything.
  //
  // Capped, and honest about the cap: past this the response keeps its next_page_url and the mod
  // carries on as it always did, rather than this quietly returning a truncated list that looks
  // complete.
  const MAX_EXTRA_PAGES = 4;
  // Which page the client should ask for next, if any. Without this the response below would
  // always say "page + 1" - which after merging pages 2..5 into this one would hand back a page
  // the caller already has, and it would see the same macros twice.
  let nextPageNum = Number(page) + 1;
  if (page === "1" && Number(data.last_page) > 1) {
    const last = Math.min(Number(data.last_page), 1 + MAX_EXTRA_PAGES);
    const rest = [];
    for (let p = 2; p <= last; p++) rest.push(p);
    const pages = await Promise.all(rest.map(async (p) => {
      try {
        const r = await fetch(`https://hyperbolus.net/api/macros?level_id=${lvl}&page=${p}`, {
          headers: { "user-agent": MACRO_UA, accept: "application/json" },
          signal: AbortSignal.timeout(9000),
        });
        if (!r.ok) return [];
        const j = await r.json();
        return Array.isArray(j && j.data) ? j.data : [];
      } catch { return []; }   // one slow page must not lose the pages that did arrive
    }));
    for (const arr of pages) data.data = data.data.concat(arr);
    const more = Number(data.last_page) > last;
    data.next_page_url = more ? "more" : "";   // only its emptiness is read below
    nextPageNum = last + 1;
    console.log("MACROS merged", lvl, "pages 1.." + last, "->", data.data.length, "items");
  }

  // Rewrite every file URL to point back here. The upstream address never reaches the
  // client, so stripping the proxy out of a patched build does not reveal where to go
  // instead - the caller would have to rediscover and reimplement the whole source.
  // Projected to the seven fields the client actually reads, rather than forwarded whole. The
  // upstream object carries the storage key ("path": "macros/lEAK...bin"), internal ids, and other
  // people's account details (last_seen, time_online) - none of which we need, none of which a
  // macro list is the right place to publish, and all of which was going out on every request.
  data.data = data.data.map((m) => {
    const f0 = (m.files || [])[0] || {};
    const slug = slugOf(f0.url);
    return {
      id: m.id,
      format: m.format,
      fps: m.fps,
      source: "hyperbolus.net",
      author: { name: (m.author && m.author.name) || "" },
      files: [{
        url: slug ? `${origin}/api/macro?f=${encodeURIComponent(slug)}` : "",
        filename: f0.filename || "",
        bytes: f0.bytes || 0,
      }],
    };
  }).filter((m) => m.files[0].url);

  // Appended on page 1 only, and AFTER the hyperbolus entries. The client's auto-fetch takes the
  // first result, so ordering this way makes the new source purely additive: it can never displace
  // a macro that already worked, and it becomes the first result exactly where there was none.
  if (page === "1") {
    try {
      const tg = await tgItems(env, Number(lvl), origin);
      if (tg.length) data.data = data.data.concat(tg);
    } catch (e) {
      console.log("tg source skipped (is migrate-tg-source.sql applied?):", e && e.message);
    }
  }

  // Only a total failure is an error, and only on page 1. The client accumulates pages and throws
  // the whole accumulator away on any non-2xx, so answering a failed page 2 with 502 would discard
  // the page-1 results it already holds - including every mirrored row, which are only ever
  // attached to page 1. An empty final page keeps what it has.
  if (!data.data.length && upErr) {
    if (page === "1") return json({ ok: false, error: upErr }, 502);
    return json({ data: [], next_page_url: "" }, 200, { "cache-control": "private, no-store" });
  }

  // Built explicitly rather than forwarding the upstream body. hyperbolus answers with a Laravel
  // paginator carrying absolute https://hyperbolus.net/... in path, first_page_url, last_page_url,
  // links[] and next_page_url - so returning it verbatim published the upstream address on every
  // successful response, which is exactly what the rewrite above exists to prevent. The client
  // reads only `data` and tests `next_page_url` for emptiness.
  const more = !!(data.next_page_url && data.data.length);
  return json({
    data: data.data,
    next_page_url: more ? `${origin}/api/macros?level_id=${lvl}&page=${nextPageNum}` : "",
  }, 200, { "cache-control": "private, no-store" });
}

// --- per-buyer watermark -------------------------------------------------------------------------
//
// Sealing the cache made a leaked macro FOLDER worthless. It did nothing about the one leak that is
// still open: a buyer pulling the library through the API with their own token and posting the
// plaintext. Nothing stops that, because from the server's side it is a customer using the product.
//
// So every macro that leaves here carries 32 bytes naming who it was served to. Not the account id -
// a tag derived from that account's vault key - so the file identifies the buyer to US and to nobody
// else, and cannot be forged onto someone innocent. A leaked pack stops being anonymous, which turns
// one person sharing into one person who gets refunded and revoked.
//
// It is not unremovable. Anyone who compares two accounts' copies of the same macro sees the 32
// bytes differ and can cut them off. That is the honest bar: it catches the ordinary case, which is
// one buyer posting a zip, and it costs a real one nothing.
const WM_MAGIC = [0x43, 0x49, 0x57, 0x4d, 0x31];   // "CIWM1"
const WM_LEN = 32;

// Does this client know what a watermark is?
//
// The site publishes v1.0.19. Stripping the 32-byte trailer landed in v1.0.70, so appending one for
// everybody would hand a file with 32 unexplained bytes to a build that has never heard of them -
// and a JSON macro would simply stop loading, for every customer still on the published build.
// There is no version of "deploy the server first" that makes that safe.
//
// So the mod reports its version and the server decides. An old client gets exactly what it gets
// today; a new one gets a marked file. Nobody has to sequence anything.
const WM_MIN = [1, 0, 70];

function clientAtLeast(req, want) {
  const m = /ClickGuide\/(\d+)\.(\d+)\.(\d+)/.exec(req.headers.get("user-agent") || "");
  if (!m) return false;                 // "ClickGuide/1.0", or anything else: treat as old
  for (let i = 0; i < 3; i++) {
    const v = Number(m[i + 1]);
    if (v !== want[i]) return v > want[i];
  }
  return true;
}

async function watermark(env, userId, macroKey, bytes, req) {
  if (!clientAtLeast(req, WM_MIN)) return bytes;
  try {
    const row = await env.DB.prepare("SELECT vault_key FROM users WHERE id = ?")
      .bind(userId).first();
    // No key yet means no mark. Serving unmarked beats serving nothing.
    if (!row || !row.vault_key) return bytes;
    const digest = await crypto.subtle.digest(
      "SHA-256", enc.encode(row.vault_key + "\u001f" + macroKey));
    const src = new Uint8Array(bytes);
    const out = new Uint8Array(src.length + WM_LEN);
    out.set(src, 0);
    const t = new Uint8Array(WM_LEN);
    t.set(WM_MAGIC, 0);
    t.set(new Uint8Array(digest).slice(0, 16), 5);
    new DataView(t.buffer).setBigInt64(21, BigInt(now()), true);
    out.set(t, src.length);
    return out.buffer;
  } catch (e) {
    console.log("WM skipped:", e && e.message);
    return bytes;
  }
}

async function macroFile(env, req, url) {
  const d = await paidDevice(env, req);
  if (!d) return json({ ok: false, error: "Sign in to Click Indicators to download macros." }, 401);
  if (!(await rateOK(env, d.id)))
    return json({ ok: false, error: "Too many requests - give it a moment." }, 429);

  // A mirrored macro: served from our own R2, no outbound fetch. The key is read from the row
  // rather than taken from the caller, so there is nothing here to point anywhere else.
  const t = url.searchParams.get("t");
  if (t !== null) {
    if (!/^\d{1,12}$/.test(t)) return json({ ok: false, error: "Bad request." }, 400);
    const row = await env.DB.prepare(
      "SELECT r2_key, bytes FROM tg_macros WHERE msg_id = ? AND serve = 1"
    ).bind(Number(t)).first();
    if (!row) return json({ ok: false, error: "That macro is no longer available." }, 404);

    const obj = await env.MACROS.get(row.r2_key);
    if (!obj) return json({ ok: false, error: "That macro is no longer available." }, 404);

    await touchDevice(env, d.id);
    // Buffered rather than streamed now, because the mark goes on the end. These are kilobytes.
    const body = await watermark(env, d.user_id, row.r2_key, await obj.arrayBuffer(), req);
    return new Response(body, {
      headers: {
        "content-type": "application/octet-stream",
        "content-length": String(body.byteLength),
        "cache-control": "private, no-store",
      },
    });
  }

  // An allowlisted slug, never a URL parameter. Accepting a URL here would turn a paid
  // endpoint into an open proxy that any account holder could aim at anything.
  const f = url.searchParams.get("f") || "";
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(f)) return json({ ok: false, error: "Bad request." }, 400);

  let up;
  try {
    up = await fetch(`https://hyperbolus.net/download/${f}`, {
      headers: { "user-agent": MACRO_UA },
      redirect: "follow",   // /download/{slug} 302s to storage
    });
  } catch (e) {
    return json({ ok: false, error: "Could not reach the macro server." }, 502);
  }
  if (!up.ok) return json({ ok: false, error: `Macro server returned ${up.status}.` }, 502);

  const declared = Number(up.headers.get("content-length") || 0);
  if (declared > MACRO_MAX) return json({ ok: false, error: "That macro is too large." }, 413);
  const buf = await up.arrayBuffer();
  if (buf.byteLength > MACRO_MAX) return json({ ok: false, error: "That macro is too large." }, 413);

  await touchDevice(env, d.id);

  const marked = await watermark(env, d.user_id, f, buf, req);
  return new Response(marked, {
    headers: {
      "content-type": "application/octet-stream",
      "content-length": String(marked.byteLength),
      "cache-control": "private, no-store",
    },
  });
}

/* ------------------------------------------------------------------ *
 * downloads
 *
 * The mod is not on the Geode index, so the .geode has to come from here.
 * It used to sit in the site's public folder, where anyone could take the
 * binary without buying - the licence check inside the mod still stopped it
 * running, but there is no reason to hand out the thing being licensed.
 * Only the newest build is served: older ones are listed with their notes so
 * people can see what changed, never offered, because supporting a version
 * whose bugs are already fixed is time nobody has.
 * ------------------------------------------------------------------ */
function releaseInfo(env) {
  return {
    ok: true,
    // Whether the site sells. Derived from the key actually in use rather than a
    // flag in the pages, because the failure it prevents is selling for real money
    // through a test-mode key - and that flag lived in two files that had to be
    // flipped together. A test key means "Launching soon", with no way to forget.
    live: String(env.STRIPE_SECRET || "").startsWith("sk_live_"),
    // Public by design - it goes in the PayPal SDK's script url. The page uses its presence to
    // decide whether to render the PayPal button at all, so a half-configured deploy shows one
    // working checkout rather than a button that dead-ends.
    paypal_client_id: paypalConfigured(env) ? env.PAYPAL_CLIENT_ID : null,
    latest: {
      version: LATEST.version, date: LATEST.date, size: LATEST.size,
      sha256: LATEST.sha256, filename: LATEST.filename, notes: LATEST.notes,
    },
    history: HISTORY.map((h) => ({ version: h.version, notes: h.notes })),
  };
}

// --- per-download binary watermark ---------------------------------------------------------------
//
// A Discord that redistributes cracked builds has a channel named after this mod. What it passes
// around is the BINARY, so that is where the mark has to go: every .geode handed out from here
// carries 32 bytes naming the account that downloaded it, and any copy of it posted anywhere names
// them too.
//
// It goes in the zip's trailing comment field. A .geode IS a zip, the comment is part of the
// End Of Central Directory record, and every zip reader in existence - including the one Geode
// installs with - ignores it. Nothing inside the archive changes, so the mod that comes out is
// byte-identical to the one that went in.
//
// The tag is over the account's vault key AND the version, so a mark lifted off one release cannot
// be pinned onto another, and it identifies the buyer to us without putting their address in a file
// that is about to be passed around.
function zipSetComment(buf, comment) {
  const b = new Uint8Array(buf);
  // The EOCD is the last 22 bytes plus any existing comment, and a comment is capped at 65,535.
  let eocd = -1;
  const from = Math.max(0, b.length - 66000);
  for (let i = b.length - 22; i >= from; i--) {
    if (b[i] === 0x50 && b[i + 1] === 0x4b && b[i + 2] === 0x05 && b[i + 3] === 0x06) {
      // Compressed data can contain those four bytes by chance. A real EOCD also declares how long
      // its comment is, and that has to be exactly the number of bytes left in the file - so this
      // costs nothing and makes a false match effectively impossible.
      const declared = b[i + 20] | (b[i + 21] << 8);
      if (i + 22 + declared !== b.length) continue;
      eocd = i;
      break;
    }
  }
  if (eocd < 0) return null;                    // not a zip we recognise - serve it untouched
  const out = new Uint8Array(eocd + 22 + comment.length);
  out.set(b.subarray(0, eocd + 22), 0);         // truncating here also drops any existing comment
  out.set(comment, eocd + 22);
  out[eocd + 20] = comment.length & 0xff;
  out[eocd + 21] = (comment.length >> 8) & 0xff;
  return out;
}

function hex(buf) {
  return [...new Uint8Array(buf)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

async function markedGeode(env, userId) {
  const plain = { body: GEODE_BYTES, size: LATEST.size, sha: LATEST.sha256 };
  try {
    const row = await env.DB.prepare("SELECT vault_key FROM users WHERE id = ?")
      .bind(userId).first();
    if (!row || !row.vault_key) return plain;   // no key yet - hand over the plain build
    const digest = await crypto.subtle.digest(
      "SHA-256", enc.encode(row.vault_key + String.fromCharCode(31) + "geode:" + LATEST.version));
    const t = new Uint8Array(WM_LEN);
    t.set(WM_MAGIC, 0);
    t.set(new Uint8Array(digest).slice(0, 16), 5);
    new DataView(t.buffer).setBigInt64(21, BigInt(now()), true);
    const out = zipSetComment(GEODE_BYTES, t);
    if (!out) return plain;
    return { body: out, size: out.byteLength, sha: hex(await crypto.subtle.digest("SHA-256", out)) };
  } catch (e) {
    // A download must never fail because the mark could not be made.
    console.log("BINWM skipped:", e && e.message);
    return plain;
  }
}

async function download(env, req) {
  const u = await userFromSession(env, req);
  // 401/403 rather than a redirect: the dashboard fetches this and wants to know
  // which of the two it is, and a browser following a redirect to an HTML page
  // would save the page as a .geode.
  if (!u) return json({ ok: false, error: "Sign in to download Click Indicators." }, 401);
  if (!u.paid) return json({ ok: false, error: "This account hasn't bought Click Indicators yet." }, 403);

  const g = await markedGeode(env, u.id);
  return new Response(g.body, {
    headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="${LATEST.filename}"`,
      "content-length": String(g.size),
      "x-ci-version": LATEST.version,
      // The hash OF THE FILE BEING SENT, not of the reference build - they differ by the 32-byte
      // tag, and a header that did not match what came down the wire would be worse than none.
      "x-ci-sha256": g.sha,
      "x-ci-build-sha256": LATEST.sha256,
      // A paid download must never be shared by a CDN or a proxy. Doubly so now that no two are
      // the same file.
      "cache-control": "private, no-store",
    },
  });
}

// The same binary as /api/download, for the mod rather than the dashboard.
//
// It needs its own door because the two present different credentials: a browser sends the
// session cookie, and the mod only ever has a device token. Off the Geode index the loader's
// own updater never sees this mod, so without this a buyer has to notice a Discord post and
// reinstall by hand - and most of them never do, which is how 62 people ended up still on an
// old build. Same paid check as everything else, so an unlicensed copy cannot self-update.
async function modDownload(env, req) {
  const d = await paidDevice(env, req);
  if (!d) return json({ ok: false, error: "Sign in to Click Indicators to update." }, 401);
  await env.DB.prepare("UPDATE devices SET last_seen = ? WHERE id = ?").bind(now(), d.id).run();
  const g = await markedGeode(env, d.user_id);
  return new Response(g.body, {
    headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="${LATEST.filename}"`,
      "content-length": String(g.size),
      "x-ci-version": LATEST.version,
      "x-ci-sha256": g.sha,
      "x-ci-build-sha256": LATEST.sha256,
      "cache-control": "private, no-store",
    },
  });
}

async function removeDevice(env, req) {
  const u = await userFromSession(env, req);
  if (!u) return json({ ok: false, error: "Not signed in." }, 401);
  const { id } = await req.json().catch(() => ({}));
  // Recorded so that install can say it was removed here, rather than reporting the
  // same blank "signed out" it would show for a licence claimed on another PC.
  const d = await env.DB.prepare("SELECT token_hash FROM devices WHERE id = ? AND user_id = ?")
    .bind(id, u.id).first();
  await env.DB.prepare("DELETE FROM devices WHERE id = ? AND user_id = ?").bind(id, u.id).run();
  if (d) await noteSignout(env, [d.token_hash], "removed");
  return json({ ok: true });
}

/**
 * SHARED PATH CACHE.
 *
 * The mod works out where a macro's run actually goes by replaying it through the game, which takes
 * around ten seconds of a level's first load. The result depends only on the level and the macro's
 * inputs, so it is the same for every customer who ever loads that pair - there is no reason for
 * more than one person to compute it.
 *
 * Keyed by the level id and a hash of the inputs, so a different macro is a different path and
 * nobody is served somebody else's route. Stored as an opaque blob: the worker never parses it and
 * has no opinion about what is inside.
 */
const PATH_MAX = 2 * 1024 * 1024;

function pathKeyOK(k) {
  return typeof k === "string" && /^gh-\d{1,12}-[0-9a-f]{1,16}$/.test(k);
}

async function pathGet(env, req, url) {
  const k = url.searchParams.get("k") || "";
  if (!pathKeyOK(k)) return json({ ok: false, error: "Bad key." }, 400);
  const obj = await env.MACROS.get("paths/" + k);
  if (!obj) return json({ ok: false, error: "Not cached." }, 404);
  return new Response(obj.body, {
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "public, max-age=86400" },
  });
}

async function pathPut(env, req) {
  // Contributing needs a licence, same as everything else here: it keeps the bucket to paying
  // customers and gives every upload an account it can be traced back to. Same device-token check
  // the mod already makes on every start, so no new credential is involved.
  let body;
  try { body = await req.json(); } catch { return json({ ok: false, error: "Bad JSON." }, 400); }
  const tok = body && body.token;
  if (!tok) return json({ ok: false, error: "Not signed in." }, 401);
  const row = await env.DB.prepare(
    "SELECT u.id AS uid, u.paid FROM devices d JOIN users u ON u.id = d.user_id " +
    "WHERE d.token_hash = ?"
  ).bind(await sha256b64(tok)).first();
  if (!row || !row.paid) return json({ ok: false, error: "Not signed in." }, 401);
  const k = body && body.key;
  const data = body && body.data;
  if (!pathKeyOK(k)) return json({ ok: false, error: "Bad key." }, 400);
  if (typeof data !== "string" || !data.length || data.length > PATH_MAX)
    return json({ ok: false, error: "Bad payload." }, 400);
  // First writer wins. A path is deterministic, so a second one should be identical - and if it is
  // not, the difference is a bug or a tampered client, and overwriting would spread it.
  const have = await env.MACROS.head("paths/" + k);
  if (have) return json({ ok: true, stored: false });
  await env.MACROS.put("paths/" + k, data);
  console.log("PATH", k, data.length, row.uid);
  return json({ ok: true, stored: true });
}

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    // Handlers below build redirects and links off this, so it has to carry the hostname the
    // client actually used, not the workers.dev name the failover proxy reaches us by.
    const ph = publicHost(req, env);
    if (ph) url.host = ph;
    const p = url.pathname;
    if (req.method === "OPTIONS") return new Response(null, { status: 204 });

    try {
      if (req.method === "POST") {
        if (p === "/api/signup")         return await signup(env, req);
        if (p === "/api/login")          return await login(env, req);
        if (p === "/api/checkout")       return await checkout(env, req, url);
        if (p === "/api/reclaim")        return await reclaim(env, req);
        if (p === "/api/stripe/webhook") return await webhook(env, req);
        // PayPal. order/capture need the session cookie; the webhook is PayPal calling us and
        // authenticates by signature instead - see paypalWebhook.
        if (p === "/api/paypal/order")   return await paypalOrder(env, req);
        if (p === "/api/paypal/capture") return await paypalCapture(env, req);
        if (p === "/api/paypal/webhook") return await paypalWebhook(env, req);
        if (p === "/api/verify")         return await verifyCode(env, req);
        if (p === "/api/forgot")         return await forgot(env, req, url);
        if (p === "/api/reset")          return await resetPassword(env, req);
        if (p === "/api/password")       return await changePassword(env, req);
        if (p === "/api/resend-code")    return await resendCode(env, req);
        if (p === "/api/mod/login")      return await modLogin(env, req);
        if (p === "/api/mod/check")      return await modCheck(env, req);
        if (p === "/api/path")           return await pathPut(env, req);
        if (p === "/api/mod/logout")     return await modLogout(env, req);
        if (p === "/api/device/remove")  return await removeDevice(env, req);
        if (p === "/api/discord/unlink") return await discordUnlink(env, req);
        if (p === "/api/logout") {
          const tok = cookieOf(req, "ci_session");
          if (tok) await env.DB.prepare("DELETE FROM sessions WHERE token_hash = ?")
            .bind(await sha256b64(tok)).run();
          return json({ ok: true }, 200, { "set-cookie": "ci_session=; Path=/; Max-Age=0" });
        }
      }
      if (req.method === "GET") {
        if (p === "/api/me")       return await me(env, req);
        if (p === "/api/discord/start")    return await discordStart(env, req, url);
        if (p === "/api/discord/callback") return await discordCallback(env, req, url);
        if (p === "/api/release")  return json(releaseInfo(env));
        if (p === "/api/download") return await download(env, req);
        // GET, not POST: the mod fetches these the same way it fetched the macro source
        // before, so only the address and the Authorization header changed.
        if (p === "/api/macros")   return await macroList(env, req, url);
        if (p === "/api/macro")    return await macroFile(env, req, url);
        if (p === "/api/mod/download") return await modDownload(env, req);
        // A path is expensive to work out and identical for everybody: the same macro on the same
        // level produces the same run, because the game is deterministic. Whoever loads it first
        // pays the ten seconds; everyone after that downloads the answer.
        if (p === "/api/path")     return await pathGet(env, req, url);
      }
    } catch (e) {
      // Log for `wrangler tail`, but never hand a stack trace to the client.
      console.log("ERR", p, e && e.message, e && e.stack);
      return json({ ok: false, error: "Server error." }, 500);
    }
    return json({ ok: false, error: "Not found." }, 404);
  },
};
