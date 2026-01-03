import { formatBtDslText } from '../src/index.js';

function assertIncludes(haystack: string, needle: string, message: string): void {
  if (!haystack.includes(needle)) {
    // Make failures obvious in CI.
    console.error('Formatted output was:');
    console.error(haystack);
    throw new Error(message);
  }
}

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

assertIncludes(
  formatted,
  'extern control Sequence();',
  'extern control without ports must keep parentheses',
);
assertIncludes(
  formatted,
  'extern decorator Inverter();',
  'extern decorator without ports must keep parentheses',
);

console.log('extern parens test passed');
