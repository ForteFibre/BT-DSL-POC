import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { generateStdlibMarkdown } from './gen-stdlib.mjs';
import { generateRailroadSvgs } from './gen-railroad.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const docsRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(docsRoot, '..');

const generatedDir = path.join(docsRoot, 'generated');
const publicRailroadDir = path.join(docsRoot, 'public', 'railroad');

await mkdir(generatedDir, { recursive: true });
await mkdir(publicRailroadDir, { recursive: true });

// 1) Standard library reference
{
  const stdlibPath = path.join(repoRoot, 'core', 'std', 'nodes.bt');
  const outMd = await generateStdlibMarkdown(stdlibPath);
  await writeFile(path.join(generatedDir, 'standard-library.generated.md'), outMd, 'utf8');
}

// 2) Railroad diagrams from tree-sitter generated grammar.json
{
  const grammarJsonPath = path.join(repoRoot, 'tree-sitter-bt-dsl', 'src', 'grammar.json');
  const { indexMarkdown, svgs } = await generateRailroadSvgs(grammarJsonPath);

  // Write SVGs
  for (const [name, svg] of Object.entries(svgs)) {
    await writeFile(path.join(publicRailroadDir, `${name}.svg`), svg, 'utf8');
  }

  // Write index markdown
  await writeFile(path.join(generatedDir, 'railroad.generated.md'), indexMarkdown, 'utf8');
}
