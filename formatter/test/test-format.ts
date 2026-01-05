import { test } from 'node:test';
import assert from 'node:assert';
import { formatBtDslText } from '../src/index.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// When running from dist/test/, shared is at ../../shared
const soldierAiPath = join(__dirname, '../../../shared/examples/soldier-ai.bt');
const input = readFileSync(soldierAiPath, 'utf8');

await test('Format soldier-ai.bt', async () => {
  const output = await formatBtDslText(input, { filepath: soldierAiPath });
  assert.ok(output, 'Output should not be empty');
  assert.match(output, /tree Main/, 'Output should contain tree Main');
});
