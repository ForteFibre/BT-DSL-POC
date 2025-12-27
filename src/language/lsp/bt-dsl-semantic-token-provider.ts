/******************************************************************************
 * Semantic Token Provider for BT DSL
 * Provides AST-aware syntax highlighting via Language Server Protocol.
 ******************************************************************************/

import { AstNode } from "langium";
import {
  AbstractSemanticTokenProvider,
  SemanticTokenAcceptor,
} from "langium/lsp";
import {
  SemanticTokenModifiers,
  SemanticTokenTypes,
} from "vscode-languageserver";
import {
  Argument,
  AssignmentExpr,
  BlackboardRef,
  DeclarePort,
  DeclareStmt,
  Decorator,
  GlobalVarDecl,
  isArgument,
  isAssignmentExpr,
  isBlackboardRef,
  isDeclarePort,
  isDecorator,
  isDeclareStmt,
  isGlobalVarDecl,
  isLocalVarDecl,
  isManifestAction,
  isManifestCondition,
  isManifestControl,
  isManifestDecorator,
  isManifestSubTree,
  isNodeStmt,
  isParamDecl,
  isTreeDef,
  isVarRefExpr,
  LocalVarDecl,
  NodeStmt,
  ParamDecl,
  TreeDef,
  VarRefExpr,
} from "../generated/ast.js";

/**
 * Semantic Token Provider for BT DSL.
 *
 * This provider enhances syntax highlighting by using AST information
 * to provide more accurate token types than regex-based TextMate grammars.
 */
export class BtDslSemanticTokenProvider extends AbstractSemanticTokenProvider {
  protected highlightElement(
    node: AstNode,
    acceptor: SemanticTokenAcceptor
  ): void {
    // Declare statement (manifest-like node signature)
    if (isDeclareStmt(node)) {
      this.highlightDeclareStmt(node, acceptor);
      return;
    }

    // Declare port (in/out/ref name: Type)
    if (isDeclarePort(node)) {
      this.highlightDeclarePort(node, acceptor);
      return;
    }

    // Tree definition name
    if (isTreeDef(node)) {
      this.highlightTreeDef(node, acceptor);
      return;
    }

    // Node statement (node call)
    if (isNodeStmt(node)) {
      this.highlightNodeStmt(node, acceptor);
      return;
    }

    // Decorator (@Repeat, @Inverter, etc.)
    if (isDecorator(node)) {
      this.highlightDecorator(node, acceptor);
      return;
    }

    // Global variable declaration
    if (isGlobalVarDecl(node)) {
      this.highlightGlobalVarDecl(node, acceptor);
      return;
    }

    // Local variable declaration
    if (isLocalVarDecl(node)) {
      this.highlightLocalVarDecl(node, acceptor);
      return;
    }

    // Parameter declaration
    if (isParamDecl(node)) {
      this.highlightParamDecl(node, acceptor);
      return;
    }

    // Blackboard reference (variable reference in node arguments)
    if (isBlackboardRef(node)) {
      this.highlightBlackboardRef(node, acceptor);
      return;
    }

    // Variable reference in expressions
    if (isVarRefExpr(node)) {
      this.highlightVarRefExpr(node, acceptor);
      return;
    }

    // Named argument (key: value)
    if (isArgument(node)) {
      this.highlightArgument(node, acceptor);
      return;
    }

    // Assignment target
    if (isAssignmentExpr(node)) {
      this.highlightAssignmentExpr(node, acceptor);
    }
  }

