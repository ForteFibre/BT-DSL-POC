import { doc, util } from 'prettier';
import type { AstPath, Doc, ParserOptions, Plugin, Printer } from 'prettier';

import { initCoreWasm } from './core-wasm.js';

// Prettier expects `parser.parse()` to be synchronous.
// We therefore initialize the WASM module at module-evaluation time.
const coreWasm = await initCoreWasm();

const { builders } = doc;
const { group, indent, line, softline, hardline, join: joinDocs, ifBreak } = builders;

type PrettierDoc = Doc;

interface Range {
  start: number | null;
  end: number | null;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value != null;
}

function normalizeRange(r: Range | null | undefined, textLength: number): Range {
  const start = r?.start ?? 0;
  const end = r?.end ?? textLength;
  return { start, end };
}

// Core ranges are UTF-8 *byte* offsets. JS strings are UTF-16 code-unit indexed.
// We convert all ranges to JS string indices right after parsing.
function createByteOffsetToIndex(text: string): (byteOffset: number) => number {
  // Fill with 0 so every index is always a number (avoids `number | undefined`).
  const byteAtIndex = new Array<number>(text.length + 1).fill(0);

  let bytes = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);

    // Surrogate pair.
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.length) {
      const d = text.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) {
        // Boundary after the high surrogate (not a valid UTF-8 boundary, but keep monotonic).
        byteAtIndex[i + 1] = bytes;

        const cp = ((c - 0xd800) << 10) + (d - 0xdc00) + 0x10000;
        bytes += cp <= 0x7f ? 1 : cp <= 0x7ff ? 2 : cp <= 0xffff ? 3 : 4;

        i++;
        byteAtIndex[i + 1] = bytes;
        continue;
      }
    }

    bytes += c <= 0x7f ? 1 : c <= 0x7ff ? 2 : 3;
    byteAtIndex[i + 1] = bytes;
  }

  const total = byteAtIndex[text.length];

  return (byteOffset: number): number => {
    if (!Number.isFinite(byteOffset)) return 0;
    if (byteOffset <= 0) return 0;
    if (byteOffset >= (total ?? 0)) return text.length;

    // Lower_bound on byteAtIndex for `byteOffset`.
    let lo = 0;
    let hi = text.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      const v = byteAtIndex[mid] ?? 0;
      if (v < byteOffset) lo = mid + 1;
      else hi = mid;
    }

    const v = byteAtIndex[lo] ?? 0;
    return v === byteOffset ? lo : Math.max(0, lo - 1);
  };
}

function convertRangesDeep(value: unknown, byteToIndex: (b: number) => number): unknown {
  if (Array.isArray(value)) {
    return value.map((x) => convertRangesDeep(x, byteToIndex));
  }

  if (!isRecord(value)) {
    return value;
  }

  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(value)) {
    if (k === 'range' && isRecord(v)) {
      const start = typeof v.start === 'number' ? byteToIndex(v.start) : null;
      const end = typeof v.end === 'number' ? byteToIndex(v.end) : null;
      out[k] = { start, end } satisfies Range;
      continue;
    }

    out[k] = convertRangesDeep(v, byteToIndex);
  }

  return out;
}

interface DiagnosticJson {
  severity: 'Error' | 'Warning' | 'Info' | 'Hint';
  range: Range;
  message?: string;
  code?: string;
}

interface CommentJson {
  kind?: string;
  range: Range;
  text?: string;
}

interface BtDslComment {
  type: 'CommentLine' | 'CommentBlock';
  value: string;
  range: Range;
  /** The original textual form including delimiters (e.g. a line comment or a block comment). */
  raw: string;
  /** Used by Prettier to avoid printing comments that we already preserved verbatim. */
  printed?: boolean;
}

interface ProgramJson {
  type: 'Program';
  range: Range;
  decls: DeclJson[];
  // Prettier attaches/prints comments when this is named `comments`.
  comments: BtDslComment[];
  diagnostics: DiagnosticJson[];
  /** When set, the printer returns the original source for this subtree. */
  verbatim?: boolean;
}

interface BehaviorAttrJson {
  type: 'BehaviorAttr';
  range: Range;
  dataPolicy: string;
  flowPolicy?: string;
}

type DeclJson =
  | { type: 'ImportDecl'; range: Range; path: string }
  | { type: 'ExternTypeDecl'; range: Range; name: string }
  | { type: 'TypeAliasDecl'; range: Range; name: string; aliasedType: TypeJson }
  | {
      type: 'ExternDecl';
      range: Range;
      category: string;
      name: string;
      behaviorAttr?: BehaviorAttrJson;
      ports: ExternPortJson[];
    }
  | {
      type: 'GlobalVarDecl';
      range: Range;
      name: string;
      typeExpr?: TypeJson;
      initialValue?: ExprJson;
    }
  | { type: 'GlobalConstDecl'; range: Range; name: string; typeExpr?: TypeJson; value: ExprJson }
  | { type: 'TreeDecl'; range: Range; name: string; params: ParamDeclJson[]; body: StmtJson[] }
  // fallback (keep discriminated-union narrowing working)
  | { type: 'UnknownDecl'; range: Range; verbatim?: boolean; [k: string]: unknown };

