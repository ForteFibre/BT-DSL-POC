import type {
  ValidationAcceptor,
  ValidationChecks,
  AstNodeDescription,
} from "langium";
import { AstUtils, UriUtils } from "langium";
import type { CancellationToken } from "vscode-jsonrpc";
import type {
  BtDslAstType,
  Decorator,
  Program,
  TreeDef,
  NodeStmt,
  BlackboardRef,
  ParamDecl,
  GlobalVarDecl,
  LocalVarDecl,
  Argument,
  ExpressionStmt,
  DeclareStmt,
} from "./generated/ast.js";
import {
  isBlackboardRef,
  isGlobalVarDecl,
  isLocalVarDecl,
  isParamDecl,
  isTreeDef,
  isStringLiteral,
  isIntLiteral,
  isFloatLiteral,
  isBoolLiteral,
  isNodeStmt,
  isLiteralExpr,
  isExpressionStmt,
} from "./generated/ast.js";
import type { BtDslServices } from "./bt-dsl-module.js";
import {
  TreeTypeContext,
  resolveTreeTypes,
  inferExpressionType,
} from "./type-resolver.js";
import { BtNodeResolver } from "./node-resolver.js";
import { computeBtImportClosureUris } from "./import-closure.js";

/**
 * Register custom validation checks.
 */
export function registerValidationChecks(services: BtDslServices): void {
  const registry = services.validation.ValidationRegistry;
  const validator = services.validation.BtDslValidator;
  const checks: ValidationChecks<BtDslAstType> = {
    Program: validator.checkProgram.bind(validator),
    TreeDef: validator.checkTreeDef.bind(validator),
    NodeStmt: validator.checkNodeStmt.bind(validator),
    Decorator: validator.checkDecorator.bind(validator),
    BlackboardRef: validator.checkBlackboardRef.bind(validator),
    LocalVarDecl: validator.checkLocalVarDecl.bind(validator),
    DeclareStmt: validator.checkDeclareStmt.bind(validator),
  };
  registry.register(checks, validator);
}

/**
 * Global registry mapping Tree names to their type contexts.
 */
class TreeTypeRegistry {
  private treeTypes = new Map<string, TreeTypeContext>();
  private treeParamOrder = new Map<string, string[]>();

  register(treeName: string, ctx: TreeTypeContext, paramOrder: string[]): void {
    this.treeTypes.set(treeName, ctx);
    this.treeParamOrder.set(treeName, paramOrder);
  }

  getTypeContext(treeName: string): TreeTypeContext | undefined {
    return this.treeTypes.get(treeName);
  }

  getParamOrder(treeName: string): string[] | undefined {
    return this.treeParamOrder.get(treeName);
  }

  hasTree(treeName: string): boolean {
    return this.treeTypes.has(treeName);
  }

  clear(): void {
    this.treeTypes.clear();
    this.treeParamOrder.clear();
  }
}

/**
 * Implementation of custom validations with 2-pass type system.
 */
export class BtDslValidator {
  private readonly nodeResolver: BtNodeResolver;
  private treeTypeRegistry = new TreeTypeRegistry();
  private readonly services: BtDslServices;

  constructor(services: BtDslServices) {
    this.services = services;
    this.nodeResolver = new BtNodeResolver(services);
  }

  private treeDescriptions(): AstNodeDescription[] {
    // Tree definitions are exported by the default ScopeComputation and indexed workspace-wide.
    return this.services.shared.workspace.IndexManager.allElements(
      "TreeDef"
    ).toArray();
  }

  private treeDescriptionsByName(name: string): AstNodeDescription[] {
    return this.services.shared.workspace.IndexManager.allElements("TreeDef")
      .filter((d) => d.name === name)
      .toArray();
  }

  private declareDescriptions(): AstNodeDescription[] {
    return this.services.shared.workspace.IndexManager.allElements(
      "DeclareStmt"
    ).toArray();
  }

  private filterToVisibleUris(
    descs: AstNodeDescription[],
    visibleUris: Set<string>
  ): AstNodeDescription[] {
    return descs.filter((d) => visibleUris.has(d.documentUri.toString()));
  }

