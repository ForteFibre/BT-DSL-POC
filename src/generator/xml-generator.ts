import type {
  Program,
  TreeDef,
  NodeStmt,
  Argument,
  BlackboardRef,
  Decorator,
  Expression,
  ExpressionStmt,
  AssignmentExpr,
  BinaryExpr,
  UnaryExpr,
  LiteralExpr,
  VarRefExpr,
} from "../language/generated/ast.js";
import {
  isBlackboardRef,
  isStringLiteral,
  isIntLiteral,
  isFloatLiteral,
  isBoolLiteral,
  isNodeStmt,
  isExpressionStmt,
  isBinaryExpr,
  isUnaryExpr,
  isLiteralExpr,
  isVarRefExpr,
} from "../language/generated/ast.js";
import {
  resolveTreeTypes,
  getParamType,
  type TreeTypeContext,
} from "../language/type-resolver.js";
import type { NodeResolver } from "../language/node-resolver.js";

/**
 * Generates BehaviorTree.CPP XML from the AST.
 */
export class XmlGenerator {
  private nodeResolver?: NodeResolver;
  private treeTypeContexts = new Map<string, TreeTypeContext>();

  /**
   * Set the node resolver for unified node/port resolution.
   * Used mainly for positional argument port-name completion.
   */
  setNodeResolver(resolver: NodeResolver): void {
    this.nodeResolver = resolver;
  }

  generate(program: Program, trees: TreeDef[] = program.trees): string {
    const lines: string[] = [];

    // Resolve types for all trees first
    this.resolveAllTreeTypes(trees);

    // XML declaration
    lines.push('<?xml version="1.0" encoding="UTF-8"?>');

    // Get main tree (first one) or default
    const mainTree = program.trees[0]?.name ?? "Main";
    lines.push(`<root BTCPP_format="4" main_tree_to_execute="${mainTree}">`);

    // Generate TreeNodesModel for subtrees with parameters
    const subtrees = trees.filter(
      (t) => t.params && t.params.params.length > 0
    );
    if (subtrees.length > 0) {
      lines.push("    <TreeNodesModel>");
      for (const tree of subtrees) {
        lines.push(this.generateTreeNodeModel(tree, 8));
      }
      lines.push("    </TreeNodesModel>");
      lines.push("");
    }

    // Generate each BehaviorTree
    for (const tree of trees) {
      lines.push(this.generateBehaviorTree(tree, program));
      lines.push("");
    }

    lines.push("</root>");

    return lines.join("\n");
  }

  /**
   * Resolve types for all trees in the program.
   */
  private resolveAllTreeTypes(trees: TreeDef[]): void {
    this.treeTypeContexts.clear();

    if (!this.nodeResolver) return;

    for (const tree of trees) {
      const ctx = resolveTreeTypes(tree, this.nodeResolver);
      this.treeTypeContexts.set(tree.name, ctx);
    }
  }

  private generateTreeNodeModel(tree: TreeDef, indent: number): string {
    const pad = " ".repeat(indent);
    const lines: string[] = [];
    const ctx = this.treeTypeContexts.get(tree.name);

    lines.push(`${pad}<SubTree ID="${tree.name}">`);

    if (tree.params) {
      for (const param of tree.params.params) {
        const portType =
          param.direction === "out"
            ? "output_port"
            : param.direction === "ref"
              ? "inout_port"
              : "input_port";
        // Use resolved type (explicit or inferred)
        const resolvedType = ctx ? getParamType(param, ctx) : param.typeName;
        const typeAttr = resolvedType ? ` type="${resolvedType}"` : "";
        lines.push(`${pad}    <${portType} name="${param.name}"${typeAttr} />`);
      }
    }

    lines.push(`${pad}</SubTree>`);

    return lines.join("\n");
  }

  private generateBehaviorTree(tree: TreeDef, program: Program): string {
    const lines: string[] = [];

    lines.push(`    <BehaviorTree ID="${tree.name}">`);

    // Add metadata from outer docs
    if (tree.outerDocs.length > 0) {
      lines.push("        <Metadata>");
      const description = tree.outerDocs
        .map((d) => d.replace(/^\/\/\/\s*/, "").trim())
        .join(" ");
      lines.push(
        `            <item key="description" value="${this.escapeXml(
          description
        )}"/>`
      );
      lines.push("        </Metadata>");
    }

    // Check if there are local vars with initial values
    const varsWithInit = (tree.localVars ?? []).filter((v) => v.initialValue);

    if (varsWithInit.length > 0) {
      // Wrap with Sequence and add Script for initialization
      lines.push("        <Sequence>");
      const scriptCode = varsWithInit
        .map((v) => `${v.name}:=${this.serializeExpression(v.initialValue!)}`)
        .join("; ");
      lines.push(`            <Script code=" ${scriptCode} " />`);
      lines.push(this.generateNode(tree.body, 12, program));
      lines.push("        </Sequence>");
    } else {
      // Generate root node without wrapper
      lines.push(this.generateNode(tree.body, 8, program));
    }

    lines.push("    </BehaviorTree>");

    return lines.join("\n");
  }