interface ParamDeclJson {
  type: 'ParamDecl';
  range: Range;
  name: string;
  direction?: string;
  typeExpr: TypeJson;
  defaultValue?: ExprJson;
}

interface ExternPortJson {
  type: 'ExternPort';
  range: Range;
  name: string;
  direction?: string;
  typeExpr: TypeJson;
  defaultValue?: ExprJson;
}

interface PreconditionJson {
  type: 'Precondition';
  range: Range;
  kind: string;
  condition: ExprJson;
}

interface ArgumentJson {
  type: 'Argument';
  range: Range;
  name: string;
  direction?: string;
  inlineDecl?: { type: 'InlineBlackboardDecl'; range: Range; name: string };
  valueExpr?: ExprJson;
}

type StmtJson =
  | {
      type: 'BlackboardDeclStmt';
      range: Range;
      name: string;
      typeExpr?: TypeJson;
      initialValue?: ExprJson;
    }
  | { type: 'ConstDeclStmt'; range: Range; name: string; typeExpr?: TypeJson; value: ExprJson }
  | {
      type: 'AssignmentStmt';
      range: Range;
      preconditions: PreconditionJson[];
      target: string;
      indices: ExprJson[];
      op: string;
      value: ExprJson;
    }
  | {
      type: 'NodeStmt';
      range: Range;
      nodeName: string;
      preconditions: PreconditionJson[];
      args: ArgumentJson[];
      hasPropertyBlock: boolean;
      hasChildrenBlock: boolean;
      children: StmtJson[];
    }
  // fallback (keep discriminated-union narrowing working)
  | { type: 'UnknownStmt'; range: Range; verbatim?: boolean; [k: string]: unknown };

type TypeJson =
  | { type: 'TypeExpr'; range: Range; base: TypeJson; nullable: boolean }
  | { type: 'InferType'; range: Range }
  | { type: 'PrimaryType'; range: Range; name: string; size?: string }
  | { type: 'DynamicArrayType'; range: Range; elementType: TypeJson }
  | {
      type: 'StaticArrayType';
      range: Range;
      elementType: TypeJson;
      size: string;
      isBounded: boolean;
    }
  // fallback (keep discriminated-union narrowing working)
  | { type: 'UnknownType'; range: Range; [k: string]: unknown };

type ExprJson =
  | { type: 'MissingExpr'; range: Range }
  | { type: 'IntLiteralExpr'; range: Range; value: number }
  | { type: 'FloatLiteralExpr'; range: Range; value: number }
  | { type: 'StringLiteralExpr'; range: Range; value: string }
  | { type: 'BoolLiteralExpr'; range: Range; value: boolean }
  | { type: 'NullLiteralExpr'; range: Range }
  | { type: 'VarRefExpr'; range: Range; name: string }
  | { type: 'UnaryExpr'; range: Range; op: string; operand: ExprJson }
  | { type: 'BinaryExpr'; range: Range; op: string; lhs: ExprJson; rhs: ExprJson }
  | { type: 'IndexExpr'; range: Range; base: ExprJson; index: ExprJson }
  | { type: 'CastExpr'; range: Range; expr: ExprJson; targetType: TypeJson }
  | { type: 'ArrayLiteralExpr'; range: Range; elements: ExprJson[] }
  | { type: 'ArrayRepeatExpr'; range: Range; value: ExprJson; count: ExprJson }
  | { type: 'VecMacroExpr'; range: Range; inner: ExprJson }
  // fallback (keep discriminated-union narrowing working)
  | { type: 'UnknownExpr'; range: Range; verbatim?: boolean; [k: string]: unknown };

function rs(r: Range): number {
  return r.start ?? 0;
}

function re(r: Range): number {
  return r.end ?? 0;
}

function intersects(a: Range, b: Range): boolean {
  if (a.start == null || a.end == null || b.start == null || b.end == null) return false;

  // Core diagnostics sometimes use a point-range (start === end) to mark an error position.
  // Treat such ranges as spanning a single character for intersection purposes.
  const aStart = a.start;
  const bStart = b.start;
  const aEnd = a.end === a.start ? a.end + 1 : a.end;
  const bEnd = b.end === b.start ? b.end + 1 : b.end;

  // Be slightly inclusive at boundaries so that errors reported *at* the end of a decl
  // (e.g. missing ';' or missing '}') still cause the decl to be preserved.
  return aStart <= bEnd && bStart <= aEnd;
}

function hasErrorInside(range: Range, diags: DiagnosticJson[]): boolean {
  return diags.some((d) => d.severity === 'Error' && intersects(range, d.range));
}

function containsUnknownOrMissing(value: unknown): boolean {
  if (Array.isArray(value)) {
    return value.some((v) => containsUnknownOrMissing(v));
  }

  if (typeof value !== 'object' || value == null) {
    return false;
  }

  const obj = value as Record<string, unknown>;
  const t = obj.type;
  if (typeof t === 'string') {
    if (t.startsWith('Unknown') || t === 'MissingExpr') {
      return true;
    }
  }

  for (const v of Object.values(obj)) {
    if (containsUnknownOrMissing(v)) return true;
  }
  return false;
}

