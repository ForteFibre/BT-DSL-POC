#!/usr/bin/env node

import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

import fg from 'fast-glob';
import { unified } from 'unified';
import remarkParse from 'remark-parse';
import { visit } from 'unist-util-visit';
import GithubSlugger from 'github-slugger';

/**
 * Minimal, repo-local link checker for Markdown.
 *
 * What it checks (by default):
 * - Local file links exist (relative links + VitePress-style site links like /guide/quick-start)
 * - Hash anchors (#...) point to an existing heading in the target Markdown
 *
 * What it does NOT check (by default):
 * - External http(s) URLs (can be enabled with --external)
 */

const repoRoot = process.cwd();
const docsRoot = path.join(repoRoot, 'docs');

const argv = new Set(process.argv.slice(2));
const checkExternal = argv.has('--external');

const globs = ['README.md', 'docs/**/*.md', 'formatter/README.md', 'formatter/test/README.md'];

const ignore = [
  '**/.git/**',
  '**/.pnpm/**',
  '**/node_modules/**',
  '**/dist/**',
  '**/out/**',
  '**/out-tsc/**',
  '**/out-test/**',
  '**/build/**',
  '**/core/build/**',
  '**/core/build-*/**',
  '**/core/**/_deps/**',
  '**/tree-sitter-bt-dsl/build/**',
  'docs/generated/**',
];

/** @type {Map<string, Set<string>>} */
const anchorCache = new Map();

