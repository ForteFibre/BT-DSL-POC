import type { CancellationToken } from "vscode-jsonrpc";
import { AstUtils, CstUtils } from "langium";
import { DefaultCompletionProvider } from "langium/lsp";
import type { LangiumDocument, LeafCstNode } from "langium";
import type {
  CompletionItem,
  CompletionList,
  CompletionParams,
} from "vscode-languageserver";
import { CompletionItemKind, InsertTextFormat } from "vscode-languageserver";
import type { BtDslServices } from "../bt-dsl-module.js";
import {
  type Argument,
  type NodeStmt,
  isArgument,
  isNodeStmt,
  isTreeDef,
} from "../generated/ast.js";
import { BtNodeResolver } from "../node-resolver.js";

export class BtDslCompletionProvider extends DefaultCompletionProvider {
  private readonly btServices: BtDslServices;
  private readonly nodeResolver: BtNodeResolver;

  /**
   * VS Code側の自動補完トリガー。
   * Linux環境では Ctrl+Space が IME 切り替え等に奪われることがあるため、
   * 記号入力でも補完要求が飛ぶようにしておく。
   */
  override readonly completionOptions = {
    triggerCharacters: ["@", "(", ",", ":"],
  };

  constructor(services: BtDslServices) {
    super(services);
    this.btServices = services;
    this.nodeResolver = new BtNodeResolver(services);
  }

  override async getCompletion(
    document: LangiumDocument,
    params: CompletionParams,
    cancelToken?: CancellationToken
  ): Promise<CompletionList | undefined> {
    const offset = document.textDocument.offsetAt(params.position);
    // コメント内では (synthetic だけでなく) cross-reference の候補も不要なので抑止する。
    if (this.isInCommentText(document, offset)) {
      return undefined;
    }

    const base = await super.getCompletion(document, params, cancelToken);
    const extra = this.getSyntheticCompletions(document, params);

    if (!extra || extra.length === 0) {
      return base;
    }

    const baseItems = base?.items ?? [];
    const baseLabels = new Set(baseItems.map((i) => i.label.toString()));
    const filteredExtra = extra.filter(
      (i) => !baseLabels.has(i.label.toString())
    );
    const merged: CompletionItem[] = [...baseItems, ...filteredExtra];
    return {
      isIncomplete: base?.isIncomplete ?? false,
      items: this.deduplicateItems(merged),
    };
  }

  private portOrParamItemsForNodeStmt(
    nodeStmt: NodeStmt | undefined
  ): CompletionItem[] {
    if (!nodeStmt?.nodeName) return [];

    const nodeName = nodeStmt.nodeName.$refText;

    const tree = this.nodeResolver.getUniqueTreeDef(nodeName);
    if (tree && isTreeDef(tree)) {
      const params = tree.params?.params ?? [];
      return params.map(
        (p) =>
          ({
            label: p.name,
            kind: CompletionItemKind.Field,
            detail: `Tree param${p.direction ? ` (${p.direction})` : ""}${
              p.typeName ? `: ${p.typeName}` : ""
            }`,
            insertText: `${p.name}: `,
            insertTextFormat: InsertTextFormat.PlainText,
          }) satisfies CompletionItem
      );
    }

    const nodeInfo = this.nodeResolver.getNode(nodeName);
    if (!nodeInfo || nodeInfo.source === "tree") return [];

    return [...nodeInfo.ports.values()].map(
      (p) =>
        ({
          label: p.name,
          kind: CompletionItemKind.Field,
          detail: p.type ? `${p.direction}: ${p.type}` : p.direction,
          documentation: p.description,
          insertText: `${p.name}: `,
          insertTextFormat: InsertTextFormat.PlainText,
        }) satisfies CompletionItem
    );
  }

  private isCommentLeaf(leaf: LeafCstNode): boolean {
    const t = leaf.tokenType?.name;
    return (
      t === "SL_COMMENT" ||
      t === "ML_COMMENT" ||
      t === "OUTER_DOC" ||
      t === "INNER_DOC"
    );
  }

