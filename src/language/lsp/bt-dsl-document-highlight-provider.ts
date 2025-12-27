import type { CancellationToken } from "vscode-jsonrpc";
import type { LangiumDocument } from "langium";
import { AstUtils, CstUtils } from "langium";
import { DefaultDocumentHighlightProvider } from "langium/lsp";
import {
  DocumentHighlight,
  type DocumentHighlightParams,
} from "vscode-languageserver";
import { isDecorator, isNodeStmt } from "../generated/ast.js";
import type { BtDslServices } from "../bt-dsl-module.js";

export class BtDslDocumentHighlightProvider {
  private readonly delegate: DefaultDocumentHighlightProvider;

  constructor(private readonly services: BtDslServices) {
    this.delegate = new DefaultDocumentHighlightProvider(services);
  }

  async getDocumentHighlight(
    document: LangiumDocument,
    params: DocumentHighlightParams,
    cancelToken?: CancellationToken
  ): Promise<DocumentHighlight[] | undefined> {
    try {
      return await this.delegate.getDocumentHighlight(
        document,
        params,
        cancelToken
      );
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      if (!msg.includes("AST node has no document")) {
        throw e;
      }
      return this.fallbackHighlight(document, params);
    }
  }

  private fallbackHighlight(
    document: LangiumDocument,
    params: DocumentHighlightParams
  ): DocumentHighlight[] | undefined {
    const rootCst = document.parseResult.value.$cstNode;
    if (!rootCst) return undefined;

    const offset = document.textDocument.offsetAt(params.position);
    const leaf = CstUtils.findLeafNodeAtOffset(rootCst, offset);
    if (!leaf) return undefined;

    // Case 1: NodeStmt.nodeName cross-reference (manifest nodes + TreeDef)
    const nodeStmt = AstUtils.getContainerOfType(leaf.astNode, isNodeStmt);
    if (nodeStmt && leaf.text === nodeStmt.nodeName.$refText) {
      const name = nodeStmt.nodeName.$refText;
      return this.collectNodeNameHighlights(document, name);
    }

    // Case 2: Decorator.name cross-reference (manifest nodes)
    const decorator = AstUtils.getContainerOfType(leaf.astNode, isDecorator);
    if (decorator && leaf.text === decorator.name.$refText) {
      const name = decorator.name.$refText;
      return this.collectDecoratorHighlights(document, name);
    }

    return undefined;
  }

  private collectNodeNameHighlights(
    document: LangiumDocument,
    name: string
  ): DocumentHighlight[] {
    const root = document.parseResult.value;

    const highlights: DocumentHighlight[] = [];

    // NodeStmt occurrences
    for (const n of AstUtils.streamAllContents(root).filter(isNodeStmt)) {
      if (n.nodeName.$refText !== name) continue;
      const refNode = n.nodeName.$refNode;
      if (!refNode) continue;
      highlights.push(DocumentHighlight.create(refNode.range));
    }

    return highlights;
  }

  private collectDecoratorHighlights(
    document: LangiumDocument,
    name: string
  ): DocumentHighlight[] {
    const root = document.parseResult.value;

    const highlights: DocumentHighlight[] = [];

    for (const deco of AstUtils.streamAllContents(root).filter(isDecorator)) {
      if (deco.name.$refText !== name) continue;
      const refNode = deco.name.$refNode;
      if (!refNode) continue;
      highlights.push(DocumentHighlight.create(refNode.range));
    }

    return highlights;
  }
}
