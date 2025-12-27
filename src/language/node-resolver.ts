import type { AstNodeDescription } from "langium";
import type { BtDslServices } from "./bt-dsl-module.js";
import type {
  DeclareStmt,
  DeclarePort,
  ParamDecl,
  TreeDef,
} from "./generated/ast.js";
import { isDeclareStmt, isTreeDef } from "./generated/ast.js";

export interface PortInfo {
  name: string;
  direction: "in" | "out" | "ref";
  type?: string;
  description?: string;
}

export interface NodeInfo {
  id: string;
  category: "Action" | "Condition" | "Control" | "Decorator" | "SubTree";
  ports: Map<string, PortInfo>;
  source: "tree" | "declare";
}

export interface NodeResolver {
  getNode(id: string): NodeInfo | undefined;
  getPort(nodeId: string, portName: string): PortInfo | undefined;

  /**
   * Convenience helper for positional arguments.
   * Returns undefined if node doesn't exist or doesn't have exactly one port.
   */
  getSinglePortName(nodeId: string): string | undefined;
}

function normalizeTreeParamDirection(
  dir: ParamDecl["direction"]
): PortInfo["direction"] {
  if (dir === "out" || dir === "ref") return dir;
  return "in";
}

function treeDefToNodeInfo(tree: TreeDef): NodeInfo {
  const ports = new Map<string, PortInfo>();
  for (const param of tree.params?.params ?? []) {
    ports.set(param.name, {
      name: param.name,
      direction: normalizeTreeParamDirection(param.direction),
      type: param.typeName,
    });
  }

  return {
    id: tree.name,
    category: "SubTree",
    ports,
    source: "tree",
  };
}

function normalizeDeclarePortDirection(
  dir: DeclarePort["direction"]
): PortInfo["direction"] {
  if (dir === "out" || dir === "ref") return dir;
  return "in";
}

function declareStmtToNodeInfo(decl: DeclareStmt): NodeInfo {
  const ports = new Map<string, PortInfo>();
  for (const port of decl.ports) {
    ports.set(port.name, {
      name: port.name,
      direction: normalizeDeclarePortDirection(port.direction),
      type: port.typeName,
      description:
        port.outerDocs.length > 0
          ? port.outerDocs
              .map((d) => d.replace(/^\/\/\/\s*/, "").trim())
              .join(" ")
          : undefined,
    });
  }

  return {
    id: decl.name,
    category: decl.category as NodeInfo["category"],
    ports,
    source: "declare",
  };
}

export class BtNodeResolver implements NodeResolver {
  private readonly services: BtDslServices;

  constructor(services: BtDslServices) {
    this.services = services;
  }

  private treeDescriptionsByName(name: string): AstNodeDescription[] {
    return this.services.shared.workspace.IndexManager.allElements("TreeDef")
      .filter((d) => d.name === name)
      .toArray();
  }

  private declareDescriptionsByName(name: string): AstNodeDescription[] {
    return this.services.shared.workspace.IndexManager.allElements(
      "DeclareStmt"
    )
      .filter((d) => d.name === name)
      .toArray();
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

  private resolveDeclareStmt(
    desc: AstNodeDescription
  ): DeclareStmt | undefined {
    if (desc.node && isDeclareStmt(desc.node)) {
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
    return isDeclareStmt(astNode) ? astNode : undefined;
  }

  /**
   * Returns the TreeDef only when it is unique across the workspace.
   * (Duplicate TreeDef names are validated elsewhere.)
   */
  getUniqueTreeDef(name: string): TreeDef | undefined {
    const descs = this.treeDescriptionsByName(name);
    if (descs.length !== 1) return undefined;
    return this.resolveTreeDef(descs[0]);
  }

  /**
   * Returns the DeclareStmt only when it is unique across the workspace.
   */
  getUniqueDeclareStmt(name: string): DeclareStmt | undefined {
    const descs = this.declareDescriptionsByName(name);
    if (descs.length !== 1) return undefined;
    return this.resolveDeclareStmt(descs[0]);
  }

  hasTreeInWorkspace(name: string): boolean {
    return this.services.shared.workspace.IndexManager.allElements(
      "TreeDef"
    ).some((d) => d.name === name);
  }

  hasDeclareInWorkspace(name: string): boolean {
    return this.services.shared.workspace.IndexManager.allElements(
      "DeclareStmt"
    ).some((d) => d.name === name);
  }

  getNode(id: string): NodeInfo | undefined {
    const tree = this.getUniqueTreeDef(id);
    const declare = this.getUniqueDeclareStmt(id);

    // Count how many sources have this ID
    const count = (tree ? 1 : 0) + (declare ? 1 : 0);

    // Conflict: same ID exists in multiple sources.
    // Policy: treat as ambiguous; validator reports this as an error.
    if (count > 1) return undefined;

    if (tree) return treeDefToNodeInfo(tree);
    if (declare) return declareStmtToNodeInfo(declare);
    return undefined;
  }

  getPort(nodeId: string, portName: string): PortInfo | undefined {
    const node = this.getNode(nodeId);
    return node?.ports.get(portName);
  }

  getSinglePortName(nodeId: string): string | undefined {
    const node = this.getNode(nodeId);
    if (node?.ports.size !== 1) return undefined;
    return [...node.ports.keys()][0];
  }

  /**
   * Returns true when the same name exists both as TreeDef and as DeclareStmt.
   * (The validator reports this as an error; we also treat it as ambiguous in resolution.)
   */
  isAmbiguousName(id: string): boolean {
    return Boolean(this.getUniqueTreeDef(id) && this.getUniqueDeclareStmt(id));
  }
}
