import type { CancellationToken } from "vscode-jsonrpc";
import type { LangiumDocument } from "langium";
import { AstUtils, CstUtils } from "langium";
import type { Hover, HoverParams } from "vscode-languageserver";
import { MarkupKind } from "vscode-languageserver";
import type { BtDslServices } from "../bt-dsl-module.js";
import { BtNodeResolver } from "../node-resolver.js";
import {
  isArgument,
  isBlackboardRef,
  isDeclareStmt,
  isDecorator,
  isGlobalVarDecl,
  isNodeStmt,
  isParamDecl,
  isTreeDef,
  type TreeDef,
} from "../generated/ast.js";
import { resolveTreeTypes } from "../type-resolver.js";

export class BtDslHoverProvider {
  private readonly btServices: BtDslServices;
  private readonly nodeResolver: BtNodeResolver;

  constructor(services: BtDslServices) {
    this.btServices = services;
    this.nodeResolver = new BtNodeResolver(services);
  }

  private getUniqueTreeDef(name: string): TreeDef | undefined {
    return this.nodeResolver.getUniqueTreeDef(name);
  }

  getHoverContent(
    document: LangiumDocument,
    params: HoverParams,
    _cancelToken?: CancellationToken
  ): Hover | undefined {
    const root = document.parseResult.value.$cstNode;
    if (!root) return undefined;

    const offset = document.textDocument.offsetAt(params.position);
    const leaf = CstUtils.findLeafNodeAtOffset(root, offset);
    if (!leaf) return undefined;

    // Hover on node name
    const nodeStmt = AstUtils.getContainerOfType(leaf.astNode, isNodeStmt);
    if (nodeStmt && leaf.text === nodeStmt.nodeName.$refText) {
      const name = nodeStmt.nodeName.$refText;

      const tree = this.getUniqueTreeDef(name);
      if (tree && isTreeDef(tree)) {
        const docs = joinDocs(tree.outerDocs);

        const paramsMd = (tree.params?.params ?? [])
          .map(
            (p) =>
              `- ${p.direction ? `\`${p.direction}\` ` : ""}\`${p.name}\`${
                p.typeName ? `: \`${p.typeName}\`` : ""
              }`
          )
          .join("\n");

        const md = [
          `### Tree \`${tree.name}\``,
          docs ? `\n${docs}\n` : "",
          (tree.params?.params?.length ?? 0) > 0
            ? `**Params**\n${paramsMd}`
            : "",
        ]
          .filter(Boolean)
          .join("\n");

        return {
          range: leaf.range,
          contents: { kind: MarkupKind.Markdown, value: md },
        };
      }

      const nodeInfo = this.nodeResolver.getNode(name);
      if (nodeInfo && nodeInfo.source !== "tree") {
        const ports = [...nodeInfo.ports.values()]
          .map(
            (p) =>
              `- \`${p.name}\` (${p.direction}${
                p.type ? `: \`${p.type}\`` : ""
              })${p.description ? ` — ${escapeMarkdown(p.description)}` : ""}`
          )
          .join("\n");

        const md = [
          `### ${nodeInfo.category} \`${nodeInfo.id}\``,
          ports ? `\n**Ports**\n${ports}` : "",
        ]
          .filter(Boolean)
          .join("\n");

        return {
          range: leaf.range,
          contents: { kind: MarkupKind.Markdown, value: md },
        };
      }
    }

    // Hover on decorator name
    const decorator = AstUtils.getContainerOfType(leaf.astNode, isDecorator);
    if (decorator && leaf.text === decorator.name.$refText) {
      const name = decorator.name.$refText;
      const nodeInfo = this.nodeResolver.getNode(name);
      if (nodeInfo && nodeInfo.category === "Decorator") {
        const md = `### Decorator \`${nodeInfo.id}\`\n\nCategory: **${nodeInfo.category}**`;
        return {
          range: leaf.range,
          contents: { kind: MarkupKind.Markdown, value: md },
        };
      }
    }

    // Hover on Argument.name (port)
    const arg = AstUtils.getContainerOfType(leaf.astNode, isArgument);
    if (arg && leaf.text === arg.name) {
      const nodeStmt = AstUtils.getContainerOfType(arg, isNodeStmt);
      if (!nodeStmt) return undefined;

      const port = this.nodeResolver.getPort(
        nodeStmt.nodeName.$refText,
        arg.name
      );
      if (port) {
        const md = [
          `### Port \`${arg.name}\``,
          `Node: \`${nodeStmt.nodeName.$refText}\``,
          `Direction: **${port.direction}**`,
          port.type ? `Type: \`${port.type}\`` : "",
          port.description ? `\n${escapeMarkdown(port.description)}` : "",
        ]
          .filter(Boolean)
          .join("\n");

        return {
          range: leaf.range,
          contents: { kind: MarkupKind.Markdown, value: md },
        };
      }

      // If this looks like a SubTree call, show param info
      const tree = this.getUniqueTreeDef(nodeStmt.nodeName.$refText);
      if (tree && isTreeDef(tree)) {
        const param = (tree.params?.params ?? []).find(
          (p) => p.name === arg.name
        );
        if (param) {
          const directionInfo = param.direction
            ? `**${param.direction}** (${param.direction === "in" || !param.direction ? "read-only" : "write"})`
            : "read-only";
          const md = `### Param \`${param.name}\`\n\nTree: \`${
            tree.name
          }\`\n\n${directionInfo}${
            param.typeName ? `\n\nType: \`${param.typeName}\`` : ""
          }`;
          return {
            range: leaf.range,
            contents: { kind: MarkupKind.Markdown, value: md },
          };
        }
      }
    }

    // Hover on BlackboardRef.varName
    const ref = AstUtils.getContainerOfType(leaf.astNode, isBlackboardRef);
    if (ref && leaf.text === ref.varName.$refText) {
      const target = ref.varName.ref;
      if (target) {
        if (isGlobalVarDecl(target)) {
          const md = `### Blackboard \`${target.name}\`\n\nType: \`${target.typeName}\``;
          return {
            range: leaf.range,
            contents: { kind: MarkupKind.Markdown, value: md },
          };
        }
        if (isParamDecl(target)) {
          const tree = AstUtils.getContainerOfType(ref, isTreeDef);
          if (!tree) return undefined;

          // Best-effort inferred type
          const ctx = resolveTreeTypes(tree, this.nodeResolver);
          const inferred = ctx.getType(target.name);

          const directionInfo = target.direction
            ? `**${target.direction}** (${target.direction === "in" || !target.direction ? "read-only" : "write"})`
            : "read-only";
          const md = [
            `### Param \`${target.name}\``,
            directionInfo,
            inferred ? `\nType: \`${inferred}\`` : "",
          ]
            .filter(Boolean)
            .join("\n");

          return {
            range: leaf.range,
            contents: { kind: MarkupKind.Markdown, value: md },
          };
        }
      }
    }

    return undefined;
  }
}

function joinDocs(docs: string[]): string {
  return docs
    .map((d) => d.replace(/^\/\/\/\s*/, "").trim())
    .filter(Boolean)
    .join(" ");
}

function escapeMarkdown(text: string): string {
  return text.replace(/[\\`*_{}\[\]()#+\-.!]/g, "\\$&");
}
