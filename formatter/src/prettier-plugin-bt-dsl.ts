import { doc } from 'prettier';
import type { AstPath, Doc, ParserOptions, Plugin } from 'prettier';
import Parser from 'web-tree-sitter';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

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
    case 'program':
      return printProgram(node, text, path, print);

    case 'import_stmt':
      return printImportStmt(node, text);

    case 'declare_stmt':
      return printDeclareStmt(node, text);

    case 'declare_port':
      return printDeclarePort(node, text);

    case 'global_var_decl':
      return printGlobalVarDecl(node, text);

    case 'tree_def':
      return printTreeDef(node, text, path, print);

    case 'param_decl':
      return printParamDecl(node, text);

    case 'local_var_decl':
      return printLocalVarDecl(node, text);

    case 'node_stmt':
      return printNodeStmt(node, text, path, print);

    case 'decorator':
      return printDecorator(node, text);

    case 'property_block':
      return printPropertyBlock(node, text);

    case 'argument':
      return printArgument(node, text);

    case 'children_block':
      return printChildrenBlock(node, text, path, print);

    case 'expression_stmt':
      return printExpressionStmt(node, text);

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
  return concat(['import ', pathText]);
}

function printDeclareStmt(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];

  // Collect outer docs
  const outerDocs: string[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'outer_doc') {
      outerDocs.push(getText(child, text));
    }
  }

  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  const category = getChild(node, 'category');
  const name = getChild(node, 'name');
  const categoryText = category ? getText(category, text) : '';
  const nameText = name ? getText(name, text) : '';

  // Get ports from declare_port_list
  const ports: BtDslNode[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'declare_port_list') {
      for (const port of getAllNamedChildren(child)) {
        if (port.type === 'declare_port') {
          ports.push(port);
        }
      }
    }
  }

  if (ports.length === 0) {
    parts.push('declare ', categoryText, ' ', nameText, '()');
  } else {
    const printedPorts = ports.map((p) => printDeclarePort(p, text));
    parts.push(
      group(
        concat([
          'declare ',
          categoryText,
          ' ',
          nameText,
          '(',
          indent(concat([softline, joinDocs(concat([',', line]), printedPorts)])),
          softline,
          ')',
        ]),
      ),
    );
  }

  return concat(parts);
}

function printDeclarePort(node: BtDslNode, text: string): PrettierDoc {
  const parts: PrettierDoc[] = [];

  // Collect outer docs
  const outerDocs: string[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'outer_doc') {
      outerDocs.push(getText(child, text));
    }
  }

  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

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

  const directionText = direction ? getText(direction, text) + ' ' : 'in ';
  const nameText = name ? getText(name, text) : '';
  const typeText = type ? getText(type, text) : '';

  parts.push(directionText, nameText, ': ', typeText);
  return concat(parts);
}

function printGlobalVarDecl(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const type = getChild(node, 'type');

  const nameText = name ? getText(name, text) : '';
  const typeText = type ? getText(type, text) : '';

  return concat(['var ', nameText, ': ', typeText]);
}

function printTreeDef(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const parts: PrettierDoc[] = [];

  // Collect outer docs
  const outerDocs: string[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'outer_doc') {
      outerDocs.push(getText(child, text));
    }
  }

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
    params.length > 0
      ? group(
          joinDocs(
            concat([',', line]),
            params.map((p) => printParamDecl(p, text)),
          ),
        )
      : '';

  // Collect local vars AND comments
  const bodyChildren: BtDslNode[] = [];
  for (const child of getAllNamedChildren(node)) {
    if (
      child.type === 'local_var_decl' ||
      child.type === 'comment' ||
      child.type === 'line_comment' ||
      child.type === 'block_comment'
    ) {
      bodyChildren.push(child);
    }
  }

  const bodyChildrenDoc =
    bodyChildren.length > 0
      ? concat([
          joinDocs(
            hardline,
            bodyChildren.map((c) => printNode(c, text, path, print)),
          ),
          hardline,
        ])
      : '';

  const body = getChild(node, 'body');
  const bodyDoc = body ? printNode(body, text, path, print) : '';

  parts.push(
    group(
      concat([
        'Tree ',
        nameText,
        '(',
        paramsDoc,
        ') ',
        '{',
        indent(concat([hardline, bodyChildrenDoc, bodyDoc])),
        hardline,
        '}',
      ]),
    ),
  );

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

  return concat(parts);
}