  private declareDescriptionsByName(name: string): AstNodeDescription[] {
    return this.services.shared.workspace.IndexManager.allElements(
      "DeclareStmt"
    )
      .filter((d) => d.name === name)
      .toArray();
  }

  private hasTreeInWorkspace(name: string): boolean {
    return this.services.shared.workspace.IndexManager.allElements(
      "TreeDef"
    ).some((d) => d.name === name);
  }

  private resolveTreeDef(desc: AstNodeDescription): TreeDef | undefined {
    if (desc.node && isTreeDef(desc.node)) {
      return desc.node;
    }
    const doc = this.services.shared.workspace.LangiumDocuments.getDocument(
      desc.documentUri
    );
    if (!doc) return undefined;
    const astNode = this.services.workspace.AstNodeLocator.getAstNode(
      doc.parseResult.value,
      desc.path
    );
    return isTreeDef(astNode) ? astNode : undefined;
  }

  /**
   * Check Program-level constraints
   */
  async checkProgram(
    program: Program,
    accept: ValidationAcceptor,
    cancelToken?: CancellationToken
  ): Promise<void> {
    // Visibility policy:
    // During validation we only consider the current document + its transitive `.bt` imports
    // (+ builtin-nodes.bt). Unimported workspace files should not affect diagnostics.
    const programDoc = AstUtils.getDocument(program);
    const visibleUris = computeBtImportClosureUris(this.services, programDoc);

    // XML-based manifest imports were removed. Only `.bt` imports are supported.
    for (const imp of program.imports) {
      const lower = imp.path.toLowerCase();
      if (lower.endsWith(".xml")) {
        accept(
          "error",
          `XML manifest imports are no longer supported: '${imp.path}'. Convert it to '.bt' declarations using the CLI (e.g. 'bt-dsl manifest:xml-to-bt ...') and import the resulting .bt file instead.`,
          { node: imp, property: "path" }
        );
      } else if (!lower.endsWith(".bt")) {
        accept(
          "warning",
          `Unknown import type: '${imp.path}'. Supported: .bt`,
          { node: imp, property: "path" }
        );
      } else {
        // `.bt` import: verify the target exists so users get a direct error on the import statement.
        // (Unresolved refs elsewhere are often much harder to interpret.)
        if (programDoc.uri.scheme === "file") {
          const baseDir = UriUtils.dirname(programDoc.uri);
          const candidates = normalizeImportCandidates(imp.path).filter((p) =>
            p.toLowerCase().endsWith(".bt")
          );

          let found = false;
          for (const candidate of candidates) {
            const resolved = UriUtils.resolvePath(baseDir, candidate);
            if (resolved.scheme !== "file") continue;

            try {
              await this.services.shared.workspace.FileSystemProvider.readFile(
                resolved
              );
              found = true;
              break;
            } catch {
              // keep trying candidates
            }

            if (cancelToken?.isCancellationRequested) {
              return;
            }
          }

          if (!found) {
            accept("error", `Imported file not found: '${imp.path}'`, {
              node: imp,
              property: "path",
            });
          }
        }
      }
    }

    // Clear tree type registry for fresh validation
    this.treeTypeRegistry.clear();

    // PHASE 1: Resolve types for all trees first (so SubTree calls can reference them)
    for (const tree of program.trees) {
      const ctx = this.resolveTypes(tree, accept);

      // Store in registry for SubTree call checking
      const paramOrder = tree.params?.params.map((p) => p.name) ?? [];
      this.treeTypeRegistry.register(tree.name, ctx, paramOrder);
    }

    // Check for duplicate tree names within the same document.
    const localTreeNames = new Set<string>();
    for (const tree of program.trees) {
      if (localTreeNames.has(tree.name)) {
        accept("error", `Duplicate tree name: '${tree.name}'`, {
          node: tree,
          property: "name",
        });
      }
      localTreeNames.add(tree.name);
    }

    // Check for duplicate tree names across the workspace.
    // Immediate error policy: any same-name TreeDef in a different document is an error.
    const documentUri = AstUtils.getDocument(program).uri.toString();
    const allTrees = this.filterToVisibleUris(
      this.treeDescriptions(),
      visibleUris
    );
    const byName = new Map<string, AstNodeDescription[]>();
    for (const d of allTrees) {
      const list = byName.get(d.name);
      if (list) {
        list.push(d);
      } else {
        byName.set(d.name, [d]);
      }
    }
    for (const tree of program.trees) {
      const others = (byName.get(tree.name) ?? []).filter(
        (d) => d.documentUri.toString() !== documentUri
      );
      if (others.length > 0) {
        const otherUris = others
          .map((o) => o.documentUri.toString())
          .join(", ");
        accept(
          "error",
          `Duplicate tree name across workspace: '${tree.name}' (also defined in ${otherUris})`,
          { node: tree, property: "name" }
        );
      }
    }

    // Check for name collisions between TreeDefs and declared nodes across the workspace.
    // Policy: any collision is an error (ambiguous reference).
    for (const tree of program.trees) {
      const decls = this.filterToVisibleUris(
        this.declareDescriptionsByName(tree.name),
        visibleUris
      );
      if (decls.length > 0) {
        const otherUris = decls.map((d) => d.documentUri.toString()).join(", ");
        accept(
          "error",
          `Tree name '${tree.name}' conflicts with a declared node (declare ...). Rename the tree or the declaration to avoid ambiguity. (${otherUris})`,
          { node: tree, property: "name" }
        );
      }
    }

    // Check for duplicate global variables
    const globalVarNames = new Set<string>();
    for (const v of program.globalVars ?? []) {
      if (globalVarNames.has(v.name)) {
        accept("error", `Duplicate global variable: '${v.name}'`, {
          node: v,
          property: "name",
        });
      }
      globalVarNames.add(v.name);
    }

    // Check for duplicate declaration names
    const declareNames = new Set<string>();
    for (const decl of program.declarations ?? []) {
      if (declareNames.has(decl.name)) {
        accept("error", `Duplicate declaration: '${decl.name}'`, {
          node: decl,
          property: "name",
        });
      }
      declareNames.add(decl.name);

      // Check collision with TreeDef names
      if (localTreeNames.has(decl.name)) {
        accept(
          "error",
          `Declaration '${decl.name}' conflicts with a Tree definition. Rename one of them.`,
          { node: decl, property: "name" }
        );
      }
    }

    // Check for duplicate declaration names across the workspace.
    const declDocumentUri = AstUtils.getDocument(program).uri.toString();
    const allDecls = this.filterToVisibleUris(
      this.declareDescriptions(),
      visibleUris
    );
    const declsByName = new Map<string, AstNodeDescription[]>();
    for (const d of allDecls) {
      const list = declsByName.get(d.name);
      if (list) list.push(d);
      else declsByName.set(d.name, [d]);
    }
    for (const decl of program.declarations ?? []) {
      const others = (declsByName.get(decl.name) ?? []).filter(
        (d) => d.documentUri.toString() !== declDocumentUri
      );
      if (others.length > 0) {
        const otherUris = others
          .map((o) => o.documentUri.toString())
          .join(", ");
        accept(
          "error",
          `Duplicate declaration across workspace: '${decl.name}' (also defined in ${otherUris})`,
          { node: decl, property: "name" }
        );
      }
    }
  }

