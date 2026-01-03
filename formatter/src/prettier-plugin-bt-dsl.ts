import { doc } from 'prettier';
import type { AstPath, Doc, ParserOptions, Plugin } from 'prettier';
import Parser from 'web-tree-sitter';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const { builders } = doc;
const { group, indent, line, softline, hardline, join: joinDocs } = builders;

type PrettierDoc = Doc;

// Tree-sitter node type (our Prettier AST)
type BtDslNode = Parser.SyntaxNode;

type PrintFn = (path: AstPath<BtDslNode>) => Doc;

let parser: Parser | null = null;
let btDslLanguage: Parser.Language | null = null;
let configuredWasmPath: string | null = null;

/**
 * Configure the WASM path for environments where import.meta.url is not available.
 * Call this before formatBtDslText if running in CJS context.
 */
export function setTreeSitterWasmPath(wasmPath: string): void {
  configuredWasmPath = wasmPath;
}

/**
 * Initialize web-tree-sitter parser with BT DSL grammar
 */
async function initParser(): Promise<Parser> {
  if (parser && btDslLanguage) {
    return parser;
  }

  await Parser.init();
  parser = new Parser();

  // Use configured path if available, otherwise try to derive from import.meta.url
  let wasmPath: string;
  if (configuredWasmPath) {
    wasmPath = configuredWasmPath;
  } else {
    // Load WASM file from dist directory (we're in dist/src/)
    const __filename = fileURLToPath(import.meta.url);
    const __dirname = dirname(__filename);
    wasmPath = join(__dirname, '..', 'tree-sitter-bt_dsl.wasm');
  }

  btDslLanguage = await Parser.Language.load(wasmPath);
  parser.setLanguage(btDslLanguage);

  return parser;
}

/**
 * Concatenate parts into a single doc
 */
const concat = (parts: PrettierDoc[]): PrettierDoc => parts;

/**
 * Get text content of a node from source
 */
function getText(node: BtDslNode, text: string): string {
  return text.slice(node.startIndex, node.endIndex);
}

/**
 * Check if the original node text ends with a semicolon
 */
function hasTrailingSemicolon(node: BtDslNode, text: string): boolean {
  const nodeText = getText(node, text);
  return nodeText.trimEnd().endsWith(';');
}

/**
 * Get child node by field name
 */
function getChild(node: BtDslNode, fieldName: string): BtDslNode | null {
  return node.childForFieldName(fieldName);
}

/**
 * Get all named children of a node
 */
function getAllNamedChildren(node: BtDslNode): BtDslNode[] {
  const children: BtDslNode[] = [];
  for (let i = 0; i < node.namedChildCount; i++) {
    const child = node.namedChild(i);
    if (child) {
      children.push(child);
    }
  }
  return children;
}

/**
 * Collect trailing comments that were absorbed into a node due to missing semicolons.
 * Returns the comment text nodes that appear after the actual content.
 */
function collectTrailingComments(node: BtDslNode, text: string): PrettierDoc[] {
  const comments: PrettierDoc[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'comment' || child.type === 'line_comment' || child.type === 'block_comment') {
      comments.push(getText(child, text));
    }
  }
  return comments;
}

/**
 * Check if there's a blank line between two nodes
 */
function hasBlankLineBetween(nodeA: BtDslNode, nodeB: BtDslNode): boolean {
  const endLine = nodeA.endPosition.row;
  const startLine = nodeB.startPosition.row;
  return startLine - endLine > 1;
}

/**
 * Join docs with preserved blank lines
 */
function joinWithPreservedBlankLines(docs: PrettierDoc[], nodes: BtDslNode[]): PrettierDoc {
  if (docs.length === 0) return '';
  if (docs.length === 1) return docs[0] ?? '';

  const first = docs[0];
  if (first === undefined) {
    return '';
  }

  const parts: PrettierDoc[] = [first];
  for (let i = 1; i < docs.length; i++) {
    const docPart = docs[i];
    if (docPart === undefined) continue;

    const prevNode = nodes[i - 1];
    const nextNode = nodes[i];

    // With `noUncheckedIndexedAccess`, index access can be undefined.
    if (!prevNode || !nextNode) {
      parts.push(hardline, docPart);
      continue;
    }

    if (hasBlankLineBetween(prevNode, nextNode)) {
      parts.push(hardline, hardline, docPart);
    } else {
      parts.push(hardline, docPart);
    }
  }
  return concat(parts);
}

/**
 * Print a string literal with proper quotes
 */
