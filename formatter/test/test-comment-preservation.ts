import { formatBtDslText } from '../src/index.js';

interface TestCase {
  name: string;
  input: string;
}

const testCases: TestCase[] = [
  {
    name: 'Inline comments in global scope',
    input: `// Comment before import
import "StandardNodes.bt"

// Comment before var
var X: int

// Comment before tree
tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}`,
  },
  {
    name: 'Inline comments in tree body',
    input: `tree Main() {
  // Comment before node
  Sequence {
    // Comment inside children
    AlwaysSuccess();
    // Another comment
    AlwaysFailure();
  }
}`,
  },
  {
    name: 'Block comments',
    input: `/* Block comment */
var X: int

tree Main() {
  /* Multi-line
     block comment */
  Sequence {
    AlwaysSuccess();
  }
}`,
  },
  {
    name: 'Mixed with doc comments',
    input: `//! Inner doc comment

// Regular comment
/// Outer doc comment
tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}`,
  },
];

for (const testCase of testCases) {
  console.log(`\n${'='.repeat(60)}`);
  console.log(`=== ${testCase.name} ===`);
  console.log('='.repeat(60));
  console.log('\nInput:');
  console.log(testCase.input);

  try {
    const formatted = await formatBtDslText(testCase.input, { filepath: 'test.bt' });
    console.log('\nFormatted output:');
    console.log(formatted);
  } catch (error) {
    console.error('\n❌ Error formatting:', (error as Error).message);
  }
}

console.log('\n' + '='.repeat(60));
console.log('=== Testing complete ===');
console.log('='.repeat(60));
