import assert from 'node:assert/strict';
import { test } from 'node:test';
import { mkdtempSync, copyFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Import setCoreWasmPath directly from core-wasm to configure the path
// BEFORE the Prettier plugin is imported (it eagerly initializes the WASM module).
import { setCoreWasmPath } from '../src/core-wasm.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

function countOccurrences(haystack: string, needle: string): number {
  let count = 0;
  let i = 0;
  let at = haystack.indexOf(needle, i);
  while (at !== -1) {
    count++;
    i = at + needle.length;
    at = haystack.indexOf(needle, i);
  }
  return count;
}

// When running from dist/test/, dist is at ../
const distDir = join(__dirname, '..');

await test('WASM Relocation and UTF-8 handling', async () => {
  const tmp = mkdtempSync(join(tmpdir(), 'bt-dsl-core-wasm-'));
  try {
    // Copy the WASM bundle to a different directory, to simulate relocation
    // (e.g. VSCode extension packaging).
    copyFileSync(join(distDir, 'formatter_wasm.js'), join(tmp, 'formatter_wasm.js'));
    copyFileSync(join(distDir, 'formatter_wasm.wasm'), join(tmp, 'formatter_wasm.wasm'));

    // Set the WASM path BEFORE importing the plugin (which eagerly loads the module).
    setCoreWasmPath(tmp);

    // Now import formatBtDslText (this will trigger the plugin load).
    const { formatBtDslText } = await import('../src/index.js');

    // Test case 1: Basic relocation
    {
      const input = `tree Main(){\n  Sequence{\n    AlwaysSuccess();\n  }\n}`;
      const out = await formatBtDslText(input, { filepath: 'relocation.bt' });

      assert.match(out, /tree Main\(\) \{/);
      assert.match(out, /Sequence \{/);
      assert.match(out, /AlwaysSuccess\(\);/);
    }

    // Test case 2: UTF-8 offset handling
    {
      // Regression: core ranges are UTF-8 byte offsets, but JS string slicing is UTF-16.
      // This used to cause incorrect slicing/duplication around non-ASCII text.
      const input = `tree Main() {\n  // 日本語コメント\n  Sequence {\n    AlwaysSuccess();\n  }\n}`;
      const out = await formatBtDslText(input, { filepath: 'byte-offset.bt' });

      assert.equal(countOccurrences(out, '日本語コメント'), 1);
      assert.equal(countOccurrences(out, 'AlwaysSuccess();'), 1);
    }
  } finally {
    try {
      rmSync(tmp, { recursive: true, force: true });
    } catch {
      // ignore
    }
  }
});