  /**
   * Check Tree definition constraints with 2-pass type system.
   */
  checkTreeDef(tree: TreeDef, accept: ValidationAcceptor): void {
    // Check for duplicate parameter names
    if (tree.params) {
      const paramNames = new Set<string>();
      for (const param of tree.params.params) {
        if (paramNames.has(param.name)) {
          accept("error", `Duplicate parameter name: '${param.name}'`, {
            node: param,
            property: "name",
          });
        }
        paramNames.add(param.name);
      }
    }

    // Check for duplicate local variable names
    const localVarNames = new Set<string>();
    for (const v of tree.localVars ?? []) {
      if (localVarNames.has(v.name)) {
        accept("error", `Duplicate local variable: '${v.name}'`, {
          node: v,
          property: "name",
        });
      }
      localVarNames.add(v.name);

      // Check that local var has either type or initial value
      if (!v.typeName && !v.initialValue) {
        accept(
          "error",
          `Local variable '${v.name}' must have either a type annotation or an initial value.`,
          { node: v, property: "name" }
        );
      }
    }

    // Get or create type context (may already be created in checkProgram)
    let ctx = this.treeTypeRegistry.getTypeContext(tree.name);
    if (!ctx) {
      ctx = this.resolveTypes(tree, accept);
      const paramOrder = tree.params?.params.map((p) => p.name) ?? [];
      this.treeTypeRegistry.register(tree.name, ctx, paramOrder);
    }

    // PASS 2: Type Checking
    this.checkTypes(tree, ctx, accept);

    // PASS 3: Check for unused ref/out parameters
    this.checkWriteParamUsage(tree, accept);
  }

