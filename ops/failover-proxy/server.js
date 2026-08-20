// Failover reverse proxy for clickindicatorsmod.com
//
// WHY THIS EXISTS
// ---------------
// Cloudflare's edge stopped serving the clickindicatorsmod.com zone entirely: it accepts the
// TCP connection and then resets it, on port 443 AND on port 80, for every hostname in the zone
// including ones that have never had a DNS record. Plain HTTP with no TLS is reset too, so no
// certificate explanation fits. The same edge IP serves other zones normally, the Workers
// themselves are healthy on workers.dev, and the zone shows Active with a valid certificate,
// no abuse reports and no billing hold. It is broken inside Cloudflare and only Cloudflare can
// fix it - and free-plan support did not answer for a day while every paying customer was
// locked out.
//
// The deployed mod hardcodes https://clickindicatorsmod.com/api/..., so the clients cannot be
// pointed anywhere else. The only thing still under our control is where that hostname
// RESOLVES. So this process runs somewhere that is not Cloudflare, the apex record is switched
// to DNS-only (grey cloud) pointing here, and traffic reaches the Workers through the back door
// at workers.dev - which works fine, because workers.dev is a different zone.
//
// Nothing moves. D1, R2, the licence keys and both Workers stay exactly where they are. Undoing
// it is one DNS change.
//
// WHAT IT HAS TO GET RIGHT
// ------------------------
//   * The real client IP. The Worker rate-limits logins and checks Turnstile against
//     cf-connecting-ip, and Cloudflare overwrites that header at its own edge with whoever
//     opened the connection - which, through here, is this proxy. Every customer in the world
//     would share one IP and the login limiter would lock all of them out at once. So the real
//     address is forwarded in a header of our own, authenticated with a shared secret, and the
//     Worker prefers it only when that secret matches.
//   * The public hostname. Anything the Worker hands back - macro download links, OAuth
//     redirects, Stripe returns, password-reset links - is built from the request's own origin,
//     which through here would be the workers.dev name. X-Forwarded-Host carries the real one.
//   * Bodies stream. The gated mod download is ~2.4 MB and macros are up to a few MB; buffering
//     them would hold the whole file in memory per request for no reason.

import http from "node:http";
import { Readable } from "node:stream";

const PORT = Number(process.env.PORT || 8080);

// The Workers, reached by their workers.dev names. These are the same two Workers the broken
// zone was routing to; only the way in is different.
const API_ORIGIN  = process.env.API_ORIGIN  || "https://clickindicators-api.msmithbh9.workers.dev";
const SITE_ORIGIN = process.env.SITE_ORIGIN || "https://autumn-paper-2321.msmithbh9.workers.dev";

// The hostname the outside world uses, and the one every generated URL must keep saying.
const PUBLIC_HOST = process.env.PUBLIC_HOST || "clickindicatorsmod.com";

// Where readers of the old site are sent. It is served by a healthy Cloudflare zone directly,
// with none of this in the path - which is the point: every browser that follows this redirect
// stops costing egress here and stops having its traffic decrypted by a third party.
const CANONICAL_HOST = process.env.CANONICAL_HOST || "clickindicators.com";

// Shared with the Worker via `wrangler secret put CI_PROXY_SECRET`. Without it the Worker
// ignores our forwarded IP, which is the safe direction to fail: the Worker is publicly
// reachable on workers.dev, so anyone could otherwise claim any address and walk through the
// per-IP rate limits.
const PROXY_SECRET = process.env.CI_PROXY_SECRET || "";

// Hop-by-hop headers belong to a single connection and must not be relayed (RFC 9110 7.6.1).
// accept-encoding is dropped separately so upstream answers uncompressed - fetch() would
// transparently decompress the body and leave a content-encoding header describing bytes we
// are no longer sending, which browsers read as a corrupt response.
const HOP = new Set([
  "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
  "te", "trailer", "transfer-encoding", "upgrade",
]);

// Headers a client must never be able to set for itself. cf-connecting-ip is the address the
// Worker trusts, and x-ci-* is how we tell the Worker something is trustworthy - so both are
// stripped off the inbound request before anything of ours is added.
const FORBIDDEN_IN = new Set([
  "cf-connecting-ip", "x-ci-client-ip", "x-ci-proxy-secret", "x-forwarded-host",
]);

function clientIp(req) {
  // The RIGHTMOST entry, not the leftmost.
  //
  // X-Forwarded-For is a trail that each proxy appends its own view of the peer to, and anything
  // to the left of the entry written by the last trusted hop was supplied by the client. A client
  // can simply send "X-Forwarded-For: 1.2.3.4"; if Railway appends rather than replaces, the
  // header arrives as "1.2.3.4, <real address>" and reading the leftmost value hands an attacker
  // a free choice of identity.
  //
  // That matters here specifically because this value becomes cf-connecting-ip at the Worker,
  // which is what the login limiter counts and what Turnstile is told - so trusting the wrong end
  // of this header turns per-IP rate limiting off for anyone who knows it exists, which is the
  // one control standing between a password and an unlimited number of guesses.
  //
  // Railway is the only hop in front of this process, so the entry it appended is the last one,
  // and reading the last entry is correct whether it appends to a client-supplied header or
  // replaces it outright.
  const xff = req.headers["x-forwarded-for"];
  if (typeof xff === "string" && xff.length) {
    const parts = xff.split(",").map((s) => s.trim()).filter(Boolean);
    if (parts.length) return parts[parts.length - 1];
  }
  return req.socket.remoteAddress || "";
}

