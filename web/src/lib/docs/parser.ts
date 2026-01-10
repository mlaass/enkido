/**
 * Markdown parser for documentation files
 *
 * Parses frontmatter and transforms markdown content.
 * Code blocks with `akk` language are marked for widget transformation.
 */

import matter from 'gray-matter';
import { Marked } from 'marked';
import type { DocFile, DocFrontmatter, LookupEntry } from './types';

/** Custom renderer that adds IDs to headings and marks akk code blocks */
const marked = new Marked({
	renderer: {
		heading({ text, depth }) {
			const id = text
				.toLowerCase()
				.replace(/[^a-z0-9]+/g, '-')
				.replace(/^-|-$/g, '');
			return `<h${depth} id="${id}">${text}</h${depth}>\n`;
		},
		code({ text, lang }) {
			const isAkkado = lang === 'akk' || lang === 'akkado';
			if (isAkkado) {
				// Wrap in a special container for widget transformation
				const escaped = text
					.replace(/&/g, '&amp;')
					.replace(/</g, '&lt;')
					.replace(/>/g, '&gt;');
				return `<div class="akk-code-block" data-code="${encodeURIComponent(text)}"><pre><code class="language-akk">${escaped}</code></pre></div>`;
			}
			// Regular code block
			const escaped = text
				.replace(/&/g, '&amp;')
				.replace(/</g, '&lt;')
				.replace(/>/g, '&gt;');
			return `<pre><code class="language-${lang || ''}">${escaped}</code></pre>`;
		}
	}
});

/**
 * Parse a markdown file with frontmatter
 */
export function parseMarkdown(raw: string, path: string): DocFile {
	const { data, content } = matter(raw);
	const frontmatter = data as DocFrontmatter;

	// Derive slug from path (e.g., "reference/builtins/oscillators.md" -> "oscillators")
	const slug = path
		.replace(/\.md$/, '')
		.split('/')
		.pop() || path;

	return {
		slug,
		path,
		frontmatter,
		content
	};
}

/**
 * Render markdown content to HTML
 */
export function renderMarkdown(content: string): string {
	return marked.parse(content) as string;
}

/**
 * Extract headings from markdown content for anchor generation
 */
export function extractHeadings(content: string): Array<{ level: number; text: string; id: string }> {
	const headings: Array<{ level: number; text: string; id: string }> = [];
	const headingRegex = /^(#{1,6})\s+(.+)$/gm;

	let match;
	while ((match = headingRegex.exec(content)) !== null) {
		const level = match[1].length;
		const text = match[2].trim();
		// Create URL-safe ID
		const id = text
			.toLowerCase()
			.replace(/[^a-z0-9]+/g, '-')
			.replace(/^-|-$/g, '');
		headings.push({ level, text, id });
	}

	return headings;
}

/**
 * Build lookup entries from a parsed document
 * Creates entries for keywords and h2 headings (function names)
 */
export function buildLookupEntries(doc: DocFile): LookupEntry[] {
	const entries: LookupEntry[] = [];
	const { frontmatter, slug } = doc;

	// Add entries for all keywords
	for (const keyword of frontmatter.keywords) {
		entries.push({
			keyword: keyword.toLowerCase(),
			slug,
			category: frontmatter.category,
			title: frontmatter.title
		});
	}

	// For builtins, also index h2 headings as function names
	if (frontmatter.category === 'builtins') {
		const headings = extractHeadings(doc.content);
		for (const heading of headings) {
			if (heading.level === 2) {
				// h2 headings are typically function names
				entries.push({
					keyword: heading.text.toLowerCase(),
					slug,
					category: frontmatter.category,
					title: heading.text,
					anchor: heading.id
				});
			}
		}
	}

	return entries;
}

/**
 * Extract all akk code blocks from rendered HTML
 */
export function extractCodeBlocks(html: string): string[] {
	const blocks: string[] = [];
	const regex = /data-code="([^"]+)"/g;

	let match;
	while ((match = regex.exec(html)) !== null) {
		blocks.push(decodeURIComponent(match[1]));
	}

	return blocks;
}
