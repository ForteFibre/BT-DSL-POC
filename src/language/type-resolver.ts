/**
 * Type Resolver - Shared type resolution logic for BT DSL.
 * Used by both validator and generator.
 */

import type {
  TreeDef,
  NodeStmt,
  ParamDecl,
  LocalVarDecl,
  Expression,
} from "./generated/ast.js";
import {
  isBlackboardRef,
  isLocalVarDecl,
  isParamDecl,
  isGlobalVarDecl,
  isStringLiteral,
  isIntLiteral,
  isFloatLiteral,
  isBoolLiteral,
  isNodeStmt,
  isLiteralExpr,
  isBinaryExpr,
  isUnaryExpr,
  isVarRefExpr,
} from "./generated/ast.js";
export interface PortLookup {
  getPort(nodeId: string, portName: string): { type?: string } | undefined;
  getSinglePortName(nodeId: string): string | undefined;
}

/**
 * Type context for a single Tree - holds resolved types for parameters and local vars.
 */
export class TreeTypeContext {
  private resolvedTypes = new Map<string, string>();

  setType(name: string, type: string): void {
    this.resolvedTypes.set(name, type);
  }

  getType(name: string): string | undefined {
    return this.resolvedTypes.get(name);
  }

  hasType(name: string): boolean {
    return this.resolvedTypes.has(name);
  }

  getAllTypes(): Map<string, string> {
    return new Map(this.resolvedTypes);
  }
}

/**
 * Resolve types for all parameters and local vars in a tree.
 * @param tree The tree definition
 * @param portLookup Port lookup (DeclareStmt/TreeDef aware)
 * @returns TreeTypeContext with resolved types
 */
export function resolveTreeTypes(
  tree: TreeDef,
  portLookup: PortLookup
): TreeTypeContext {
  const ctx = new TreeTypeContext();

  // Step 1: Register explicit types from parameters
  for (const param of tree.params?.params ?? []) {
    if (param.typeName) {
      ctx.setType(param.name, param.typeName);
    }
  }

  // Step 2: Register types from local vars (explicit type or inferred from initial value)
  for (const localVar of tree.localVars ?? []) {
    if (localVar.typeName) {
      ctx.setType(localVar.name, localVar.typeName);
    } else if (localVar.initialValue) {
      const inferredType = inferTypeFromLiteral(localVar.initialValue);
      if (inferredType) {
        ctx.setType(localVar.name, inferredType);
      }
    }
  }

  // Step 3: Infer types from usage (only if body exists)
  if (tree.body) {
    inferTypesFromNode(tree.body, ctx, portLookup);
  }

  return ctx;
}

/**
 * Infer types by traversing nodes and looking at port definitions.
 */
function inferTypesFromNode(
  node: NodeStmt,
  ctx: TreeTypeContext,
  portLookup: PortLookup
): void {
  // Check arguments in this node
  for (const arg of node.propertyBlock?.args ?? []) {
    if (!isBlackboardRef(arg.value)) continue;

    const varDecl = arg.value.varName.ref;
    // Support type inference for both ParamDecl and LocalVarDecl
    if (!varDecl || (!isParamDecl(varDecl) && !isLocalVarDecl(varDecl)))
      continue;
    if (ctx.hasType(varDecl.name)) continue; // Already resolved

    // Get port name: use explicit name or resolve from single-port node
    const portName =
      arg.name ?? portLookup.getSinglePortName(node.nodeName.$refText);
    if (!portName) continue;

    // Get type from port definition
    const port = portLookup.getPort(node.nodeName.$refText, portName);
    if (port?.type) {
      ctx.setType(varDecl.name, port.type);
    }
  }

  // Recurse into children (only NodeStmt, skip ExpressionStmt)
  for (const child of node.childrenBlock?.children ?? []) {
    if (isNodeStmt(child)) {
      inferTypesFromNode(child, ctx, portLookup);
    }
  }
}

/**
 * Get the resolved type for a parameter.
 * Returns explicit type if present, otherwise inferred type from context.
 */
export function getParamType(
  param: ParamDecl,
  ctx: TreeTypeContext
): string | undefined {
  return param.typeName ?? ctx.getType(param.name);
}

/**
 * Infer type from a literal or LiteralExpr.
 */
function inferTypeFromLiteral(expr: any): string | undefined {
  // Handle LiteralExpr wrapper
  if (isLiteralExpr(expr)) {
    return inferTypeFromLiteral(expr.literal);
  }
  if (isStringLiteral(expr)) return "std::string";
  if (isIntLiteral(expr)) return "int";
  if (isFloatLiteral(expr)) return "double";
  if (isBoolLiteral(expr)) return "bool";
  return undefined;
}

/**
 * Result of expression type inference.
 */
