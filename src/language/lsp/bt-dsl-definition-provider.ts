import type { CancellationToken } from "vscode-jsonrpc";
import { AstUtils, CstUtils, URI, UriUtils } from "langium";
import { DefaultDefinitionProvider } from "langium/lsp";
import type { LangiumDocument } from "langium";
import type { DefinitionParams, LocationLink } from "vscode-languageserver";
import type { BtDslServices } from "../bt-dsl-module.js";
import { isArgument, isImportStmt, isNodeStmt } from "../generated/ast.js";
import { BtNodeResolver } from "../node-resolver.js";

export class BtDslDefinitionProvider extends DefaultDefinitionProvider {
  private readonly btServices: BtDslServices;
  private readonly nodeResolver: BtNodeResolver;

  constructor(services: BtDslServices) {
    super(services);
    this.btServices = services;
    this.nodeResolver = new BtNodeResolver(services);
  }

  override async getDefinition(
    document: LangiumDocument,
    params: DefinitionParams,
    cancelToken?: CancellationToken
  ): Promise<LocationLink[] | undefined> {
    const base = await super.getDefinition(document, params, cancelToken);
    if (base && base.length > 0) {
      return base;
    }

    const root = document.parseResult.value.$cstNode;
    if (!root) return undefined;

    const offset = document.textDocument.offsetAt(params.position);
    const leaf = CstUtils.findLeafNodeAtOffset(root, offset);
    if (!leaf) return undefined;

    // Import path: jump to the imported file.
    // We intentionally only do this when the cursor is on the string literal.
    const importStmt = AstUtils.getContainerOfType(leaf.astNode, isImportStmt);
    if (importStmt && document.uri.scheme === "file") {
      const leafText = leaf.text ?? "";
      const looksLikeString =
        (leafText.startsWith('"') && leafText.endsWith('"')) ||
        (leafText.startsWith("'") && leafText.endsWith("'"));

      if (looksLikeString) {
        const baseDir = UriUtils.dirname(document.uri);
        const importPath = (importStmt.path ?? "").trim();
        const candidates = normalizeImportCandidates(importPath).filter((p) =>
          p.toLowerCase().endsWith(".bt")
        );

        for (const candidate of candidates) {
          const resolved = UriUtils.resolvePath(baseDir, candidate);
          if (resolved.scheme !== "file") continue;

          try {
            const imported =
              await this.btServices.shared.workspace.LangiumDocuments.getOrCreateDocument(
                resolved
              );

            // Jump to the top of the target document.
            const start = { line: 0, character: 0 };
            return [
              {
                originSelectionRange: leaf.range,
                targetUri: imported.uri.toString(),
                targetRange: { start, end: start },
                targetSelectionRange: { start, end: start },
              },
            ];
          } catch {
            // Try next candidate
          }

          if (cancelToken?.isCancellationRequested) return undefined;
        }
      }
    }

    // Node name: declared node (TreeDef is handled by Langium default definition provider)
    const nodeStmt = AstUtils.getContainerOfType(leaf.astNode, isNodeStmt);
    if (nodeStmt && leaf.text === nodeStmt.nodeName.$refText) {
      const name = nodeStmt.nodeName.$refText;

      const originSelectionRange = leaf.range;

      const decl = this.nodeResolver.getUniqueDeclareStmt(name);
      const range = decl?.$cstNode?.range;
      if (decl && range) {
        return [
          {
            originSelectionRange,
            targetUri:
              decl.$document?.uri.toString() ?? document.uri.toString(),
            targetRange: range,
            targetSelectionRange: range,
          },
        ];
      }
    }

    // Port name definition: Argument.name -> declared port
    const arg = AstUtils.getContainerOfType(leaf.astNode, isArgument);
    if (arg && leaf.text === arg.name) {
      const nodeStmt = AstUtils.getContainerOfType(arg, isNodeStmt);
      if (!nodeStmt) return undefined;

      const decl = this.nodeResolver.getUniqueDeclareStmt(
        nodeStmt.nodeName.$refText
      );
      if (decl) {
        const portDecl = decl.ports.find((p) => p.name === arg.name);
        const range = portDecl?.$cstNode?.range;
        if (range) {
          return [
            {
              originSelectionRange: leaf.range,
              targetUri:
                portDecl.$document?.uri.toString() ??
                decl.$document?.uri.toString() ??
                document.uri.toString(),
              targetRange: range,
              targetSelectionRange: range,
            },
          ];
        }
      }
    }

    return undefined;
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

function toFileUri(maybeFsPathOrUri: string): string {
  // In this project we often store fsPath as source; normalize to file URI.
  if (maybeFsPathOrUri.startsWith("file:")) {
    return maybeFsPathOrUri;
  }
  return URI.file(maybeFsPathOrUri).toString();
}
