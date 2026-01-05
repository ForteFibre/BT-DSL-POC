import { test } from 'node:test';
import assert from 'node:assert';
import { formatBtDslText } from '../src/index.js';

await test('Tree parameters formatting', async () => {
  // Test with just a tree definition (typed params are required by the grammar)
  const testInput = `tree SearchAndDestroy(ref target: Vector3, ref ammo: int, ref alert: bool) {
  Sequence {
    AlwaysSuccess();
  }
}`;

  const output = await formatBtDslText(testInput, { filepath: 'test.bt' });
  assert.ok(output, 'Output should not be empty');
  assert.match(output, /tree SearchAndDestroy/, 'Output should contain tree declaration');
  assert.match(output, /ref target: Vector3/, 'Output should contain typed parameters');
});