  /**
   * Check if ref/out parameters are actually used for write access.
   * Warns if a writable parameter is never written to.
   */
  private checkWriteParamUsage(
    tree: TreeDef,
    accept: ValidationAcceptor
  ): void {
    const writeParams = (tree.params?.params ?? []).filter(
      (p) => p.direction === "ref" || p.direction === "out"
    );
    if (writeParams.length === 0) return;

    // Track which write params are used with write access
    const usedForWrite = new Set<string>();
    this.collectWriteUsages(tree.body, usedForWrite);

    // Warn for write params never used for write
    for (const param of writeParams) {
      if (!usedForWrite.has(param.name)) {
        accept(
          "warning",
          `Parameter '${param.name}' has '${param.direction}' direction but is never used for write access. ` +
            "Consider using 'in' (or omit direction) if write access is not needed.",
          { node: param, property: "direction" }
        );
      }
    }
  }

  /**
   * Collect parameter names that are used for write access (ref/out).
   */
  private collectWriteUsages(node: NodeStmt, usedForWrite: Set<string>): void {
    for (const arg of node.propertyBlock?.args ?? []) {
      if (!isBlackboardRef(arg.value)) continue;
      const dir = arg.value.direction;
      if (dir !== "ref" && dir !== "out") continue;

      const varDecl = arg.value.varName.ref;
      if (varDecl && isParamDecl(varDecl)) {
        usedForWrite.add(varDecl.name);
      }
    }

    for (const child of node.childrenBlock?.children ?? []) {
      if (isNodeStmt(child)) {
        this.collectWriteUsages(child, usedForWrite);
      }
    }
  }

  // =========================================================================
  // PASS 1: Type Resolution
  // =========================================================================

  /**
   * Resolve types for all parameters in the tree using shared resolver.
   */
  private resolveTypes(
    tree: TreeDef,
    accept: ValidationAcceptor
  ): TreeTypeContext {
    const ctx = resolveTreeTypes(tree, this.nodeResolver);

    // Report unresolved parameters as errors
    for (const param of tree.params?.params ?? []) {
      if (!ctx.hasType(param.name)) {
        accept(
          "error",
          `Cannot infer type for parameter '${param.name}'. Add an explicit type annotation.`,
          { node: param, property: "name" }
        );
      }
    }

    return ctx;
  }

  // =========================================================================
  // PASS 2: Type Checking
  // =========================================================================

  /**
   * Check types against port definitions.
   */
  private checkTypes(
    tree: TreeDef,
    ctx: TreeTypeContext,
    accept: ValidationAcceptor
  ): void {
    this.checkNodeTypes(tree.body, ctx, accept);
  }

  /**
   * Check types in a node and its children.
   */
  private checkNodeTypes(
    node: NodeStmt,
    ctx: TreeTypeContext,
    accept: ValidationAcceptor
  ): void {
    // Skip if nodeName is missing (parse error)
    if (!node.nodeName) return;

    const nodeId = node.nodeName.$refText;

    // Conflict policy: fail fast with a clear error.
    if (this.nodeResolver.isAmbiguousName(nodeId)) {
      accept(
        "error",
        `Ambiguous node name '${nodeId}': it exists both as a TreeDef and as a declared node (declare ...). Rename one of them.`,
        { node, property: "nodeName" }
      );
    }

    // NOTE:
    // - nodeName / decorator.name are cross-references.
    // - When a symbol isn't available (missing manifest import, typo, etc.), Langium's linker
    //   already reports an error ("Could not resolve reference...").
    // - Emitting additional validator warnings here creates duplicate/contradicting diagnostics.

    // Unified node call checking using NodeResolver
    this.checkNodeCall(node, ctx, accept);

    // Recurse into children
    for (const child of node.childrenBlock?.children ?? []) {
      if (isNodeStmt(child)) {
        this.checkNodeTypes(child, ctx, accept);
      } else if (isExpressionStmt(child)) {
        this.checkExpressionStmt(child, ctx, accept);
      }
    }
  }