function printLocalVarDecl(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const type = getChild(node, 'type');
  const init = getChild(node, 'init');

  const parts: PrettierDoc[] = ['var '];

  if (name) {
    parts.push(getText(name, text));
  }

  if (type) {
    parts.push(': ', getText(type, text));
  }

  if (init) {
    parts.push(' = ', getText(init, text));
  }

  return concat(parts);
}

function printNodeStmt(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  const parts: PrettierDoc[] = [];

  // Collect outer docs and decorators
  const outerDocs: string[] = [];
  const decorators: BtDslNode[] = [];

  for (const child of getAllNamedChildren(node)) {
    if (child.type === 'outer_doc') {
      outerDocs.push(getText(child, text));
    } else if (child.type === 'decorator') {
      decorators.push(child);
    }
  }

  if (outerDocs.length > 0) {
    parts.push(joinDocs(hardline, outerDocs), hardline);
  }

  if (decorators.length > 0) {
    parts.push(
      joinDocs(
        hardline,
        decorators.map((d) => printDecorator(d, text)),
      ),
      hardline,
    );
  }

  const name = getChild(node, 'name');
  const nameText = name ? getText(name, text) : '';

  const propertyBlock = getAllNamedChildren(node).find((c) => c.type === 'property_block');
  const childrenBlock = getAllNamedChildren(node).find((c) => c.type === 'children_block');

  parts.push(nameText);

  if (propertyBlock) {
    parts.push(printPropertyBlock(propertyBlock, text));
  }

  if (childrenBlock) {
    parts.push(' '); // Always add space before children block
    parts.push(printChildrenBlock(childrenBlock, text, path, print));
  }

  return concat(parts);
}

function printDecorator(node: BtDslNode, text: string): PrettierDoc {
  const name = getChild(node, 'name');
  const nameText = name ? getText(name, text) : '';

  const propertyBlock = getAllNamedChildren(node).find((c) => c.type === 'property_block');

  const parts: PrettierDoc[] = ['@', nameText];

  if (propertyBlock) {
    parts.push(printPropertyBlock(propertyBlock, text));
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
    return value ? getText(value, text) : '';
  }

  // Named argument
  const nameText = getText(name, text);
  const valueText = value ? getText(value, text) : '';

  return concat([nameText, ': ', valueText]);
}

function printChildrenBlock(
  node: BtDslNode,
  text: string,
  path: AstPath<BtDslNode>,
  print: PrintFn,
): PrettierDoc {
  // Get ALL children including comments
  const children = getAllNamedChildren(node);

  if (children.length === 0) {
    return concat(['{', '}']);
  }

  const printedChildren = children.map((c) => printNode(c, text, path, print));
  const childrenDoc = joinWithPreservedBlankLines(printedChildren, children);

  return group(concat(['{', indent(concat([hardline, childrenDoc])), hardline, '}']));
}

function printExpressionStmt(node: BtDslNode, text: string): PrettierDoc {
  const assignment = getAllNamedChildren(node).find((c) => c.type === 'assignment_expr');
  if (!assignment) return '';

  const target = getChild(assignment, 'target');
  const op = getChild(assignment, 'op');
  const value = getChild(assignment, 'value');

  const targetText = target ? getText(target, text) : '';
  const opText = op ? getText(op, text) : '=';
  const valueText = value ? getText(value, text) : '';

  return concat([targetText, ' ', opText, ' ', valueText]);
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