function hasNullRangeDeep(value: unknown): boolean {
  if (Array.isArray(value)) {
    return value.some((v) => hasNullRangeDeep(v));
  }

  if (typeof value !== 'object' || value == null) {
    return false;
  }

  const obj = value as Record<string, unknown>;
  const r = obj.range;
  if (typeof r === 'object' && r != null) {
    const rr = r as Range;
    if (rr.start == null || rr.end == null) return true;
  }

  for (const v of Object.values(obj)) {
    if (hasNullRangeDeep(v)) return true;
  }

  return false;
}

function toBtDslComment(c: CommentJson): BtDslComment {
  const raw = c.text ?? '';
  if (raw.startsWith('//')) {
    return { type: 'CommentLine', value: raw.slice(2), raw, range: c.range };
  }
  if (raw.startsWith('/*')) {
    const inner = raw.endsWith('*/') ? raw.slice(2, -2) : raw.slice(2);
    return { type: 'CommentBlock', value: inner, raw, range: c.range };
  }
  // Fallback: treat unknown comment kinds as line-like.
  return { type: 'CommentLine', value: raw, raw, range: c.range };
}

function attachCommentToExternPortIfInside(
  program: ProgramJson,
  comment: BtDslComment,
  text: string,
): boolean {
  const cs = comment.range.start;
  const ce = comment.range.end;
  if (cs == null || ce == null) return false;

  // Heuristic: if this comment is immediately followed by a closing paren, it's very likely
  // intended to live at the end of an extern argument list.
  const next = util.getNextNonSpaceNonCommentCharacter(text, ce);
  if (next !== ')') return false;

  for (const decl of program.decls) {
    if (decl.type !== 'ExternDecl') continue;
    if (decl.range.start == null || decl.range.end == null) continue;
    if (cs < decl.range.start || cs > decl.range.end) continue;

    for (const port of decl.ports) {
      if (port.range.start == null || port.range.end == null) continue;
      if (cs >= port.range.start && cs <= port.range.end) {
        // Prettier's util API is intentionally loose; keep the cast local.
        type AddTrailingCommentArgs = Parameters<typeof util.addTrailingComment>;
        util.addTrailingComment(
          port as AddTrailingCommentArgs[0],
          comment as AddTrailingCommentArgs[1],
        );
        return true;
      }
    }
  }

  return false;
}

function hasTrailingCommentOnSameLine(text: string, offset: number): boolean {
  // Diagnostics for missing semicolons often point at the *next* token (possibly on the next line).
  // We therefore scan backwards from `offset` to the previous non-whitespace character and inspect
  // that character's line.
  let i = Math.min(Math.max(0, offset - 1), Math.max(0, text.length - 1));
  while (i > 0 && /\s/.test(text[i] ?? '')) i--;

  const lineStart = text.lastIndexOf('\n', i) + 1;
  const lineEnd = text.indexOf('\n', lineStart);
  const end = lineEnd === -1 ? text.length : lineEnd;
  const lineText = text.slice(lineStart, end);
  return lineText.includes('//') || lineText.includes('/*');
}

function isRecoverableMissingSemicolonError(d: DiagnosticJson, text: string): boolean {
  if (d.severity !== 'Error') return false;
  const msg = d.message ?? '';
  const isMissingSemicolon =
    msg === "expected ';' after import" ||
    msg === "expected ';' after global var" ||
    msg === "expected ';' after global const";
  if (!isMissingSemicolon) return false;

  // Prefer the start offset (often points at the next declaration's first token).
  // We then scan backwards to the line that actually missed the semicolon.
  const anchor = d.range.start ?? d.range.end ?? 0;
  return hasTrailingCommentOnSameLine(text, anchor);
}

function markVerbatimFlags(program: ProgramJson, text: string): void {
  const diags = program.diagnostics;

  const errors = diags.filter((d) => d.severity === 'Error');
  const hasErrors = errors.length > 0;

  // Generally, when the core parser reports an error, we fall back to fully lossless output.
  // Exception: a missing semicolon *before a trailing comment on the same line* is easy to
  // fix without risking token loss, and is required by the trailing-comment tests.
  if (hasErrors) {
    const allRecoverable = errors.every((d) => isRecoverableMissingSemicolonError(d, text));
    if (!allRecoverable) {
      program.verbatim = true;
      program.comments = [];
      for (const decl of program.decls) {
        (decl as { verbatim?: boolean }).verbatim = true;
      }
      return;
    }
  }

  // If the core returns null ranges for any nodes/comments, Prettier cannot reliably attach
  // comments. Prefer a safe lossless output instead of throwing.
  if (hasNullRangeDeep(program)) {
    program.verbatim = true;
    program.comments = [];
    for (const d of program.decls) {
      (d as { verbatim?: boolean }).verbatim = true;
    }
    return;
  }

  const markStmt = (s: StmtJson): void => {
    const verbatim = hasErrorInside(s.range, diags) || containsUnknownOrMissing(s);
    if (verbatim) {
      (s as { verbatim?: boolean }).verbatim = true;
      return;
    }

    if (s.type === 'NodeStmt') {
      for (const child of s.children) {
        markStmt(child);
      }
    }
  };

  for (const d of program.decls) {
    const verbatim = hasErrorInside(d.range, diags) || containsUnknownOrMissing(d);
    if (verbatim) {
      (d as { verbatim?: boolean }).verbatim = true;
      continue;
    }

    if (d.type === 'TreeDecl') {
      for (const s of d.body) {
        markStmt(s);
      }
    }
  }

  // If we will print a node verbatim, avoid double-printing comments that fall inside it.
  const verbatimRanges: Range[] = [];
  for (const d of program.decls) {
    if ((d as { verbatim?: boolean }).verbatim) verbatimRanges.push(d.range);
    if (d.type === 'TreeDecl') {
      const collectStmt = (s: StmtJson): void => {
        if ((s as { verbatim?: boolean }).verbatim) verbatimRanges.push(s.range);
        if (s.type === 'NodeStmt') for (const c of s.children) collectStmt(c);
      };
      for (const s of d.body) collectStmt(s);
    }
  }

  for (const c of program.comments) {
    const cs = c.range.start;
    const ce = c.range.end;
    if (cs == null || ce == null) continue;
    const insideVerbatim = verbatimRanges.some((r) => {
      if (r.start == null || r.end == null) return false;
      return cs >= r.start && ce <= r.end;
    });
    // Note: Prettier resets `comment.printed` internally, so this is only a best-effort hint.
    // We keep it for tooling/debugging, but do not rely on it for correctness.
    if (insideVerbatim) c.printed = true;
  }
}