  /**
   * Check expression statement for type correctness.
   */
  private checkExpressionStmt(
    stmt: ExpressionStmt,
    ctx: TreeTypeContext,
    accept: ValidationAcceptor
  ): void {
    const assign = stmt.assignment;
    if (!assign) return;

    // Get target variable type
    const targetName = assign.target.$refText;
    const targetDecl = assign.target.ref;

    let targetType: string | undefined;
    if (targetDecl) {
      if (isGlobalVarDecl(targetDecl)) {
        targetType = targetDecl.typeName;
      } else if (isLocalVarDecl(targetDecl)) {
        targetType = targetDecl.typeName ?? ctx.getType(targetDecl.name);
      } else if (isParamDecl(targetDecl)) {
        targetType = ctx.getType(targetDecl.name);
      }
    }

    // Create global var type lookup function
    const program = this.findProgram(stmt);
    const getGlobalVarType = (name: string): string | undefined => {
      if (!program) return undefined;
      const globalVar = program.globalVars?.find((v) => v.name === name);
      return globalVar?.typeName;
    };

    // Get expression type and check for errors
    const exprResult = inferExpressionType(assign.value, ctx, getGlobalVarType);

    // Report expression type errors
    if (exprResult.error) {
      accept("error", exprResult.error, { node: stmt, property: "assignment" });
      return;
    }

    const exprType = exprResult.type;

    // Check assignment type compatibility
    if (targetType && exprType && targetType !== exprType) {
      // Allow int/double coercion
      const isNumericCoercion =
        (targetType === "int" || targetType === "double") &&
        (exprType === "int" || exprType === "double");

      if (!isNumericCoercion) {
        accept(
          "error",
          `Cannot assign '${exprType}' to '${targetName}' of type '${targetType}'.`,
          { node: stmt, property: "assignment" }
        );
      }
    }
  }

  /**
   * Unified node call checking - works for both SubTree and Manifest calls.
   * Uses NodeResolver to get port/param info uniformly.
   */
  private checkNodeCall(
    node: NodeStmt,
    callerCtx: TreeTypeContext,
    accept: ValidationAcceptor
  ): void {
    const nodeId = node.nodeName.$refText;
    const nodeInfo = this.nodeResolver.getNode(nodeId);
    if (!nodeInfo) return; // Unknown node - linker already reports

    for (const arg of node.propertyBlock?.args ?? []) {
      // Resolve port name: explicit or single-port positional
      const portName = arg.name ?? this.nodeResolver.getSinglePortName(nodeId);
      if (!portName) continue;

      const port = nodeInfo.ports.get(portName);
      if (!port) {
        accept("warning", `Unknown port '${portName}' for node '${nodeId}'.`, {
          node: arg,
          property: arg.name !== undefined ? "name" : "value",
        });
        continue;
      }

      // Type check
      if (isBlackboardRef(arg.value)) {
        const varDecl = arg.value.varName.ref;
        if (varDecl) {
          const varType = this.getResolvedType(varDecl, callerCtx);
          if (varType && port.type && varType !== port.type) {
            accept(
              "error",
              `Type mismatch: '${varDecl.name}' is '${varType}' but port '${portName}' expects '${port.type}'.`,
              { node: arg.value, property: "varName" }
            );
          }
        }
      }

      // Direction check
      this.checkDirectionCompatibility(arg, port, portName, accept);
    }
  }

