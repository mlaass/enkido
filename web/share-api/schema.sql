-- nkido-share-api D1 schema
-- Apply with: wrangler d1 execute <db-name> --file=schema.sql
-- (PRD: docs/prd-shareable-patches.md §5.4)

CREATE TABLE IF NOT EXISTS patches (
	slug          TEXT PRIMARY KEY,                                -- 8-char base62
	code          TEXT NOT NULL CHECK (length(code) BETWEEN 1 AND 16384),
	title         TEXT CHECK (title IS NULL OR length(title) <= 200),
	description   TEXT CHECK (description IS NULL OR length(description) <= 500),
	parent_slug   TEXT REFERENCES patches(slug) ON DELETE SET NULL,
	created_at    INTEGER NOT NULL DEFAULT (unixepoch() * 1000),  -- epoch ms
	ip_hash       TEXT,                                            -- SHA-256(ip + daily salt) — operator analytics
	reported_at   INTEGER,
	report_count  INTEGER NOT NULL DEFAULT 0,
	deleted_at    INTEGER                                          -- soft-delete via D1 console
);

CREATE INDEX IF NOT EXISTS idx_patches_created_at
	ON patches(created_at DESC) WHERE deleted_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_patches_parent
	ON patches(parent_slug);

CREATE INDEX IF NOT EXISTS idx_patches_reported
	ON patches(reported_at) WHERE reported_at IS NOT NULL;
