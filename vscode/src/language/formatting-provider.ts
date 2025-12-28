import * as vscode from 'vscode';
import * as path from 'node:path';
import { formatBtDslText, setTreeSitterWasmPath } from '@bt-dsl/formatter';

export function registerBtDslFormattingProvider(context: vscode.ExtensionContext): void {
  const selector: vscode.DocumentSelector = [
    { scheme: 'file', language: 'bt-dsl' },
    { scheme: 'untitled', language: 'bt-dsl' },
  ];

  // Configure tree-sitter WASM path for the formatter (CJS environment)
  const treeSitterWasmPath = path.join(context.extensionPath, 'out', 'tree-sitter-bt_dsl.wasm');
  setTreeSitterWasmPath(treeSitterWasmPath);

  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      async provideDocumentFormattingEdits(
        document: vscode.TextDocument,
        options: vscode.FormattingOptions,
      ): Promise<vscode.TextEdit[]> {
        const text = document.getText();
        try {
          const formatted = await formatBtDslText(text, {
            filepath: document.uri.fsPath,
            tabWidth: options.tabSize,
            useTabs: !options.insertSpaces,
          });

          const fullRange = new vscode.Range(
            document.positionAt(0),
            document.positionAt(text.length),
          );
          return [vscode.TextEdit.replace(fullRange, formatted)];
        } catch (e) {
          const msg = e instanceof Error ? e.message : String(e);
          void vscode.window.showErrorMessage(`BT DSL: Format failed: ${msg}`);
          return [];
        }
      },
    }),
  );
}