  /**
   * Check if argument direction is compatible with port/param direction.
   */
  private checkDirectionCompatibility(
    arg: Argument,
    port: { direction: "in" | "out" | "ref" },
    portName: string,
    accept: ValidationAcceptor
  ): void {
    if (!isBlackboardRef(arg.value)) return;

    const argDir = arg.value.direction ?? "in"; // default to 'in'
    const portDir = port.direction;

    // Compatibility matrix:
    // in->in OK, in->out ERROR, in->ref ERROR
    // out->in ERROR, out->out OK, out->ref ERROR
    // ref->in WARN, ref->out WARN, ref->ref OK

    if (argDir === "in" && (portDir === "out" || portDir === "ref")) {
      accept(
        "error",
        `Port '${portName}' requires '${portDir}' but argument is 'in'. Add '${portDir}' to enable write access.`,
        { node: arg.value, property: "direction" }
      );
    } else if (argDir === "out" && (portDir === "in" || portDir === "ref")) {
      accept(
        "error",
        `Port '${portName}' is '${portDir}' but argument is 'out'. ${portDir === "in" ? "Remove 'out' since this is read-only." : "Use 'ref' for read-write."}`,
        { node: arg.value, property: "direction" }
      );
    } else if (argDir === "ref" && portDir === "in") {
      accept(
        "warning",
        `Port '${portName}' is input-only. 'ref' write access will be ignored.`,
        { node: arg.value, property: "direction" }
      );
    } else if (argDir === "ref" && portDir === "out") {
      accept(
        "warning",
        `Port '${portName}' is output-only. Consider using 'out' instead of 'ref'.`,
        { node: arg.value, property: "direction" }
      );
    }

    // Also check that the variable declaration allows this access
    const varDecl = arg.value.varName.ref;
    if (varDecl && isParamDecl(varDecl)) {
      const paramDir = varDecl.direction ?? "in";
      const needsWrite = argDir === "out" || argDir === "ref";
      const paramAllowsWrite = paramDir === "out" || paramDir === "ref";

      if (needsWrite && !paramAllowsWrite) {
        accept(
          "error",
          `Parameter '${varDecl.name}' is '${paramDir}' but used with '${argDir}'. Add '${argDir}' to the parameter declaration.`,
          { node: arg.value, property: "direction" }
        );
      }
    }
  }

  /**
   * Find the Program ancestor of a node.
   */
  private findProgram(node: any): Program | undefined {
    let current = node.$container;
    while (current) {
      if (current.$type === "Program") {
        return current as Program;
      }
      current = current.$container;
    }
    return undefined;
  }

  /**
   * Get resolved type for a variable.
   */
  private getResolvedType(
    varDecl: GlobalVarDecl | LocalVarDecl | ParamDecl,
    ctx: TreeTypeContext
  ): string | undefined {
    if (isGlobalVarDecl(varDecl)) {
      return varDecl.typeName;
    }
    if (isLocalVarDecl(varDecl)) {
      // Local vars can have explicit type or inferred type
      return varDecl.typeName ?? ctx.getType(varDecl.name);
    }
    if (isParamDecl(varDecl)) {
      return ctx.getType(varDecl.name);
    }
    return undefined;
  }

  // =========================================================================
  // Node Statement Checks
  // =========================================================================

  /**
   * Check Decorator statement constraints.
   *
   * NOTE: The grammar already restricts Decorator.name to ManifestDecorator,
   * but the default linker error for category mismatch is not very friendly.
   * We add a focused diagnostic when the name exists in the manifest with a
   * non-Decorator category.
   */
  checkDecorator(decorator: Decorator, accept: ValidationAcceptor): void {
    // If name ref is missing, we can't validate
    if (!decorator.name) return;

    // If it resolved, it's a ManifestDecorator and we are good.
    if (decorator.name.ref) return;

    const id = decorator.name.$refText;
    if (!id) return;

    const nodeInfo = this.nodeResolver.getNode(id);
    if (nodeInfo && nodeInfo.category !== "Decorator") {
      accept(
        "error",
        `@${id} refers to a ${nodeInfo.category} node. Only 'Decorator' category nodes can be used after '@'.`,
        { node: decorator, property: "name" }
      );
    }
  }