function escapeJsonString(s: string): string {
  return JSON.stringify(s);
}

function precedence(e: ExprJson): number {
  if (e.type !== 'BinaryExpr') {
    if (e.type === 'UnaryExpr') return 10;
    if (e.type === 'CastExpr') return 11;
    if (e.type === 'IndexExpr') return 12;
    return 13;
  }

  switch (e.op) {
    case '||':
      return 1;
    case '&&':
      return 2;
    case '|':
      return 3;
    case '^':
      return 4;
    case '&':
      return 5;
    case '==':
    case '!=':
      return 6;
    case '<':
    case '<=':
    case '>':
    case '>=':
      return 7;
    case '+':
    case '-':
      return 8;
    case '*':
    case '/':
    case '%':
      return 9;
    default:
      return 8;
  }
}

type BtDslAnyNode =
  | ProgramJson
  | DeclJson
  | StmtJson
  | ExprJson
  | TypeJson
  | ParamDeclJson
  | ExternPortJson
  | PreconditionJson
  | ArgumentJson
  | BtDslComment
  | null
  | undefined;

type PrettierPrintFn = (path: AstPath<BtDslAnyNode>) => Doc;

function isRange(value: unknown): value is Range {
  if (!isRecord(value)) return false;
  return (
    (value.start === null || typeof value.start === 'number') &&
    (value.end === null || typeof value.end === 'number')
  );
}

function isBtDslComment(value: unknown): value is BtDslComment {
  if (!isRecord(value)) return false;
  const t = value.type;
  return (
    (t === 'CommentLine' || t === 'CommentBlock') &&
    typeof value.raw === 'string' &&
    typeof value.value === 'string' &&
    isRange(value.range)
  );
}

function isProgramJson(value: unknown): value is ProgramJson {
  return isRecord(value) && value.type === 'Program' && isRange(value.range);
}

function printVerbatim(node: { range: Range; verbatim?: boolean }, text: string): PrettierDoc {
  if (!node.verbatim) return '';
  const s = rs(node.range);
  const e = re(node.range);
  return text.slice(s, e).trimEnd();
}

function printTypePath(
  path: AstPath<TypeJson>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const t = path.node;
  switch (t.type) {
    case 'TypeExpr':
      return group([
        path.call((p) => printTypePath(p as AstPath<TypeJson>, options, print), 'base'),
        t.nullable ? '?' : '',
      ]);
    case 'InferType':
      return '_';
    case 'PrimaryType':
      return t.size ? `${t.name}<${t.size}>` : t.name;
    case 'DynamicArrayType':
      return group([
        'vec<',
        path.call((p) => printTypePath(p as AstPath<TypeJson>, options, print), 'elementType'),
        '>',
      ]);
    case 'StaticArrayType':
      return group([
        '[',
        path.call((p) => printTypePath(p as AstPath<TypeJson>, options, print), 'elementType'),
        '; ',
        t.isBounded ? '<=' : '',
        t.size,
        ']',
      ]);
    default:
      return '<unknown_type>';
  }
}