function slugifyNumberedHeadingLikeVitePress(input) {
  // This repo's docs commonly link to anchors like:
  //   "#_2-7-定数式constant-expressions"
  //   "#_6-4-ポート方向と引数制約port-direction-and-argument-constraints"
  //
  // That convention is close to a "markdown-it-anchor" style slugify with:
  // - '.' => '-'
  // - ASCII lowercased
  // - punctuation stripped (including parentheses)
  // - spaces => '-'
  // - leading '_' when the id would start with a digit

  let s = String(input).trim();
  if (!s) return '';

  // Lowercase ASCII letters (keep non-ASCII as-is)
  s = s.replace(/[A-Z]/g, (c) => c.toLowerCase());

  // Dots are meaningful for section numbers; keep them as hyphens.
  s = s.replace(/\./g, '-');

  // Remove parentheses and common punctuation. Keep unicode letters/numbers, underscores and hyphens.
  // Also drop Japanese parentheses.
  s = s
    .replace(/[()\[\]{}<>]/g, '')
    .replace(/[（）」「『』【】]/g, '')
    .replace(/[!"#$%&'*,/:;=?@\\^`|~]/g, '')
    .replace(/\s+/g, '-')
    .replace(/-+/g, '-')
    .replace(/^-|-$/g, '');

  if (/^[0-9]/.test(s)) s = `_${s}`;
  return s;
}

function isProbablyExternal(url) {
  return /^https?:\/\//i.test(url);
}

function splitHash(url) {
  const idx = url.indexOf('#');
  if (idx === -1) return { pathPart: url, hash: '' };
  return { pathPart: url.slice(0, idx), hash: url.slice(idx + 1) };
}

function normalizeWindows(p) {
  return p.replace(/\\/g, '/');
}

function resolveVitePressRoute(route) {
  // Examples:
  //  - /guide/quick-start        -> docs/guide/quick-start.md
  //  - /reference/               -> docs/reference/index.md
  //  - /reference/type-system/   -> docs/reference/type-system/index.md
  //  - /examples                 -> docs/examples.md

  let r = route;
  if (r.startsWith('/')) r = r.slice(1);
  if (r === '') return path.join(docsRoot, 'index.md');

  // strip query params (rare in docs but harmless)
  r = r.split('?')[0];

  if (r.endsWith('/')) r += 'index';

  // VitePress resolves extensionless pages to .md
  if (!path.posix.extname(r)) r += '.md';

  return path.join(docsRoot, ...r.split('/'));
}

async function resolveRelativeTarget(baseDir, rawPath) {
  // Try to emulate VitePress/Markdown behavior for local links.
  // - "foo" can mean "foo.md"
  // - "dir/" means "dir/index.md"
  // - "dir" can mean "dir.md" OR "dir/index.md" (prefer existing)
  const clean = rawPath.split('?')[0];
  if (clean === '') return path.resolve(baseDir, 'index.md');

  const candidates = [];

  if (clean.endsWith('/')) {
    candidates.push(path.resolve(baseDir, clean, 'index.md'));
  } else if (path.extname(clean)) {
    candidates.push(path.resolve(baseDir, clean));
  } else {
    candidates.push(path.resolve(baseDir, `${clean}.md`));
    candidates.push(path.resolve(baseDir, clean, 'index.md'));
    candidates.push(path.resolve(baseDir, clean));
  }

  for (const c of candidates) {
    if (await fileExists(c)) return c;
  }

  // fall back to the first candidate (for a nice error message)
  return candidates[0] ?? path.resolve(baseDir, clean);
}

async function fileExists(filePath) {
  try {
    const st = await fs.stat(filePath);
    return st.isFile();
  } catch {
    return false;
  }
}

async function getAnchorsForMarkdownFile(absMdPath) {
  const key = path.resolve(absMdPath);
  const cached = anchorCache.get(key);
  if (cached) return cached;

  const text = await fs.readFile(key, 'utf8');
  const tree = unified().use(remarkParse).parse(text);

  const slugger = new GithubSlugger();
  /** @type {Map<string, number>} */
  const numberedCounts = new Map();
  /** @type {Set<string>} */
  const anchors = new Set();

  visit(tree, 'heading', (node) => {
    // Gather plain text content from heading children
    let headingText = '';
    visit(node, (child) => {
      if (child.type === 'text' || child.type === 'inlineCode') {
        headingText += child.value;
      }
    });

    const t = headingText.trim();
    if (!t) return;

    // GitHubSlugger produces ids like "some-heading" and de-dupes.
    anchors.add(slugger.slug(t));

    // Also add the repo's numbered-heading convention (used by many existing links).
    const base = slugifyNumberedHeadingLikeVitePress(t);
    if (base) {
      const n = numberedCounts.get(base) ?? 0;
      numberedCounts.set(base, n + 1);
      anchors.add(n === 0 ? base : `${base}-${n}`);
    }
  });

  anchorCache.set(key, anchors);
  return anchors;
}

async function checkExternalUrl(url) {
  // Keep it simple and resilient: HEAD first, fallback to GET.
  try {
    const res = await fetch(url, { method: 'HEAD', redirect: 'follow' });
    if (res.ok) return { ok: true };

    // Some hosts block HEAD.
    const res2 = await fetch(url, { method: 'GET', redirect: 'follow' });
    if (res2.ok) return { ok: true };

    return { ok: false, status: res2.status || res.status };
  } catch (e) {
    return { ok: false, error: e instanceof Error ? e.message : String(e) };
  }
}

function shouldIgnoreUrl(url) {
  return (
    url === '' ||
    url.startsWith('mailto:') ||
    url.startsWith('tel:') ||
    url.startsWith('javascript:')
  );
}

function formatLoc(file, line, col) {
  return `${normalizeWindows(path.relative(repoRoot, file))}:${line}:${col}`;
}

async function main() {
  const files = await fg(globs, {
    cwd: repoRoot,
    ignore,
    onlyFiles: true,
    dot: false,
    absolute: true,
  });

  const processor = unified().use(remarkParse);

  /** @type {Array<string>} */
  const errors = [];

  for (const mdFile of files) {
    const text = await fs.readFile(mdFile, 'utf8');
    const tree = processor.parse(text);

    visit(tree, (node) => {
      if (node.type !== 'link' && node.type !== 'image') return;

      const url = node.url ?? '';
      if (shouldIgnoreUrl(url)) return;

      const pos = node.position?.start;
      const line = pos?.line ?? 1;
      const col = pos?.column ?? 1;

      // Handle pure anchors (#foo)
      if (url.startsWith('#')) {
        const hash = url.slice(1);
        if (!hash) return;
        errors.push(
          // will be validated asynchronously below; placeholder stored for now
          JSON.stringify({ kind: 'anchor-only', mdFile, hash, line, col }),
        );
        return;
      }

      if (isProbablyExternal(url)) {
        if (!checkExternal) return;
        errors.push(JSON.stringify({ kind: 'external', mdFile, url, line, col }));
        return;
      }

      // Internal link
      errors.push(JSON.stringify({ kind: 'internal', mdFile, url, line, col }));
    });
  }

  /** @type {Array<string>} */
  const finalErrors = [];

  for (const raw of errors) {
    /** @type {{kind: string, mdFile: string, url?: string, hash?: string, line: number, col: number}} */
    const item = JSON.parse(raw);

    if (item.kind === 'external') {
      const result = await checkExternalUrl(item.url);
      if (!result.ok) {
        const extra = result.status
          ? ` (status ${result.status})`
          : result.error
            ? ` (${result.error})`
            : '';
        finalErrors.push(
          `${formatLoc(item.mdFile, item.line, item.col)} Broken external link: ${item.url}${extra}`,
        );
      }
      continue;
    }

    if (item.kind === 'anchor-only') {
      const anchors = await getAnchorsForMarkdownFile(item.mdFile);
      if (!anchors.has(item.hash)) {
        finalErrors.push(
          `${formatLoc(item.mdFile, item.line, item.col)} Broken anchor: #${item.hash} (no such heading in this file)`,
        );
      }
      continue;
    }

    // Internal: relative path or VitePress route
    const { pathPart, hash } = splitHash(item.url);

    /** @type {string} */
    let targetPath;

    if (pathPart.startsWith('/')) {
      targetPath = resolveVitePressRoute(pathPart);
    } else {
      // Relative to current markdown file
      const baseDir = path.dirname(item.mdFile);
      targetPath = await resolveRelativeTarget(baseDir, pathPart);
    }

    const targetExists = await fileExists(targetPath);
    if (!targetExists) {
      finalErrors.push(
        `${formatLoc(item.mdFile, item.line, item.col)} Broken link: ${item.url} (target not found: ${normalizeWindows(path.relative(repoRoot, targetPath))})`,
      );
      continue;
    }

    if (hash) {
      // Only validate anchors for Markdown targets.
      if (targetPath.toLowerCase().endsWith('.md')) {
        const anchors = await getAnchorsForMarkdownFile(targetPath);
        if (!anchors.has(hash)) {
          finalErrors.push(
            `${formatLoc(item.mdFile, item.line, item.col)} Broken anchor: ${item.url} (no heading "#${hash}" in ${normalizeWindows(path.relative(repoRoot, targetPath))})`,
          );
        }
      }
    }
  }

  if (finalErrors.length > 0) {
    for (const e of finalErrors) console.error(e);
    console.error(`\nFound ${finalErrors.length} broken link(s).`);
    process.exitCode = 1;
    return;
  }

  console.log(`OK: checked links in ${files.length} markdown file(s).`);
}

await main();
