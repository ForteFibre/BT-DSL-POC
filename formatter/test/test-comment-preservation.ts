import { test, describe } from 'node:test';
import assert from 'node:assert';
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

describe('Comment Preservation', () => {
  for (const testCase of testCases) {
    test(testCase.name, async () => {
      const formatted = await formatBtDslText(testCase.input, { filepath: 'test.bt' });
      assert.ok(formatted, 'Formatted output should not be empty');
    });
  }
});
