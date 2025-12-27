import type { LangiumDocument } from "langium";
import type { Formatter } from "langium/lsp";
import type {
  DocumentFormattingParams,
  DocumentOnTypeFormattingOptions,
  DocumentOnTypeFormattingParams,
  DocumentRangeFormattingParams,
} from "vscode-languageserver";
import { TextEdit } from "vscode-languageserver";
import { formatBtDslText } from "../../formatter/format-bt-dsl.js";

/**
 * BT DSL document formatter.
 *
 * We intentionally do NOT use Langium's diff-based formatter API.
 * Instead we delegate to a Prettier plugin and return a single full-document replacement edit.
 * This avoids indentation accumulation and makes comment handling explicit.
 */
export class BtDslFormatter implements Formatter {
  /**
   * Minimal on-type formatting triggers.
   * We still format the whole document, but this enables the LSP capability.
   */
  readonly formatOnTypeOptions: DocumentOnTypeFormattingOptions = {
    firstTriggerCharacter: "}",
    moreTriggerCharacter: ["\n"],
  };

  async formatDocument(
    document: LangiumDocument,
    params: DocumentFormattingParams
  ): Promise<TextEdit[]> {
    const text = document.textDocument.getText();
    const fp =
      (document.uri as any)?.fsPath ??
      (document.uri as any)?.path ??
      document.uri?.toString();
    const formatted = await formatBtDslText(text, {
      filepath: typeof fp === "string" ? fp : undefined,
      tabWidth: params.options.tabSize,
      useTabs: !params.options.insertSpaces,
      endOfLine: "lf",
    });

    if (formatted === text) {
      return [];
    }

    const start = { line: 0, character: 0 };
    const end = document.textDocument.positionAt(text.length);
    return [TextEdit.replace({ start, end }, formatted)];
  }

  async formatDocumentRange(
    document: LangiumDocument,
    params: DocumentRangeFormattingParams
  ): Promise<TextEdit[]> {
    // Prettier is a whole-document formatter; keep behavior consistent.
    return this.formatDocument(document, {
      textDocument: params.textDocument,
      options: params.options,
    });
  }

  async formatDocumentOnType(
    document: LangiumDocument,
    params: DocumentOnTypeFormattingParams
  ): Promise<TextEdit[]> {
    // Same rationale: whole-document formatting.
    return this.formatDocument(document, {
      textDocument: params.textDocument,
      options: params.options,
    });
  }
}
