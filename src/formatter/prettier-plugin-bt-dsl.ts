import type { Plugin } from "prettier";
import type { AstNode, LangiumDocument, LeafCstNode } from "langium";
import { AstUtils, URI } from "langium";
import { NodeFileSystem } from "langium/node";
import { createBtDslServicesCore } from "../language/bt-dsl-module-core.js";
import {
  isArgument,
  isBlackboardRef,
  isBoolLiteral,
  isChildrenBlock,
  isDeclarePort,
  isDeclareStmt,
  isDecorator,
  isFloatLiteral,
  isGlobalVarDecl,
  isImportStmt,
  isIntLiteral,
  isLocalVarDecl,
  isNodeStmt,
  isParamDecl,
  isParamList,
  isProgram,
  isPropertyBlock,
  isStringLiteral,
  isTreeDef,
  isExpressionStmt,
  isAssignmentExpr,
  isBinaryExpr,
  isUnaryExpr,
  isLiteralExpr,
  isVarRefExpr,
} from "../language/generated/ast.js";

// Prettier's doc builders are exposed via the `doc` namespace.
// We intentionally keep this file free of VS Code / LSP types.
import { doc } from "prettier";

type PrettierDoc = any;

type BtDslComment = {
  type: "Line" | "Block";
  raw: string;
  value: string;
  // Prettier uses numeric offsets.
  range: [number, number];
  __locStart: number;
  __locEnd: number;
  // Keep original token classification for debugging.
  tokenType: "SL_COMMENT" | "ML_COMMENT";
};

type WithLoc = {
  __locStart?: number;
  __locEnd?: number;
};

const services = createBtDslServicesCore(NodeFileSystem);

function computeLineOffsets(text: string): number[] {
  const lineOffsets: number[] = [0];
  for (let i = 0; i < text.length; i++) {
    const ch = text.charCodeAt(i);
    if (ch === 13 /* \r */) {
      if (text.charCodeAt(i + 1) === 10 /* \n */) {
        i++;
      }
      lineOffsets.push(i + 1);
    } else if (ch === 10 /* \n */) {
      lineOffsets.push(i + 1);
    }
  }
  return lineOffsets;
}

function offsetAt(
  lineOffsets: number[],
  position: { line: number; character: number }
): number {
  const line = Math.max(0, Math.min(position.line, lineOffsets.length - 1));
  const lineOffset = lineOffsets[line] ?? 0;
  return lineOffset + position.character;
}

function attachLoc(node: WithLoc, start: number, end: number): void {
  node.__locStart = start;
  node.__locEnd = end;
}

function nodeRangeOffsets(
  n: { $cstNode?: { range?: any } } | undefined,
  lineOffsets: number[]
): [number, number] | undefined {
  const r = n?.$cstNode?.range;
  if (!r) return undefined;
  const start = offsetAt(lineOffsets, r.start);
  const end = offsetAt(lineOffsets, r.end);
  return [start, end];
}

function isCommentLeaf(leaf: LeafCstNode): boolean {
  const t = leaf.tokenType?.name;
  return t === "SL_COMMENT" || t === "ML_COMMENT";
}

function leafTextFromSource(
  text: string,
  lineOffsets: number[],
  leaf: LeafCstNode
): { tokenType: string; raw: string; start: number; end: number } | undefined {
  const tokenType = leaf.tokenType?.name;
  if (!tokenType) return undefined;
  if (tokenType !== "SL_COMMENT" && tokenType !== "ML_COMMENT")
    return undefined;
  const r: any = (leaf as any).range;
  if (!r) return undefined;
  const start = offsetAt(lineOffsets, r.start);
  const end = offsetAt(lineOffsets, r.end);
  const raw = text.slice(start, end);
  return { tokenType, raw, start, end };
}