  /**
   * Highlight Tree definition name.
   */
  private highlightTreeDef(
    node: TreeDef,
    acceptor: SemanticTokenAcceptor
  ): void {
    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.function,
      modifier: SemanticTokenModifiers.declaration,
    });

    // Highlight parameter type annotations
    for (const param of node.params?.params ?? []) {
      if (param.typeName) {
        acceptor({
          node: param,
          property: "typeName",
          type: SemanticTokenTypes.type,
        });
      }
    }
  }

  /**
   * Highlight declare statement.
   *
   * Example:
   *   declare Action MoveTo(in target: Vector3)
   */
  private highlightDeclareStmt(
    node: DeclareStmt,
    acceptor: SemanticTokenAcceptor
  ): void {
    // Highlight the `declare` keyword itself (some themes barely color TextMate keywords)
    acceptor({
      node,
      keyword: "declare",
      type: SemanticTokenTypes.keyword,
    });

    // Category: Action / Condition / Control / Decorator / SubTree
    // Treat as a type name to make it pop.
    acceptor({
      node,
      property: "category",
      type: SemanticTokenTypes.type,
      modifier: SemanticTokenModifiers.defaultLibrary,
    });

    // Declared node name behaves like a function signature.
    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.function,
      modifier: SemanticTokenModifiers.declaration,
    });
  }

  /**
   * Highlight declare port.
   *
   * Example:
   *   in target: Vector3
   */
  private highlightDeclarePort(
    node: DeclarePort,
    acceptor: SemanticTokenAcceptor
  ): void {
    if (node.direction) {
      acceptor({
        node,
        property: "direction",
        type: SemanticTokenTypes.keyword,
      });
    }

    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.parameter,
      modifier: SemanticTokenModifiers.declaration,
    });

    acceptor({
      node,
      property: "typeName",
      type: SemanticTokenTypes.type,
    });
  }

  /**
   * Highlight node statement (node call).
   * Differentiates by node category from manifest.
   */
  private highlightNodeStmt(
    node: NodeStmt,
    acceptor: SemanticTokenAcceptor
  ): void {
    // NOTE:
    // `nodeName` cross-references are declared as a union type for scope filtering.
    // In our post-XML world, those references are resolved to actual `DeclareStmt` nodes.
    // The generated TypeScript types for the union don't reflect that, so we widen here.
    const target = node.nodeName.ref as any;

    if (!target) {
      // Unresolved reference - still highlight as function
      acceptor({
        node,
        property: "nodeName",
        type: SemanticTokenTypes.function,
      });
      return;
    }

    // Decide token type/modifiers, then emit exactly once.
    // This avoids duplicated-branch warnings while keeping behavior identical.
    let type = SemanticTokenTypes.function;
    let modifier: SemanticTokenModifiers | undefined;

    if (isDeclareStmt(target)) {
      // Declared nodes (manifest definitions in .bt)
      modifier = SemanticTokenModifiers.defaultLibrary;
      if (target.category === "Control") {
        type = SemanticTokenTypes.keyword;
      }
    } else if (isManifestControl(target)) {
      // Control nodes (Sequence, Selector, Parallel, etc.)
      type = SemanticTokenTypes.keyword;
      modifier = SemanticTokenModifiers.defaultLibrary;
    } else if (
      isManifestAction(target) ||
      isManifestCondition(target) ||
      isManifestSubTree(target)
    ) {
      // Other manifest-provided nodes
      modifier = SemanticTokenModifiers.defaultLibrary;
    } else {
      // TreeDef and any other fallbacks stay as plain function.
      // (TreeDef is intentionally not marked as defaultLibrary.)
    }

    acceptor({
      node,
      property: "nodeName",
      type,
      modifier,
    });
  }

  /**
   * Highlight decorator.
   */
  private highlightDecorator(
    node: Decorator,
    acceptor: SemanticTokenAcceptor
  ): void {
    // See note in highlightNodeStmt about cross-reference typing.
    const target = node.name.ref as any;

    // Highlight the @ symbol
    acceptor({
      node,
      keyword: "@",
      type: SemanticTokenTypes.decorator,
    });

    // Highlight the decorator name
    const isDefaultDecorator =
      (isDeclareStmt(target) && target.category === "Decorator") ||
      isManifestDecorator(target);

    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.decorator,
      modifier: isDefaultDecorator
        ? SemanticTokenModifiers.defaultLibrary
        : undefined,
    });
  }

  /**
   * Highlight global variable declaration.
   */
  private highlightGlobalVarDecl(
    node: GlobalVarDecl,
    acceptor: SemanticTokenAcceptor
  ): void {
    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.variable,
      modifier: SemanticTokenModifiers.declaration,
    });

    // Type annotation
    acceptor({
      node,
      property: "typeName",
      type: SemanticTokenTypes.type,
    });
  }

  /**
   * Highlight local variable declaration.
   */
  private highlightLocalVarDecl(
    node: LocalVarDecl,
    acceptor: SemanticTokenAcceptor
  ): void {
    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.variable,
      modifier: SemanticTokenModifiers.declaration,
    });

    // Type annotation (optional)
    if (node.typeName) {
      acceptor({
        node,
        property: "typeName",
        type: SemanticTokenTypes.type,
      });
    }
  }

  /**
   * Highlight parameter declaration.
   */
  private highlightParamDecl(
    node: ParamDecl,
    acceptor: SemanticTokenAcceptor
  ): void {
    // Direction keyword (in, out, ref)
    if (node.direction) {
      acceptor({
        node,
        property: "direction",
        type: SemanticTokenTypes.keyword,
      });
    }

    acceptor({
      node,
      property: "name",
      type: SemanticTokenTypes.parameter,
      modifier: SemanticTokenModifiers.declaration,
    });

    // Type annotation (optional)
    if (node.typeName) {
      acceptor({
        node,
        property: "typeName",
        type: SemanticTokenTypes.type,
      });
    }
  }

  /**
   * Highlight blackboard reference (variable reference in node arguments).
   */
  private highlightBlackboardRef(
    node: BlackboardRef,
    acceptor: SemanticTokenAcceptor
  ): void {
    // Direction keyword (in, out, ref)
    if (node.direction) {
      acceptor({
        node,
        property: "direction",
        type: SemanticTokenTypes.keyword,
      });
    }

    const target = node.varName.ref;

    if (isParamDecl(target)) {
      acceptor({
        node,
        property: "varName",
        type: SemanticTokenTypes.parameter,
      });
    } else {
      acceptor({
        node,
        property: "varName",
        type: SemanticTokenTypes.variable,
      });
    }
  }

  /**
   * Highlight variable reference in expressions.
   */
  private highlightVarRefExpr(
    node: VarRefExpr,
    acceptor: SemanticTokenAcceptor
  ): void {
    const target = node.varRef.ref;

    if (isParamDecl(target)) {
      acceptor({
        node,
        property: "varRef",
        type: SemanticTokenTypes.parameter,
      });
    } else {
      acceptor({
        node,
        property: "varRef",
        type: SemanticTokenTypes.variable,
      });
    }
  }

  /**
   * Highlight named argument key.
   */
  private highlightArgument(
    node: Argument,
    acceptor: SemanticTokenAcceptor
  ): void {
    if (node.name) {
      acceptor({
        node,
        property: "name",
        type: SemanticTokenTypes.parameter,
      });
    }
  }

  /**
   * Highlight assignment expression target.
   */
  private highlightAssignmentExpr(
    node: AssignmentExpr,
    acceptor: SemanticTokenAcceptor
  ): void {
    const target = node.target.ref;

    if (isParamDecl(target)) {
      acceptor({
        node,
        property: "target",
        type: SemanticTokenTypes.parameter,
      });
    } else {
      acceptor({
        node,
        property: "target",
        type: SemanticTokenTypes.variable,
      });
    }
  }
}
