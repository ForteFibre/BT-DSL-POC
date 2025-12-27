import * as assert from "node:assert/strict";
import * as path from "node:path";
import * as vscode from "vscode";

function hoverContentsToText(contents: vscode.Hover["contents"]): string {
  return contents
    .map((c) => {
      if (typeof c === "string") return c;
      if (typeof c === "object" && c && "value" in c)
        return (c as any).value ?? "";
      return "";
    })
    .join("\n");
}

async function waitFor<T>(
  fn: () => T | undefined | null | false,
  timeoutMs = 30000,
  intervalMs = 200
): Promise<T> {
  const start = Date.now();
  for (;;) {
    const v = fn();
    if (v) return v as T;
    if (Date.now() - start > timeoutMs) {
      throw new Error(`Timed out after ${timeoutMs}ms`);
    }
    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }
}

async function waitForAsync<T>(
  fn: () => Thenable<T | undefined | null | false>,
  timeoutMs = 30000,
  intervalMs = 200
): Promise<T> {
  const start = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v as T;
    if (Date.now() - start > timeoutMs) {
      throw new Error(`Timed out after ${timeoutMs}ms`);
    }
    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }
}

suite("bt-dsl VS Code integration", function () {
  this.timeout(60000);

  test("activates and provides completion/definition/hover/formatting", async () => {
    const ext = vscode.extensions.getExtension("bt-dsl.bt-dsl");
    assert.ok(ext, "Extension bt-dsl.bt-dsl should be discoverable in tests");

    // Open fixture file
    const ws = vscode.workspace.workspaceFolders?.[0];
    assert.ok(ws, "A workspace folder should be opened by vscode-test");

    const docUri = vscode.Uri.file(path.join(ws.uri.fsPath, "main.bt"));
    const doc = await vscode.workspace.openTextDocument(docUri);
    await vscode.window.showTextDocument(doc);

    // Trigger activation
    await ext.activate();

    // Wait for diagnostics to appear (server has parsed + validated)
    await waitFor(() => vscode.languages.getDiagnostics(docUri), 30000);

    // We expect no errors in the fixture
    const diags = vscode.languages.getDiagnostics(docUri);
    const errors = diags.filter(
      (d) => d.severity === vscode.DiagnosticSeverity.Error
    );
    assert.equal(
      errors.length,
      0,
      `Expected no errors, got: ${errors.map((e) => e.message).join("; ")}`
    );

    // Completion at the node name should include TestAction from imported declarations
    const text = doc.getText();
    const idx = text.indexOf("TestAction");
    assert.ok(idx >= 0, "Fixture should contain TestAction");
    const pos = doc.positionAt(idx + 2); // inside the identifier

    const completion =
      await vscode.commands.executeCommand<vscode.CompletionList>(
        "vscode.executeCompletionItemProvider",
        docUri,
        pos
      );
    assert.ok(completion, "CompletionList should be returned");
    const labels = completion.items.map((i) => i.label.toString());
    assert.ok(
      labels.includes("TestAction"),
      `Expected completion to include TestAction, got: ${labels
        .slice(0, 30)
        .join(", ")}`
    );

    // Definition should jump to test-nodes.bt
    const defs = await vscode.commands.executeCommand<vscode.LocationLink[]>(
      "vscode.executeDefinitionProvider",
      docUri,
      pos
    );
    assert.ok(
      defs && defs.length > 0,
      "Expected at least one definition result"
    );
    const defUri = (defs[0] as any).targetUri as vscode.Uri | undefined;
    assert.ok(defUri, "Definition should return a targetUri");
    assert.ok(
      defUri.fsPath.endsWith(path.join("fixture-workspace", "test-nodes.bt"))
    );

    // Hover should include port info
    const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
      "vscode.executeHoverProvider",
      docUri,
      pos
    );
    assert.ok(hovers && hovers.length > 0, "Expected hover result");
    const hoverText = hovers
      .map((h) => hoverContentsToText(h.contents))
      .join("\n");
    assert.ok(
      /Ports/i.test(hoverText) || /input|output/i.test(hoverText),
      `Unexpected hover: ${hoverText}`
    );

    // Formatting should produce at least one edit (or no edits if already formatted)
    const edits = await waitForAsync(
      () =>
        vscode.commands.executeCommand<vscode.TextEdit[] | undefined>(
          "vscode.executeFormatDocumentProvider",
          docUri,
          { insertSpaces: true, tabSize: 4 }
        ),
      30000,
      200
    );
    assert.ok(Array.isArray(edits), "Formatting should return an array");
    assert.ok(
      edits.length > 0,
      "Expected formatting to produce at least one edit"
    );
  });
});