function collectCommentsFromCst(
  rootCst: any,
  text: string,
  lineOffsets: number[]
): BtDslComment[] {
  const leaves: LeafCstNode[] = [];

  const visit = (n: any): void => {
    if (!n) return;
    // LeafCstNode has tokenType.
    if (n.tokenType) {
      leaves.push(n as LeafCstNode);
      return;
    }
    const children = n.children;
    if (!children) return;

    // Langium CST nodes usually have `children: Record<string, CstNode[]>`.
    if (Array.isArray(children)) {
      for (const c of children) visit(c);
      return;
    }
    if (typeof children === "object") {
      for (const key of Object.keys(children)) {
        const arr = (children as any)[key];
        if (Array.isArray(arr)) {
          for (const c of arr) visit(c);
        }
      }
    }
  };

  visit(rootCst);

  const comments: BtDslComment[] = [];
  for (const leaf of leaves) {
    if (!isCommentLeaf(leaf)) continue;
    const info = leafTextFromSource(text, lineOffsets, leaf);
    if (!info) continue;

    const type: "Line" | "Block" = info.raw.startsWith("/*") ? "Block" : "Line";

    comments.push({
      type,
      raw: info.raw,
      value: info.raw,
      range: [info.start, info.end],
      __locStart: info.start,
      __locEnd: info.end,
      tokenType: info.tokenType as "SL_COMMENT" | "ML_COMMENT",
    });
  }

  // Keep comment order stable.
  comments.sort((a, b) => a.__locStart - b.__locStart);
  return comments;
}

function assignLocToAllAstNodes(root: AstNode, lineOffsets: number[]): void {
  const r = nodeRangeOffsets(root as any, lineOffsets);
  if (r) attachLoc(root as any, r[0], r[1]);

  for (const n of AstUtils.streamAllContents(root)) {
    const rr = nodeRangeOffsets(n as any, lineOffsets);
    if (!rr) continue;
    attachLoc(n as any, rr[0], rr[1]);
  }
}

function getRefText(ref: any): string {
  return ref?.$refText ?? ref?.ref?.name ?? ref?.toString?.() ?? "";
}

const builders = doc.builders;
const { group, indent, line, softline, hardline, join } = builders;

// Prettier doc format treats arrays as concatenation.
const concat = (parts: PrettierDoc[]): PrettierDoc => parts;

function printSeparated(
  items: PrettierDoc[],
  separator: PrettierDoc
): PrettierDoc {
  if (items.length === 0) return "";
  if (items.length === 1) return items[0];
  return join(separator, items);
}

function printArgs(path: any, print: any): PrettierDoc {
  const node = path.getValue();
  if (!node?.args || node.args.length === 0) {
    return concat(["(", ")"]);
  }

  const printedArgs = path.map(print, "args");
  return group(
    concat([
      "(",
      indent(concat([softline, join(concat([",", line]), printedArgs)])),
      softline,
      ")",
    ])
  );
}

function printChildrenBlock(path: any, print: any): PrettierDoc {
  const node = path.getValue();
  const children = node?.children ?? [];
  if (children.length === 0) {
    return concat(["{", "}"]);
  }

  const printedChildren = path.map(print, "children");
  const childrenDoc = joinWithPreservedBlankLines(printedChildren, children);
  return group(
    concat([
      "{",
      indent(concat([hardline, childrenDoc])),
      hardline,
      "}",
    ])
  );
}

function printNodeStmtHead(path: any, print: any): PrettierDoc {
  const node = path.getValue();

  const parts: PrettierDoc[] = [node.nodeName?.$refText ?? ""];
  if (node.propertyBlock) {
    parts.push(path.call(print, "propertyBlock"));
  }

  return concat(parts);
}

function printNodeStmt(path: any, options: any, print: any): PrettierDoc {
  const node = path.getValue();

  const docs: string[] = node.outerDocs ?? [];
  const docsDoc =
    docs.length > 0 ? concat([join(hardline, docs), hardline]) : "";

  const decorators = node.decorators ?? [];
  const printedDecorators =
    decorators.length > 0 ? path.map(print, "decorators") : [];

  const head = printNodeStmtHead(path, print);

  const body = node.childrenBlock
    ? concat([head, " ", path.call(print, "childrenBlock")])
    : head;

  if (printedDecorators.length === 0) {
    return concat([docsDoc, body]);
  }

  return concat([docsDoc, join(hardline, printedDecorators), hardline, body]);
}

