import * as vscode from 'vscode';
import { BtDslCore } from './wasm/btDslCore';
import { registerBtDslProviders } from './language/providers';
import { byteOffsetAtPosition } from './language/utf8';

let core: BtDslCore | undefined;
let providersRegistered = false;
let stdlibUriForTests: string | undefined;

async function __testLoadFileIntoCore(uri: string): Promise<void> {
  if (!core) return;
  let target: vscode.Uri;
  try {
    target = vscode.Uri.parse(uri);
  } catch {
    return;
  }

  // Prefer VS Code's document model (handles in-memory edits). Fallback to FS.
  try {
    const doc = await vscode.workspace.openTextDocument(target);
    core.setDocument(uri, doc.getText());
    return;
  } catch {
    // ignore
  }

  try {
    const data = await vscode.workspace.fs.readFile(target);
    const text = Buffer.from(data).toString('utf8');
    core.setDocument(uri, text);
  } catch {
    // ignore
  }
}

async function __testEnsureWorkspaceLoaded(uri: string, importedUris: string[]): Promise<void> {
  if (!core) return;

  await __testLoadFileIntoCore(uri);

  if (stdlibUriForTests) {
    await __testLoadFileIntoCore(stdlibUriForTests);
  }

  // Best-effort: load imported documents into the WASM workspace so completion
  // can see node declarations + ports.
  for (const u of importedUris) {
    await __testLoadFileIntoCore(u);
  }
}

// Test hook (used by vscode/test/e2e). Not part of the public extension API.
export function __testIsCoreLoaded(): boolean {
  return core != null;
}

export function __testAreProvidersRegistered(): boolean {
  return providersRegistered;
}

export async function __testResolveImportsJson(uri: string): Promise<string> {
  if (!core) return '{"uris":[]}';
  // Ensure the root doc + stdlib are present; core resolves imports only across
  // documents already loaded into the workspace.
  await __testLoadFileIntoCore(uri);
  if (stdlibUriForTests) {
    await __testLoadFileIntoCore(stdlibUriForTests);
  }
  return core.resolveImportsJson(uri, stdlibUriForTests ?? '');
}

export async function __testCompletionJsonWithImports(
  uri: string,
  byteOffset: number,
  importedUris: string[],
): Promise<string> {
  if (!core) return '{"items":[]}';
  await __testEnsureWorkspaceLoaded(uri, importedUris);
  return core.completionJsonWithImports(uri, byteOffset, importedUris);
}

export async function __testCompletionJsonAtPositionWithImports(
  uri: string,
  line: number,
  character: number,
  importedUris: string[],
): Promise<string> {
  if (!core) return '{"items":[]}';
  await __testEnsureWorkspaceLoaded(uri, importedUris);

  const doc = await vscode.workspace.openTextDocument(vscode.Uri.parse(uri));
  const pos = new vscode.Position(line, character);
  const byteOffset = byteOffsetAtPosition(doc, pos);

  const raw = core.completionJsonWithImports(uri, byteOffset, importedUris);
  try {
    const parsed: unknown = JSON.parse(raw);
    if (parsed && typeof parsed === 'object') {
      (parsed as Record<string, unknown>).__debug = {
        uri,
        line,
        character,
        byteOffset,
      };
      return JSON.stringify(parsed);
    }
    return raw;
  } catch {
    return raw;
  }
}

export async function __testHoverJsonWithImports(
  uri: string,
  byteOffset: number,
  importedUris: string[],
): Promise<string> {
  if (!core) return '{}';
  await __testEnsureWorkspaceLoaded(uri, importedUris);
  return core.hoverJsonWithImports(uri, byteOffset, importedUris);
}

export async function __testDefinitionJsonWithImports(
  uri: string,
  byteOffset: number,
  importedUris: string[],
): Promise<string> {
  if (!core) return '{"locations":[]}';
  await __testEnsureWorkspaceLoaded(uri, importedUris);
  return core.definitionJsonWithImports(uri, byteOffset, importedUris);
}

export function __testDocumentSymbolsJson(uri: string): string {
  if (!core) return '{"symbols":[]}';
  return core.documentSymbolsJson(uri);
}

function registerTestCommands(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.commands.registerCommand('bt-dsl.__test.isCoreLoaded', () => {
      return core != null;
    }),
    vscode.commands.registerCommand('bt-dsl.__test.areProvidersRegistered', () => {
      return providersRegistered;
    }),
    vscode.commands.registerCommand('bt-dsl.__test.resolveImportsJson', async (uri: string) =>
      __testResolveImportsJson(uri),
    ),
    vscode.commands.registerCommand(
      'bt-dsl.__test.completionJsonWithImports',
      async (uri: string, byteOffset: number, importedUris: string[]) =>
        __testCompletionJsonWithImports(uri, byteOffset, importedUris),
    ),
    vscode.commands.registerCommand(
      'bt-dsl.__test.completionJsonAtPositionWithImports',
      async (uri: string, line: number, character: number, importedUris: string[]) =>
        __testCompletionJsonAtPositionWithImports(uri, line, character, importedUris),
    ),
    vscode.commands.registerCommand(
      'bt-dsl.__test.hoverJsonWithImports',
      async (uri: string, byteOffset: number, importedUris: string[]) =>
        __testHoverJsonWithImports(uri, byteOffset, importedUris),
    ),
    vscode.commands.registerCommand(
      'bt-dsl.__test.definitionJsonWithImports',
      async (uri: string, byteOffset: number, importedUris: string[]) =>
        __testDefinitionJsonWithImports(uri, byteOffset, importedUris),
    ),
    vscode.commands.registerCommand('bt-dsl.__test.documentSymbolsJson', (uri: string) => {
      return __testDocumentSymbolsJson(uri);
    }),
  );
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  // Load WASM module & create a workspace handle.
  try {
    core = await BtDslCore.create(context);
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    void vscode.window.showErrorMessage(
      `BT DSL: failed to load core WASM. Run the workspace script "build:wasm" and ensure emsdk is activated. Details: ${msg}`,
    );
    return;
  }

  context.subscriptions.push({ dispose: () => core?.dispose() });

  // Keep a stable stdlib URI for e2e/test helpers.
  stdlibUriForTests = vscode.Uri.file(context.asAbsolutePath('stdlib/StandardNodes.bt')).toString();

  registerBtDslProviders(context, core);
  providersRegistered = true;

  registerTestCommands(context);
}

export function deactivate(): void {
  core?.dispose();
  core = undefined;
  providersRegistered = false;
  stdlibUriForTests = undefined;
}
