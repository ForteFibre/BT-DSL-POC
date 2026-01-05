/**
 * Test suite for formatter behavior on malformed/broken input.
 *
 * Requirements:
 * 1. The formatter does NOT need to pretty-print broken/invalid syntax
 * 2. The formatter MUST format valid portions as much as possible
 * 3. Characters MUST NEVER be silently deleted (critical bug)
 *
 * Each test verifies that:
 * - All characters from the input are preserved in the output
 * - Valid portions are formatted correctly (where verifiable)
 */

import { formatBtDslText } from '../src/index.js';

// Helper: assert that non-whitespace characters are preserved EXACTLY.
// Formatting may change whitespace, but must never add/delete/reorder non-whitespace.
function assertNoNonWhitespaceChange(input: string, output: string, testName: string): void {
  const inStripped = input.replace(/\s+/g, '');
  const outStripped = output.replace(/\s+/g, '');

  if (inStripped !== outStripped) {
    // Find first mismatch for a more actionable error.
    const n = Math.min(inStripped.length, outStripped.length);
    let idx = 0;
    while (idx < n && inStripped[idx] === outStripped[idx]) idx++;

    const inCh = inStripped[idx] ?? '(end)';
    const outCh = outStripped[idx] ?? '(end)';
    console.error(`[${testName}] Non-whitespace change detected!`);
    console.error(`  First mismatch at stripped index ${String(idx)}`);
    console.error(`  Input char:  ${JSON.stringify(inCh)}`);
    console.error(`  Output char: ${JSON.stringify(outCh)}`);
    console.error('  Input:', JSON.stringify(input));
    console.error('  Output:', JSON.stringify(output));
    throw new Error(`[${testName}] Formatter changed non-whitespace characters`);
  }
}

// Helper: assert specific tokens/strings are preserved
function assertPreserved(output: string, tokens: string[], testName: string): void {
  for (const token of tokens) {
    if (!output.includes(token)) {
      console.error(`[${testName}] Expected token not found in output!`);
      console.error(`  Missing: ${JSON.stringify(token)}`);
      console.error('  Output:', JSON.stringify(output));
      throw new Error(`[${testName}] Token ${JSON.stringify(token)} was lost`);
    }
  }
}

interface TestCase {
  name: string;
  input: string;
  mustPreserve: string[]; // tokens that must appear in output
  description: string;
}

// ============================================================================
// TEST CASES: Malformed extern declarations
// ============================================================================