function printTreeDef(path: any, options: any, print: any): PrettierDoc {
  const node = path.getValue();

  const docs: string[] = node.outerDocs ?? [];
  const docsDoc =
    docs.length > 0 ? concat([join(hardline, docs), hardline]) : "";

  const paramsDoc = node.params ? path.call(print, "params") : "";

  // Print local vars
  const localVars = node.localVars ?? [];
  const printedLocalVars = localVars.length > 0 ? path.map(print, "localVars") : [];
  const localVarsDoc = printedLocalVars.length > 0
    ? concat([join(hardline, printedLocalVars), hardline])
    : "";

  return concat([
    docsDoc,
    group(
      concat([
        "Tree ",
        node.name,
        "(",
        paramsDoc,
        ") ",
        "{",
        indent(concat([hardline, localVarsDoc, path.call(print, "body")])),
        hardline,
        "}",
      ])
    ),
  ]);
}

function printDeclareStmt(path: any, options: any, print: any): PrettierDoc {
  const node = path.getValue();

  const docs: string[] = node.outerDocs ?? [];
  const docsDoc =
    docs.length > 0 ? concat([join(hardline, docs), hardline]) : "";

  const ports = node.ports ?? [];
  if (ports.length === 0) {
    return concat([docsDoc, "declare ", node.category, " ", node.name, "()"]);
  }

  const printedPorts = path.map(print, "ports");
  return concat([
    docsDoc,
    group(
      concat([
        "declare ",
        node.category,
        " ",
        node.name,
        "(",
        indent(concat([softline, join(concat([",", line]), printedPorts)])),
        softline,
        ")",
      ])
    ),
  ]);
}

function printDeclarePort(path: any, options: any, print: any): PrettierDoc {
  const node = path.getValue();

  const docs: string[] = node.outerDocs ?? [];
  const docsDoc =
    docs.length > 0 ? concat([join(hardline, docs), hardline]) : "";

  const head = node.direction
    ? concat([node.direction, " ", node.name])
    : concat(["in ", node.name]);

  return concat([docsDoc, head, ": ", node.typeName]);
}



/**
 * Check if there's a blank line between two AST nodes in the original source.
 * Uses CST node range information to detect blank lines.
 */
function hasBlankLineBetween(prevNode: any, nextNode: any): boolean {
  const prevEnd = prevNode?.$cstNode?.range?.end;
  const nextStart = nextNode?.$cstNode?.range?.start;
  if (!prevEnd || !nextStart) return false;
  // A blank line exists if there's more than 1 line difference
  return nextStart.line - prevEnd.line > 1;
}

/**
 * Join printed docs with hardline, but insert an extra hardline (blank line)
 * where the original source had a blank line between nodes.
 */
function joinWithPreservedBlankLines(
  printedDocs: PrettierDoc[],
  originalNodes: any[]
): PrettierDoc {
  if (printedDocs.length === 0) return "";
  if (printedDocs.length === 1) return printedDocs[0];

  const parts: PrettierDoc[] = [printedDocs[0]];
  for (let i = 1; i < printedDocs.length; i++) {
    const prevNode = originalNodes[i - 1];
    const currNode = originalNodes[i];
    if (hasBlankLineBetween(prevNode, currNode)) {
      parts.push(hardline, hardline, printedDocs[i]);
    } else {
      parts.push(hardline, printedDocs[i]);
    }
  }
  return concat(parts);
}

function printProgram(path: any, options: any, print: any): PrettierDoc {
  const node = path.getValue();

  const innerDocs: string[] = node.innerDocs ?? [];

  const imports = node.imports ?? [];
  const globalVars = node.globalVars ?? [];
  const trees = node.trees ?? [];

  const printedImports = imports.length > 0 ? path.map(print, "imports") : [];
  const printedGlobalVars = globalVars.length > 0 ? path.map(print, "globalVars") : [];
  const printedTrees = trees.length > 0 ? path.map(print, "trees") : [];

  const blankLine = concat([hardline, hardline]);

  // Collect all top-level items in order for blank line detection
  const declarations = node.declarations ?? [];
  const printedDeclarations = declarations.length > 0 ? path.map(print, "declarations") : [];

  const allItems: any[] = [
    ...imports,
    ...declarations,
    ...globalVars,
    ...trees,
  ];
  const allPrintedItems: PrettierDoc[] = [
    ...printedImports,
    ...printedDeclarations,
    ...printedGlobalVars,
    ...printedTrees,
  ];

  // Build result with preserved blank lines
  const parts: PrettierDoc[] = [];

  if (innerDocs.length > 0) {
    parts.push(join(hardline, innerDocs));
  }

  if (allPrintedItems.length > 0) {
    const itemsDoc = joinWithPreservedBlankLines(allPrintedItems, allItems);
    if (parts.length > 0) {
      parts.push(blankLine, itemsDoc);
    } else {
      parts.push(itemsDoc);
    }
  }

  // Ensure final newline in output document.
  if (parts.length === 0) return hardline;
  return concat([concat(parts), hardline]);
}

