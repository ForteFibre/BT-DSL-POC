import * as assert from 'node:assert/strict';
import * as path from 'node:path';
import * as vscode from 'vscode';

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

async function waitFor<T>(
  fn: () => Promise<T | undefined | null | false> | (T | undefined | null | false),
  timeoutMs = 30000,
  intervalMs = 100,
): Promise<T> {
  const start = Date.now();
  for (;;) {
    const v = await fn();
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

suite('bt-dsl (LSP) VS Code e2e', function () {
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

    const completion = await waitFor<vscode.CompletionList>(async () => {
      const list = await vscode.commands.executeCommand<vscode.CompletionList>(
        'vscode.executeCompletionItemProvider',
        doc.uri,
        posInside,
      );
      return list;
    });
    const labels = (completion.items ?? []).map((i) => i.label.toString());
    assert.ok(
      labels.includes('TestAction'),
      `Expected TestAction in completion: ${labels.slice(0, 30).join(', ')}`,
    );

    // Completion while typing an arg name should suggest ports like pos/found
    const callStart = text.indexOf('TestAction(');
    assert.ok(callStart >= 0);
    // Place cursor inside the first arg name ("pos") to avoid boundary edge-cases.
    const afterParen = doc.positionAt(callStart + 'TestAction('.length + 1);

    const completion2 = await waitFor<vscode.CompletionList>(async () => {
      const list = await vscode.commands.executeCommand<vscode.CompletionList>(
        'vscode.executeCompletionItemProvider',
        doc.uri,
        afterParen,
      );
      return list;
    });
    const labels2 = (completion2.items ?? []).map((i) => i.label.toString());
    const lineText = doc.lineAt(afterParen.line).text;
    assert.ok(
      labels2.includes('pos'),
      `Expected 'pos' in port completion: ${labels2.slice(0, 30).join(', ')}
at ${afterParen.line + 1}:${afterParen.character + 1} line="${lineText}"
`,
    );
    assert.ok(
      labels2.includes('found'),
      `Expected 'found' in port completion: ${labels2.slice(0, 30).join(', ')}
at ${afterParen.line + 1}:${afterParen.character + 1} line="${lineText}"
`,
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

    const defs = await waitFor<(vscode.Location | vscode.LocationLink)[]>(async () => {
      const res = await vscode.commands.executeCommand<(vscode.Location | vscode.LocationLink)[]>(
        'vscode.executeDefinitionProvider',
        doc.uri,
        pos,
      );
      return res && res.length > 0 ? res : false;
    });

    const first = defs[0];
    assert.ok(first, 'Expected at least one definition');

    const defUri = (first as vscode.Location).uri ?? (first as vscode.LocationLink).targetUri;
    assert.ok(
      defUri.fsPath.endsWith(path.join('fixture-workspace', 'test-nodes.bt')),
      `Expected definition in test-nodes.bt, got: ${defUri.toString()}`,
    );
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

    const hovers = await waitFor<vscode.Hover[]>(async () => {
      const res = await vscode.commands.executeCommand<vscode.Hover[]>(
        'vscode.executeHoverProvider',
        doc.uri,
        nodePos,
      );
      return res && res.length > 0 ? res : false;
    });

    assert.ok(hovers.length > 0, 'Expected hover results');

    const symbols = await waitFor<vscode.DocumentSymbol[]>(async () => {
      const res = await vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
        'vscode.executeDocumentSymbolProvider',
        doc.uri,
      );
      return res && res.length > 0 ? res : false;
    });

    assert.ok(symbols.length > 0, 'Expected document symbols');

    const hoverText = hovers
      .flatMap((h) => h.contents)
      .map((c) => (typeof c === 'string' ? c : c.value))
      .join('\n');
    assert.ok(hoverText.length > 0, 'Expected hover text');
    assert.ok(
      /TestAction/.test(hoverText) && (/Ports/i.test(hoverText) || /pos/.test(hoverText)),
      `Unexpected hover text: ${hoverText}`,
    );

    const names = symbols.map((s) => s.name);
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