function printString(value: string): string {
  // If already quoted, return as-is
  const trimmed = value.trim();
  if (trimmed.startsWith('"') && trimmed.endsWith('"')) {
    return trimmed;
  }
  return JSON.stringify(value);
}

/**
 * Main printer function - recursively prints AST nodes
 */
function printNode(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const type = node.type;

  switch (type) {
    // Preserve ERROR and MISSING nodes as-is to prevent content loss
    case 'ERROR':
    case 'MISSING':
      return getText(node, text);

    case 'program':
      return printProgram(node, text, path, print);

    case 'import_stmt':
      return printImportStmt(node, text);

    case 'extern_type_stmt':
      return printExternTypeStmt(node, text);

    case 'type_alias_stmt':
      return printTypeAliasStmt(node, text);

    case 'extern_stmt':
      return printExternStmt(node, text);

    case 'global_blackboard_decl':
      return printGlobalBlackboardDecl(node, text);

    case 'global_const_decl':
      return printGlobalConstDecl(node, text);

    case 'tree_def':
      return printTreeDef(node, text, path, print);

    case 'tree_body':
      return printStatementBlock(node, text, path, print);

    case 'param_decl':
      return printParamDecl(node, text);

    case 'statement':
      return printStatement(node, text, path, print);

    case 'blackboard_decl':
      return printBlackboardDecl(node, text);

    case 'local_const_decl':
      return printLocalConstDecl(node, text);

    case 'assignment_stmt':
      return printAssignmentStmt(node, text);

    case 'leaf_node_call':
      return printLeafNodeCall(node, text);

    case 'compound_node_call':
      return printCompoundNodeCall(node, text, path, print);

    case 'property_block':
      return printPropertyBlock(node, text);

    case 'argument':
      return printArgument(node, text);

    case 'precondition':
      return printPrecondition(node, text);

    case 'decorator':
      return printDecorator(node, text);

    case 'children_block':
      return printChildrenBlock(node, text, path, print);

    case 'comment':
    case 'line_comment':
    case 'block_comment':
      // Regular comments - return raw text
      return getText(node, text);

    case 'outer_doc':
    case 'inner_doc':
      // Doc comments - already handled by parent nodes, return raw text
      return getText(node, text);

    default:
      // Return raw text for unknown nodes
      return getText(node, text);
  }
}

function printProgram(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const parts: PrettierDoc[] = [];

  // Collect all top-level elements INCLUDING COMMENTS
  const allItems: BtDslNode[] = [];
  const innerDocs: BtDslNode[] = [];

  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'inner_doc') {
      innerDocs.push(child);
    } else {
      // Include everything: imports, declares, vars, trees, AND comments
      allItems.push(child);
    }
  }

  // Print inner docs first
  if (innerDocs.length > 0) {
    parts.push(
      joinDocs(
        hardline,
        innerDocs.map((n) => getText(n, text)),
      ),
    );
  }

  // Print all items in their original order
  if (allItems.length > 0) {
    const printedItems = allItems.map((item) => printNode(item, text, path, print));
    const itemsDoc = joinWithPreservedBlankLines(printedItems, allItems);

    if (parts.length > 0) {
      parts.push(hardline, hardline, itemsDoc);
    } else {
      parts.push(itemsDoc);
    }
  }

  // Ensure final newline
  if (parts.length === 0) return hardline;
  return concat([concat(parts), hardline]);
}

function printImportStmt(node: BtDslNode, text: string): PrettierDoc {
  const pathNode = getChild(node, 'path');
  const pathText = pathNode ? printString(getText(pathNode, text)) : '""';
  const semi = hasTrailingSemicolon(node, text) ? ';' : '';
  const parts: PrettierDoc[] = ['import ', pathText, semi];

  // Append any trailing comments that were absorbed into this node
  const trailingComments = collectTrailingComments(node, text);
  if (trailingComments.length > 0) {
    parts.push(hardline, joinDocs(hardline, trailingComments));
  }
  return concat(parts);
}

function printExternTypeStmt(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];
  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const name = getChild(node, 'name');
  const nameText = name ? getText(name, text) : '';
  parts.push('extern type ', nameText, ';');
  return concat(parts);
}

function printTypeAliasStmt(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];
  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const name = getChild(node, 'name');
  const value = getChild(node, 'value');
  const nameText = name ? getText(name, text) : '';
  const valueText = value ? getText(value, text) : '';
  parts.push('type ', nameText, ' = ', valueText, ';');
  return concat(parts);
}

