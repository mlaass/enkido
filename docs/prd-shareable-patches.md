> **Status: NOT STARTED** — OSS-side groundwork for shareable patches. Lands in the public `nkido` repo before the closed-source SaaS PRD can layer accounts, billing, and assets on top.

# PRD: Persistent Shareable Patches

**Author:** Moritz Laass + Claude
**Date:** 2026-05-02
**Repo:** `nkido` (public, MIT)

**Related:**
- [prd-nkido-web-ide.md](prd-nkido-web-ide.md) — the IDE this PRD extends with tabs, drafts, and a share button
- [prd-uri-resolver.md](prd-uri-resolver.md) — the resolver this PRD does **not** touch (asset uploads land in the SaaS PRD, not here)
- [prd-project-website.md](prd-project-website.md) — the marketing site at `nkido.cc`; sibling deploy
- `nkido-pro/docs/prd-open-core-saas.md` — the parent SaaS PRD; this OSS PRD **supersedes** it for everything that lives in `nkido/`

---

## 1. Executive Summary

Today the nkido web IDE auto-saves a single buffer of code into `localStorage`. There is no way to share a patch with anyone except by copy-pasting the source. This PRD adds two things that together unlock the entire share-and-iterate loop:

1. **Multi-draft local persistence.** The single `nkido-code` localStorage entry becomes a list of named drafts with a tab bar in the editor and a Patches tab in the sidebar.
2. **Anonymous public shares.** A "Share" button POSTs the active draft to a small Cloudflare Worker, which returns a permalink at `share.nkido.cc/p/<slug>` (auto-redirects to `live.nkido.cc/p/<slug>` for human visitors, with OG metadata for link unfurls).

The backend is a hand-rolled ~150-LOC Worker backed by a D1 SQLite database. **No Supabase, no accounts, no auth.** Anonymous shares are immutable: edits to `/p/<slug>` save locally as a phantom draft, and "Fork" creates a new share with `parent_slug` recorded for fork history. The wire protocol is documented so self-hosters can run their own Worker.

This PRD is intentionally narrow. It is the smallest deliverable that lets the future SaaS PRD (`nkido-pro/docs/prd-open-core-saas.md`) layer Supabase auth, private patches, asset uploads, and a gallery on top — by swapping in a different `StorageProvider` implementation. **Nothing closed-source touches this repo.**

### 1.1 Why now

The IDE has settled (`prd-nkido-web-ide.md` Phases 1–6 substantially complete) and the URI resolver consolidation is shipping. Shareable patches are the most-requested community feature and a precondition for any SaaS-tier work. Doing the local persistence + share UI in OSS first means the SaaS PRD becomes additive, not invasive.

### 1.2 Headline design decisions (locked during PRD intake)

- **OSS-only slice.** This PRD ships anonymous shares + multi-draft local persistence. Accounts, gallery, profiles, hearts, comments, private patches, asset uploads — all deferred to the parent SaaS PRD.
- **Pluggable `StorageProvider` interface.** OSS ships `LocalDraftProvider` (drafts) + `WorkerShareProvider` (anonymous shares via the reference Worker). The parent SaaS PRD will inject a `SupabaseStorageProvider` that fully implements the same interface.
- **Cheap-and-portable backend, no Supabase.** Cloudflare Worker + D1 SQLite. Free tier covers expected traffic; OSS self-hosters can deploy their own with `wrangler publish` and one env var change. The SaaS PRD adds Postgres later when accounts arrive.
- **Hand-rolled Worker, not a second SvelteKit app.** `web/share-api/` is ~150 LOC of TypeScript with a `wrangler.toml` and a `schema.sql`. Easier for self-hosters to read and fork than a framework-laden second app.
- **Anonymous shares are immutable.** No edit token. To "edit" a share, you Fork — you get a new `/p/<slug>`. Cuts the entire "lost my edit token" / hijack class of issues.
- **Edits to `/p/<slug>` save locally as a phantom draft.** Visitors can play with someone else's patch; their changes auto-save under a slug-keyed phantom. "Keep" promotes the phantom to a named draft. "Fork" publishes it as a new share.
- **Two domains:** `share.nkido.cc` (Worker) is the canonical share URL with OG-tagged HTML; auto-redirects (meta-refresh) human visitors to `live.nkido.cc/p/<slug>` (SPA) which fetches the patch and boots the IDE.
- **Tighter caps than the parent PRD.** 16 KB code + 2 KB params (parent SaaS PRD says 64 KB / 16 KB; we start tight, relax later if real users hit it).
- **8-char base62 slugs.** ~218 trillion namespace; collision retry up to 3x. Shorter than the parent PRD's 10-char; more typeable.
- **Cloudflare Rate Limiting Rules.** 10 POST `/share` per minute per IP, configured in `wrangler.toml`. Zero application code.
- **Source-declared params, no params payload in the share record.** Patch params are whatever the source `param()` / `toggle()` / `dropdown()` widgets declare as defaults. Sharing doesn't snapshot runtime values. Simpler schema; future PRD can add a snapshot field.
- **POST `/report` endpoint** flags rows for operator review. No public moderation UI in v1; soft-delete via D1 console.

---

## 2. Problem Statement

### 2.1 Current state

| Concern | Today |
|---|---|
| Patch persistence | Single `localStorage` key (`nkido-code`). One buffer. No naming. Auto-saved on type, on `beforeunload`. |
| Sharing | Copy-paste source into a chat. No URLs, no link unfurls. |
| Multiple WIPs | Not possible. Switching to a different idea overwrites the old one unless you copy it elsewhere first. |
| Visiting someone else's code | Doesn't exist. There is no `/p/<slug>` route. |
| OSS/SaaS boundary | None — there is no SaaS yet, but also no abstraction in place. Adding sharing without an interface means refactoring later. |

### 2.2 Proposed state

| Concern | After this PRD |
|---|---|
| Patch persistence | List of named drafts in `localStorage` (`nkido-drafts` key); `nkido-code` migrated to draft `Untitled` on first launch. Active draft is what the editor shows. |
| Sharing | "Share" button publishes the active draft to `share.nkido.cc/p/<slug>` via a Cloudflare Worker. Slug is shareable on social with OG metadata. |
| Multiple WIPs | Editor has a tab bar; each tab is an open draft or a visited `/p/<slug>` phantom. Sidebar Patches tab lists all drafts + recently visited slugs. |
| Visiting someone else's code | `live.nkido.cc/p/<slug>` boots the IDE in a special tab; auto-runs after click-to-unmute. Visitor edits save locally as a phantom; "Keep" promotes to a named draft, "Fork" creates a new share. |
| OSS/SaaS boundary | `StorageProvider` interface in `web/src/lib/ide/storage/`. OSS ships `LocalDraftProvider` + `WorkerShareProvider`. SaaS PRD injects `SupabaseStorageProvider` later, no OSS-side changes needed. |