function printExprPath(
  path: AstPath<ExprJson>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
  parentPrec = 0,
): PrettierDoc {
  const e = path.node;
  if ((e as { verbatim?: boolean }).verbatim) {
    return printVerbatim(e as { range: Range; verbatim?: boolean }, options.originalText);
  }

  const selfPrec = precedence(e);
  const needsParens = selfPrec < parentPrec;

  const inner: PrettierDoc = (() => {
    switch (e.type) {
      case 'MissingExpr':
        return '<missing_expr>';
      case 'IntLiteralExpr':
        return String(e.value);
      case 'FloatLiteralExpr':
        return String(e.value);
      case 'StringLiteralExpr':
        return escapeJsonString(e.value);
      case 'BoolLiteralExpr':
        return e.value ? 'true' : 'false';
      case 'NullLiteralExpr':
        return 'null';
      case 'VarRefExpr':
        return e.name;
      case 'UnaryExpr':
        return group([
          e.op,
          path.call(
            (p) => printExprPath(p as AstPath<ExprJson>, options, print, selfPrec),
            'operand',
          ),
        ]);
      case 'BinaryExpr':
        return group([
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, selfPrec), 'lhs'),
          ' ',
          e.op,
          ' ',
          // left-associative: rhs uses +1
          path.call(
            (p) => printExprPath(p as AstPath<ExprJson>, options, print, selfPrec + 1),
            'rhs',
          ),
        ]);
      case 'IndexExpr':
        return group([
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, selfPrec), 'base'),
          '[',
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, 0), 'index'),
          ']',
        ]);
      case 'CastExpr':
        return group([
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, selfPrec), 'expr'),
          ' as ',
          path.call((p) => printTypePath(p as AstPath<TypeJson>, options, print), 'targetType'),
        ]);
      case 'ArrayLiteralExpr': {
        const elems = path.map(
          (p) => printExprPath(p as AstPath<ExprJson>, options, print, 0),
          'elements',
        );
        return group(['[', joinDocs(', ', elems), ']']);
      }
      case 'ArrayRepeatExpr':
        return group([
          '[',
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, 0), 'value'),
          '; ',
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, 0), 'count'),
          ']',
        ]);
      case 'VecMacroExpr':
        return group([
          'vec!',
          path.call((p) => printExprPath(p as AstPath<ExprJson>, options, print, 0), 'inner'),
        ]);
      default:
        return '<unknown_expr>';
    }
  })();

  return needsParens ? group(['(', inner, ')']) : inner;
}

// Helper: Check if there's a blank line after the node in the original source
function hasBlankLineAfter(text: string, node: { range: Range }): boolean {
  const endPos = node.range.end ?? 0;
  // Find the end of the current line
  let pos = endPos;
  while (pos < text.length && text[pos] !== '\n') {
    pos++;
  }
  if (pos >= text.length) return false;
  pos++; // Skip the newline

  // Skip whitespace on the next line
  while (pos < text.length && (text[pos] === ' ' || text[pos] === '\t')) {
    pos++;
  }

  // Check if the next line is empty (i.e., another newline)
  return pos < text.length && text[pos] === '\n';
}

function printStmtBlockPath(
  path: AstPath<{ range: Range; body?: StmtJson[]; children?: StmtJson[] }>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
  prop: 'body' | 'children',
): PrettierDoc {
  const list = path.node[prop];
  if (!list || list.length === 0) {
    return group(['{', '}']);
  }

  // Build docs with preserved blank lines
  const docs: PrettierDoc[] = [];
  path.each((childPath: AstPath<unknown>, index: number) => {
    const printed = print(childPath as unknown as AstPath<BtDslAnyNode>);
    docs.push(printed);

    // Add extra hardline if there was a blank line after this statement in the original source
    const childNode = childPath.node as { range: Range };
    if (index < list.length - 1 && hasBlankLineAfter(options.originalText, childNode)) {
      docs.push(hardline, hardline);
    } else if (index < list.length - 1) {
      docs.push(hardline);
    }
  }, prop);

  // Force expansion when block has content
  return group(['{', indent([hardline, docs]), hardline, '}'], {
    shouldBreak: true,
  });
}

type PrinterFn = (
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
) => PrettierDoc;

// Individual printer functions for each node type
function printProgram(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as ProgramJson;
  const decls = node.decls;

  if (decls.length === 0) {
    return '';
  }

  // Build docs with preserved blank lines
  const docs: PrettierDoc[] = [];
  path.each((childPath: AstPath<unknown>, index: number) => {
    const printed = print(childPath as unknown as AstPath<BtDslAnyNode>);
    docs.push(printed);

    // Add extra hardline if there was a blank line after this declaration in the original source
    const childNode = childPath.node as { range: Range };
    if (index < decls.length - 1 && hasBlankLineAfter(options.originalText, childNode)) {
      docs.push(hardline, hardline);
    } else if (index < decls.length - 1) {
      docs.push(hardline);
    }
  }, 'decls');

  return group(docs);
}

function printImportDecl(
  path: AstPath<BtDslAnyNode>,
  _options: ParserOptions<ProgramJson>,
  _print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'ImportDecl' }>;
  return group(['import ', escapeJsonString(node.path), ';']);
}

function printExternTypeDecl(
  path: AstPath<BtDslAnyNode>,
  _options: ParserOptions<ProgramJson>,
  _print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'ExternTypeDecl' }>;
  return group(['extern type ', node.name, ';']);
}

function printTypeAliasDecl(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'TypeAliasDecl' }>;
  return group([
    'type ',
    node.name,
    ' = ',
    path.call(
      (p) => printTypePath(p as unknown as AstPath<TypeJson>, options, print),
      'aliasedType',
    ),
    ';',
  ]);
}

function printGlobalVarDecl(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'GlobalVarDecl' }>;
  const parts: PrettierDoc[] = ['var ', node.name];
  if (node.typeExpr)
    parts.push(
      ': ',
      path.call(
        (p) => printTypePath(p as unknown as AstPath<TypeJson>, options, print),
        'typeExpr',
      ),
    );
  if (node.initialValue)
    parts.push(
      ' = ',
      path.call(
        (p) => printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0),
        'initialValue',
      ),
    );
  parts.push(';');
  return group(parts);
}