function printExternStmt(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];

  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  // Optional behavior_attr as its own line (common style in docs)
  const behavior = getAllNamedChildren(node).find((c) => c.type === 'behavior_attr');
  if (behavior) {
    parts.push(getText(behavior, text), hardline);
  }

  const def = getChild(node, 'def');
  if (!def) {
    parts.push('extern ;');
    return concat(parts);
  }

  parts.push('extern ', printExternDef(def, text));
  return concat(parts);
}

function printExternDef(node: BtDslNode, text: string): PrettierDoc {
  // extern_def is a choice of sequences. Most keywords are anonymous tokens,
  // so we derive the category from the raw text.
  const raw = getText(node, text).trim();
  const kindMatch = /^(action|subtree|condition|control|decorator)\b/.exec(raw);
  const kind = kindMatch?.[1] ?? '';

  const name = getChild(node, 'name');
  const nameText = name ? getText(name, text) : '';

  // Collect ports
  const ports: BtDslNode[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'extern_port_list') {
      for (const port of getAllNamedChildren(child)) {
        if (port.type === 'extern_port') {
          ports.push(port);
        }
      }
    }
  }

  const printedPorts = ports.map((p) => printExternPort(p, text));

  // Reference syntax requires parentheses for all extern declarations, even
  // when there are zero ports (e.g. `extern control Sequence();`).
  // Only fall back to a minimal print if we couldn't determine the kind.
  const needsParens = kind !== '';
  if (!needsParens) {
    return concat([raw, ';']);
  }

  return group(
    concat([
      kind,
      ' ',
      nameText,
      '(',
      ports.length === 0
        ? ''
        : indent(concat([softline, joinDocs(concat([',', line]), printedPorts)])),
      ports.length === 0 ? '' : softline,
      ')',
      ';',
    ]),
  );
}

function printExternPort(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];
  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const direction = findFirstChildOfType(node, 'port_direction');
  const name = getChild(node, 'name');
  const type = getChild(node, 'type');
  const def = getChild(node, 'default');

  if (direction) {
    parts.push(getText(direction, text), ' ');
  }
  parts.push(name ? getText(name, text) : '', ': ', type ? getText(type, text) : '');
  if (def) {
    parts.push(' = ', getText(def, text));
  }
  return concat(parts);
}

function printGlobalBlackboardDecl(node: BtDslNode, text: string): PrettierDoc {
  return printVarDeclLike(node, text, 'init');
}

function printGlobalConstDecl(node: BtDslNode, text: string): PrettierDoc {
  return printConstDeclLike(node, text);
}

function printTreeDef(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const parts: PrettierDoc[] = [];

  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const name = getChild(node, 'name');
  const nameText = name ? getText(name, text) : '';

  // Get params from param_list
  const params: BtDslNode[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'param_list') {
      for (const param of getAllNamedChildren(child)) {
        if (param.type === 'param_decl') {
          params.push(param);
        }
      }
    }
  }

  const paramsDoc =
    params.length === 0
      ? concat(['(', ')'])
      : group(
          concat([
            '(',
            indent(
              concat([
                softline,
                joinDocs(
                  concat([',', line]),
                  params.map((p) => printParamDecl(p, text)),
                ),
              ]),
            ),
            softline,
            ')',
          ]),
        );

  const body = getChild(node, 'body');
  const bodyDoc = body ? printNode(body, text, path, print) : '';

  parts.push(group(concat(['tree ', nameText, paramsDoc, ' ', bodyDoc])));

  return concat(parts);
}

function printParamDecl(node: BtDslNode, text: string): PrettierDoc {
  // Find port_direction child node (not a named field)
  let direction: BtDslNode | null = null;
  for (let i = 0; i < node.childCount; i++) {
    const child = node.child(i);
    if (child?.type === 'port_direction') {
      direction = child;
      break;
    }
  }

  const name = getChild(node, 'name');
  const type = getChild(node, 'type');
  const def = getChild(node, 'default');

  const parts: PrettierDoc[] = [];

  if (direction) {
    parts.push(getText(direction, text), ' ');
  }

  if (name) {
    parts.push(getText(name, text));
  }

  if (type) {
    parts.push(': ', getText(type, text));
  }

  if (def) {
    parts.push(' = ', getText(def, text));
  }

  return concat(parts);
}

function printStatement(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  // statement := simple_stmt [';'] | block_stmt
  const inner = findFirstNamedDescendantOfAny(node, [
    'compound_node_call',
    'leaf_node_call',
    'assignment_stmt',
    'blackboard_decl',
    'local_const_decl',
  ]);
  if (!inner) {
    return '';
  }

  if (inner.type === 'compound_node_call') {
    return printCompoundNodeCall(inner, text, path, print);
  }

  const innerDoc = printNode(inner, text, path, print);
  const raw = getText(node, text);
  const hasSemicolon = raw.trimEnd().endsWith(';');
  return hasSemicolon ? concat([innerDoc, ';']) : innerDoc;
}