  /**
   * Check Node statement constraints
   */
  checkNodeStmt(node: NodeStmt, accept: ValidationAcceptor): void {
    // Skip if nodeName is missing (parse error)
    if (!node.nodeName) return;

    // Check for duplicate argument names in property block
    if (node.propertyBlock) {
      const argNames = new Set<string>();
      let positionalCount = 0;

      for (const arg of node.propertyBlock.args) {
        if (arg.name === undefined) {
          // Positional argument (no key)
          positionalCount++;
          if (positionalCount > 1) {
            accept("error", `Only one positional argument is allowed.`, {
              node: arg,
              property: "value",
            });
          }
        } else {
          // Named argument - check for duplicates
          if (argNames.has(arg.name)) {
            accept("error", `Duplicate argument name: '${arg.name}'`, {
              node: arg,
              property: "name",
            });
          }
          argNames.add(arg.name);
        }
      }

      // Validate positional argument is only allowed for single-port nodes
      if (positionalCount > 0) {
        const nodeId = node.nodeName.$refText;
        const nodeInfo = this.nodeResolver.getNode(nodeId);
        if (nodeInfo && nodeInfo.source !== "tree") {
          const portCount = nodeInfo.ports.size;
          if (portCount === 0) {
            accept(
              "error",
              `Node '${nodeId}' has no ports; positional argument not allowed.`,
              {
                node: node.propertyBlock.args.find(
                  (a) => a.name === undefined
                )!,
                property: "value",
              }
            );
          } else if (portCount > 1) {
            accept(
              "error",
              `Node '${nodeId}' has ${portCount} ports; positional argument is only allowed for single-port nodes. ` +
                `Use named arguments like '${[...nodeInfo.ports.keys()].join(": ..., ")}: ...'.`,
              {
                node: node.propertyBlock.args.find(
                  (a) => a.name === undefined
                )!,
                property: "value",
              }
            );
          }
        }
      }
    }

    // Name conflict check (TreeDef vs manifest ID).
    const nodeId = node.nodeName.$refText;
    if (this.nodeResolver.isAmbiguousName(nodeId)) {
      accept(
        "error",
        `Ambiguous node name '${nodeId}': it exists both as a TreeDef and as a declared node (declare ...). Rename one of them.`,
        { node, property: "nodeName" }
      );
    }

    // Check if nodeName is a manifest Decorator misused as a regular node statement.
    // (Grammar/scope prevent it from resolving, so the default linker error is vague.)
    const nodeInfo = this.nodeResolver.getNode(nodeId);
    if (nodeInfo?.category === "Decorator") {
      accept(
        "error",
        `'${nodeId}' is a Decorator node and cannot be used as a normal node statement. Use '@${nodeId} ...' instead.`,
        { node, property: "nodeName" }
      );
      // Continue: other checks (e.g. duplicate args) can still be useful.
    }

    // Only Control nodes may have a children block.
    if (node.childrenBlock) {
      if (nodeInfo && nodeInfo.source !== "tree") {
        if (nodeInfo.category !== "Control") {
          accept(
            "error",
            `Node '${nodeId}' has a children block, but its category is '${nodeInfo.category}'. Only 'Control' nodes can have '{ ... }'.`,
            { node, property: "childrenBlock" }
          );
        }
      } else if (
        this.treeTypeRegistry.hasTree(nodeId) ||
        this.hasTreeInWorkspace(nodeId)
      ) {
        accept(
          "error",
          `SubTree call '${nodeId}' cannot have a children block. Only 'Control' nodes can have '{ ... }'.`,
          { node, property: "childrenBlock" }
        );
      }
      // Unknown nodes: don't enforce; category may be provided by an external manifest.
    }

    // Conversely, Control nodes should not be used as leaf calls.
    // (A Control node without children is almost certainly a mistake.)
    if (nodeInfo?.category === "Control" && !node.childrenBlock) {
      accept(
        "error",
        `Control node '${nodeId}' must have a children block '{ ... }'. (Example: ${nodeId} { Child() })`,
        { node, property: "nodeName" }
      );
    }

    // Check if parentheses are required but missing.
    // Action, Condition, and SubTree calls require (), but Control nodes can omit them.

    if (!node.propertyBlock) {
      // Parentheses are missing
      if (nodeInfo && nodeInfo.source !== "tree") {
        // Node is declared - check category
        const category = nodeInfo.category;
        if (
          category === "Action" ||
          category === "Condition" ||
          category === "SubTree"
        ) {
          accept(
            "error",
            `${category} node '${nodeId}' requires parentheses '()'. Control nodes like Sequence can omit them.`,
            { node, property: "nodeName" }
          );
        }
      } else if (
        this.treeTypeRegistry.hasTree(nodeId) ||
        this.hasTreeInWorkspace(nodeId)
      ) {
        // This is a SubTree call
        accept("error", `SubTree call '${nodeId}' requires parentheses '()'.`, {
          node,
          property: "nodeName",
        });
      }
      // Unknown nodes: don't enforce () since we can't determine the category
    }
  }

