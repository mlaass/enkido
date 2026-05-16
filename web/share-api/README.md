# nkido-share-api

Cloudflare Worker backing `share.nkido.cc` — anonymous patch shares for the
nkido live-coding IDE.

This is the **reference implementation**. Self-hosters can deploy their own
copy in under 30 minutes and point a local nkido build at it by changing one
env var. The full design rationale lives in
`docs/prd-shareable-patches.md`.

## Endpoints

| Method  | Path           | Purpose                                                  |
| ------- | -------------- | -------------------------------------------------------- |
| POST    | `/share`       | Publish a patch. Body: `{ code, title?, description?, parent_slug? }`. Returns `{ slug }`. |
| GET     | `/api/p/:slug` | JSON read for the SPA. Cached forever.                   |
| GET     | `/p/:slug`     | OG-tagged HTML + meta-refresh to `live.nkido.cc/p/:slug`. |
| POST    | `/report`      | Flag a patch for operator review. Body: `{ slug, reason? }`. Returns `{ ok: true }`. |
| OPTIONS | any            | CORS preflight.                                          |

## Prerequisites

To run the Worker locally you need:

- **[Bun](https://bun.sh)** ≥ 1.0 (or Node.js 18+ with `npm`/`pnpm` — the
  scripts assume `bun`).
- **wrangler** — installed automatically as a devDependency by
  `bun install`. No global install needed.

To deploy your own share endpoint you additionally need:

- A free **[Cloudflare account](https://dash.cloudflare.com/sign-up)**. The
  Worker, D1 database, and a single rate-limit rule all fit comfortably under
  Cloudflare's free tier at any reasonable share volume.
- A domain on Cloudflare DNS (or use the free `*.workers.dev` subdomain
  Cloudflare gives every account).
- `wrangler` authenticated against your account:
  ```bash
  bunx wrangler login
  ```
  This opens a browser tab; approve the access and the CLI stores a token in
  `~/.config/.wrangler/config/default.toml`.

## Local development

```bash
cd web/share-api
bun install
bun run dev          # wrangler dev on http://localhost:8787
```

Then in `web/.env`:

```
PUBLIC_SHARE_API_BASE=http://localhost:8787
```

…and start the SPA with `bun run dev` from `web/`. Sharing in the IDE will hit
your local Worker. Miniflare auto-provisions a local D1 database; the schema
is applied automatically when tests run (`bun test`).

## Tests

```bash
bun test
```

Tests run via `@cloudflare/vitest-pool-workers` against an in-process Miniflare
D1 — no network, no Cloudflare account required.

## Deploy your own share endpoint

The walkthrough below takes a fresh clone of nkido to a working share
endpoint at your own Worker URL. Estimated time: 20–30 minutes including
DNS propagation.

### 1. Authenticate wrangler

```bash
cd web/share-api
bun install
bunx wrangler login
```

### 2. Create a D1 database

```bash
bunx wrangler d1 create nkido-shares-prod
```

The command prints something like:

```
✅ Successfully created DB 'nkido-shares-prod'
[[d1_databases]]
binding = "DB"
database_name = "nkido-shares-prod"
database_id = "9f3b7c2e-…"
```

Copy the `database_id` — you'll paste it into `wrangler.toml` in step 3.

### 3. Wire D1 into `wrangler.toml`

Open `web/share-api/wrangler.toml`. Two changes:

1. **Uncomment the production `[[d1_databases]]` block** (the one commented
   out near the top of the file) and paste your `database_id` from step 2.
2. **Remove or comment out the `nkido-shares-dev` placeholder block** so
   the deployed Worker doesn't accidentally bind to a non-existent dev
   database.

The relevant section should end up looking like:

```toml
[[d1_databases]]
binding = "DB"
database_name = "nkido-shares-prod"
database_id = "9f3b7c2e-…"   # ← your real ID
```

Leave the `[vars]` block alone for now — you'll edit `ALLOW_ORIGIN` and
`LIVE_ORIGIN` in step 6 once you know your SPA's origin.

### 4. Apply the schema

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --file=schema.sql
```

`--remote` writes to the real Cloudflare D1; omit it and you'd only touch
the local Miniflare DB. Re-running this is safe — every statement uses
`CREATE … IF NOT EXISTS`.

Sanity check:

```bash
bunx wrangler d1 execute nkido-shares-prod --remote \
  --command "SELECT name FROM sqlite_master WHERE type='table';"
```

You should see the `patches` table.

### 5. Deploy the Worker

```bash
bun run deploy   # alias for `wrangler deploy`
```

Wrangler will print the Worker URL — by default
`https://nkido-share-api.<your-subdomain>.workers.dev`. Smoke-test it:

```bash
curl https://nkido-share-api.<your-subdomain>.workers.dev/p/does-not-exist
# → 404 HTML with "Patch not found"
```

If you see that, the Worker is live and D1 is reachable.

### 6. Point a custom domain at the Worker (optional but recommended)

The `workers.dev` URL works but is ugly and not cacheable on a custom
hostname. To use your own domain (`share.example.com`):

1. In the Cloudflare dashboard, go to **Workers & Pages → Your worker →
   Settings → Triggers → Custom Domains**.
2. Click **Add Custom Domain**, enter `share.example.com`, save.
3. Cloudflare auto-provisions the DNS record and SSL cert. Propagation is
   usually under a minute.

Then update `wrangler.toml` so CORS and the meta-refresh target know about
the new hostname:

```toml
[vars]
ALLOW_ORIGIN = "https://your-spa-host.example.com"   # where the SPA runs
LIVE_ORIGIN  = "https://your-spa-host.example.com"   # used by /p/:slug to
                                                     # redirect into the editor
```

Re-deploy:

```bash
bun run deploy
```

Now `https://share.example.com/p/<slug>` serves the OG meta page that
redirects to your SPA's `/p/<slug>` route.

### 7. Rate limiting (recommended before going public)

The Worker has no built-in rate limit. Configure a **Cloudflare Rate
Limiting Rule** in the dashboard:

1. **Security → WAF → Rate limiting rules → Create rule**.
2. Match: `(http.request.method eq "POST" and http.request.uri.path eq "/share")`
3. Rate: **10 requests per 60 seconds per IP**.
4. Action: **Block**, duration 60s.

Repeat for `/report` if you start seeing report spam (see §Triage below).

If you prefer enforcement inside the Worker instead, `wrangler.toml` has a
commented-out `[[unsafe.bindings]]` block for Cloudflare's experimental
ratelimit binding — uncomment and adapt.

### 8. Configure the SPA to use your endpoint

Two ways to point the nkido SPA at your Worker:

**Local dev / personal build** — in `web/.env`:

```
PUBLIC_SHARE_API_BASE=https://share.example.com
```

(Leave empty to disable backend sharing entirely; the Inline-link tab of
the Share dialog still works.)

**Production build** — set the env var before `bun run build`:

```bash
cd web
PUBLIC_SHARE_API_BASE=https://share.example.com bun run build
```

Or in your hosting platform (Netlify, Cloudflare Pages, Vercel, …) add
`PUBLIC_SHARE_API_BASE` to the build environment.

#### SPA env var checklist

| Var | Where | Required? | Notes |
| --- | --- | --- | --- |
| `PUBLIC_SHARE_API_BASE` | `web/.env` (dev) or build env (prod) | Optional | Empty → only inline-link sharing. Set to your Worker's URL to enable Permalink sharing. Must start with `PUBLIC_` so SvelteKit exposes it to the client. |

#### Worker env var checklist (`wrangler.toml [vars]`)

| Var | Required? | Notes |
| --- | --- | --- |
| `ALLOW_ORIGIN` | Yes | The origin the SPA is served from. Used as `Access-Control-Allow-Origin` for POST `/share` and `/report`. |
| `LIVE_ORIGIN` | Yes | Where `/p/:slug` HTML meta-refreshes to. Typically the same as `ALLOW_ORIGIN`. |
| `ALLOW_ORIGIN_DEV_PATTERN` | No | Regex matched against the request origin. Use during local dev to allow `^http://localhost:\d+$` without baking it into prod. |

## Backups

D1 ships **automatic daily backups with 7-day retention** on every
database. No configuration required — Cloudflare snapshots overnight and
you can restore from the dashboard at **Workers & Pages → D1 → your DB
→ Backups**.

For ad-hoc local snapshots (e.g. before a risky schema migration), export
the DB to a local file:

```bash
bunx wrangler d1 export nkido-shares-prod --remote --output backup.sql
```

`backup.sql` is the full SQL dump and can be re-applied to a fresh DB
with `wrangler d1 execute --file=backup.sql`. Treat the file as
**sensitive** — it contains every patch's full code and the salted
`ip_hash` column.

For higher-volume deployments where 7-day retention isn't enough, schedule
a nightly `d1 export` from CI to your object storage of choice.

## Triage / takedown

Reports go to D1 — there is no email or webhook. Run the queue query daily
(or on alert from a user) and decide whether to soft-delete.

**1. Show the report queue:**

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --command \
  "SELECT slug, title, reported_at, report_count
   FROM patches
   WHERE reported_at IS NOT NULL AND deleted_at IS NULL
   ORDER BY reported_at DESC LIMIT 50;"
```

`reported_at` is preserved across re-reports (first-report wins via
`COALESCE`), so the queue sort is stable and reflects when each issue was
first surfaced.

**2. Inspect a single report's context** — full row including the code:

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --command \
  "SELECT slug, title, description, code, created_at, report_count, ip_hash
   FROM patches WHERE slug = '<slug>';"
```

**3. Reasons** — the optional one-line reason a reporter submits is **not**
persisted to D1. It is logged to the Worker only, along with the salted
hash of the reporter's IP (same daily-rotating hash used for `ip_hash` on
patch rows). Stream the live log feed with:

```bash
bunx wrangler tail --format=pretty | grep '\[report\]'
```

Each line looks like `[report] slug=<slug> ip_hash=<hash> reason=<reason>`.
`wrangler tail` is a **live** stream — historical reasons are not stored
durably. With `[observability] enabled = true` in `wrangler.toml`
(the default for this Worker), past log lines are queryable for ~24h via
the Cloudflare dashboard at **Workers & Pages → your worker → Logs**, but
nothing about reports survives beyond the count and timestamp on the row.

**4. Soft-delete a patch:**

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --command \
  "UPDATE patches SET deleted_at = unixepoch()*1000 WHERE slug = '<slug>';"
```

Read handlers (and `/report` itself) filter `WHERE deleted_at IS NULL`, so
the share returns 404 from `/api/p/:slug`, `/p/:slug`, and `/report`
immediately after. The OG HTML response carries a `Cache-Control:
max-age=300`, so an in-flight CDN cached copy of the meta page can linger
for up to 5 minutes; the JSON read clears immediately.

**5. Clear a false-positive report flag** — leave the patch visible, reset
the queue marker:

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --command \
  "UPDATE patches SET reported_at = NULL, report_count = 0
   WHERE slug = '<slug>';"
```

**Spam-of-reports.** `POST /report` has no server-side rate limit in v1
(PRD §10.8). The SPA tracks reported slugs in `localStorage` and shows
"Already reported" on a re-click, but a determined client can spam by
clearing storage. The `reported_at` column is set once via `COALESCE` —
repeated reports only bump `report_count`. To spot patches getting hit
hardest, sort by count instead of recency:

```bash
bunx wrangler d1 execute nkido-shares-prod --remote --command \
  "SELECT slug, title, report_count, reported_at
   FROM patches
   WHERE reported_at IS NOT NULL AND deleted_at IS NULL
   ORDER BY report_count DESC LIMIT 50;"
```

If spam reports become a real problem, add a Cloudflare Rate Limiting
Rule on `/report` similar to the one on `/share`.

## Troubleshooting

**`bun run deploy` fails with `D1_ERROR: no such table: patches`** — you
skipped step 4 or ran `wrangler d1 execute` without `--remote`. Re-run
with `--remote`.

**Share works locally but the deployed Worker returns CORS errors** —
`ALLOW_ORIGIN` in `wrangler.toml` doesn't match the SPA's actual origin.
Check the browser console for the exact origin string and update
`[vars] ALLOW_ORIGIN` to match (scheme + host + optional port, no path,
no trailing slash).

**`/p/<slug>` HTML loads but the "Open in Editor" link 404s** — `LIVE_ORIGIN`
is misconfigured. Point it at the SPA host where your `/p/:slug` route
is served.

**Rate-limit rule has no effect** — Cloudflare Rate Limiting Rules apply
at the zone level. Make sure the rule's hostname filter includes your
Worker's custom domain, not just `*.workers.dev`.
