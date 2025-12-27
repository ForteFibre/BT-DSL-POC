import type { AstNodeDescription, ReferenceInfo, Scope } from "langium";
import { AstUtils, DefaultScopeProvider, EMPTY_SCOPE, MapScope } from "langium";
import { computeBtImportClosureUris } from "./import-closure.js";
import { isDecorator, isProgram, isTreeDef } from "./generated/ast.js";
import type { BtDslServices } from "./bt-dsl-module.js";

/**
 * Custom scope provider for BT DSL.
 * Handles resolution of variable references to:
 * 1. Global Blackboard variables
 * 2. Tree parameters (local scope)
 */
export class BtDslScopeProvider extends DefaultScopeProvider {
  private readonly btServices: BtDslServices;

  constructor(services: BtDslServices) {
    super(services);
    this.btServices = services;
  }

  override getScope(context: ReferenceInfo): Scope {
    // 1) NamedElement references (BlackboardRef.varName)
    if (context.property === "varName") {
      return this.getVarNameScope(context);
    }

    // 2) Node name references (NodeStmt.nodeName): include manifest and declared nodes
    if (context.property === "nodeName") {
      const document = AstUtils.getDocument(context.container);
      const visibleUris = computeBtImportClosureUris(this.btServices, document);

      // TreeDefs are part of NodeNameTarget and are provided by IndexManager.
      // We must filter them to the import-closure to avoid workspace-wide visibility.
      const treeDescs =
        this.btServices.shared.workspace.IndexManager.allElements("TreeDef")
          .filter((d) => visibleUris.has(d.documentUri.toString()))
          .toArray();

      const declared = this.createDeclaredNodeNameDescriptions(visibleUris);
      const combined = [...declared, ...treeDescs];

      return combined.length > 0 ? new MapScope(combined) : EMPTY_SCOPE;
    }

    // 3) Decorator references (Decorator.name): include manifest and declared nodes
    if (context.property === "name" && isDecorator(context.container)) {
      const document = AstUtils.getDocument(context.container);
      const visibleUris = computeBtImportClosureUris(this.btServices, document);
      const declared = this.createDeclaredDecoratorDescriptions(visibleUris);
      return declared.length > 0 ? new MapScope(declared) : EMPTY_SCOPE;
    }

    return super.getScope(context);
  }

  private getVarNameScope(context: ReferenceInfo): Scope {
    const descriptions = this.btServices.workspace.AstNodeDescriptionProvider;
    const scopes: ReturnType<typeof descriptions.createDescription>[] = [];

    // 1. Find containing Tree and add its parameters and local vars to scope
    const tree = AstUtils.getContainerOfType(context.container, isTreeDef);
    if (tree) {
      // Add parameters
      if (tree.params) {
        for (const param of tree.params.params) {
          scopes.push(descriptions.createDescription(param, param.name));
        }
      }
      // Add local variables
      for (const localVar of tree.localVars ?? []) {
        scopes.push(descriptions.createDescription(localVar, localVar.name));
      }
    }

    // 2. Find Program and add global variables to scope
    const program = AstUtils.getContainerOfType(context.container, isProgram);
    if (program) {
      for (const v of program.globalVars ?? []) {
        scopes.push(descriptions.createDescription(v, v.name));
      }
    }

    if (scopes.length === 0) {
      return EMPTY_SCOPE;
    }

    return new MapScope(scopes);
  }

  /**
   * Create scope descriptions for DSL-declared nodes (non-Decorator categories).
   * These are nodes declared with `declare Action ...`, `declare Control ...`, etc.
   * Includes declarations from ALL workspace documents (including builtin-nodes.bt).
   */
  private createDeclaredNodeNameDescriptions(
    visibleUris: Set<string>
  ): AstNodeDescription[] {
    const out: AstNodeDescription[] = [];

    // Include declarations from ALL workspace documents
    for (const doc of this.btServices.shared.workspace.LangiumDocuments.all) {
      if (!visibleUris.has(doc.uri.toString())) continue;
      const program = doc.parseResult.value;
      if (!isProgram(program)) continue;

      for (const decl of program.declarations ?? []) {
        // Exclude Decorator from NodeStmt.nodeName scope
        if (decl.category === "Decorator") continue;

        // Use manifest-like types to match existing type filtering
        const $type = `Manifest${decl.category}` as
          | "ManifestAction"
          | "ManifestCondition"
          | "ManifestControl"
          | "ManifestSubTree";

        out.push({
          node: decl,
          name: decl.name,
          type: $type,
          documentUri: doc.uri,
          path: this.btServices.workspace.AstNodeLocator.getAstNodePath(decl),
        } satisfies AstNodeDescription);
      }
    }

    return out;
  }

  /**
   * Create scope descriptions for DSL-declared Decorator nodes.
   * Includes declarations from ALL workspace documents (including builtin-nodes.bt).
   */
  private createDeclaredDecoratorDescriptions(
    visibleUris: Set<string>
  ): AstNodeDescription[] {
    const out: AstNodeDescription[] = [];

    // Include declarations from ALL workspace documents
    for (const doc of this.btServices.shared.workspace.LangiumDocuments.all) {
      if (!visibleUris.has(doc.uri.toString())) continue;
      const program = doc.parseResult.value;
      if (!isProgram(program)) continue;

      for (const decl of program.declarations ?? []) {
        if (decl.category !== "Decorator") continue;

        out.push({
          node: decl,
          name: decl.name,
          type: "ManifestDecorator",
          documentUri: doc.uri,
          path: this.btServices.workspace.AstNodeLocator.getAstNodePath(decl),
        } satisfies AstNodeDescription);
      }
    }

    return out;
  }
}
