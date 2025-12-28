import * as vscode from 'vscode';
import * as fs from 'node:fs';
import * as path from 'node:path';
import {
  LanguageClient,
  type LanguageClientOptions,
  type ServerOptions,
} from 'vscode-languageclient/node.js';
import { registerBtDslFormattingProvider } from './language/formatting-provider';

let client: LanguageClient | undefined;

function candidateServerPaths(context: vscode.ExtensionContext): string[] {
  const exe = process.platform === 'win32' ? 'bt_dsl_lsp_server.exe' : 'bt_dsl_lsp_server';

  const fromEnv = process.env.BT_DSL_LSP_PATH;
  const candidates: string[] = [];
  if (fromEnv) {
    candidates.push(fromEnv);
  }

  // Packaged with the extension (preferred)
  candidates.push(context.asAbsolutePath(path.join('server', exe)));

  // Common dev build locations (best-effort)
  candidates.push(path.resolve(context.extensionPath, '..', 'core', 'build-lsp', exe));
  candidates.push(path.resolve(context.extensionPath, '..', 'core', 'build', exe));

  return candidates;
}

function findServerPath(context: vscode.ExtensionContext): string | null {
  for (const p of candidateServerPaths(context)) {
    try {
      if (p && fs.existsSync(p) && fs.statSync(p).isFile()) {
        return p;
      }
    } catch {
      // ignore
    }
  }
  return null;
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  const serverPath = findServerPath(context);
  if (!serverPath) {
    void vscode.window.showErrorMessage(
      'BT DSL: LSP server binary not found. Build core target "bt_dsl_lsp_server" or set BT_DSL_LSP_PATH.',
    );
    return;
  }

  const stdlibPath = context.asAbsolutePath('stdlib/StandardNodes.bt');

  const serverOptions: ServerOptions = {
    command: serverPath,
    args: [],
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'bt-dsl' },
      { scheme: 'untitled', language: 'bt-dsl' },
    ],
    initializationOptions: {
      stdlibPath,
    },
  };

  client = new LanguageClient('bt-dsl', 'BT DSL Language Server', serverOptions, clientOptions);
  context.subscriptions.push({ dispose: () => void client?.stop() });

  try {
    await client.start();
  } catch (err) {
    // Avoid crashing the extension host (and therefore e2e) if the native server fails to start.
    console.error('BT DSL: failed to start language server', err);
    try {
      await client.stop();
    } catch {
      // ignore
    }
    client = undefined;
    void vscode.window.showErrorMessage(
      'BT DSL: failed to start LSP server. See console output for details.',
    );
    return;
  }

  // Keep formatter local (independent of LSP)
  registerBtDslFormattingProvider(context);
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  const stopping = client.stop();
  client = undefined;
  return stopping;
}
