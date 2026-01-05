import * as vscode from 'vscode';
import * as path from 'node:path';
import { formatBtDslText, setCoreWasmPath } from '@bt-dsl/formatter';

export function registerBtDslFormattingProvider(context: vscode.ExtensionContext): void {
  const selector: vscode.DocumentSelector = [
    { scheme: 'file', language: 'bt-dsl' },
    { scheme: 'untitled', language: 'bt-dsl' },
  ];

  // Configure core WASM bundle path for the formatter.
  // The extension build copies `formatter_wasm.{js,wasm}` into `out/`.
  setCoreWasmPath(path.join(context.extensionPath, 'out'));

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