  /**
   * Serialize an expression to BTCPP script format.
   * Binary operations are wrapped in parentheses for BTCPP compatibility.
   */
  private serializeExpression(expr: Expression): string {
    if (isBinaryExpr(expr)) {
      const left = this.serializeExpression((expr as BinaryExpr).left);
      const right = this.serializeExpression((expr as BinaryExpr).right);
      const op = (expr as BinaryExpr).op;
      return `(${left} ${op} ${right})`;
    }
    if (isUnaryExpr(expr)) {
      const operand = this.serializeExpression((expr as UnaryExpr).operand);
      const op = (expr as UnaryExpr).op;
      return `${op}${operand}`;
    }
    if (isLiteralExpr(expr)) {
      return this.formatLiteralForScript((expr as LiteralExpr).literal);
    }
    if (isVarRefExpr(expr)) {
      return (expr as VarRefExpr).varRef.$refText;
    }
    // Fallback for literal types directly
    if (isStringLiteral(expr)) {
      return `'${(expr as any).value}'`;
    }
    if (isIntLiteral(expr)) {
      return String((expr as any).value);
    }
    if (isFloatLiteral(expr)) {
      return String((expr as any).value);
    }
    if (isBoolLiteral(expr)) {
      return (expr as any).value ? "true" : "false";
    }
    return "";
  }

  /**
   * Serialize an assignment expression to BTCPP script format.
   */
  private serializeAssignment(stmt: ExpressionStmt): string {
    const assign = stmt.assignment as AssignmentExpr;
    const target = assign.target.$refText;
    const op = assign.op;
    const value = this.serializeExpression(assign.value);
    return `${target} ${op} ${value}`;
  }

  /**
   * Format a literal value for use in a BT.CPP Script node.
   */
  private formatLiteralForScript(literal: any): string {
    if (isStringLiteral(literal)) {
      // BT.CPP Script uses single quotes for strings
      return `'${literal.value}'`;
    }
    if (isIntLiteral(literal)) {
      return String(literal.value);
    }
    if (isFloatLiteral(literal)) {
      return String(literal.value);
    }
    if (isBoolLiteral(literal)) {
      return literal.value ? "true" : "false";
    }
    return "";
  }

  private generateNode(
    node: NodeStmt,
    indent: number,
    program: Program
  ): string {
    const pad = " ".repeat(indent);
    const lines: string[] = [];

    // Handle decorators by wrapping the node
    const decoratorStack = [...node.decorators].reverse();

    // Open decorator tags (outermost first)
    for (const decorator of decoratorStack) {
      const decoratorLines = this.generateDecoratorOpen(decorator, indent);
      lines.push(decoratorLines);
      indent += 4;
    }

    // Generate the actual node
    const nodePad = " ".repeat(indent);
    const attrs = this.generateNodeAttributes(node, program);
    const hasChildren =
      node.childrenBlock && node.childrenBlock.children.length > 0;

    const nodeTag = node.nodeName.$refText;

    if (hasChildren) {
      lines.push(`${nodePad}<${nodeTag}${attrs}>`);
      for (const child of node.childrenBlock!.children) {
        if (isNodeStmt(child)) {
          lines.push(this.generateNode(child, indent + 4, program));
        } else if (isExpressionStmt(child)) {
          // Generate <Script> node for expression statements
          const childPad = " ".repeat(indent + 4);
          const code = this.serializeAssignment(child);
          lines.push(`${childPad}<Script code=" ${code} " />`);
        }
      }
      lines.push(`${nodePad}</${nodeTag}>`);
    } else {
      lines.push(`${nodePad}<${nodeTag}${attrs} />`);
    }

    // Close decorator tags (innermost first)
    for (const decorator of [...decoratorStack].reverse()) {
      indent -= 4;
      const closePad = " ".repeat(indent);
      lines.push(`${closePad}</${decorator.name.$refText}>`);
    }

    return lines.join("\n");
  }

  private generateDecoratorOpen(decorator: Decorator, indent: number): string {
    const pad = " ".repeat(indent);
    let attrs = "";
    const decoratorId = decorator.name.$refText;

    if (decorator.propertyBlock) {
      for (const arg of decorator.propertyBlock.args) {
        const value = this.generateArgValue(arg);
        // Resolve port name for positional argument
        const portName =
          arg.name ?? this.nodeResolver?.getSinglePortName(decoratorId);
        if (portName) {
          attrs += ` ${portName}="${value}"`;
        }
      }
    }

    return `${pad}<${decoratorId}${attrs}>`;
  }

  private generateNodeAttributes(node: NodeStmt, program: Program): string {
    let attrs = "";
    const nodeId = node.nodeName.$refText;

    if (node.propertyBlock) {
      for (const arg of node.propertyBlock.args) {
        const value = this.generateArgValue(arg);
        // Resolve port name for positional argument
        const portName =
          arg.name ?? this.nodeResolver?.getSinglePortName(nodeId);
        if (portName) {
          attrs += ` ${portName}="${value}"`;
        }
      }
    }

    // Add description from outer docs
    const outerDocs = node.outerDocs
      .map((d: string) => d.replace(/^\/\/\/\s*/, "").trim())
      .join(" ");
    if (outerDocs) {
      attrs += ` _description="${this.escapeXml(outerDocs)}"`;
    }

    return attrs;
  }

  private generateArgValue(arg: Argument): string {
    const value = arg.value;

    if (isBlackboardRef(value)) {
      return `{${(value as BlackboardRef).varName.$refText}}`;
    } else if (isStringLiteral(value)) {
      // Langium already strips quotes from STRING terminal
      return this.escapeXml(value.value);
    } else if (isIntLiteral(value)) {
      return String(value.value);
    } else if (isFloatLiteral(value)) {
      return String(value.value);
    } else if (isBoolLiteral(value)) {
      return String(value.value);
    }

    return "";
  }

  private escapeXml(text: string): string {
    return text
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&apos;");
  }
}