const server = http.createServer(async (req, res) => {
  // Railway's own health probe, and a way to see this process is alive without touching a Worker.
  if (req.url === "/__proxy_health") {
    res.writeHead(200, { "content-type": "text/plain" });
    res.end("ok\n");
    return;
  }

  // The site answered http with a redirect before the outage; keep that so nothing that used to
  // work over plain http silently changes behaviour.
  if ((req.headers["x-forwarded-proto"] || "").toString().split(",")[0].trim() === "http") {
    res.writeHead(301, { location: `https://${PUBLIC_HOST}${req.url}` });
    res.end();
    return;
  }

  // Send people reading the site on the old domain to the new one.
  //
  // Only browsers, and only on the old domain. /api/ is deliberately exempt: roughly 1,300
  // installs of v1.0.19 have clickindicatorsmod.com compiled in with no fallback, and since the
  // mod is not on the Geode index its only update path is /api/mod/download on that same host.
  // Redirecting the API would cut off the very people who cannot yet move, and a redirect on a
  // POST is its own kind of broken.
  //
  // The proxy's own railway.app address is also exempt, and is left serving the site directly.
  // It is the last address that still answers for anyone who can reach neither domain, and
  // bouncing it to a name they cannot resolve would take that away.
  //
  // 302 rather than 301: the new domain has been serving for hours, not months, and a permanent
  // redirect is cached hard enough by browsers that undoing it means waiting out other people's
  // caches. Worth promoting once it has proven itself.
  const reqHost = String(req.headers["host"] || "").toLowerCase().split(":")[0];
  const isOldDomain = reqHost === "clickindicatorsmod.com" || reqHost === "www.clickindicatorsmod.com";
  if (isOldDomain && !req.url.startsWith("/api/") && req.url !== "/__proxy_health") {
    res.writeHead(302, {
      location: `https://${CANONICAL_HOST}${req.url}`,
      "cache-control": "no-store",
    });
    res.end();
    return;
  }

  let upstream;
  try {
    const base = req.url.startsWith("/api/") ? API_ORIGIN : SITE_ORIGIN;
    upstream = new URL(req.url, base);
    upstream.protocol = "https:";
    const b = new URL(base);
    upstream.host = b.host;
  } catch {
    res.writeHead(400, { "content-type": "text/plain" });
    res.end("bad request\n");
    return;
  }

  const headers = new Headers();
  for (const [k, v] of Object.entries(req.headers)) {
    const key = k.toLowerCase();
    if (HOP.has(key) || FORBIDDEN_IN.has(key)) continue;
    if (key === "host" || key === "accept-encoding" || key === "content-length") continue;
    if (Array.isArray(v)) { for (const one of v) headers.append(key, one); }
    else if (v != null) headers.set(key, String(v));
  }
  headers.set("accept-encoding", "identity");
  headers.set("x-forwarded-host", PUBLIC_HOST);
  headers.set("x-ci-client-ip", clientIp(req));
  if (PROXY_SECRET) headers.set("x-ci-proxy-secret", PROXY_SECRET);

  const hasBody = req.method !== "GET" && req.method !== "HEAD";

  let up;
  try {
    up = await fetch(upstream, {
      method: req.method,
      headers,
      body: hasBody ? Readable.toWeb(req) : undefined,
      duplex: hasBody ? "half" : undefined,
      redirect: "manual",
    });
  } catch (err) {
    console.error("upstream failed", req.method, req.url, String(err));
    res.writeHead(502, { "content-type": "text/plain" });
    res.end("upstream unavailable\n");
    return;
  }

  const out = {};
  for (const [k, v] of up.headers) {
    const key = k.toLowerCase();
    if (HOP.has(key) || key === "content-encoding" || key === "content-length") continue;
    if (key === "set-cookie") continue; // handled below - there can be several
    out[key] = v;
  }
  // getSetCookie keeps multiple Set-Cookie headers separate; reading them through the normal
  // header accessor would join them with commas and produce one broken cookie.
  const cookies = up.headers.getSetCookie ? up.headers.getSetCookie() : [];
  if (cookies.length) out["set-cookie"] = cookies;

  res.writeHead(up.status, out);
  if (!up.body) { res.end(); return; }
  Readable.fromWeb(up.body).pipe(res).on("error", (e) => {
    console.error("stream failed", req.url, String(e));
    res.destroy();
  });
});

server.listen(PORT, () => {
  console.log(`failover proxy listening on ${PORT}`);
  console.log(`  /api/* -> ${API_ORIGIN}`);
  console.log(`  else   -> ${SITE_ORIGIN}`);
  console.log(`  public host: ${PUBLIC_HOST}`);
  console.log(`  client-ip forwarding: ${PROXY_SECRET ? "enabled" : "DISABLED (no CI_PROXY_SECRET)"}`);
});