---

## 3. Goals and Non-Goals

### 3.1 Goals

1. **One-click anonymous share** with a permalink that loads on any device, anywhere, without signup.
2. **Multiple drafts** survive across sessions; user can name them and switch via tabs.
3. **Forkable shares** preserve a `parent_slug` chain so future PRDs can render fork trees.
4. **Self-hostable backend** — the Worker is documented and shipped as reference code; a self-hoster can deploy their own with one env var change.
5. **Clean abstraction boundary.** The `StorageProvider` interface is defined here so the SaaS PRD layers on without re-architecting.
6. **No regression** in the existing single-buffer auto-save UX. First launch after the upgrade silently migrates old localStorage into a draft named "Untitled".
7. **Link previews work.** Twitter/Discord/Slack unfurl `share.nkido.cc/p/<slug>` with title, description, and a code snippet preview.

### 3.2 Non-Goals (hard cuts)

1. **Accounts, signin, profiles, gallery, hearts, comments.** All deferred to the SaaS PRD.
2. **Private patches and asset uploads.** SaaS PRD only.
3. **Editing a published share.** Anonymous shares are immutable. To change one, Fork.
4. **Edit tokens / "claim my anon share later".** No mechanism in v1. The SaaS PRD's "Import drafts on signup" flow handles claiming local drafts; anonymous shares from a different browser are permanently unattributed.
5. **Patch-level versioning beyond fork.** Each share is a snapshot; reshare = new slug.
6. **Public moderation UI.** `/report` endpoint flags rows; takedown is a SQL operation by the operator.
7. **Real-time collab on a draft.** Out of scope.
8. **Server-side rendering of the IDE.** Viewer at `live.nkido.cc/p/<slug>` is SPA; only the OG landing on `share.nkido.cc/p/<slug>` is server-rendered.
9. **Param snapshot at share time.** Future PRD if real users miss it.

### 3.3 Success metrics

| Metric | v1 target | Notes |
|---|---|---|
| Anonymous shares per week | > 30 by week 4 | Lower bar than the SaaS PRD because we have no signup funnel yet. |
| Median patch-load latency from `live.nkido.cc/p/<slug>` (TTFB to audio playable) | < 1s | URL hit → bytecode running. Fetch + WASM init + click-to-unmute. |
| Drafts per active user (p50) | > 2 | Indicator that multi-draft persistence is being used. |
| Worker error rate | < 0.5% of requests | Includes 429s. |
| D1 storage growth | < 100 MB / month | At 16 KB code cap, that's ~6,500 shares/month. Well within free tier. |

---

## 4. Target User Experience

### 4.1 Sharing your active draft

1. User edits in the IDE. Auto-save persists into the active draft (e.g., `draft-3`).
2. They click **Share** in the toolbar.
3. Modal opens with `Title` (defaults to draft name) and `Description` (optional, one line) fields. "Share publicly" button.
4. Click → POST to `share.nkido.cc/share`. Modal shows the returned URL `https://share.nkido.cc/p/k7gp2x` and a Copy button.
5. Toast: "Link copied. Anyone with the URL can view this patch."

### 4.2 Visiting a share

1. Friend pastes `https://share.nkido.cc/p/k7gp2x` in Discord. Discord unfurls: title, description, code preview, "nkido" branding via OG tags.
2. User clicks. `share.nkido.cc/p/k7gp2x` returns HTML with a `<meta http-equiv="refresh" content="0; url=https://live.nkido.cc/p/k7gp2x">` and a manual "Open in editor" link as a JS-disabled fallback.
3. Browser navigates to `live.nkido.cc/p/k7gp2x`. SPA boots, `WorkerShareProvider.fetchShare("k7gp2x")` calls `GET share.nkido.cc/api/p/k7gp2x` (JSON), receives `{ slug, title, description, code, parent_slug, created_at }`.
4. Editor opens a new tab labeled with the title. Code loads. Big "Click anywhere to start audio" overlay (browser autoplay policy).
5. User clicks. WASM compiles, hot-swaps, audio plays.

### 4.3 Editing what you've visited

1. User starts typing in the `/p/k7gp2x` tab.
2. First keystroke: a phantom draft is auto-created with key `draft:p/k7gp2x`. The tab title shows a `●` (unsaved) indicator.
3. From now on, every keystroke debounce-saves to the phantom. URL stays `/p/k7gp2x`.
4. Two new buttons appear in the toolbar: **Keep** and **Fork**.
5. **Keep** (local): promotes the phantom to a regular named draft. Modal asks for a name (defaults to `Fork of <title>`). Phantom is deleted; new draft is opened as the active tab. URL navigates to `/editor`.
6. **Fork** (publishes): POSTs the current code to `/share` with `parent_slug=k7gp2x`. Worker returns `{ slug: "9m2n7q" }`. Tab title updates; URL changes in place to `/p/9m2n7q`. Phantom under `draft:p/k7gp2x` is deleted. Local phantom for the new slug is unnecessary because the user just published exactly what was in the editor.
7. If the user closes the tab without Keep or Fork, the phantom remains in localStorage and is reachable from the Patches sidebar under "Recently visited" → "k7gp2x (edited)".

### 4.4 Drafts and tabs

