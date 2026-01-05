import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { generateStdlibMarkdown } from './gen-stdlib.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const docsRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(docsRoot, '..');

const generatedDir = path.join(docsRoot, 'generated');

await mkdir(generatedDir, { recursive: true });

// 1) Standard library reference
{
  const stdlibPath = path.join(repoRoot, 'core', 'std', 'nodes.bt');
  const outMd = await generateStdlibMarkdown(stdlibPath);
  await writeFile(path.join(generatedDir, 'standard-library.generated.md'), outMd, 'utf8');
}
