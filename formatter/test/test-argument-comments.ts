import { test, describe } from 'node:test';
import assert from 'node:assert';
import { formatBtDslText } from '../src/index.js';

interface TestCase {
  name: string;
  input: string;
  expected: string;
}

const testCases: TestCase[] = [
  {
    name: 'Comment at start of extern action arguments',
    input: `extern action FollowPath(
  // comment
  in plan_id: int32,
  in tolerance: float = DEFAULT_TOLERANCE,
  out ok: bool
);`,
    expected: `extern action FollowPath(
  // comment
  in plan_id: int32,
  in tolerance: float = DEFAULT_TOLERANCE,
  out ok: bool,
);
`,
  },
  {
    name: 'Comment between extern action arguments',
    input: `extern action Move(
  in x: int32,
  // Y coordinate for movement
  in y: int32,
  out success: bool
);`,
    expected: `extern action Move(
  in x: int32,
  // Y coordinate for movement
  in y: int32,
  out success: bool,
);
`,
  },
  {
    name: 'Comment at end of extern action arguments',
    input: `extern action Stop(
  in force: bool,
  out ok: bool
  // Additional notes
);`,
    expected: `extern action Stop(
  in force: bool,
  out ok: bool,
  // Additional notes
);
`,
  },
  {
    name: 'Multiple comments in extern action arguments',
    input: `extern action ComplexAction(
  // Input parameters
  in a: int32,
  in b: float,
  // Output parameters
  out result: bool
);`,
    expected: `extern action ComplexAction(
  // Input parameters
  in a: int32,
  in b: float,
  // Output parameters
  out result: bool,
);
`,
  },
  {
    name: 'Block comment in extern action arguments',
    input: `extern action WithBlockComment(
  /* This is an input */ in x: int32,
  out y: float
);`,
    expected: `extern action WithBlockComment(
  /* This is an input */ in x: int32,
  out y: float,
);
`,
  },
  {
    name: 'Extern condition with comment',
    input: `extern condition IsReady(
  // Check if system is ready
  in timeout: float
);`,
    expected: `extern condition IsReady(
  // Check if system is ready
  in timeout: float,
);
`,
  },
  {
    name: 'Extern action without comments formats normally',
    input: `extern action SimpleAction(
    in   x:   int32  ,
    out  y:   float
);`,
    expected: `extern action SimpleAction(in x: int32, out y: float);
`,
  },
];

await describe('Argument Comments Preservation', async () => {
  for (const testCase of testCases) {
    await test(testCase.name, async () => {
      const formatted = await formatBtDslText(testCase.input, { filepath: 'test.bt' });
      assert.strictEqual(formatted, testCase.expected);
    });
  }
});