function printAsStringToken(value: string): string {
  // The AST stores STRING terminals as plain strings (often without surrounding quotes).
  // The grammar requires quotes, so we normalize here.
  const v = String(value);
  const trimmed = v.trim();
  if (trimmed.length >= 2 && trimmed.startsWith('"') && trimmed.endsWith('"')) {
    return trimmed;
  }
  return JSON.stringify(v);
}

function printValueExpr(node: any): string {
  if (isStringLiteral(node)) return printAsStringToken(node.value);
  if (isIntLiteral(node)) return String(node.value);
  if (isFloatLiteral(node)) return String(node.value);
  if (isBoolLiteral(node)) return node.value ? "true" : "false";
  if (isBlackboardRef(node)) {
    const name = getRefText(node.varName);
    return node.direction ? `${node.direction} ${name}` : name;
  }
  // Handle LiteralExpr wrapper
  if (isLiteralExpr(node)) {
    return printValueExpr(node.literal);
  }
  return "";
}

/**
 * Print an expression for formatter output.
 */
function printExpression(node: any): string {
  if (isBinaryExpr(node)) {
    const left = printExpression(node.left);
    const right = printExpression(node.right);
    return `${left} ${node.op} ${right}`;
  }
  if (isUnaryExpr(node)) {
    const operand = printExpression(node.operand);
    return `${node.op}${operand}`;
  }
  if (isLiteralExpr(node)) {
    return printValueExpr(node.literal);
  }
  if (isVarRefExpr(node)) {
    return getRefText(node.varRef);
  }
  // Fallback to literal types
  if (isStringLiteral(node)) return printAsStringToken(node.value);
  if (isIntLiteral(node)) return String(node.value);
  if (isFloatLiteral(node)) return String(node.value);
  if (isBoolLiteral(node)) return node.value ? "true" : "false";
  return "";
}

