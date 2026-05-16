# nkido-share-api

Cloudflare Worker backing `share.nkido.cc` — anonymous patch shares for the
nkido live-coding IDE.

This is the reference implementation. Self-hosters can deploy their own copy
with one env var change in the SPA. See PRD: `docs/prd-shareable-patches.md`.

## Endpoints

| Method | Path           | Purpose                                                  |
| ------ | -------------- | -------------------------------------------------------- |
| POST   | `/share`       | Publish a patch. Body: `{ code, title?, description?, parent_slug? }`. Returns `{ slug }`. |
| GET    | `/api/p/:slug` | JSON read for the SPA. Cached forever.                   |
| GET    | `/p/:slug`     | OG-tagged HTML + meta-refresh to `live.nkido.cc/p/:slug`. |
| POST   | `/report`      | Flag a patch for operator review. Body: `{ slug, reason? }`. Returns `{ ok: true }`. |
| OPTIONS | any           | CORS preflight.                                          |

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

## Deploy (operator)

One-time setup:

```bash
# 1. Create a D1 database (records the ID Cloudflare assigns).
wrangler d1 create nkido-shares-prod

# 2. Edit wrangler.toml — uncomment the [[d1_databases]] block above the
#    dev placeholder and paste in the database_id from step 1. Remove or
#    rename the dev placeholder block so prod doesn't accidentally bind to
#    `nkido-shares-dev`.

# 3. Apply the schema to the new database.
wrangler d1 execute nkido-shares-prod --remote --file=schema.sql

# 4. Deploy.
wrangler deploy
```

DNS: route `share.nkido.cc` to the Worker via a Cloudflare DNS CNAME or a
Worker Custom Domain.

### Rate limiting

The Worker relies on a Cloudflare **Rate Limiting Rule** at the account level
(not in `wrangler.toml`): 10 POST `/share` per minute per IP. Configure in the
Cloudflare dashboard once. The Worker also has a commented-out
`[[unsafe.bindings]]` block for the experimental ratelimit binding — if you
prefer in-Worker enforcement, uncomment and adapt.

### Triage / takedown

Reports go to D1 — there is no email or webhook. Run the queue query daily
(or on alert from a user) and decide whether to soft-delete.

**1. Show the report queue:**

```bash
wrangler d1 execute nkido-shares-prod --remote --command \
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
wrangler d1 execute nkido-shares-prod --remote --command \
  "SELECT slug, title, description, code, created_at, report_count, ip_hash
   FROM patches WHERE slug = '<slug>';"
```

**3. Reasons** — the optional one-line reason a reporter submits is **not**
persisted to D1. It is logged to the Worker only, along with the salted
hash of the reporter's IP (same daily-rotating hash used for `ip_hash` on
patch rows). Tail recent reports with:

```bash
wrangler tail --format=pretty | grep '\[report\]'
```

Each line looks like `[report] slug=<slug> ip_hash=<hash> reason=<reason>`.
Cloudflare keeps Worker logs for roughly 24h via `wrangler tail`; nothing
about reports is durably stored beyond the count and timestamp on the row.

**4. Soft-delete a patch:**

```bash
wrangler d1 execute nkido-shares-prod --remote --command \
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
wrangler d1 execute nkido-shares-prod --remote --command \
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
wrangler d1 execute nkido-shares-prod --remote --command \
  "SELECT slug, title, report_count, reported_at
   FROM patches
   WHERE reported_at IS NOT NULL AND deleted_at IS NULL
   ORDER BY report_count DESC LIMIT 50;"
```

If spam reports become a real problem, add a Cloudflare Rate Limiting
Rule on `/report` similar to the one on `/share`.

### Backups

D1 ships daily automatic backups with 7-day retention. Acceptable for v1.
