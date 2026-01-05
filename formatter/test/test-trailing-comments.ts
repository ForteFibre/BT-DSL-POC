import { formatBtDslText } from '../src/index.js';
import assert from 'node:assert';

interface TestCase {
  name: string;
  input: string;
  expected?: string; // If provided, verify exact match; otherwise, verify comment not separated
}

const testCases: TestCase[] = [
  {
    name: 'Trailing comment on const declaration (tree body)',
    input: `tree Main() {
  const X = 10; // requires const folding
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `tree Main() {
  const X = 10; // requires const folding
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
  {
    name: 'Trailing comment on var declaration (tree body)',
    input: `tree Main() {
  var Y: int; // local variable
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `tree Main() {
  var Y: int; // local variable
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
  {
    name: 'Trailing comment on node statement',
    input: `tree Main() {
  Sequence {
    AlwaysSuccess(); // this always succeeds
    AlwaysFailure();
  }
}`,
    expected: `tree Main() {
  Sequence {
    AlwaysSuccess(); // this always succeeds
    AlwaysFailure();
  }
}
`,
  },
  {
    name: 'Trailing comment on global var declaration',
    input: `var X: int // global variable comment

tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `var X: int; // global variable comment

tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
  {
    name: 'Trailing comment on global const declaration',
    input: `const Y = 42 // constant comment

tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `const Y = 42; // constant comment

tree Main() {
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
  {
    name: 'Multiple trailing comments',
    input: `tree Main() {
  const A = 1; // first const
  const B = 2; // second const
  Sequence {
    AlwaysSuccess(); // success node
  }
}`,
    expected: `tree Main() {
  const A = 1; // first const
  const B = 2; // second const
  Sequence {
    AlwaysSuccess(); // success node
  }
}
`,
  },
  {
    name: 'Trailing comment on assignment statement',
    input: `tree Main() {
  var x: int = 0;
  x = 10; // update x
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `tree Main() {
  var x: int = 0;
  x = 10; // update x
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
  {
    name: 'Comment on next line is NOT a trailing comment',
    input: `tree Main() {
  const X = 10;
  // This is on a separate line
  Sequence {
    AlwaysSuccess();
  }
}`,
    expected: `tree Main() {
  const X = 10;
  // This is on a separate line
  Sequence {
    AlwaysSuccess();
  }
}
`,
  },
];

let passed = 0;
let failed = 0;

for (const testCase of testCases) {
  try {
    const formatted = await formatBtDslText(testCase.input, { filepath: 'test.bt' });

    if (testCase.expected) {
      assert.strictEqual(formatted, testCase.expected, `Output does not match expected`);
    } else {
      // Verify the comment text still exists on the expected line
      const inputLines = testCase.input.split('\n');
      const outputLines = formatted.split('\n');
      for (const inputLine of inputLines) {
        const regex = /;\s*(\/\/.*)/;
        const trailingMatch = regex.exec(inputLine);
        if (trailingMatch) {
          const comment = trailingMatch[1];
          if (comment !== undefined) {
            // Check that the comment appears on a line with a semicolon in output
            const found = outputLines.some((line) => line.includes(';') && line.includes(comment));
            assert(found, `Trailing comment "${comment}" was separated from its statement`);
          }
        }
      }
    }

    console.log(`✅ ${testCase.name}`);
    passed++;
  } catch (error) {
    console.error(`❌ ${testCase.name}`);
    console.error(`   Error: ${(error as Error).message}`);
    failed++;
  }
}

console.log(`\n${'='.repeat(60)}`);
console.log(`Results: ${String(passed)} passed, ${String(failed)} failed`);
console.log('='.repeat(60));

if (failed > 0) {
  process.exit(1);
}
