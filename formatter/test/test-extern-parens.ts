import { test } from 'node:test';
import assert from 'node:assert';
import { formatBtDslText } from '../src/index.js';

test('Extern declaration parentheses', async () => {
  const input = `extern control Sequence();
extern decorator Inverter();

// With ports as well
extern action DoThing(in x: int32);

tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}
`;

  const formatted = await formatBtDslText(input, { filepath: 'test.bt' });

  assert.ok(formatted.includes('extern control Sequence();'),
    'extern control without ports must keep parentheses');
  assert.ok(formatted.includes('extern decorator Inverter();'),
    'extern decorator without ports must keep parentheses');
});
