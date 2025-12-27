import * as path from "node:path";
import type { ExtensionContext } from "vscode";
import { commands, workspace, window } from "vscode";
import {
  LanguageClient,
  type LanguageClientOptions,
  type ServerOptions,
  Trace,
  TransportKind,
} from "vscode-languageclient/node.js";

let client: LanguageClient | undefined;
const outputChannel = window.createOutputChannel("BT DSL");

export async function activate(context: ExtensionContext): Promise<void> {
  outputChannel.appendLine("BT DSL extension activating...");

  const serverModule = context.asAbsolutePath(
    path.join("out", "lsp-server", "main.cjs")
  );

  outputChannel.appendLine(`Server module path: ${serverModule}`);

  const debugOptions = {
    execArgv: ["--nolazy", "--inspect=6009"],
  };

  const serverOptions: ServerOptions = {
    run: {
      module: serverModule,
      transport: TransportKind.ipc,
    },
    debug: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: debugOptions,
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "bt-dsl" },
      { scheme: "untitled", language: "bt-dsl" },
    ],
    // Keep diagnostics and other notifications flowing for the whole workspace.
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher("**/*.bt"),
    },
    outputChannel,
    traceOutputChannel: outputChannel,
    middleware: {
      // Log completion plumbing regardless of whether the server responds.
      provideCompletionItem: async (
        document,
        position,
        context,
        token,
        next
      ) => {
        outputChannel.appendLine(
          `[completion] requested uri=${document.uri.toString()} pos=${
            position.line
          }:${position.character} triggerKind=${
            context.triggerKind
          } triggerChar=${context.triggerCharacter ?? ""}`
        );
        const result = await next(document, position, context, token);
        const count = Array.isArray(result)
          ? result.length
          : result?.items
          ? result.items.length
          : 0;
        outputChannel.appendLine(`[completion] result items=${count}`);
        return result;
      },
    },
  };

  client = new LanguageClient(
    "btDsl",
    "BT DSL Language Server",
    serverOptions,
    clientOptions
  );

  context.subscriptions.push(client);

  // Debug command: directly request LSP completion at the current cursor and log the result.
  // This bypasses VS Code suggestion UI so we can prove whether the server responds.
  context.subscriptions.push(
    commands.registerCommand("bt-dsl.debug.requestCompletion", async () => {
      if (!client) {
        window.showWarningMessage("BT DSL: LanguageClient is not running.");
        return;
      }
      const editor = window.activeTextEditor;
      if (!editor) {
        window.showWarningMessage("BT DSL: No active editor.");
        return;
      }
      const uri = editor.document.uri.toString();
      const position = editor.selection.active;

      outputChannel.appendLine(
        `[debug] sending textDocument/completion uri=${uri} pos=${position.line}:${position.character}`
      );

      try {
        const res: any = await client.sendRequest("textDocument/completion", {
          textDocument: { uri },
          position: { line: position.line, character: position.character },
          context: { triggerKind: 1 },
        });

        const items = Array.isArray(res) ? res : res?.items ?? [];
        outputChannel.appendLine(
          `[debug] completion response items=${items.length}`
        );
        outputChannel.appendLine(
          `[debug] first items: ${items
            .slice(0, 10)
            .map((i: any) => i?.label)
            .filter(Boolean)
            .join(", ")}`
        );
      } catch (e) {
        outputChannel.appendLine(`[debug] completion request failed: ${e}`);
        window.showErrorMessage(
          "BT DSL: completion request failed. See Output → BT DSL for details."
        );
      }
    })
  );

  try {
    await client.start();
    client.setTrace(Trace.Verbose);
    outputChannel.appendLine("BT DSL Language Server started successfully.");

    // Best-effort: log server capabilities for debugging (helps diagnose missing completion support).
    const caps = client.initializeResult?.capabilities;
    if (caps) {
      outputChannel.appendLine(
        `Server capabilities: ${JSON.stringify(caps, null, 2)}`
      );
    }
  } catch (error) {
    outputChannel.appendLine(`Failed to start Language Server: ${error}`);
    window.showErrorMessage(
      `BT DSL: Failed to start Language Server. Check Output panel for details.`
    );
  }
}

export async function deactivate(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}
