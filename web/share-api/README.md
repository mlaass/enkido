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
| POST   | `/report`      | Flag a patch for operator review. (Phase 3 — currently a 501 stub.) |
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

### Triage / takedown (Phase 3 territory)

Until Phase 3 of the PRD lands, the report endpoint is a 501 stub. Operator
takedown is a manual SQL update:

```bash
wrangler d1 execute nkido-shares-prod --remote --command \
  "UPDATE patches SET deleted_at = unixepoch()*1000 WHERE slug = '<slug>';"
```

Read handlers already filter `WHERE deleted_at IS NULL`, so the share returns
404 from both `/api/p/:slug` and `/p/:slug` immediately after.

### Backups

D1 ships daily automatic backups with 7-day retention. Acceptable for v1.