function printGlobalConstDecl(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'GlobalConstDecl' }>;
  const parts: PrettierDoc[] = ['const ', node.name];
  if (node.typeExpr)
    parts.push(
      ': ',
      path.call(
        (p) => printTypePath(p as unknown as AstPath<TypeJson>, options, print),
        'typeExpr',
      ),
    );
  parts.push(
    ' = ',
    path.call((p) => printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0), 'value'),
    ';',
  );
  return group(parts);
}

function printExternDecl(
  path: AstPath<BtDslAnyNode>,
  _options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'ExternDecl' }>;
  const parts: PrettierDoc[] = [];
  if (node.behaviorAttr) {
    parts.push(
      path.call((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'behaviorAttr'),
      hardline,
    );
  }
  const header = group(['extern ', node.category, ' ', node.name]);
  const ports = Array.isArray(node.ports)
    ? path.map((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'ports')
    : [];
  const portList = printParamLikeList(ports);
  parts.push(group([header, portList, ';']));
  return group(parts);
}

function printTreeDecl(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<DeclJson, { type: 'TreeDecl' }>;
  const params = Array.isArray(node.params)
    ? path.map((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'params')
    : [];
  const header = group(['tree ', node.name, printParamLikeList(params), ' ']);
  const block = printStmtBlockPath(
    path as unknown as AstPath<{ range: Range; body: StmtJson[] }>,
    options,
    print,
    'body',
  );
  return group([header, block]);
}

function printBehaviorAttr(
  path: AstPath<BtDslAnyNode>,
  _options: ParserOptions<ProgramJson>,
  _print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as unknown as BehaviorAttrJson;
  return group([
    '#[behavior(',
    node.dataPolicy,
    node.flowPolicy ? [', ', node.flowPolicy] : '',
    ')]',
  ]);
}

function printExternPort(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as ExternPortJson;
  const seg: PrettierDoc[] = [];
  if (node.direction) seg.push(node.direction, ' ');
  seg.push(
    node.name,
    ': ',
    path.call(
      (pp) => printTypePath(pp as unknown as AstPath<TypeJson>, options, print),
      'typeExpr',
    ),
  );
  if (node.defaultValue)
    seg.push(
      ' = ',
      path.call(
        (pp) => printExprPath(pp as unknown as AstPath<ExprJson>, options, print, 0),
        'defaultValue',
      ),
    );
  return group(seg);
}

function printParamDecl(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as ParamDeclJson;
  const seg: PrettierDoc[] = [];
  if (node.direction) seg.push(node.direction, ' ');
  seg.push(
    node.name,
    ': ',
    path.call(
      (pp) => printTypePath(pp as unknown as AstPath<TypeJson>, options, print),
      'typeExpr',
    ),
  );
  if (node.defaultValue)
    seg.push(
      ' = ',
      path.call(
        (pp) => printExprPath(pp as unknown as AstPath<ExprJson>, options, print, 0),
        'defaultValue',
      ),
    );
  return group(seg);
}

function printPrecondition(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as PreconditionJson;
  return group([
    '@',
    node.kind,
    '(',
    path.call(
      (pp) => printExprPath(pp as unknown as AstPath<ExprJson>, options, print, 0),
      'condition',
    ),
    ')',
  ]);
}

function printArgument(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as ArgumentJson;
  const parts: PrettierDoc[] = [node.name, ': '];
  if (node.direction) parts.push(node.direction, ' ');
  if (node.inlineDecl) {
    parts.push('out var ', node.inlineDecl.name);
    return group(parts);
  }
  if (node.valueExpr) {
    parts.push(
      path.call(
        (pp) => printExprPath(pp as unknown as AstPath<ExprJson>, options, print, 0),
        'valueExpr',
      ),
    );
  } else {
    parts.push('<missing_expr>');
  }
  return group(parts);
}

function printBlackboardDeclStmt(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<StmtJson, { type: 'BlackboardDeclStmt' }>;
  const parts: PrettierDoc[] = ['var ', node.name];
  if (node.typeExpr)
    parts.push(
      ': ',
      path.call(
        (p) => printTypePath(p as unknown as AstPath<TypeJson>, options, print),
        'typeExpr',
      ),
    );
  if (node.initialValue)
    parts.push(
      ' = ',
      path.call(
        (p) => printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0),
        'initialValue',
      ),
    );
  parts.push(';');
  return group(parts);
}

function printConstDeclStmt(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<StmtJson, { type: 'ConstDeclStmt' }>;
  const parts: PrettierDoc[] = ['const ', node.name];
  if (node.typeExpr)
    parts.push(
      ': ',
      path.call(
        (p) => printTypePath(p as unknown as AstPath<TypeJson>, options, print),
        'typeExpr',
      ),
    );
  parts.push(
    ' = ',
    path.call((p) => printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0), 'value'),
    ';',
  );
  return group(parts);
}

function printAssignmentStmt(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<StmtJson, { type: 'AssignmentStmt' }>;
  const lines: PrettierDoc[] = [];
  const precs = Array.isArray(node.preconditions)
    ? path.map((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'preconditions')
    : [];
  for (const pd of precs) {
    lines.push(pd, hardline);
  }
  const idx = Array.isArray(node.indices)
    ? path.map(
        (p) => ['[', printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0), ']'],
        'indices',
      )
    : [];
  lines.push(
    group([
      node.target,
      ...idx.flat(),
      ' ',
      node.op,
      ' ',
      path.call(
        (p) => printExprPath(p as unknown as AstPath<ExprJson>, options, print, 0),
        'value',
      ),
      ';',
    ]),
  );
  return group(lines);
}