  /**
   * CST の leaf 取得は hidden token を拾えない場合があるため、
   * テキストからもコメント内かどうかを判定する。
   */
  private isInCommentText(document: LangiumDocument, offset: number): boolean {
    const text = document.textDocument.getText();
    const safeOffset = Math.max(0, Math.min(offset, text.length));

    // Block comment: last "/*" without a matching "*/".
    const lastBlockStart = text.lastIndexOf("/*", safeOffset);
    if (lastBlockStart >= 0) {
      const lastBlockEnd = text.lastIndexOf("*/", safeOffset);
      if (lastBlockEnd < lastBlockStart) {
        return true;
      }
    }

    // Line comment: "//" after the last newline.
    const lineStart = text.lastIndexOf("\n", safeOffset - 1) + 1;
    const linePrefix = text.slice(lineStart, safeOffset);
    const lineCommentIdx = linePrefix.indexOf("//");
    return lineCommentIdx >= 0;
  }

  private startOffsetOf(
    document: LangiumDocument,
    node: { $cstNode?: { range: any } } | undefined
  ): number | undefined {
    const r = node?.$cstNode?.range;
    if (!r) return undefined;
    return document.textDocument.offsetAt(r.start);
  }

  private endOffsetOf(
    document: LangiumDocument,
    node: { $cstNode?: { range: any } } | undefined
  ): number | undefined {
    const r = node?.$cstNode?.range;
    if (!r) return undefined;
    return document.textDocument.offsetAt(r.end);
  }

  private isWithin(
    document: LangiumDocument,
    offset: number,
    node: { $cstNode?: { range: any } } | undefined
  ): boolean {
    const start = this.startOffsetOf(document, node);
    const end = this.endOffsetOf(document, node);
    if (typeof start !== "number" || typeof end !== "number") return false;
    return start <= offset && offset <= end;
  }

  private tryArgumentNameCompletion(
    document: LangiumDocument,
    offset: number,
    arg: Argument | undefined
  ): CompletionItem[] | undefined {
    if (!arg || !this.isWithin(document, offset, arg)) return undefined;

    const argStart = this.startOffsetOf(document, arg);
    const argEnd = this.endOffsetOf(document, arg);
    if (typeof argStart !== "number" || typeof argEnd !== "number")
      return undefined;

    const segment = document.textDocument.getText({
      start: document.textDocument.positionAt(argStart),
      end: document.textDocument.positionAt(argEnd),
    });
    const colonRel = segment.indexOf(":");
    const colonAbs = colonRel >= 0 ? argStart + colonRel : undefined;
    const inNameRegion =
      typeof colonAbs === "number" ? offset <= colonAbs : true;
    if (!inNameRegion) return undefined;

    const nodeStmt = AstUtils.getContainerOfType(arg, isNodeStmt);
    return this.portOrParamItemsForNodeStmt(nodeStmt);
  }

  private tryPropertyBlockCompletion(
    document: LangiumDocument,
    offset: number,
    nodeStmt: NodeStmt | undefined,
    arg: Argument | undefined
  ): CompletionItem[] | undefined {
    if (!nodeStmt?.propertyBlock) return undefined;
    if (!this.isWithin(document, offset, nodeStmt.propertyBlock))
      return undefined;
    if (arg) return undefined;
    return this.portOrParamItemsForNodeStmt(nodeStmt);
  }

  private getSyntheticCompletions(
    document: LangiumDocument,
    params: CompletionParams
  ): CompletionItem[] {
    const root = document.parseResult.value.$cstNode;
    if (!root) return [];

    const offset = document.textDocument.offsetAt(params.position);

    // Avoid noisy suggestions inside comments even if CST leaf selection misses hidden tokens.
    if (this.isInCommentText(document, offset)) return [];

    // `findLeafNodeAtOffset` returns undefined on whitespace.
    // Completion is often requested on whitespace/newlines, so we fall back.
    const leaf =
      CstUtils.findLeafNodeAtOffset(root, offset) ??
      CstUtils.findLeafNodeBeforeOffset(root, offset);
    if (!leaf) return [];

    // Avoid noisy suggestions inside comments.
    // (We still allow base keyword completion to work as usual.)
    if (this.isCommentLeaf(leaf)) return [];

    const astNode = leaf.astNode;

    const nodeStmt = AstUtils.getContainerOfType(astNode, isNodeStmt);
    const arg = AstUtils.getContainerOfType(astNode, isArgument);

    return (
      this.tryArgumentNameCompletion(document, offset, arg) ??
      this.tryPropertyBlockCompletion(document, offset, nodeStmt, arg) ??
      []
    );
  }
}