export type ExpressionTypeResult = {
  type: string | undefined;
  error?: string;
};

/**
 * Infer the type of an expression, returning error messages if types are incompatible.
 */
export function inferExpressionType(
  expr: Expression,
  ctx: TreeTypeContext,
  getGlobalVarType: (name: string) => string | undefined
): ExpressionTypeResult {
  if (isBinaryExpr(expr)) {
    const leftResult = inferExpressionType(expr.left, ctx, getGlobalVarType);
    const rightResult = inferExpressionType(expr.right, ctx, getGlobalVarType);

    if (leftResult.error) return leftResult;
    if (rightResult.error) return rightResult;

    const leftType = leftResult.type;
    const rightType = rightResult.type;
    const op = expr.op;

    // Arithmetic operators: +, -, *, /, %
    if (["+", "-", "*", "/", "%"].includes(op)) {
      // String concatenation (+ only)
      if (
        op === "+" &&
        leftType === "std::string" &&
        rightType === "std::string"
      ) {
        return { type: "std::string" };
      }
      // Numeric operations
      if (isNumericType(leftType) && isNumericType(rightType)) {
        // double if either is double
        if (leftType === "double" || rightType === "double") {
          return { type: "double" };
        }
        return { type: "int" };
      }
      return {
        type: undefined,
        error: `Operator '${op}' cannot be applied to types '${leftType}' and '${rightType}'.`,
      };
    }

    // Comparison operators: <, <=, >, >=
    if (["<", "<=", ">", ">="].includes(op)) {
      if (isNumericType(leftType) && isNumericType(rightType)) {
        return { type: "bool" };
      }
      return {
        type: undefined,
        error: `Comparison '${op}' requires numeric types, got '${leftType}' and '${rightType}'.`,
      };
    }

    // Equality operators: ==, !=
    if (["==", "!="].includes(op)) {
      // Allow comparison of same types
      if (leftType === rightType) {
        return { type: "bool" };
      }
      // Allow numeric comparison (int vs double)
      if (isNumericType(leftType) && isNumericType(rightType)) {
        return { type: "bool" };
      }
      return {
        type: undefined,
        error: `Cannot compare '${leftType}' with '${rightType}' using '${op}'.`,
      };
    }

    // Logical operators: &&, ||
    if (["&&", "||"].includes(op)) {
      if (leftType === "bool" && rightType === "bool") {
        return { type: "bool" };
      }
      return {
        type: undefined,
        error: `Logical operator '${op}' requires bool operands, got '${leftType}' and '${rightType}'.`,
      };
    }

    // Bitwise operators: &, |
    if (["&", "|"].includes(op)) {
      if (leftType === "int" && rightType === "int") {
        return { type: "int" };
      }
      return {
        type: undefined,
        error: `Bitwise operator '${op}' requires int operands, got '${leftType}' and '${rightType}'.`,
      };
    }

    return { type: undefined };
  }

  if (isUnaryExpr(expr)) {
    const operandResult = inferExpressionType(
      expr.operand,
      ctx,
      getGlobalVarType
    );
    if (operandResult.error) return operandResult;

    const operandType = operandResult.type;
    const op = expr.op;

    if (op === "!") {
      if (operandType === "bool") {
        return { type: "bool" };
      }
      return {
        type: undefined,
        error: `Operator '!' requires bool operand, got '${operandType}'.`,
      };
    }

    if (op === "-") {
      if (isNumericType(operandType)) {
        return { type: operandType };
      }
      return {
        type: undefined,
        error: `Unary '-' requires numeric operand, got '${operandType}'.`,
      };
    }

    return { type: undefined };
  }

  if (isVarRefExpr(expr)) {
    const varName = expr.varRef.$refText;
    const varDecl = expr.varRef.ref;

    if (varDecl) {
      if (isGlobalVarDecl(varDecl)) {
        return { type: varDecl.typeName };
      }
      if (isLocalVarDecl(varDecl)) {
        return { type: varDecl.typeName ?? ctx.getType(varDecl.name) };
      }
      if (isParamDecl(varDecl)) {
        return { type: ctx.getType(varDecl.name) };
      }
    }

    // Try global var lookup
    const globalType = getGlobalVarType(varName);
    if (globalType) {
      return { type: globalType };
    }

    // Try context
    const ctxType = ctx.getType(varName);
    return { type: ctxType };
  }

  if (isLiteralExpr(expr)) {
    return { type: inferTypeFromLiteral(expr.literal) };
  }

  // Direct literals
  const literalType = inferTypeFromLiteral(expr);
  if (literalType) {
    return { type: literalType };
  }

  return { type: undefined };
}

/**
 * Check if type is numeric (int or double).
 */
function isNumericType(type: string | undefined): boolean {
  return type === "int" || type === "double";
}
