import { formatBtDslText } from '../src/index.js';

// Test with just a tree definition (typed params are required by the grammar)
const testInput = `tree SearchAndDestroy(ref target: Vector3, ref ammo: int, ref alert: bool) {
  Sequence {
    AlwaysSuccess();
  }
}`;

console.log('=== Test Input ===');
console.log(testInput);
console.log('\n=== Formatted Output ===');

try {
  const output = await formatBtDslText(testInput, { filepath: 'test.bt' });
  console.log(output);
} catch (error) {
  console.error('Error:', error);
  process.exit(1);
}
