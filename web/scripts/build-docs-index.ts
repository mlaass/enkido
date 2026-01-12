/**
 * Build script to generate the unified docs manifest from markdown files.
 * Single source of truth: markdown files + frontmatter
 *
 * Run with: bun scripts/build-docs-index.ts
 */

import { readdir, readFile, writeFile } from 'fs/promises';
import { join } from 'path';
import matter from 'gray-matter';

interface DocEntry {
	slug: string;
	path: string;
	title: string;
	category: string;
	order: number;
	preview: string;
}

interface LookupEntry {
	slug: string;
	category: string;
	title: string;
	anchor?: string;
}

interface NavItem {
	slug: string;
	title: string;
}

const DOCS_DIR = join(import.meta.dir, '../static/docs');
const OUTPUT_FILE = join(import.meta.dir, '../src/lib/docs/manifest.ts');

// Valid categories in display order
const CATEGORY_ORDER = ['tutorials', 'builtins', 'language', 'mini-notation', 'concepts'];

async function findMarkdownFiles(dir: string): Promise<string[]> {
	const files: string[] = [];
	const entries = await readdir(dir, { withFileTypes: true });

	for (const entry of entries) {
		const fullPath = join(dir, entry.name);
		if (entry.isDirectory()) {
			files.push(...(await findMarkdownFiles(fullPath)));
		} else if (entry.name.endsWith('.md')) {
			files.push(fullPath);
		}
	}

	return files;
}

function extractPreview(content: string): string {
	// Remove headers, code blocks, and get first meaningful paragraph
	const stripped = content
		.replace(/^#.*$/gm, '') // Remove headers
		.replace(/```[\s\S]*?```/g, '') // Remove code blocks
		.replace(/`[^`]+`/g, '') // Remove inline code
		.replace(/\[([^\]]+)\]\([^)]+\)/g, '$1') // Convert links to text
		.replace(/\*\*([^*]+)\*\*/g, '$1') // Remove bold
		.replace(/\*([^*]+)\*/g, '$1') // Remove italic
		.trim();

	// Get first non-empty paragraph
	const paragraphs = stripped.split(/\n\n+/).filter((p) => p.trim().length > 0);
	const firstPara = paragraphs[0]?.replace(/\n/g, ' ').trim() ?? '';

	// Truncate to ~150 chars at word boundary
	if (firstPara.length <= 150) return firstPara;
	const truncated = firstPara.slice(0, 147);
	const lastSpace = truncated.lastIndexOf(' ');
	return (lastSpace > 100 ? truncated.slice(0, lastSpace) : truncated) + '...';
}

function extractHeadings(content: string): Array<{ level: number; text: string; id: string }> {
	const headings: Array<{ level: number; text: string; id: string }> = [];
	const headingRegex = /^(#{1,6})\s+(.+)$/gm;

	let match;
	while ((match = headingRegex.exec(content)) !== null) {
		const level = match[1].length;
		const text = match[2].trim();
		const id = text
			.toLowerCase()
			.replace(/[^a-z0-9]+/g, '-')
			.replace(/^-|-$/g, '');
		headings.push({ level, text, id });
	}

	return headings;
}

async function buildManifest() {
	const files = await findMarkdownFiles(DOCS_DIR);
	const docs: DocEntry[] = [];
	const lookup = new Map<string, LookupEntry>();

	for (const filePath of files) {
		const relativePath = filePath.replace(DOCS_DIR + '/', '');
		const raw = await readFile(filePath, 'utf-8');
		const { data: frontmatter, content } = matter(raw);

		if (!frontmatter.title || !frontmatter.category) {
			console.warn(`Skipping ${relativePath}: missing title or category`);
			continue;
		}

		// Derive slug from filename
		const slug = relativePath
			.replace(/\.md$/, '')
			.split('/')
			.pop()!;

		const order = typeof frontmatter.order === 'number' ? frontmatter.order : 999;
		const preview = extractPreview(content);

		docs.push({
			slug,
			path: relativePath,
			title: frontmatter.title,
			category: frontmatter.category,
			order,
			preview
		});

		// For builtins, index h2 headings (function names with anchors)
		if (frontmatter.category === 'builtins') {
			const headings = extractHeadings(content);
			for (const heading of headings) {
				if (heading.level === 2) {
					const key = heading.text.toLowerCase();
					lookup.set(key, {
						slug,
						category: frontmatter.category,
						title: heading.text,
						anchor: heading.id
					});
				}
			}
		}

		// Add frontmatter keywords (won't overwrite h2 entries with anchors)
		if (Array.isArray(frontmatter.keywords)) {
			for (const keyword of frontmatter.keywords) {
				const key = keyword.toLowerCase();
				if (!lookup.has(key)) {
					lookup.set(key, {
						slug,
						category: frontmatter.category,
						title: frontmatter.title
					});
				}
			}
		}
	}

	// Build slugToPath map
	const slugToPath: Record<string, string> = {};
	for (const doc of docs) {
		slugToPath[doc.slug] = doc.path;
	}

	// Build navigation by category, sorted by order then title
	const navigation: Record<string, NavItem[]> = {};
	for (const category of CATEGORY_ORDER) {
		const categoryDocs = docs
			.filter((d) => d.category === category)
			.sort((a, b) => {
				if (a.order !== b.order) return a.order - b.order;
				return a.title.localeCompare(b.title);
			});

		navigation[category] = categoryDocs.map((d) => ({
			slug: d.slug,
			title: d.title
		}));
	}

	// Convert lookup map to object
	const lookupObj: Record<string, LookupEntry> = {};
	for (const [key, value] of lookup) {
		lookupObj[key] = value;
	}

	// Build previews map
	const previews: Record<string, string> = {};
	for (const doc of docs) {
		previews[doc.slug] = doc.preview;
	}

	return { slugToPath, navigation, lookup: lookupObj, previews };
}

async function main() {
	console.log('Building docs manifest...');
	const { slugToPath, navigation, lookup, previews } = await buildManifest();

	const output = `// Auto-generated by scripts/build-docs-index.ts
// Do not edit manually - run: bun run build:docs

export const slugToPath: Record<string, string> = ${JSON.stringify(slugToPath, null, '\t')};

export const navigation: Record<string, Array<{ slug: string; title: string }>> = ${JSON.stringify(navigation, null, '\t')};

export const lookup: Record<string, { slug: string; category: string; title: string; anchor?: string }> = ${JSON.stringify(lookup, null, '\t')};

export const previews: Record<string, string> = ${JSON.stringify(previews, null, '\t')};
`;

	await writeFile(OUTPUT_FILE, output);

	const docCount = Object.keys(slugToPath).length;
	const lookupCount = Object.keys(lookup).length;
	console.log(`Generated ${OUTPUT_FILE}`);
	console.log(`  - ${docCount} documents`);
	console.log(`  - ${lookupCount} lookup entries`);
	console.log(`  - ${docCount} previews`);
}

main().catch(console.error);
