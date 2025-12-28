import * as assert from 'node:assert/strict';
import * as path from 'node:path';
import * as vscode from 'vscode';

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

async function waitFor<T>(
  fn: () => T | undefined | null | false,
  timeoutMs = 30000,
  intervalMs = 100,
): Promise<T> {
  const start = Date.now();
  for (;;) {
    const v = fn();
    if (v) return v as T;
    if (Date.now() - start > timeoutMs) {
      throw new Error(`Timed out after ${timeoutMs}ms`);
    }
    await sleep(intervalMs);
  }
}

async function ensureBtDslExtensionActivated(): Promise<void> {
  const ext = vscode.extensions.getExtension('bt-dsl.bt-dsl-vscode');
  assert.ok(ext, 'Extension should be discoverable');
  if (!ext.isActive) {
    await ext.activate();
  }

  // Verify test hooks via commands (works even for ESM extensions where ext.exports may be undefined).
  const coreLoaded = await vscode.commands.executeCommand<boolean>('bt-dsl.__test.isCoreLoaded');
  assert.equal(coreLoaded, true, 'WASM core should be loaded');

  const providers = await vscode.commands.executeCommand<boolean>(
    'bt-dsl.__test.areProvidersRegistered',
  );
  assert.equal(providers, true, 'BT DSL providers should be registered');
}

async function openBtDslDocument(docUri: vscode.Uri): Promise<vscode.TextDocument> {
  const doc = await vscode.workspace.openTextDocument(docUri);
  // Be explicit: in some test-electron runs, language association may lag.
  const btDoc =
    doc.languageId === 'bt-dsl'
      ? doc
      : await vscode.languages.setTextDocumentLanguage(doc, 'bt-dsl');
  await vscode.window.showTextDocument(btDoc);
  assert.equal(btDoc.languageId, 'bt-dsl');
  return btDoc;
}

function utf8ByteOffsetAtUtf16Offset(text: string, utf16Offset: number): number {
  // NOTE:
  // VS Code APIs use UTF-16 code unit offsets.
  // Buffer.byteLength(text.slice(...)) can become wrong if the slice splits a
  // surrogate pair (or otherwise doesn't align to code points).
  // Compute the UTF-8 byte count by iterating code points safely.
  if (utf16Offset <= 0) return 0;

  let bytes = 0;
  let i = 0;
  while (i < text.length && i < utf16Offset) {
    const cp = text.codePointAt(i);
    if (cp === undefined) break;

    const utf16Units = cp > 0xffff ? 2 : 1;
    if (i + utf16Units > utf16Offset) {
      // Don't count partial surrogate pairs.
      break;
    }

    if (cp <= 0x7f) bytes += 1;
    else if (cp <= 0x7ff) bytes += 2;
    else if (cp <= 0xffff) bytes += 3;
    else bytes += 4;

    i += utf16Units;
  }

  return bytes;
}

function parseImportedUris(raw: string): string[] {
  try {
    const parsed = JSON.parse(raw) as { uris?: unknown };
    return Array.isArray(parsed.uris)
      ? parsed.uris.filter((x): x is string => typeof x === 'string')
      : [];
  } catch {
    return [];
  }
}

async function completionJsonWithImportsAtPosition(
  uri: vscode.Uri,
  pos: vscode.Position,
  importedUris: string[],
): Promise<{ items?: Array<{ label: string; kind?: string }> }> {
  const raw = await vscode.commands.executeCommand<string>(
    'bt-dsl.__test.completionJsonAtPositionWithImports',
    uri.toString(),
    pos.line,
    pos.character,
    importedUris,
  );
  return JSON.parse(raw);
}