const externTestCases: TestCase[] = [
  // User's original example - incomplete extern action declarations
  {
    name: 'incomplete-extern-action-declarations',
    input: `extern action ReadPose(out current: Vector3)
extern action SetFixedGoal(out goal: Vector3)
extern action FormatStatus(in ok: bool, in txt: string, out out_txt: string);
extern action Print(in txt: string);
`,
    mustPreserve: [
      'extern',
      'action',
      'ReadPose',
      'current',
      'Vector3',
      'SetFixedGoal',
      'goal',
      'FormatStatus',
      'ok',
      'bool',
      'txt',
      'string',
      'out_txt',
      'Print',
    ],
    description: 'Missing semicolons on some extern action declarations',
  },

  // Missing closing parenthesis
  {
    name: 'extern-missing-closing-paren',
    input: `extern action Move(in x: int32, in y: int32
extern action Stop();
`,
    mustPreserve: ['extern', 'action', 'Move', 'x', 'int32', 'y', 'Stop'],
    description: 'Missing closing parenthesis on extern action',
  },

  // Missing type annotation
  {
    name: 'extern-missing-type',
    input: `extern action Foo(in x: );
extern action Bar(in y: int32);
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', 'Bar', 'y', 'int32'],
    description: 'Missing type after colon',
  },

  // Invalid direction keyword
  {
    name: 'extern-invalid-direction',
    input: `extern action Test(inout z: float);
extern action Valid(in a: int32);
`,
    mustPreserve: ['extern', 'action', 'Test', 'inout', 'z', 'float', 'Valid', 'a', 'int32'],
    description: 'Invalid direction keyword (inout instead of in/out/ref/mut)',
  },

  // Mixed valid and invalid
  {
    name: 'extern-mixed-valid-invalid',
    input: `extern control Sequence();
extern action Broken(in x:
extern decorator Inverter();
extern action Another(out y: string
`,
    mustPreserve: [
      'extern',
      'control',
      'Sequence',
      'action',
      'Broken',
      'x',
      'decorator',
      'Inverter',
      'Another',
      'y',
      'string',
    ],
    description: 'Mix of valid and broken extern declarations',
  },

  // Multiple ports with missing types
  {
    name: 'extern-multiple-broken-ports',
    input: `extern action Complex(
  in a: int32,
  in b: ,
  out c: float,
  in d:
);
`,
    mustPreserve: ['extern', 'action', 'Complex', 'a', 'int32', 'b', 'c', 'float', 'd'],
    description: 'Multiple ports with some missing types',
  },
];

// ============================================================================
// TEST CASES: Malformed tree declarations
// ============================================================================

const treeTestCases: TestCase[] = [
  // Missing opening brace
  {
    name: 'tree-missing-open-brace',
    input: `tree Main()
  Sequence {
    AlwaysSuccess();
  }
}
`,
    mustPreserve: ['tree', 'Main', 'Sequence', 'AlwaysSuccess'],
    description: 'Tree declaration missing opening brace',
  },

  // Missing closing brace
  {
    name: 'tree-missing-close-brace',
    input: `tree Main() {
  Sequence {
    AlwaysSuccess();
  }
`,
    mustPreserve: ['tree', 'Main', 'Sequence', 'AlwaysSuccess'],
    description: 'Tree declaration missing closing brace',
  },

  // Unclosed node
  {
    name: 'tree-unclosed-node',
    input: `tree Main() {
  Sequence {
    AlwaysSuccess();
    Fallback {
      NodeA();
  }
}
`,
    mustPreserve: ['tree', 'Main', 'Sequence', 'AlwaysSuccess', 'Fallback', 'NodeA'],
    description: 'Unclosed Fallback node',
  },

  // Missing parentheses on tree name
  {
    name: 'tree-missing-parens',
    input: `tree Main {
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'AlwaysSuccess'],
    description: 'Tree declaration missing parentheses',
  },

  // Invalid parameter syntax
  {
    name: 'tree-invalid-param',
    input: `tree WithParam(x int32) {
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'WithParam', 'x', 'int32', 'AlwaysSuccess'],
    description: 'Tree parameter missing colon',
  },

  // Multiple trees with errors
  {
    name: 'tree-multiple-mixed-errors',
    input: `tree First() {
  AlwaysSuccess();
}

tree Second(
  AlwaysFailure();
}

tree Third() {
  NodeX();
}
`,
    mustPreserve: ['tree', 'First', 'AlwaysSuccess', 'Second', 'AlwaysFailure', 'Third', 'NodeX'],
    description: 'Multiple trees with mixed valid/invalid declarations',
  },
];

// ============================================================================
// TEST CASES: Malformed statements
// ============================================================================

const stmtTestCases: TestCase[] = [
  // Missing semicolon on var declaration
  {
    name: 'stmt-var-missing-semicolon',
    input: `tree Main() {
  var x: int32 = 5
  var y: int32 = 10;
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'var', 'x', 'int32', '5', 'y', '10', 'AlwaysSuccess'],
    description: 'Variable declaration missing semicolon',
  },

  // Invalid assignment operator
  {
    name: 'stmt-invalid-assignment-op',
    input: `tree Main() {
  x := 5;
  y = 10;
}
`,
    mustPreserve: ['tree', 'Main', 'x', ':=', '5', 'y', '10'],
    description: 'Invalid assignment operator :=',
  },

  // Missing value in assignment
  {
    name: 'stmt-missing-assignment-value',
    input: `tree Main() {
  x = ;
  y = 10;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'y', '10'],
    description: 'Assignment missing value',
  },

  // Broken precondition
  {
    name: 'stmt-broken-precondition',
    input: `tree Main() {
  @run_if(x > 5
  AlwaysSuccess();
  
  @run_if(y < 10)
  AlwaysFailure();
}
`,
    mustPreserve: [
      'tree',
      'Main',
      '@run_if',
      'x',
      '5',
      'AlwaysSuccess',
      'y',
      '10',
      'AlwaysFailure',
    ],
    description: 'Precondition with unclosed parenthesis',
  },

  // Dangling decorator
  {
    name: 'stmt-dangling-decorator',
    input: `tree Main() {
  @run_while(running)
}
`,
    mustPreserve: ['tree', 'Main', '@run_while', 'running'],
    description: 'Decorator with no following statement',
  },

  // Const without value
  {
    name: 'stmt-const-no-value',
    input: `tree Main() {
  const LIMIT: int32;
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'const', 'LIMIT', 'int32', 'AlwaysSuccess'],
    description: 'Const declaration without value',
  },
];

// ============================================================================
// TEST CASES: Malformed expressions
// ============================================================================

const exprTestCases: TestCase[] = [
  // Unclosed parenthesis in expression
  {
    name: 'expr-unclosed-paren',
    input: `tree Main() {
  x = (a + b * c;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', 'b', 'c'],
    description: 'Unclosed parenthesis in expression',
  },

  // Missing operand
  {
    name: 'expr-missing-operand',
    input: `tree Main() {
  x = a + ;
  y = b * 2;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', 'y', 'b', '2'],
    description: 'Binary expression missing right operand',
  },

  // Invalid operator
  {
    name: 'expr-invalid-operator',
    input: `tree Main() {
  x = a +++ b;
  y = c + d;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', 'b', 'y', 'c', 'd'],
    description: 'Invalid operator +++',
  },

  // Unclosed array literal
  {
    name: 'expr-unclosed-array',
    input: `tree Main() {
  arr = [1, 2, 3;
  other = 5;
}
`,
    mustPreserve: ['tree', 'Main', 'arr', '1', '2', '3', 'other', '5'],
    description: 'Unclosed array literal',
  },

  // Unclosed string
  {
    name: 'expr-unclosed-string',
    input: `tree Main() {
  s = "hello world;
  t = "valid";
}
`,
    mustPreserve: ['tree', 'Main', 's', 'hello', 'world', 't', 'valid'],
    description: 'Unclosed string literal',
  },

  // Invalid cast syntax
  {
    name: 'expr-invalid-cast',
    input: `tree Main() {
  x = value as ;
  y = other as int32;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'value', 'as', 'y', 'other', 'int32'],
    description: 'Cast expression missing target type',
  },

  // Chained comparison (invalid in many languages)
  {
    name: 'expr-chained-comparison',
    input: `tree Main() {
  x = a < b < c;
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', 'b', 'c'],
    description: 'Chained comparison operators',
  },
];

// ============================================================================
// TEST CASES: Malformed type expressions
// ============================================================================

const typeTestCases: TestCase[] = [
  // Unclosed generic
  {
    name: 'type-unclosed-generic',
    input: `extern action Foo(in x: vec<int32);
extern action Bar(in y: vec<string>);
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', 'vec', 'int32', 'Bar', 'y', 'string'],
    description: 'Unclosed generic type parameter',
  },

  // Missing nullable type
  {
    name: 'type-missing-nullable',
    input: `extern action Foo(in x: ?);
extern action Bar(in y: int32?);
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', 'Bar', 'y', 'int32'],
    description: 'Nullable marker without type',
  },

  // Invalid array syntax
  {
    name: 'type-invalid-array',
    input: `extern action Foo(in x: [int32);
extern action Bar(in y: [int32; 10]);
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', 'int32', 'Bar', 'y', '10'],
    description: 'Invalid array type syntax',
  },

  // Nested broken types
  {
    name: 'type-nested-broken',
    input: `extern action Foo(in x: vec<vec<int32>);
extern action Bar(in y: vec<vec<int32>>);
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', 'vec', 'int32', 'Bar', 'y'],
    description: 'Nested generic with missing closing bracket',
  },
];

// ============================================================================
// TEST CASES: Import and global declarations
// ============================================================================

const globalTestCases: TestCase[] = [
  // Missing import path quotes
  {
    name: 'import-missing-quotes',
    input: `import other.bt;
import "valid.bt";
`,
    mustPreserve: ['import', 'other', 'bt', 'valid'],
    description: 'Import path without quotes',
  },

  // Missing import semicolon
  {
    name: 'import-missing-semicolon',
    input: `import "first.bt"
import "second.bt";
`,
    mustPreserve: ['import', 'first', 'second'],
    description: 'Import statement missing semicolon',
  },

  // Broken global var
  {
    name: 'global-var-broken',
    input: `var globalX: int32 =
var globalY: int32 = 42;
`,
    mustPreserve: ['var', 'globalX', 'int32', 'globalY', '42'],
    description: 'Global variable with missing initializer',
  },

  // Broken type alias
  {
    name: 'type-alias-broken',
    input: `type MyInt = 
type MyFloat = float32;
`,
    mustPreserve: ['type', 'MyInt', 'MyFloat', 'float32'],
    description: 'Type alias with missing aliased type',
  },

  // Extern type with extra tokens
  {
    name: 'extern-type-extra-tokens',
    input: `extern type Foo Bar;
extern type ValidType;
`,
    mustPreserve: ['extern', 'type', 'Foo', 'Bar', 'ValidType'],
    description: 'Extern type with unexpected tokens',
  },
];

// ============================================================================
// TEST CASES: Comments in broken code
// ============================================================================

const commentTestCases: TestCase[] = [
  // Comment after broken statement
  {
    name: 'comment-after-broken-stmt',
    input: `tree Main() {
  x = a + // broken expression
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', '//', 'broken', 'expression', 'AlwaysSuccess'],
    description: 'Line comment after incomplete expression',
  },

  // Block comment in broken declaration
  {
    name: 'comment-block-in-broken',
    input: `extern action Foo(in x: /* inline */ int32
extern action Bar();
`,
    mustPreserve: ['extern', 'action', 'Foo', 'x', '/*', 'inline', '*/', 'int32', 'Bar'],
    description: 'Block comment inside broken extern',
  },

  // Japanese comment preservation
  {
    name: 'comment-japanese-in-broken',
    input: `tree Main() {
  // 日本語コメント
  x = broken
  // もう一つのコメント
  AlwaysSuccess();
}
`,
    mustPreserve: [
      'tree',
      'Main',
      '日本語コメント',
      'x',
      'broken',
      'もう一つのコメント',
      'AlwaysSuccess',
    ],
    description: 'Japanese comments around broken code',
  },

  // Multi-line block comment
  {
    name: 'comment-multiline-block',
    input: `tree Main() {
  /* This is a
     multi-line comment
     that spans lines */
  x = y
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', '/*', 'multi-line', '*/', 'x', 'y', 'AlwaysSuccess'],
    description: 'Multi-line block comment with broken statement',
  },
];

// ============================================================================
// TEST CASES: Node invocations
// ============================================================================

const nodeTestCases: TestCase[] = [
  // Missing argument value
  {
    name: 'node-missing-arg-value',
    input: `tree Main() {
  MyAction(x: );
  OtherAction(y: 10);
}
`,
    mustPreserve: ['tree', 'Main', 'MyAction', 'x', 'OtherAction', 'y', '10'],
    description: 'Node call with missing argument value',
  },

  // Invalid argument syntax
  {
    name: 'node-invalid-arg-syntax',
    input: `tree Main() {
  MyAction(x = 10);
  OtherAction(y: 20);
}
`,
    mustPreserve: ['tree', 'Main', 'MyAction', 'x', '10', 'OtherAction', 'y', '20'],
    description: 'Node call with = instead of :',
  },

  // Unclosed argument list
  {
    name: 'node-unclosed-args',
    input: `tree Main() {
  MyAction(x: 10, y: 20
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'MyAction', 'x', '10', 'y', '20', 'AlwaysSuccess'],
    description: 'Node call with unclosed argument list',
  },

  // Empty children block
  {
    name: 'node-empty-children-error',
    input: `tree Main() {
  Sequence {
  }
}
`,
    mustPreserve: ['tree', 'Main', 'Sequence'],
    description: 'Control node with empty children block (may be error)',
  },

  // Missing semicolon on leaf node
  {
    name: 'node-leaf-missing-semicolon',
    input: `tree Main() {
  AlwaysSuccess()
  AlwaysFailure();
}
`,
    mustPreserve: ['tree', 'Main', 'AlwaysSuccess', 'AlwaysFailure'],
    description: 'Leaf node missing semicolon',
  },

  // Inline out var with broken syntax
  {
    name: 'node-inline-out-broken',
    input: `tree Main() {
  GetValue(result: out var );
  UseValue(x: result);
}
`,
    mustPreserve: ['tree', 'Main', 'GetValue', 'result', 'out', 'var', 'UseValue', 'x'],
    description: 'Inline out var declaration with missing name',
  },
];

// ============================================================================
// TEST CASES: Unicode and special characters
// ============================================================================

const unicodeTestCases: TestCase[] = [
  // Emoji in string
  {
    name: 'unicode-emoji-in-string',
    input: `tree Main() {
  Print(msg: "Hello 👋 World 🌍"
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'Print', 'msg', 'Hello', '👋', 'World', '🌍', 'AlwaysSuccess'],
    description: 'Emoji in unclosed string',
  },

  // CJK identifiers (if supported)
  {
    name: 'unicode-cjk-broken',
    input: `tree 主要() {
  変数 = 42
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', '主要', '変数', '42', 'AlwaysSuccess'],
    description: 'CJK characters in identifiers with broken statement',
  },

  // Mixed scripts
  {
    name: 'unicode-mixed-scripts',
    input: `tree Main() {
  // Ελληνικά comment
  x = "日本語"
  // Кириллица
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'Ελληνικά', 'x', '日本語', 'Кириллица', 'AlwaysSuccess'],
    description: 'Multiple scripts in comments and strings',
  },

  // Zero-width characters (potential edge case)
  {
    name: 'unicode-zero-width',
    input: `tree Main() {
  x = "test\u200bvalue";
  y = 10
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'test', 'value', 'y', '10'],
    description: 'Zero-width space in string',
  },
];

// ============================================================================
// TEST CASES: Edge cases and stress tests
// ============================================================================

const edgeCaseTestCases: TestCase[] = [
  // Deeply nested broken structure
  {
    name: 'edge-deeply-nested-broken',
    input: `tree Main() {
  Sequence {
    Fallback {
      Sequence {
        Fallback {
          x = broken
        }
      }
    }
  }
}
`,
    mustPreserve: ['tree', 'Main', 'Sequence', 'Fallback', 'x', 'broken'],
    description: 'Deeply nested structure with inner error',
  },

  // Multiple consecutive errors
  {
    name: 'edge-consecutive-errors',
    input: `tree Main() {
  x = 
  y = 
  z = 
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'y', 'z', 'AlwaysSuccess'],
    description: 'Multiple consecutive broken statements',
  },

  // Error at very start
  {
    name: 'edge-error-at-start',
    input: `xyzzy unknown token
tree Main() {
  AlwaysSuccess();
}
`,
    mustPreserve: ['xyzzy', 'unknown', 'token', 'tree', 'Main', 'AlwaysSuccess'],
    description: 'Unknown tokens before first valid declaration',
  },

  // Error at very end
  {
    name: 'edge-error-at-end',
    input: `tree Main() {
  AlwaysSuccess();
}
trailing garbage here
`,
    mustPreserve: ['tree', 'Main', 'AlwaysSuccess', 'trailing', 'garbage', 'here'],
    description: 'Unknown tokens after last valid declaration',
  },

  // Only errors, no valid code
  {
    name: 'edge-only-errors',
    input: `this is not valid bt-dsl code at all
just random tokens here and there
`,
    mustPreserve: [
      'this',
      'is',
      'not',
      'valid',
      'bt-dsl',
      'code',
      'at',
      'all',
      'just',
      'random',
      'tokens',
    ],
    description: 'File with no valid declarations',
  },

  // Empty file
  {
    name: 'edge-empty-file',
    input: '',
    mustPreserve: [],
    description: 'Empty input file',
  },

  // Whitespace only
  {
    name: 'edge-whitespace-only',
    input: `   
  
     
`,
    mustPreserve: [],
    description: 'File with only whitespace',
  },

  // Very long line with error
  {
    name: 'edge-long-line-error',
    input: `tree Main() {
  x = ${'a'.repeat(500)}
  AlwaysSuccess();
}
`,
    mustPreserve: ['tree', 'Main', 'x', 'a', 'AlwaysSuccess'],
    description: 'Very long line with syntax error',
  },
];

// ============================================================================
// RUN ALL TESTS
// ============================================================================

const allTestCases: TestCase[] = [
  ...externTestCases,
  ...treeTestCases,
  ...stmtTestCases,
  ...exprTestCases,
  ...typeTestCases,
  ...globalTestCases,
  ...commentTestCases,
  ...nodeTestCases,
  ...unicodeTestCases,
  ...edgeCaseTestCases,
];

let passed = 0;
let failed = 0;
const failures: { name: string; error: Error }[] = [];

console.log(`Running ${String(allTestCases.length)} malformed input tests...\n`);

for (const tc of allTestCases) {
  try {
    const output = await formatBtDslText(tc.input, { filepath: `${tc.name}.bt` });

    // Critical: non-whitespace must be preserved exactly
    assertNoNonWhitespaceChange(tc.input, output, tc.name);

    // Verify required tokens are preserved
    assertPreserved(output, tc.mustPreserve, tc.name);

    passed++;
    console.log(`✓ ${tc.name}`);
  } catch (error) {
    failed++;
    const err = error instanceof Error ? error : new Error(String(error));
    failures.push({ name: tc.name, error: err });
    console.log(`✗ ${tc.name}: ${err.message}`);
  }
}

console.log(`\n${'='.repeat(60)}`);
console.log(`Results: ${String(passed)} passed, ${String(failed)} failed`);

if (failures.length > 0) {
  console.log('\nFailures:');
  for (const f of failures) {
    console.log(`\n--- ${f.name} ---`);
    console.log(f.error.message);
  }
  process.exit(1);
}

console.log('\ntest-malformed-input: ALL TESTS PASSED');