const btDslPrettierPlugin: Plugin = {
  languages: [
    {
      name: "BT DSL",
      parsers: ["bt-dsl"],
      extensions: [".bt"],
      linguistLanguageId: 0,
      vscodeLanguageIds: ["bt-dsl"],
    } as any,
  ],
  parsers: {
    "bt-dsl": {
      astFormat: "bt-dsl-ast",
      // Prettier calls this synchronously.
      parse: (text: string, options: any) => {
        const lineOffsets = computeLineOffsets(text);

        const filepath = options?.filepath;
        const uri =
          typeof filepath === "string" && filepath.length > 0
            ? URI.file(filepath)
            : URI.parse("file:///__bt-dsl__/in-memory.bt");

        const document: LangiumDocument =
          services.shared.workspace.LangiumDocumentFactory.fromString(
            text,
            uri
          );

        const parseErrors: any[] =
          (document.parseResult as any)?.parserErrors ?? [];
        if (parseErrors.length > 0) {
          const first = parseErrors[0];
          const msg =
            typeof first?.message === "string"
              ? first.message
              : "Failed to parse bt-dsl document";
          throw new Error(msg);
        }

        const root = document.parseResult.value as AstNode;
        assignLocToAllAstNodes(root, lineOffsets);

        const rootCst: any = (root as any).$cstNode;
        const comments = rootCst
          ? collectCommentsFromCst(rootCst, text, lineOffsets)
          : [];

        (root as any).comments = comments;
        return root as any;
      },
      locStart: (node: any) => {
        if (!node) return 0;
        if (typeof node.__locStart === "number") return node.__locStart;
        return 0;
      },
      locEnd: (node: any) => {
        if (!node) return 0;
        if (typeof node.__locEnd === "number") return node.__locEnd;
        return 0;
      },
    } as any,
  },
  printers: {
    "bt-dsl-ast": {
      print: (path: any, options: any, print: any) => {
        const node = path.getValue();
        if (!node) return "";

        if (isProgram(node)) return printProgram(path, options, print);
        if (isImportStmt(node))
          return concat(["import ", printAsStringToken(node.path)]);
        if (isGlobalVarDecl(node))
          return concat(["var ", node.name, ": ", node.typeName]);
        if (isLocalVarDecl(node)) {
          const base = concat(["var ", node.name]);
          const withType = node.typeName ? concat([base, ": ", node.typeName]) : base;
          if (node.initialValue) {
            return concat([withType, " = ", printExpression(node.initialValue)]);
          }
          return withType;
        }
        if (isExpressionStmt(node)) {
          const assign = node.assignment;
          if (isAssignmentExpr(assign)) {
            const target = getRefText(assign.target);
            const value = printExpression(assign.value);
            return concat([target, " ", assign.op, " ", value]);
          }
          return "";
        }
        if (isTreeDef(node)) return printTreeDef(path, options, print);
        if (isDeclareStmt(node)) return printDeclareStmt(path, options, print);
        if (isDeclarePort(node)) return printDeclarePort(path, options, print);
        if (isParamList(node)) {
          const printed = path.map(print, "params");
          return group(join(concat([",", line]), printed));
        }
        if (isParamDecl(node)) {
          const head = node.direction ? concat([node.direction, " ", node.name]) : node.name;
          return node.typeName ? concat([head, ": ", node.typeName]) : head;
        }
        if (isNodeStmt(node)) return printNodeStmt(path, options, print);
        if (isDecorator(node)) {
          const head: PrettierDoc[] = ["@", node.name?.$refText ?? ""];
          if (node.propertyBlock) head.push(path.call(print, "propertyBlock"));
          return concat(head);
        }
        if (isPropertyBlock(node)) return printArgs(path, print);
        if (isArgument(node)) {
          // Positional argument: just print the value
          if (node.name === undefined) {
            return printValueExpr(node.value);
          }
          return concat([node.name, ": ", printValueExpr(node.value)]);
        }
        if (isChildrenBlock(node)) return printChildrenBlock(path, print);

        // Literals and references are printed by their parents.
        return "";
      },

      // ----- Comment handling -----
      printComment: (commentPath: any) => {
        const c: BtDslComment = commentPath.getValue();
        return c.raw;
      },
      canAttachComment: (node: any) => {
        // Attach to AST nodes only.
        return Boolean(
          node && typeof node === "object" && typeof node.$type === "string"
        );
      },
      getCommentChildNodes: (node: any) => {
        if (!node || typeof node !== "object") return [];

        if (isProgram(node)) {
          return [
            ...(node.imports ?? []),
            ...(node.declarations ?? []),
            ...(node.globalVars ?? []),
            ...(node.trees ?? []),
          ].filter(Boolean);
        }
        if (isGlobalVarDecl(node) || isLocalVarDecl(node)) return [];
        if (isTreeDef(node)) return [node.params, ...(node.localVars ?? []), node.body].filter(Boolean);
        if (isParamList(node)) return node.params ?? [];
        if (isNodeStmt(node)) {
          const kids: any[] = [];
          if (node.decorators) kids.push(...node.decorators);
          if (node.propertyBlock) kids.push(node.propertyBlock);
          if (node.childrenBlock) kids.push(node.childrenBlock);
          return kids;
        }
        if (isDecorator(node))
          return node.propertyBlock ? [node.propertyBlock] : [];
        if (isPropertyBlock(node)) return node.args ?? [];
        if (isArgument(node)) return node.value ? [node.value] : [];
        if (isChildrenBlock(node)) return node.children ?? [];

        return [];
      },
    } as any,
  },
};

export default btDslPrettierPlugin;