- **Tab bar** sits above the editor. Each tab is either a named local draft (`draft-1`, `draft-2`) or a `/p/<slug>` phantom (titled with the share title + `●` if unsaved-in-phantom).
- Tab actions: click to switch active, middle-click or × to close (closing **does not delete** the underlying draft — it's still in the sidebar).
- "+" button at the end of the tab bar → "New draft" picker.
- **Patches sidebar tab** has two sections:
  - **My drafts** (named, renamable inline, deletable via context menu). Click → opens or focuses the tab.
  - **Recently visited** (last N=20 `/p/<slug>` URLs the user has opened in this browser, with title + slug). Click → opens or focuses the tab.

### 4.5 Self-hosting your own share endpoint

A user clones nkido and wants to run their own. They:

```bash
cd nkido/web/share-api/
bun install
wrangler d1 create my-nkido-shares      # one-time
# update wrangler.toml database_id, then:
wrangler d1 execute my-nkido-shares --file=schema.sql
wrangler deploy
```

In `web/.env`:
```
PUBLIC_SHARE_API_BASE=https://my-shares.example.workers.dev
```

That's it. The SPA now POSTs shares to their backend, reads `/p/<slug>` from theirs. Their `share.example.com/p/<slug>` URLs unfurl with their own OG tags. **Documented in the PRD's Implementation Phases (§8) and a `web/share-api/README.md`.**

---

## 5. Architecture

### 5.1 Component layout

```
┌──────────────────────────────────────────────────────────────────┐
│  nkido (this repo)                                                │
│                                                                   │
│  web/                                                             │
│  ├── src/lib/ide/storage/         ← StorageProvider interface     │
│  │   ├── types.ts                                                 │
│  │   ├── local-draft.ts           LocalDraftProvider              │
│  │   ├── worker-share.ts          WorkerShareProvider             │
│  │   └── index.ts                                                 │
│  ├── src/lib/components/                                          │
│  │   ├── EditorTabs.svelte        new                             │
│  │   ├── ShareDialog.svelte       new                             │
│  │   └── Panel/PatchesPanel.svelte  new                           │
│  ├── src/lib/stores/                                              │
│  │   ├── drafts.svelte.ts         new — replaces single buffer    │
│  │   └── editor.svelte.ts         modified — delegates to drafts  │
│  ├── src/routes/                                                  │
│  │   └── p/[slug]/+page.svelte    new — viewer route              │
│  └── share-api/                   new — Cloudflare Worker         │
│      ├── src/                                                     │
│      │   ├── index.ts             ~150 LOC, hand-rolled router    │
│      │   ├── handlers.ts                                          │
│      │   └── og.ts                OG HTML rendering               │
│      ├── schema.sql                                               │
│      ├── wrangler.toml                                            │
│      ├── package.json                                             │
│      └── README.md                self-hosting docs               │
└──────────────────────────────────────────────────────────────────┘
                               │
                               │ HTTPS
                               ▼
┌──────────────────────────────────────────────────────────────────┐
│  Cloudflare                                                       │
│   Worker @ share.nkido.cc                                         │
│   ├─ POST /share                  → INSERT into D1                │
│   ├─ GET  /api/p/:slug            → JSON for SPA                  │
│   ├─ GET  /p/:slug                → OG-tagged HTML +              │
│   │                                  meta-refresh to live.*       │
│   └─ POST /report                 → flag for review               │
│   D1 SQLite                                                       │
│   └─ patches (slug, code, …)                                      │
│   Rate Limiting Rules: 10 POST /share / min / IP                  │
└──────────────────────────────────────────────────────────────────┘
```

### 5.2 The `StorageProvider` interface

Lives in `web/src/lib/ide/storage/types.ts`. Both the OSS shipping providers implement only the relevant subset; the SaaS PRD adds a third provider (`SupabaseStorageProvider`) that implements the full surface including the deferred social methods.

```ts
// web/src/lib/ide/storage/types.ts

export interface DraftSummary {
  id: string;            // UUID
  name: string;          // user-editable
  updatedAt: number;     // epoch ms
  isPhantom: boolean;    // true if keyed by a /p/<slug> phantom
  phantomSlug: string | null;  // when isPhantom: the slug
}

export interface DraftFull extends DraftSummary {
  code: string;
}

export interface ShareInput {
  code: string;
  title?: string;
  description?: string;
  parentSlug?: string;   // set when forking
}

export interface ShareSummary {
  slug: string;
  title: string | null;
  description: string | null;
  parentSlug: string | null;
  createdAt: number;     // epoch ms
}

export interface ShareFull extends ShareSummary {
  code: string;
}

export interface StorageProvider {
  // Drafts — always available, all impls implement.
  saveDraft(draft: DraftFull): Promise<void>;
  loadDraft(id: string): Promise<DraftFull | null>;
  listDrafts(): Promise<DraftSummary[]>;
  deleteDraft(id: string): Promise<void>;
  renameDraft(id: string, newName: string): Promise<void>;

  // Shares — WorkerShareProvider implements; LocalDraftProvider stubs/throws.
  share?(input: ShareInput): Promise<{ slug: string }>;
  fetchShare?(slug: string): Promise<ShareFull | null>;
  reportShare?(slug: string, reason?: string): Promise<void>;

  // Visited slugs registry — local-only convenience.
  recordVisited?(slug: string, title: string | null): Promise<void>;
  listRecentlyVisited?(): Promise<Array<{ slug: string; title: string | null; visitedAt: number }>>;
}
```

Two impls in this PRD:

| Impl | Lives in | Methods | Notes |
|---|---|---|---|
| `LocalDraftProvider` | `web/src/lib/ide/storage/local-draft.ts` | drafts + `recordVisited` + `listRecentlyVisited` | Pure localStorage. No network. Throws on share/fetchShare. |
| `WorkerShareProvider` | `web/src/lib/ide/storage/worker-share.ts` | `share`, `fetchShare`, `reportShare` | Wraps `LocalDraftProvider` for drafts and proxies share methods to `PUBLIC_SHARE_API_BASE`. |

The IDE composes them: `new WorkerShareProvider({ local: new LocalDraftProvider(), apiBase: env.PUBLIC_SHARE_API_BASE })`.

### 5.3 Cloudflare Worker (`web/share-api/`)

Hand-rolled router, ~150 LOC. No framework. Bindings in `wrangler.toml` are: `DB` (D1) + `RATE_LIMITER` (Cloudflare Rate Limiting binding).

```typescript
// web/share-api/src/index.ts (sketch)

interface Env {
  DB: D1Database;
  RATE_LIMITER: RateLimit;
  ALLOW_ORIGIN: string;       // "https://live.nkido.cc"
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const cors = corsHeaders(env.ALLOW_ORIGIN, req);
    if (req.method === 'OPTIONS') return new Response(null, { headers: cors });

    if (req.method === 'POST' && url.pathname === '/share') {
      return rateLimit(env, req, () => handleShare(req, env, cors));
    }
    if (req.method === 'GET' && /^\/api\/p\/[a-z0-9]{8}$/i.test(url.pathname)) {
      return handleApiGet(url.pathname.slice(7), env, cors);
    }
    if (req.method === 'GET' && /^\/p\/[a-z0-9]{8}$/i.test(url.pathname)) {
      return handleHtmlGet(url.pathname.slice(3), env);
    }
    if (req.method === 'POST' && url.pathname === '/report') {
      return handleReport(req, env, cors);
    }
    return new Response('not found', { status: 404, headers: cors });
  }
};
```

#### Handler details

- **POST `/share`** — body `{ code, title?, description?, parent_slug? }`. Validates: `code` is a string with `1 <= byteLength(code) <= 16384`; `title <= 200` chars; `description <= 500` chars; `parent_slug` is 8 chars base62 if present. Generates an 8-char base62 slug, INSERTs, retries up to 3x on UNIQUE collision. Returns `{ slug }`.
- **GET `/api/p/:slug`** — returns JSON `{ slug, title, description, code, parent_slug, created_at }`. `Cache-Control: public, max-age=31536000, immutable`. CORS headers for `live.nkido.cc` (and `localhost:5173` in dev — driven by `ALLOW_ORIGIN`).
- **GET `/p/:slug`** — returns server-rendered HTML with OG tags (`og:title`, `og:description`, `og:url`, `og:type=website`, optional `og:image` placeholder) and a `<meta http-equiv="refresh" content="0; url=https://live.nkido.cc/p/<slug>">`. Body shows the title, description, a `<pre>`-formatted code snippet, and a manual "Open in editor →" link (visible if JS is disabled or the meta-refresh is blocked).
- **POST `/report`** — body `{ slug, reason? }`. Sets `reported_at = now()`, increments `report_count`. Returns `{ ok: true }`. No auth.

### 5.4 D1 schema (`web/share-api/schema.sql`)

```sql
CREATE TABLE patches (
  slug          TEXT PRIMARY KEY,                                -- 8-char base62
  code          TEXT NOT NULL CHECK (length(code) BETWEEN 1 AND 16384),
  title         TEXT CHECK (length(title) <= 200),
  description   TEXT CHECK (length(description) <= 500),
  parent_slug   TEXT REFERENCES patches(slug) ON DELETE SET NULL,
  created_at    INTEGER NOT NULL DEFAULT (unixepoch() * 1000),  -- epoch ms
  ip_hash       TEXT,                                            -- SHA-256(ip + daily salt) — operator analytics only
  reported_at   INTEGER,
  report_count  INTEGER NOT NULL DEFAULT 0,
  deleted_at    INTEGER                                          -- soft-delete via D1 console
);

CREATE INDEX idx_patches_created_at ON patches(created_at DESC) WHERE deleted_at IS NULL;
CREATE INDEX idx_patches_parent ON patches(parent_slug);
CREATE INDEX idx_patches_reported ON patches(reported_at) WHERE reported_at IS NOT NULL;
```

The `ip_hash` exists for operator-side abuse triage only — there is no API to query by it, and the daily-salted hash is not a long-lived identifier. SaaS PRD will keep this column when migrating to Postgres.

### 5.5 Slug generation

```typescript
const ALPHABET = '23456789abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ';  // 54 chars; ambiguity-stripped

function newSlug(): string {
  const buf = crypto.getRandomValues(new Uint8Array(8));
  return Array.from(buf, b => ALPHABET[b % ALPHABET.length]).join('');
}
```

54^8 ≈ 7.2 × 10^13. At 1M shares total, collision odds < 10⁻⁵; retry-on-INSERT-collision is safety net, not a hot path.

(The parent SaaS PRD specs 10-char Crockford base62; our shorter 8-char is friendlier to typing and still well over collision-safe at OSS-scale traffic. SaaS PRD can opt to migrate to 10-char if needed; existing 8-char slugs continue to resolve since the column accepts variable length up to PRIMARY KEY max.)

### 5.6 Domains and CORS

| URL | Served by | Purpose |
|---|---|---|
| `https://live.nkido.cc/` | Netlify (adapter-static) | The IDE SPA |
| `https://live.nkido.cc/p/<slug>` | Netlify, SPA fallback to `index.html` | SPA viewer route; on mount, fetches from share.* |
| `https://share.nkido.cc/p/<slug>` | Cloudflare Worker | Canonical share URL; OG-tagged HTML + meta-refresh to live.* |
| `https://share.nkido.cc/api/p/<slug>` | Cloudflare Worker | JSON read for SPA |
| `https://share.nkido.cc/share` | Cloudflare Worker | POST — anonymous publish |
| `https://share.nkido.cc/report` | Cloudflare Worker | POST — flag for review |

CORS: Worker sets `Access-Control-Allow-Origin: https://live.nkido.cc` (and `http://localhost:5173`/`5174` in dev via env). `Access-Control-Allow-Methods: GET, POST, OPTIONS`. `Access-Control-Allow-Headers: content-type`.

---

## 6. UX Specifications

### 6.1 Share dialog

```
┌────────────────────────────────────────┐
│  Share patch                       [×] │
│                                        │
│  Title                                 │
│  ┌──────────────────────────────────┐  │
│  │ moody bass                        │  │
│  └──────────────────────────────────┘  │
│                                        │
│  Description (optional)                │
│  ┌──────────────────────────────────┐  │
│  │ tweak @.cutoff to taste           │  │
│  └──────────────────────────────────┘  │
│                                        │
│  ⚠ Public and immutable. To change it, │
│  fork instead.                         │
│                                        │
│  [ Cancel ]      [ Share publicly ]   │
└────────────────────────────────────────┘
```

After publish:
```
┌────────────────────────────────────────┐
│  Shared!                           [×] │
│                                        │
│  https://share.nkido.cc/p/k7gp2x       │
│  [ Copy link ]   [ Open in new tab ]   │
└────────────────────────────────────────┘
```

### 6.2 Editor tab bar

```
[ draft-1 ][ moody bass ][ /p/k7gp2x ●][ + ]   ← active tab is `moody bass`
```

- Active tab gets a contrasting background (`var(--accent)`).
- A `●` after the title means unsaved-in-phantom (only meaningful for `/p/<slug>` tabs).
- `×` on hover for close. Closing does not delete the draft.

### 6.3 Patches sidebar tab

```
PATCHES
─────────────
▼ My drafts             [+ New]
   draft-1
   moody bass           (active)
   wavetable test

▼ Recently visited
   📌 /p/k7gp2x  hard kick
   /p/9m2n7q  drone
   /p/abc123  sweepy

(empty state for both: "No patches yet — start coding!")
```

- Click to open in tab bar (or focus existing).
- Right-click "My drafts" entry → context menu: Rename / Duplicate / Delete.
- Right-click "Recently visited" entry → Forget (removes from list; phantom draft also deleted).

### 6.4 `/p/<slug>` viewer (SPA route)

- Same IDE chrome as `/editor`, but with two subtle additions:
  - Toolbar shows the patch title + "by anonymous" + a "Fork" button always visible.
  - First-time-visit overlay: "Click anywhere to start audio" (browser autoplay policy).
- Editing the code is enabled. First keystroke creates the phantom draft. **Keep** button replaces the share-modal Share button while in this state. **Fork** button is always visible (for "publish my edits as a new share").
- URL stays `/p/<slug>` while editing. After Fork → URL changes in place to `/p/<new_slug>`, tab updates.

### 6.5 `share.nkido.cc/p/<slug>` (server-rendered, OG)

```html
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>moody bass — nkido</title>
  <meta name="description" content="tweak @.cutoff to taste">
  <meta property="og:title" content="moody bass">
  <meta property="og:description" content="tweak @.cutoff to taste">
  <meta property="og:url" content="https://share.nkido.cc/p/k7gp2x">
  <meta property="og:type" content="website">
  <meta property="og:site_name" content="nkido">
  <meta http-equiv="refresh" content="0; url=https://live.nkido.cc/p/k7gp2x">
  <link rel="canonical" href="https://live.nkido.cc/p/k7gp2x">
  <style>/* minimal, system fonts */</style>
</head>
<body>
  <main>
    <h1>moody bass</h1>
    <p>tweak @.cutoff to taste</p>
    <pre><code>osc("saw", 80) |> lp(@, 400, 6) |> out(@, @)</code></pre>
    <a href="https://live.nkido.cc/p/k7gp2x">Open in editor →</a>
  </main>
</body>
</html>
```

---

## 7. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/` (synth engine) | **No change** | This PRD is web-only. |
| `akkado/` (compiler) | **No change** | No new opcodes or grammar. |
| `cedar::UriResolver` | **No change** | Asset uploads are SaaS PRD scope. |
| `web/src/lib/stores/editor.svelte.ts` | **Modified** | Single-buffer logic replaced with delegation to a new `drafts` store. Backwards-compat shim for first-launch migration. |
| `web/static/worklet/cedar-processor.js` | **No change** | Audio path is unaffected. |
| `web/svelte.config.js` (`adapter-static`) | **No change** | SPA stays static; Worker is a separate deploy. |
| `web/netlify.toml` | **Minor change** | Add `share.nkido.cc` to permitted CSP `connect-src` once we tighten CSP (out of scope but flagged). |
| `web/.env.example` | **Modified** | Add `PUBLIC_SHARE_API_BASE`. |
| `web/share-api/` | **New** | Reference Worker. |
| `web/src/lib/ide/storage/` | **New** | Interface + two impls. |
| `web/src/lib/components/Panel/` | **Modified** | Add `PatchesPanel.svelte` to the tab list. |
| `web/src/routes/p/[slug]/+page.svelte` | **New** | SPA viewer route. |
| `web/src/routes/+page.svelte` | **Modified** | Mount editor with new tab bar. |

---

## 8. Implementation Phases

Each phase ends with a deployable, demo-able artifact. Phases run end-to-end in `nkido/`; nothing in this PRD touches `nkido-pro/`.

### Phase 1 — Multi-draft local persistence

**Goal:** The single-buffer editor becomes a multi-draft editor with a tab bar. No network. No share button yet.

**Steps:**

1. Create `web/src/lib/ide/storage/types.ts` (interface from §5.2).
2. Create `web/src/lib/ide/storage/local-draft.ts` (`LocalDraftProvider`).
3. Create `web/src/lib/stores/drafts.svelte.ts` (Svelte 5 rune store wrapping the provider; exposes `drafts`, `activeDraftId`, `setActive`, `createDraft`, `renameDraft`, `deleteDraft`, `updateActiveCode`).
4. Modify `web/src/lib/stores/editor.svelte.ts` to delegate `code`, `setCode`, persistence to `drafts`. Keep the old export shape for components that import it.
5. **First-launch migration:** in the drafts store init, if `nkido-drafts` is absent and `nkido-code` is present, create a draft `{ name: 'Untitled', code: <old code> }`, set it active. Leave `nkido-code` untouched for one release as a recovery safety net (tracked under a future cleanup issue).
6. Create `EditorTabs.svelte` and place it above the editor in the main layout. Implements: tab list, active highlight, close button, "+ New draft" picker.
7. Add `PatchesPanel.svelte` to the existing right-side Panel system. Sections: My drafts (with rename/delete via context menu), Recently visited (empty in this phase).
8. Wire keyboard shortcut `Ctrl/Cmd+T` → new draft, `Ctrl/Cmd+W` → close active tab.

**Verification:**
- Open IDE on a fresh browser → see one draft "Untitled" with the default sample code, single tab.
- Open IDE with existing `nkido-code` → see one draft "Untitled" with the user's code preserved.
- Create three drafts, edit each, switch via tabs → each retains its code on switch and across reload.
- Close a tab → tab disappears; sidebar still lists the draft; click in sidebar → tab reopens with code intact.
- Rename a draft inline → name persists across reload.

### Phase 2 — Share Worker (backend) + viewer route (frontend)

**Goal:** Active draft can be published; resulting URL loads in any browser.

**Steps:**

1. Create `web/share-api/` with `package.json` (deps: `@cloudflare/workers-types`), `wrangler.toml`, `schema.sql`, `src/index.ts`, `src/handlers.ts`, `src/og.ts`, `README.md`.
2. Implement Worker handlers per §5.3. Includes the OG HTML rendering for `GET /p/:slug` with meta-refresh.
3. Configure Cloudflare Rate Limiting Rule on the route `share.nkido.cc/share` for 10 req/min/IP.
4. Provision D1 database `nkido-shares-prod`. Apply `schema.sql`. Bind in `wrangler.toml`.
5. Configure DNS: `share.nkido.cc` → Worker, `live.nkido.cc` → Netlify (already in place per `prd-project-website.md`).
6. Create `web/src/lib/ide/storage/worker-share.ts` (`WorkerShareProvider`) — composes a `LocalDraftProvider` and proxies `share`/`fetchShare`/`reportShare` to `PUBLIC_SHARE_API_BASE`.
7. Add `PUBLIC_SHARE_API_BASE` to `.env.example`. Default to empty string locally → Share button is disabled with a tooltip "Sharing is hosted at live.nkido.cc; build with PUBLIC_SHARE_API_BASE set to your Worker URL."
8. Create `ShareDialog.svelte` (the modal shown in §6.1). Wire to a Share toolbar button.
9. Create `web/src/routes/p/[slug]/+page.svelte`:
   - On mount: `await provider.fetchShare(slug)`. If null, render "Patch not found" with "Back to editor".
   - If found: open as a new tab in the drafts store with id `phantom:p/<slug>`, set active. Mark phantom: true. Title from share.
   - Toolbar shows Title (read-only), "by anonymous", and "Fork" button.
   - Click-to-unmute overlay (browser autoplay policy).
10. On first keystroke in a phantom tab, save edits under `draft:p/<slug>` localStorage key. Add a `●` to the tab title.
11. Wire **Keep** button (visible only on phantoms): converts phantom to a regular draft, removes phantom storage key, navigates to `/editor`.
12. Wire **Fork** button (always visible on phantoms; also shown on the share modal as an alternate action): POSTs current code with `parent_slug=<original_slug>`, navigates URL in place to `/p/<new_slug>`, deletes the original phantom from local storage.
13. Wire `recordVisited` in `LocalDraftProvider`: each successful `fetchShare` records `{ slug, title, visitedAt: now }` in a localStorage list capped at N=20.
14. Populate the "Recently visited" section of the Patches sidebar from `listRecentlyVisited`.

**Verification:**
- From the editor, click Share, fill title + description, publish. See the URL.
- Open the URL in incognito → SPA loads, code visible, click → audio plays.
- Edit the code in incognito → tab gets `●`. Close incognito browser, reopen URL → edit is gone (different storage); reopen on the original browser → edit persists.
- Click Keep → modal asks for name → confirm → tab and address bar move to `/editor`, draft visible in sidebar.
- Edit again, click Fork → new URL `/p/<new>`. Confirm in DB that `parent_slug = <original>`.
- Curl `share.nkido.cc/p/<slug>` → see OG-tagged HTML with meta-refresh.
- Paste URL in Discord → unfurl shows title/description.
- Burst-test rate limit: send 11 POSTs in a minute from one IP → 11th returns 429.

### Phase 3 — Reporting + operator runbook

**Goal:** A reported patch can be hidden by the operator within minutes.

**Steps:**

1. Implement `POST /report` handler. Body: `{ slug, reason? }`. Sets `reported_at`, increments `report_count`. No auth.
2. Add a Report link to `/p/<slug>` toolbar overflow menu. Confirmation modal; submit → toast "Reported."
3. Document operator playbook in `web/share-api/README.md` (§ "Triage"):
   - `wrangler d1 execute nkido-shares-prod --command "SELECT slug, title, reported_at, report_count FROM patches WHERE reported_at IS NOT NULL ORDER BY reported_at DESC LIMIT 50;"` for the queue.
   - `wrangler d1 execute nkido-shares-prod --command "UPDATE patches SET deleted_at = unixepoch() * 1000 WHERE slug = '<slug>';"` to soft-delete.
   - Worker handlers add `WHERE deleted_at IS NULL` filter.

**Verification:**
- Open `/p/<slug>`, click Report, submit → row in D1 has `reported_at` set.
- Run the SQL queue command → see the row.
- Soft-delete via SQL → `/p/<slug>` returns 404 from both the JSON endpoint and the OG HTML endpoint within seconds (no Worker cache; D1 reads are immediate).

### Phase 4 — Self-hosting documentation polish

**Goal:** A motivated user can run their own share endpoint in under 30 minutes, with zero help.

**Steps:**

1. Flesh out `web/share-api/README.md`: prerequisites, step-by-step `wrangler` walkthrough, env var checklist on the SPA side, how to point a custom domain at the Worker, how to back up D1.
2. Add a section to top-level `web/README.md` linking to it: "Want your own share backend? See web/share-api/README.md."
3. Verify a clean clone + the README's instructions produces a working share endpoint with no other guidance.

**Verification:**
- Manual: a developer who has not seen the codebase before, given only the README, can deploy a working share endpoint and configure a local SPA build to use it.

---

## 9. File-Level Changes

### 9.1 New files

| File | Phase | Purpose |
|---|---|---|
| `web/src/lib/ide/storage/types.ts` | 1 | `StorageProvider`, `DraftSummary`, `DraftFull`, `ShareInput`, `ShareSummary`, `ShareFull` interfaces. |
| `web/src/lib/ide/storage/local-draft.ts` | 1 | `LocalDraftProvider` — drafts + recently-visited in localStorage. |
| `web/src/lib/ide/storage/worker-share.ts` | 2 | `WorkerShareProvider` — composes local + share API proxy. |
| `web/src/lib/ide/storage/index.ts` | 2 | Re-exports + factory `getProvider()` based on env. |
| `web/src/lib/stores/drafts.svelte.ts` | 1 | Svelte 5 rune store: drafts list, active draft, mutations. |
| `web/src/lib/components/EditorTabs.svelte` | 1 | Tab bar above the editor. |
| `web/src/lib/components/ShareDialog.svelte` | 2 | Share modal. |
| `web/src/lib/components/Panel/PatchesPanel.svelte` | 1, 2 | Sidebar tab. P1: drafts only. P2: + recently visited. |
| `web/src/routes/p/[slug]/+page.svelte` | 2 | SPA viewer route. Loads share, opens phantom tab, click-to-unmute. |
| `web/src/routes/p/[slug]/+page.ts` | 2 | `export const prerender = false; export const ssr = false;` (client-only since static adapter). |
| `web/share-api/src/index.ts` | 2 | Worker entry — router. |
| `web/share-api/src/handlers.ts` | 2 | `handleShare`, `handleApiGet`, `handleHtmlGet`, `handleReport`. |
| `web/share-api/src/og.ts` | 2 | OG HTML template. |
| `web/share-api/src/slug.ts` | 2 | Slug generation + validation. |
| `web/share-api/schema.sql` | 2 | D1 schema (§5.4). |
| `web/share-api/wrangler.toml` | 2 | D1 binding, rate-limit binding, env vars. |
| `web/share-api/package.json` | 2 | Deps: `@cloudflare/workers-types`, `wrangler` (dev). |
| `web/share-api/README.md` | 2, 4 | Self-hosting guide; Phase 4 polishes it. |
| `web/share-api/test/handlers.test.ts` | 2 | Vitest with `@cloudflare/vitest-pool-workers` — POST /share, GET /api/p/:slug, GET /p/:slug, POST /report. |

### 9.2 Modified files

| File | Phase | Change |
|---|---|---|
| `web/src/lib/stores/editor.svelte.ts` | 1 | Delegate `code` getter/setter to `drafts.activeDraft.code`. Keep the same exported shape. Migration: see §8 step 5. |
| `web/src/routes/+page.svelte` | 1 | Mount `EditorTabs` above the existing editor component. |
| `web/src/lib/components/Panel/Panel.svelte` (or wherever the tab list lives) | 1 | Add the `PatchesPanel` tab. |
| `web/.env.example` | 2 | Add `PUBLIC_SHARE_API_BASE=` (empty default). Document both production (`https://share.nkido.cc`) and self-host examples. |
| `web/README.md` | 4 | Add "Hosting your own share endpoint" section linking to `share-api/README.md`. |

### 9.3 Files explicitly NOT changed

| File | Why |
|---|---|
| `cedar/**` | No engine changes. |
| `akkado/**` | No language changes. |
| `web/static/worklet/cedar-processor.js` | Audio path unaffected. |
| `web/wasm/nkido_wasm.cpp` | No new WASM exports. |
| `web/svelte.config.js` | Stays `adapter-static`. |

---

## 10. Edge Cases

### 10.1 Sharing

- **Empty code:** Worker rejects with 400 (`code` must be ≥ 1 byte).
- **Code at exactly 16 KB:** accepted. > 16 KB → 400 with body `{ error: "code_too_large", limit: 16384 }`.
- **Title with newlines / control chars:** Worker strips `\r\n\t` from title before insert. Trims to 200 chars.
- **Description with > 500 chars:** trimmed to 500 chars before insert.
- **Slug retry exhausts after 3 tries:** Worker returns 500 with `{ error: "slug_collision_giveup" }` — should never trigger at normal scale; logs a warning for operator.
- **Rate limit hit (10/min/IP):** 429 with `Retry-After` header. SPA shows toast "Too many shares from your network — try again in a minute."
- **`parent_slug` provided but doesn't exist:** Worker accepts (FK is `ON DELETE SET NULL`), but sets `parent_slug = NULL` if INSERT fails on FK check. Logs as anomaly.
- **Same user shares the same code twice:** Both succeed; two slugs. No dedup. "I want to share this in two contexts" is a legitimate use.

### 10.2 Viewing `/p/<slug>`

- **Slug doesn't exist:** SPA renders "Patch not found" + "Back to editor". `share.nkido.cc/p/<slug>` returns 404 HTML with the same message + canonical link to `live.nkido.cc/`.
- **Slug points to a soft-deleted patch:** treated as "not found" — both Worker and SPA. UI does not reveal that it once existed.
- **Slug exists but D1 read fails (network blip):** SPA shows "Couldn't load patch — try again" with retry button.
- **Visitor has JS disabled:** `share.nkido.cc/p/<slug>` shows the code in `<pre>`; "Open in editor" link points to `live.nkido.cc/p/<slug>` which itself requires JS — they get a "JS required" notice on live. We accept this; the SPA is JS-only by nature.

### 10.3 Phantom drafts

- **Visitor edits 5 different shares:** 5 phantom drafts in localStorage, keyed `draft:p/<slug>`. Each survives independently.
- **Visitor revisits a share with a phantom:** The phantom code loads (not the original). UI shows a banner: "You have local edits to this patch. [Discard local edits] to view the original."
- **localStorage full / quota exceeded:** Phantom save fails silently in v1; tab loses the `●` indicator. The drafts store should catch and console-warn. Future PRD: graceful UI for "your storage is full — please clean up drafts."
- **Visitor closes tab while phantom has unsaved (in-flight debounce):** debounce flushes synchronously on `beforeunload`; same as the existing single-buffer behavior.
- **Phantom for a slug that gets soft-deleted:** the phantom survives in localStorage; revisiting `/p/<slug>` shows "Patch not found", and the phantom is unreachable from there. The Patches sidebar "Recently visited" section still shows it; clicking offers to open in `/editor` with the phantom's code under a temporary draft.

### 10.4 Forking

- **Fork from a phantom with edits:** Worker stores the *current* (edited) code with `parent_slug` = the visited slug. Phantom is deleted after successful fork.
- **Fork without edits:** allowed. Creates a literal duplicate with `parent_slug` set. Useful for "I want a stable URL of this version even though I'm not changing anything."
- **Fork chain length:** unlimited. UI shows direct parent only ("forked from /p/<x>"). Tree visualization is future PRD.
- **Fork of a soft-deleted patch:** Worker's INSERT will leave `parent_slug` as the deleted slug, but the FK is `ON DELETE SET NULL` so once the parent is hard-deleted (manual cleanup), `parent_slug` becomes NULL. Soft-delete leaves the FK intact — the link is preserved but the parent's `/p/<slug>` returns 404. Fork itself remains viewable.

### 10.5 Drafts and tabs

- **20 drafts, 5 visited slugs, 25 tabs open at once:** allowed but noisy. UI provides a horizontal scroller on the tab bar.
- **User deletes the active draft:** confirmation modal first. After confirm, switches active to the most-recently-updated remaining draft, or creates a fresh "Untitled" if none.
- **All drafts deleted:** drafts store auto-creates "Untitled" with the default sample code so the editor is never empty.
- **First-launch migration with corrupted `nkido-code`:** treat as absent; create empty "Untitled" with default sample.
- **First-launch migration runs twice (e.g., user clears `nkido-drafts` manually):** detect via a one-time flag `nkido-drafts-migrated-v1`. After it's set, never re-import — the user's deletion was intentional.

### 10.6 Self-hosting

- **Self-hoster forgets to create the D1 database:** `wrangler deploy` succeeds, but POST /share returns 500. README covers this.
- **Self-hoster uses HTTP instead of HTTPS for `PUBLIC_SHARE_API_BASE`:** dev only; production CSP/COOP would block. README warns.
- **Two installs share a D1 instance:** allowed but slugs are global to that DB. README clarifies.

### 10.7 Reporting

- **Spam reports from one IP:** no rate limit on `/report` in v1. `report_count` increments freely; operator triages by `reported_at DESC` and uses judgment. If spam-of-reports becomes a real issue, add rate limit (future).
- **Reporter clicks Report twice:** `report_count` increments twice. UX toast "Already reported" is best-effort client-side via a localStorage `reported-slugs` set; not enforced server-side.

---

## 11. Testing Strategy

### 11.1 Worker unit / integration tests

`web/share-api/test/handlers.test.ts` using `@cloudflare/vitest-pool-workers` against a Miniflare D1.

| Test | Expected |
|---|---|
| POST /share with valid body | 200 + `{ slug }` matching `/^[2-9a-zA-Z]{8}$/`; row visible in D1 |
| POST /share with empty code | 400 |
| POST /share with 16,385-byte code | 400 |
| POST /share with title > 200 chars | accepted, trimmed |
| POST /share with `parent_slug` referencing existing slug | 200, INSERT preserves `parent_slug` |
| POST /share with `parent_slug` referencing nonexistent slug | 200, `parent_slug` becomes NULL (logged) |
| GET /api/p/:slug for existing | 200 + JSON; `Cache-Control: public, max-age=31536000, immutable` |
| GET /api/p/:slug for nonexistent | 404 |
| GET /api/p/:slug for soft-deleted | 404 |
| GET /p/:slug for existing | 200 HTML containing `og:title`, meta-refresh to `live.nkido.cc/p/<slug>` |
| POST /report flags a row | `reported_at` set, `report_count` = 1 |
| Slug collision retry | mock 2 collisions then success; verify behavior |
| OPTIONS preflight | returns CORS headers |

### 11.2 SPA unit tests

Vitest in `web/`:

- `LocalDraftProvider` round-trip: save → list → load → delete; recently-visited insertion + cap at 20.
- `WorkerShareProvider` against a stubbed fetch: share returns slug; fetchShare parses JSON.
- `drafts.svelte.ts`: first-launch migration from `nkido-code`; switching active; delete-active fallback to most-recent; empty fallback to Untitled.
- Phantom draft lifecycle: visit slug → no phantom; first keystroke → phantom created; Keep → phantom deleted + named draft created; Fork → phantom deleted, no named draft created (caller navigates).

### 11.3 SPA E2E (manual checklist per phase)

Each phase has a smoke-test checklist (under §8 Verification). Run on a staging Cloudflare Worker + Netlify Deploy Preview. No Playwright in v1.

### 11.4 Adversarial / abuse

- Burst 100 POST /share from one IP → ~10 succeed, rest 429.
- POST 70 KB code → 400.
- POST malformed JSON → 400.
- POST with `slug` field in body → ignored; server-generated slug used.
- Direct INSERT attempt against D1 from a different origin → CORS preflight fails.

### 11.5 Performance verification

- p95 cold patch-load (URL → audio playable, US-East from West Coast) < 1s for a 4 KB patch. Measure manually + via WebPageTest.
- Worker cold start < 100ms (Cloudflare Worker baseline).
- D1 read for a single row < 30ms (Cloudflare baseline).
- SPA initial bundle size delta < 10 KB gzipped (the storage providers + tab UI).

---

## 12. Operational Readiness

| Concern | v1 handling |
|---|---|
| Backups | Cloudflare D1 daily automatic backups (7-day retention). Acceptable for v1; revisit if `patches` grows past 1 GB. |
| Monitoring | Cloudflare dashboard: requests, errors, rate-limit triggers. No custom dashboards. |
| Logs | Worker `console.log` aggregated in Cloudflare dashboard. 7-day retention. |
| Incident playbook | `web/share-api/README.md` covers: D1 down (Worker returns 503), runaway abuse (raise rate limit / disable POST temporarily via env flag), single-row takedown (SQL UPDATE). |
| Status page | None in v1. Both backends (Cloudflare, Netlify) have public status pages. |
| GDPR | Anonymous patches carry no PII. `ip_hash` is daily-salted SHA-256 — not personally identifying after rotation. No "delete me" flow needed in OSS PRD. SaaS PRD will handle account-level deletion. |
| Terms of use | A short note on share dialog: "Public and immutable. Don't share content you don't have rights to." Full ToS is a launch-blocker for the SaaS PRD, not this OSS PRD. |
| `daily-ip-salt-rotate` | A one-line cron Worker (Cloudflare Cron Triggers) rotates the salt daily. Out of v1 scope; documented as "leave the salt static; on first SaaS deploy, switch to rotating." |

---

## 13. Open Questions

- **[OPEN QUESTION] OG image rendering.** The OG HTML in §6.5 has `og:image` placeholder commented. A static "nkido" logo is fine for v1; rendering a per-patch waveform thumbnail (à la Strudel) is a separate small PRD using either a satori-style HTML→PNG Worker or a precomputed pixel waveform per share. Not blocking launch.
- **[OPEN QUESTION] CSP tightening.** Adding `share.nkido.cc` to `connect-src` is straightforward; reworking the existing `unsafe-eval` (required by some WASM tooling) needs separate audit. Out of scope here.
- **[OPEN QUESTION] Recently-visited size cap N.** 20 is a guess. Could be a setting (5/20/100). v1 ships 20 hardcoded.
- **[OPEN QUESTION] Tab bar on mobile.** Horizontal scroller is acceptable but cramped. Whether to switch to a tab dropdown < 600px wide is a UI polish question for Phase 1's v0.

---

## 14. Future Work

Out of v1 scope, but anticipated and worth flagging so the v1 design doesn't paint future PRDs into a corner:

- **The SaaS PRD** (`nkido-pro/docs/prd-open-core-saas.md`) layers Supabase Auth, private patches, profile pages, gallery, hearts, comments, asset uploads, and Stripe billing on top via a third `StorageProvider` impl. This OSS PRD is the prerequisite.
- **Per-patch waveform thumbnail** for OG previews. Standalone small PRD.
- **Fork tree visualization** on `/p/<slug>` (a "Forks of this patch" section reading by `parent_slug` in reverse). Requires GET /api/p/:slug/forks endpoint.
- **Embedded share widget** for blog posts: `<iframe src="https://live.nkido.cc/embed/p/<slug>">`. Reuses the existing `/embed` route plus the new viewer.
- **Slug aliases** ("vanity URLs") — `/p/my-cool-bass` mapping to a slug. Requires a `/u/<handle>` namespace, which is a SaaS feature.
- **Cleanup of `nkido-code` localStorage key** after one release of safety-net retention.
- **Param snapshot at share time.** When a real user requests it, add a `params` JSONB column with the same field-level cap as code. Schema is forward-compatible.
- **Soft-deleted patch tombstone page** ("This patch was removed. [Why?]"). v1 returns 404 indistinguishably.

---

## 15. Cross-PRD Dependencies

| PRD | Status | Dependency |
|---|---|---|
| `prd-nkido-web-ide.md` | PARTIAL | The IDE component being extended. No blocker. |
| `prd-uri-resolver.md` | IN PROGRESS | None. This PRD does not touch the resolver. |
| `prd-project-website.md` | SHIPPED | Uses the existing Cloudflare/Netlify DNS setup. We add `share.nkido.cc` as a new Worker route. |
| `nkido-pro/docs/prd-open-core-saas.md` | NOT STARTED | This PRD is the prerequisite. The SaaS PRD's Phase 1 (anonymous shares) is **fulfilled by this PRD** and will be marked complete once this lands. The SaaS PRD's Phases 2–4 (accounts, social, assets, billing) layer on top of the `StorageProvider` interface defined here. |

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **Draft** | A locally-stored patch in this user's browser. Has a name, code, `updatedAt`. Survives across reloads via `localStorage`. |
| **Active draft** | The draft currently shown in the editor. Tracked in the drafts store; persisted in `localStorage`. |
| **Phantom draft** | A localStorage entry keyed by `draft:p/<slug>` holding the visitor's local edits to a patch they did not author. Created on first keystroke at `/p/<slug>`. Promoted to a regular draft via Keep, or discarded via Fork. |
| **Share** | An anonymous, immutable copy of a patch published to the share Worker. Identified by a `slug`. Persists indefinitely. |
| **Slug** | The 8-char base62 ID in `/p/<slug>` URLs. |
| **Fork** | A new share whose `parent_slug` references an existing share. Created by clicking Fork on `/p/<slug>` (with or without local edits). |
| **Keep** | Promote a phantom draft to a regular named draft, locally. No network call. |
| **`StorageProvider`** | The TS interface in `web/src/lib/ide/storage/types.ts`. Two implementations ship in this PRD; the SaaS PRD adds a third. |
| **share.nkido.cc** | The Cloudflare Worker handling POST /share, GET /api/p/:slug, GET /p/:slug (OG-tagged HTML), POST /report. |
| **live.nkido.cc** | The Netlify-hosted SPA (this repo's `web/`). |
