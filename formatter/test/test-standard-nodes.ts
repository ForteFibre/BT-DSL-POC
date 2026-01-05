import { test } from 'node:test';
import assert from 'node:assert';
import { formatBtDslText } from '../src/index.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// When running from dist/test/, shared is at ../../../shared
const standardNodesPath = join(__dirname, '../../../shared/examples/StandardNodes.bt');
const input = readFileSync(standardNodesPath, 'utf8');

test('Format StandardNodes.bt', async () => {
  const output = await formatBtDslText(input, { filepath: standardNodesPath });
  assert.ok(output, 'Output should not be empty');
  assert.match(output, /extern action FindEnemy/, 'Output should contain extern action declaration');
});