  // =========================================================================
  // BlackboardRef Checks
  // =========================================================================

  /**
   * Check BlackboardRef constraints - direction permission checking.
   */
  checkBlackboardRef(ref: BlackboardRef, accept: ValidationAcceptor): void {
    const varDecl = ref.varName.ref;
    if (!varDecl) return;

    const argDir = ref.direction; // 'in' | 'out' | 'ref' | undefined
    const isWriteAccess = argDir === "out" || argDir === "ref";

    // Check if non-writable parameter is used with write access
    if (isParamDecl(varDecl) && isWriteAccess) {
      const paramDir = varDecl.direction;
      const paramIsWritable = paramDir === "out" || paramDir === "ref";
      if (!paramIsWritable) {
        accept(
          "error",
          `Parameter '${varDecl.name}' is input-only but used with '${argDir}' for write access. ` +
            `Add 'out' or 'ref' to the parameter declaration, or remove '${argDir}' from this usage.`,
          { node: ref, property: "direction" }
        );
      }
    }

    // Note: Warning for unused write params is handled at tree level in checkWriteParamUsage
  }

  // =========================================================================
  // LocalVarDecl Checks
  // =========================================================================

  /**
   * Check LocalVarDecl constraints - type consistency with initial value.
   */
  checkLocalVarDecl(varDecl: LocalVarDecl, accept: ValidationAcceptor): void {
    if (!varDecl.initialValue) return;

    const inferredType = this.inferTypeFromLiteral(varDecl.initialValue);
    if (!inferredType) return;

    // If explicit type is provided, check for mismatch
    if (varDecl.typeName) {
      if (varDecl.typeName !== inferredType) {
        accept(
          "error",
          `Type mismatch: explicit type '${varDecl.typeName}' does not match initial value type '${inferredType}'.`,
          { node: varDecl, property: "initialValue" }
        );
      }
    }
  }

  /**
   * Infer type from a literal or LiteralExpr.
   */
  private inferTypeFromLiteral(expr: any): string | undefined {
    // Handle LiteralExpr wrapper
    if (isLiteralExpr(expr)) {
      return this.inferTypeFromLiteral(expr.literal);
    }
    if (isStringLiteral(expr)) return "std::string";
    if (isIntLiteral(expr)) return "int";
    if (isFloatLiteral(expr)) return "double";
    if (isBoolLiteral(expr)) return "bool";
    return undefined;
  }

  // =========================================================================
  // Declaration Checks
  // =========================================================================

  /**
   * Check DeclareStmt constraints.
   */
  checkDeclareStmt(decl: DeclareStmt, accept: ValidationAcceptor): void {
    // Check for duplicate port names
    const portNames = new Set<string>();
    for (const port of decl.ports) {
      if (portNames.has(port.name)) {
        accept("error", `Duplicate port name: '${port.name}'`, {
          node: port,
          property: "name",
        });
      }
      portNames.add(port.name);
    }

    // Valid categories
    const validCategories = [
      "Action",
      "Condition",
      "Control",
      "Decorator",
      "SubTree",
    ];
    if (!validCategories.includes(decl.category)) {
      accept(
        "error",
        `Invalid category '${decl.category}'. Must be one of: ${validCategories.join(", ")}.`,
        { node: decl, property: "category" }
      );
    }
  }
}

function normalizeImportCandidates(pathText: string): string[] {
  const trimmed = (pathText ?? "").trim();
  if (!trimmed) return [];

  // Support extension-less imports as convenience: import "./foo" -> try "./foo.bt".
  if (/[.][A-Za-z0-9]+$/.test(trimmed)) {
    return [trimmed];
  }
  return [trimmed, `${trimmed}.bt`];
}