function printBlackboardDecl(node: BtDslNode, text: string): PrettierDoc {
  return printVarDeclLike(node, text, 'init');
}

function printLocalConstDecl(node: BtDslNode, text: string): PrettierDoc {
  return printConstDeclLike(node, text);
}

function printAssignmentStmt(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];

  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const preconds = collectPreconditions(node, text);
  if (preconds.length > 0) {
    parts.push(joinDocs(hardline, preconds), hardline);
  }

  const target = getChild(node, 'target');
  const op = getChild(node, 'op');
  const value = getChild(node, 'value');

  const targetText = target ? getText(target, text) : '';
  const opText = op ? getText(op, text) : '=';
  const valueText = value ? getText(value, text) : '';

  parts.push(concat([targetText, ' ', opText, ' ', valueText]));
  return concat(parts);
}

function printLeafNodeCall(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];

  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const decorators = collectDecorators(node, text);
  if (decorators.length > 0) {
    parts.push(joinDocs(hardline, decorators), hardline);
  }

  const preconds = collectPreconditions(node, text);
  if (preconds.length > 0) {
    parts.push(joinDocs(hardline, preconds), hardline);
  }

  const name = getChild(node, 'name');
  const args = getChild(node, 'args');

  parts.push(name ? getText(name, text) : '', args ? printPropertyBlock(args, text) : '()');
  return concat(parts);
}

function printCompoundNodeCall(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const parts: PrettierDoc[] = [];

  const outerDocs = collectOuterDocs(node, text);
  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const decorators = collectDecorators(node, text);
  if (decorators.length > 0) {
    parts.push(joinDocs(hardline, decorators), hardline);
  }

  const preconds = collectPreconditions(node, text);
  if (preconds.length > 0) {
    parts.push(joinDocs(hardline, preconds), hardline);
  }

  const name = getChild(node, 'name');
  parts.push(name ? getText(name, text) : '');

  const body = getChild(node, 'body');
  if (!body) {
    return concat(parts);
  }

  // node_body_with_children := (property_block children_block) | children_block
  const prop = getAllNamedChildren(body).find((c) => c.type === 'property_block');
  const children = getAllNamedChildren(body).find((c) => c.type === 'children_block');

  if (prop) {
    parts.push(printPropertyBlock(prop, text));
  }
  if (children) {
    parts.push(' ', printChildrenBlock(children, text, path, print));
  }

  return concat(parts);
}

function printPropertyBlock(node: BtDslNode, text: string): PrettierDoc {
  // Get arguments from argument_list
  const args: BtDslNode[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'argument_list') {
      for (const arg of getAllNamedChildren(child)) {
        if (arg.type === 'argument') {
          args.push(arg);
        }
      }
    }
  }

  if (args.length === 0) {
    return concat(['(', ')']);
  }

  const printedArgs = args.map((a) => printArgument(a, text));
  return group(
    concat([
      '(',
      indent(concat([softline, joinDocs(concat([',', line]), printedArgs)])),
      softline,
      ')',
    ]),
  );
}

function printArgument(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const value = getChild(node, 'value');

  if (!name) {
    // Positional argument
    return value ? printArgumentExpr(value, text) : '';
  }

  // Named argument
  const nameText = getText(name, text);
  const valueText = value ? printArgumentExpr(value, text) : '';

  return concat([nameText, ': ', valueText]);
}

function printArgumentExpr(node: BtDslNode, text: string): PrettierDoc {
  // argument_expr := 'out' inline_blackboard_decl | [port_direction] expression
  const inlineDecl = getChild(node, 'inline_decl');
  if (inlineDecl) {
    const name = getChild(inlineDecl, 'name');
    return concat(['out var ', name ? getText(name, text) : '']);
  }

  const direction = findFirstChildOfType(node, 'port_direction');
  const value = getChild(node, 'value');
  const valueText = value ? getText(value, text) : '';
  if (direction) {
    return concat([getText(direction, text), ' ', valueText]);
  }
  return valueText;
}

function printPrecondition(node: BtDslNode, text: string): PrettierDoc {
  const kind = getChild(node, 'kind');
  const cond = getChild(node, 'cond');
  const kindText = kind ? getText(kind, text) : '';
  const condText = cond ? getText(cond, text) : '';
  return concat(['@', kindText, '(', condText, ')']);
}