function printNodeStmt(
  path: AstPath<BtDslAnyNode>,
  options: ParserOptions<ProgramJson>,
  print: PrettierPrintFn,
): PrettierDoc {
  const node = path.node as Extract<StmtJson, { type: 'NodeStmt' }>;
  const lines: PrettierDoc[] = [];
  const precs = Array.isArray(node.preconditions)
    ? path.map((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'preconditions')
    : [];
  for (const pd of precs) {
    lines.push(pd, hardline);
  }

  const head: PrettierDoc[] = [node.nodeName];
  if (node.hasPropertyBlock) {
    const args = Array.isArray(node.args)
      ? path.map((p) => print(p as unknown as AstPath<BtDslAnyNode>), 'args')
      : [];
    head.push(printParamLikeList(args));
  }

  if (!node.hasChildrenBlock) {
    head.push(';');
    lines.push(group(head));
    return group(lines);
  }

  const block = printStmtBlockPath(
    path as unknown as AstPath<{ range: Range; children: StmtJson[] }>,
    options,
    print,
    'children',
  );
  lines.push(group([...head, ' ', block]));
  return group(lines);
}

// Printer map for all node types
const nodePrinters: Record<string, PrinterFn> = {
  Program: printProgram,
  ImportDecl: printImportDecl,
  ExternTypeDecl: printExternTypeDecl,
  TypeAliasDecl: printTypeAliasDecl,
  GlobalVarDecl: printGlobalVarDecl,
  GlobalConstDecl: printGlobalConstDecl,
  ExternDecl: printExternDecl,
  TreeDecl: printTreeDecl,
  BehaviorAttr: printBehaviorAttr,
  ExternPort: printExternPort,
  ParamDecl: printParamDecl,
  Precondition: printPrecondition,
  Argument: printArgument,
  BlackboardDeclStmt: printBlackboardDeclStmt,
  ConstDeclStmt: printConstDeclStmt,
  AssignmentStmt: printAssignmentStmt,
  NodeStmt: printNodeStmt,
  // Expression types - delegate to printExprPath
  MissingExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  IntLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  FloatLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  StringLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  BoolLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  NullLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  VarRefExpr: (path, options, print) => printExprPath(path as AstPath<ExprJson>, options, print, 0),
  UnaryExpr: (path, options, print) => printExprPath(path as AstPath<ExprJson>, options, print, 0),
  BinaryExpr: (path, options, print) => printExprPath(path as AstPath<ExprJson>, options, print, 0),
  IndexExpr: (path, options, print) => printExprPath(path as AstPath<ExprJson>, options, print, 0),
  CastExpr: (path, options, print) => printExprPath(path as AstPath<ExprJson>, options, print, 0),
  ArrayLiteralExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  ArrayRepeatExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  VecMacroExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  UnknownExpr: (path, options, print) =>
    printExprPath(path as AstPath<ExprJson>, options, print, 0),
  // Type types - delegate to printTypePath
  TypeExpr: (path, options, print) => printTypePath(path as AstPath<TypeJson>, options, print),
  InferType: (path, options, print) => printTypePath(path as AstPath<TypeJson>, options, print),
  PrimaryType: (path, options, print) => printTypePath(path as AstPath<TypeJson>, options, print),
  DynamicArrayType: (path, options, print) =>
    printTypePath(path as AstPath<TypeJson>, options, print),
  StaticArrayType: (path, options, print) =>
    printTypePath(path as AstPath<TypeJson>, options, print),
  UnknownType: (path, options, print) => printTypePath(path as AstPath<TypeJson>, options, print),
};

function printParamLikeList(items: PrettierDoc[]): PrettierDoc {
  if (items.length === 0) return group(['(', ')']);
  return group([
    '(',
    indent([softline, joinDocs([',', line], items), ifBreak(',', '')]),
    softline,
    ')',
  ]);
}

const btDslPrettierPlugin: Plugin<ProgramJson> = {
  languages: [
    {
      name: 'BT DSL',
      parsers: ['bt-dsl'],
      extensions: ['.bt'],
      vscodeLanguageIds: ['bt-dsl'],
    },
  ],
  parsers: {
    'bt-dsl': {
      astFormat: 'bt-dsl-core-ast',
      parse: (text: string): ProgramJson => {
        try {
          const jsonText = coreWasm.parseToAstJson(text);
          const rawBytes = JSON.parse(jsonText) as {
            type?: unknown;
            range?: unknown;
            decls?: unknown;
            comments?: unknown;
            btDslComments?: unknown;
            diagnostics?: unknown;
            [k: string]: unknown;
          };

          const byteToIndex = createByteOffsetToIndex(text);
          const rawUnknown = convertRangesDeep(rawBytes, byteToIndex);
          const raw = isRecord(rawUnknown) ? rawUnknown : ({} as Record<string, unknown>);

          const program: ProgramJson = {
            type: 'Program',
            range: normalizeRange(raw.range as Range | null | undefined, text.length),
            decls: Array.isArray(raw.decls) ? (raw.decls as unknown as DeclJson[]) : [],
            comments: Array.isArray(raw.btDslComments)
              ? (raw.btDslComments as unknown as CommentJson[]).map(toBtDslComment)
              : Array.isArray(raw.comments)
                ? (raw.comments as unknown as CommentJson[]).map(toBtDslComment)
                : [],
            diagnostics: Array.isArray(raw.diagnostics)
              ? (raw.diagnostics as unknown as DiagnosticJson[])
              : [],
          };

          markVerbatimFlags(program, text);
          return program;
        } catch (e) {
          // If the core parser crashes (e.g. on unsupported unicode identifiers),
          // fall back to a "lossless" AST that preserves the original text.
          const msg = e instanceof Error ? e.message : String(e);
          const all: Range = { start: 0, end: text.length };
          const unknownDecl: DeclJson = { type: 'UnknownDecl', range: all, verbatim: true };
          const diag: DiagnosticJson = {
            severity: 'Error',
            range: all,
            message: `Core parser failed: ${msg}`,
          };

          return {
            type: 'Program',
            range: all,
            decls: [unknownDecl],
            comments: [],
            diagnostics: [diag],
            verbatim: true,
          };
        }
      },
      locStart: (node: { range: Range }): number => rs(node.range),
      locEnd: (node: { range: Range }): number => re(node.range),
    },
  },
  printers: {
    'bt-dsl-core-ast': {
      // Prettier's public type definitions model `print` as if it only ever sees the root AST.
      // In practice it prints every node kind, so we intentionally loosen types here.
      print: (path: AstPath<unknown>, options: unknown, print: unknown): Doc => {
        const node = path.node;
        if (node == null) return '';

        const typedOptions = options as ParserOptions<ProgramJson> & { originalText: string };
        const typedPrint = print as PrettierPrintFn;

        // Comments are printed by Prettier via `printComment`.
        if (isBtDslComment(node)) {
          return node.raw;
        }

        // Verbatim nodes (with errors)
        if (isRecord(node) && node.verbatim === true && isRange(node.range)) {
          return printVerbatim({ range: node.range, verbatim: true }, typedOptions.originalText);
        }

        const nodeType = isRecord(node) && typeof node.type === 'string' ? node.type : null;
        if (!nodeType) return '';

        // Use printer map
        const printer = nodePrinters[nodeType];
        if (printer) {
          return printer(path as unknown as AstPath<BtDslAnyNode>, typedOptions, typedPrint);
        }

        // Fallback for unknown node types
        return '';
      },
      printComment: (commentPath: AstPath<unknown>): Doc => {
        const c = commentPath.node;
        return isBtDslComment(c) ? c.raw : '';
      },
      handleComments: {
        ownLine: (comment: unknown, text: string, _options: unknown, ast: unknown): boolean => {
          // Most comments can be handled by Prettier's default attachment.
          // This one is a known edge case: a comment at the end of an extern argument list
          // (right before ')') can remain unattached and trip "Comment was not printed".
          if (isProgramJson(ast) && isBtDslComment(comment)) {
            return attachCommentToExternPortIfInside(ast, comment, text);
          }
          return false;
        },
        endOfLine: (comment: unknown, text: string, _options: unknown, ast: unknown): boolean => {
          if (isProgramJson(ast) && isBtDslComment(comment)) {
            return attachCommentToExternPortIfInside(ast, comment, text);
          }
          return false;
        },
        remaining: (comment: unknown, text: string, _options: unknown, ast: unknown): boolean => {
          if (isProgramJson(ast) && isBtDslComment(comment)) {
            return attachCommentToExternPortIfInside(ast, comment, text);
          }
          return false;
        },
      },
      canAttachComment: (node: unknown): boolean => {
        if (!isRecord(node)) return false;
        const t = node.type;
        if (t === 'CommentLine' || t === 'CommentBlock') return false;
        return 'range' in node;
      },
      isBlockComment: (node: unknown): boolean =>
        isBtDslComment(node) && node.type === 'CommentBlock',
      getCommentChildNodes: (node: unknown) => {
        if (!isRecord(node)) return [];
        if (isProgramJson(node)) return node.decls;
        const t = node.type;

        // Let Prettier handle most node types automatically through path.map/path.call
        // Only handle critical cases that require explicit traversal
        if (t === 'TreeDecl') {
          const d = node as { params?: unknown[]; body?: unknown[] };
          return [...(d.params ?? []), ...(d.body ?? [])];
        }
        if (t === 'NodeStmt') {
          const n = node as { preconditions?: unknown[]; args?: unknown[]; children?: unknown[] };
          return [...(n.preconditions ?? []), ...(n.args ?? []), ...(n.children ?? [])];
        }
        if (t === 'ExternDecl') {
          const e = node as { behaviorAttr?: unknown; ports?: unknown[] };
          const parts = [...(e.ports ?? [])];
          if (e.behaviorAttr) parts.unshift(e.behaviorAttr);
          return parts;
        }

        // For other nodes, Prettier will automatically traverse through path.map/path.call
        return [];
      },
    } as unknown as Printer<ProgramJson>,
  },
};

export default btDslPrettierPlugin;
