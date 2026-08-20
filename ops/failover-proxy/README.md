# Failover proxy for clickindicatorsmod.com

Serves the site and API from outside Cloudflare while Cloudflare's edge refuses to serve the
zone. Nothing moves: D1, R2 and both Workers stay exactly where they are. The proxy forwards to
them through `workers.dev`, which is a different zone and works normally.

## Why

Cloudflare's edge accepts the TCP connection for every hostname in `clickindicatorsmod.com` and
then resets it, on port 443 **and** port 80, including hostnames that have never had a DNS
record. Plain HTTP with no TLS is reset too, so no certificate explanation fits. The same edge
IP serves other zones normally. Zone Active, Universal certificate Active, no abuse reports, no
billing hold. Removing the Workers Custom Domain, rebuilding the apex as a Workers Route, and
deleting and re-adding the zone all changed nothing — the re-add came back with the *same* zone
ID, so Cloudflare restored the broken state rather than provisioning fresh.

The deployed mod hardcodes `https://clickindicatorsmod.com/api/...`, so ~1,300 clients cannot be
repointed. The only thing still under our control is where that hostname **resolves**.

## Shape

```
mod / browser ──► clickindicatorsmod.com  (DNS-only, grey cloud, → Railway)
                        │
                        ▼
                  failover proxy  (Railway, not Cloudflare)
                        │
          /api/*  ──────┼──────►  clickindicators-api.msmithbh9.workers.dev
          else    ──────┴──────►  autumn-paper-2321.msmithbh9.workers.dev
```

## Deploy

```bash
cd ops/failover-proxy
railway init --name clickindicators-failover     # needs a plan that allows another project
railway variables --set "CI_PROXY_SECRET=$(cat <secret file>)"
railway up
railway domain clickindicatorsmod.com            # Railway issues the certificate itself
```

Then in Cloudflare DNS, point the apex at Railway **with the proxy off (grey cloud)**. The grey
cloud is the whole point: an orange-clouded record goes back through the broken edge.

`CI_PROXY_SECRET` must match the Worker secret set with:

```bash
cd server && npx wrangler secret put CI_PROXY_SECRET
```

## What the secret is for

The Worker rate-limits logins and checks Turnstile against `cf-connecting-ip`. Cloudflare
rewrites that header at its own edge with whoever opened the connection — through the proxy,
that is the proxy. Every customer would share one address and the login limiter would lock all
of them out together. So the proxy sends the real address as `x-ci-client-ip`, and the Worker
believes it only when `x-ci-proxy-secret` matches.

The same gate covers `x-forwarded-host`, which decides the hostname the Worker builds macro
links, OAuth redirects, Stripe returns and password-reset links from. Both are ungated attack
surface otherwise, because the Worker is publicly reachable on `workers.dev`: an unauthenticated
IP header walks past the rate limits, and an unauthenticated host header is an open redirect
through the OAuth callback. No secret configured means neither is trusted — the behaviour from
before the outage.

## Rollback

One DNS change. In Cloudflare, set the apex `A` record back to `192.0.2.1` and turn the orange
cloud **on**. The Workers Routes are still in place and untouched, so the moment Cloudflare's
edge works again the site serves directly and the proxy is bypassed. Delete the Railway service
whenever convenient.

## Security constraints - read before changing anything here

**Never delete the Railway project while v1.0.24 is in the wild.** That release has
`clickindicators-failover-production.up.railway.app` compiled into it as a fallback, and the mod
posts an email and password to whichever host answers. Platform-assigned hostnames return to the
pool when the project goes away, so deleting it hands every still-unupdated install's credentials
to whoever registers the name next. v1.0.25 removes it from the list - the apex points here
anyway, so the fallback bought nothing - but installs that never update keep the old list forever.
Retire the project only once that release is old enough that nobody is on v1.0.24, and even then
prefer leaving an empty service holding the name.

**This proxy sees everything in clear.** TLS terminates here and a fresh connection is made to the
Worker, so every password, licence token and macro passes through this container decrypted. That
is a third party in the path who was not there before, and it is the price of being reachable at
all while Cloudflare's edge refuses the zone. It is therefore temporary by design: the moment the
edge serves again, point the apex back at Cloudflare and this stops carrying anything. Nothing is
persisted here and only the method and URL are logged on failure - no bodies, no headers, no
credentials - and it must stay that way.

**Zone-level protections do not apply to this path.** Cloudflare WAF, Under Attack mode and any
rate limiting configured on the zone only cover requests arriving at the zone. Traffic through
here, and through the workers.dev name, reaches the same API without passing any of it. Every
control that actually matters therefore has to live in the Worker: the per-IP login limiter,
Turnstile, and device binding all do. Do not move a security control to the zone and assume it
covers production - today it does not, because the apex resolves here.

**X-Forwarded-For is read from the right, not the left.** See `clientIp()`. The leftmost entries
are whatever the client sent.

## Do not remove

`"workers_dev": true` in `server/wrangler.jsonc` and `wrangler.site.jsonc`. Wrangler **disables
workers.dev on every deploy** unless it is set, and workers.dev is the door this proxy comes in
through. It was silently switched off by the first deploy made during this outage, which took
the only working entrance down with it.

`wrangler.site.jsonc` also must keep using a plain route rather than `custom_domain: true` — the
custom domain carries its own dedicated certificate and redeploying with it would recreate the
configuration that was removed.