function printDecorator(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const args = getChild(node, 'args');
  const nameText = name ? getText(name, text) : '';
  const argsDoc = args ? printPropertyBlock(args, text) : concat(['(', ')']);
  return concat(['@', nameText, argsDoc]);
}

function printChildrenBlock(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  return printStatementBlock(node, text, path, print);
}

function printStatementBlock(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  // tree_body / children_block := '{' { statement } '}'
  // Include comment nodes as well (they come from `extras`).
  const children = getAllNamedChildren(node);
  if (children.length === 0) {
    return concat(['{', '}']);
  }

  const printedChildren = children.map((c) => printNode(c, text, path, print));
  const childrenDoc = joinWithPreservedBlankLines(printedChildren, children);
  return group(concat(['{', indent(concat([hardline, childrenDoc])), hardline, '}']));
}

function collectOuterDocs(node: BtDslNode, text: string): string[] {
  const docs: string[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'outer_doc') {
      docs.push(getText(child, text));
    }
  }
  return docs;
}

function collectDecorators(node: BtDslNode, text: string): PrettierDoc[] {
  const decorators: PrettierDoc[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'decorator') {
      decorators.push(printDecorator(child, text));
    }
  }
  return decorators;
}

function collectPreconditions(node: BtDslNode, text: string): PrettierDoc[] {
  const list = getAllNamedChildren(node).find((c) => c.type === 'precondition_list');
  if (!list) return [];
  const preconds: PrettierDoc[] = [];
  for (const child of getAllNamedChildren(list)) {
    if (child.type === 'precondition') {
      preconds.push(printPrecondition(child, text));
    }
  }
  return preconds;
}

function printVarDeclLike(node: BtDslNode, text: string, initField: 'init'): PrettierDoc {
  const name = getChild(node, 'name');
  const type = getChild(node, 'type');
  const init = getChild(node, initField);

  const parts: PrettierDoc[] = ['var ', name ? getText(name, text) : ''];
  if (type) {
    parts.push(': ', getText(type, text));
  }
  if (init) {
    parts.push(' = ', getText(init, text));
  }
  if (hasTrailingSemicolon(node, text)) {
    parts.push(';');
  }

  // Append any trailing comments that were absorbed into this node
  const trailingComments = collectTrailingComments(node, text);
  if (trailingComments.length > 0) {
    parts.push(hardline, joinDocs(hardline, trailingComments));
  }
  return concat(parts);
}

function printConstDeclLike(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const type = getChild(node, 'type');
  const value = getChild(node, 'value');

  const parts: PrettierDoc[] = ['const ', name ? getText(name, text) : ''];
  if (type) {
    parts.push(': ', getText(type, text));
  }
  parts.push(' = ', value ? getText(value, text) : '');
  if (hasTrailingSemicolon(node, text)) {
    parts.push(';');
  }

  // Append any trailing comments that were absorbed into this node
  const trailingComments = collectTrailingComments(node, text);
  if (trailingComments.length > 0) {
    parts.push(hardline, joinDocs(hardline, trailingComments));
  }
  return concat(parts);
}

function findFirstChildOfType(node: BtDslNode, type: string): BtDslNode | null {
  for (let i = 0; i < node.childCount; i++) {
    const child = node.child(i);
    if (child?.type === type) {
      return child;
    }
  }
  return null;
}

function findFirstNamedDescendantOfAny(node: BtDslNode, types: string[]): BtDslNode | null {
  // BFS across named nodes.
  const queue: BtDslNode[] = [node];
  while (queue.length > 0) {
    const cur = queue.shift();
    if (!cur) continue;
    for (const child of getAllNamedChildren(cur)) {
      if (types.includes(child.type)) {
        return child;
      }
      queue.push(child);
    }
  }
  return null;
}

const btDslPrettierPlugin: Plugin<BtDslNode> = {
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
      astFormat: 'bt-dsl-ast',
      parse: async (text: string, _options: ParserOptions<BtDslNode>): Promise<BtDslNode> => {
        const parser = await initParser();
        const tree = parser.parse(text);
        const root: BtDslNode = tree.rootNode;

        // Don't extract comments separately - they're already in the AST!
        // tree-sitter includes comment nodes as named children

        return root;
      },
      locStart: (node: BtDslNode): number => node.startIndex,
      locEnd: (node: BtDslNode): number => node.endIndex,
    },
  },
  printers: {
    'bt-dsl-ast': {
      print: (path: AstPath<BtDslNode>, options: ParserOptions<BtDslNode>, print: PrintFn): Doc => {
        const node = path.node;
        return printNode(node, options.originalText, path, print);
      },
    },
  },
};

export default btDslPrettierPlugin;