suite('bt-dsl (WASM core) VS Code e2e', function () {
  this.timeout(90000);

  test('activates and produces no error diagnostics on fixture', async () => {
    await ensureBtDslExtensionActivated();

    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws, 'workspace folder should be opened by vscode-test');

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, 'main.bt'));
    await openBtDslDocument(docUri);

    await waitFor(() => vscode.languages.getDiagnostics(docUri));

    const diags = vscode.languages.getDiagnostics(docUri);
    const errors = diags.filter((d) => d.severity === vscode.DiagnosticSeverity.Error);
    assert.equal(
      errors.length,
      0,
      `Expected no errors, got: ${errors.map((e) => e.message).join('; ')}`,
    );
  });

  test('completion: suggests declared node and port names inside args', async () => {
    await ensureBtDslExtensionActivated();

    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws);

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, 'main.bt'));
    const doc = await openBtDslDocument(docUri);

    const text = doc.getText();

    // Completion on node identifier should include TestAction
    const idx = text.indexOf('TestAction');
    assert.ok(idx >= 0);
    const posInside = doc.positionAt(idx + 2);

    const importsRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.resolveImportsJson',
      doc.uri.toString(),
    );
    const importedUris = parseImportedUris(importsRaw);

    const completion = await completionJsonWithImportsAtPosition(doc.uri, posInside, importedUris);
    const labels = (completion.items ?? []).map((i) => i.label);
    assert.ok(
      labels.includes('TestAction'),
      `Expected TestAction in completion: ${labels.slice(0, 30).join(', ')}`,
    );

    // Completion while typing an arg name should suggest ports like pos/found
    const callStart = text.indexOf('TestAction(');
    assert.ok(callStart >= 0);
    // Place cursor inside the first arg name ("pos") to avoid boundary edge-cases.
    const afterParen = doc.positionAt(callStart + 'TestAction('.length + 1);

    const completion2 = await completionJsonWithImportsAtPosition(
      doc.uri,
      afterParen,
      importedUris,
    );
    const labels2 = (completion2.items ?? []).map((i) => i.label);
    const lineText = doc.lineAt(afterParen.line).text;
    const dbg = JSON.stringify((completion2 as any).__debug ?? null);
    assert.ok(
      labels2.includes('pos'),
      `Expected 'pos' in port completion: ${labels2.slice(0, 30).join(', ')}
at ${afterParen.line + 1}:${afterParen.character + 1} line="${lineText}"
debug=${dbg}`,
    );
    assert.ok(
      labels2.includes('found'),
      `Expected 'found' in port completion: ${labels2.slice(0, 30).join(', ')}
at ${afterParen.line + 1}:${afterParen.character + 1} line="${lineText}"
debug=${dbg}`,
    );
  });

  test('definition: jumps to declaration in imported file', async () => {
    await ensureBtDslExtensionActivated();

    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws);

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, 'main.bt'));
    const doc = await openBtDslDocument(docUri);

    const text = doc.getText();
    const idx = text.indexOf('TestAction');
    assert.ok(idx >= 0);
    const pos = doc.positionAt(idx + 2);

    const importsRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.resolveImportsJson',
      doc.uri.toString(),
    );
    const importedUris = parseImportedUris(importsRaw);

    const off16 = doc.offsetAt(pos);
    const byteOff = utf8ByteOffsetAtUtf16Offset(text, off16);

    const defRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.definitionJsonWithImports',
      doc.uri.toString(),
      byteOff,
      importedUris,
    );
    const defs = JSON.parse(defRaw) as {
      locations?: Array<{
        uri: string;
        range: { startByte: number; endByte: number };
      }>;
    };
    assert.ok(defs.locations && defs.locations.length > 0, 'Expected at least one definition');

    const first = defs.locations[0];
    assert.ok(first !== undefined, 'Expected at least one location');
    assert.ok(
      first.uri.endsWith('/fixture-workspace/test-nodes.bt'),
      `Expected definition in test-nodes.bt, got: ${first.uri}`,
    );
    assert.ok(first.range.endByte > first.range.startByte, 'Expected non-empty range');
  });

  test('hover and document symbols: return structured results', async () => {
    await ensureBtDslExtensionActivated();

    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws);

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, 'main.bt'));
    const doc = await openBtDslDocument(docUri);

    const text = doc.getText();

    // Hover on node usage should include port info
    const nodeIdx = text.indexOf('TestAction');
    assert.ok(nodeIdx >= 0);
    const nodePos = doc.positionAt(nodeIdx + 2);

    const importsRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.resolveImportsJson',
      doc.uri.toString(),
    );
    const importedUris = parseImportedUris(importsRaw);

    const off16 = doc.offsetAt(nodePos);
    const byteOff = utf8ByteOffsetAtUtf16Offset(text, off16);
    const hoverRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.hoverJsonWithImports',
      doc.uri.toString(),
      byteOff,
      importedUris,
    );
    const hover = JSON.parse(hoverRaw) as { contents?: string | null };
    const hoverText = hover.contents ?? '';
    assert.ok(hoverText.length > 0, 'Expected hover result');

    assert.ok(
      /TestAction/.test(hoverText) && (/Ports/i.test(hoverText) || /pos/.test(hoverText)),
      `Unexpected hover text: ${hoverText}`,
    );

    // Document symbols should include Tree Main
    const symRaw = await vscode.commands.executeCommand<string>(
      'bt-dsl.__test.documentSymbolsJson',
      doc.uri.toString(),
    );
    const sym = JSON.parse(symRaw) as { symbols?: Array<{ name: string }> };
    const names = (sym.symbols ?? []).map((s) => s.name);
    assert.ok(names.length > 0, 'Expected document symbols');
    assert.ok(
      names.includes('Main'),
      `Expected document symbols to include 'Main', got: ${names.join(', ')}`,
    );
  });

  test('formatting: formats bt-dsl documents', async () => {
    await ensureBtDslExtensionActivated();

    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws);

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, 'main.bt'));
    const doc = await openBtDslDocument(docUri);

    // Make the document intentionally uglier, then invoke format.
    const before = doc.getText();
    const sloppy = before.replace(
      'TestAction(pos: 1, found: out Found)',
      'TestAction(  pos:1,found:out Found  )',
    );

    const editor = vscode.window.activeTextEditor;
    assert.ok(editor, 'Expected an active editor');
    await editor.edit((eb) => {
      const fullRange = new vscode.Range(doc.positionAt(0), doc.positionAt(before.length));
      eb.replace(fullRange, sloppy);
    });

    await waitFor(() => vscode.window.activeTextEditor?.document === doc);

    await vscode.commands.executeCommand('editor.action.formatDocument');

    // Wait for formatting to apply.
    await waitFor(() => doc.getText() !== sloppy);

    const after = doc.getText();
    assert.ok(
      after.includes('TestAction(pos: 1, found: out Found)'),
      `Expected formatted callsite, got:\n${after}`,
    );
  });
});
