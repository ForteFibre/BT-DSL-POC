import { beforeEach, describe, expect, it } from "vitest";
import { NodeFileSystem } from "langium/node";
import { URI } from "langium";
import type {
  CompletionParams,
  DocumentFormattingParams,
  DocumentHighlightParams,
  DefinitionParams,
  HoverParams,
} from "vscode-languageserver";
import { createBtDslServices } from "../src/language/bt-dsl-module.js";

function applyEdits(
  doc: { textDocument: { getText(): string; offsetAt(p: any): number } },
  edits: { range: { start: any; end: any }; newText: string }[]
): string {
  let text = doc.textDocument.getText();
  // Apply in reverse order to keep offsets stable.
  const sorted = [...edits].sort((a, b) => {
    const aStart = doc.textDocument.offsetAt(a.range.start);
    const bStart = doc.textDocument.offsetAt(b.range.start);
    return bStart - aStart;
  });
  for (const e of sorted) {
    const start = doc.textDocument.offsetAt(e.range.start);
    const end = doc.textDocument.offsetAt(e.range.end);
    text = text.slice(0, start) + e.newText + text.slice(end);
  }
  return text;
}

describe("LSP feature providers (unit)", () => {
  let services: ReturnType<typeof createBtDslServices>;
  let testId = 0;

  beforeEach(() => {
    services = createBtDslServices(NodeFileSystem);
    testId++;
  });

  async function buildDoc(content: string, uri: URI) {
    // Ensure builtin nodes are loaded before indexing/validation.
    await services.BtDsl.manifest.ManifestManager.initialize();
    const document =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        content,
        uri
      );
    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(document);
    await services.BtDsl.shared.workspace.DocumentBuilder.build([document], {
      validation: true,
    });
    return document;
  }

  it("definition: nodeName should jump to TreeDef across documents", async () => {
    const uri1 = URI.file(`/test/lsp-def-${testId}-sub.bt`);
    const uri2 = URI.file(`/test/lsp-def-${testId}-main.bt`);

    const subDoc =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        `Tree Sub() { Sequence() {} }\n`,
        uri1
      );
    // With import-closure scoping, TreeDef from another file is only visible after importing it.
    const mainText = `import "lsp-def-${testId}-sub.bt"\n\nTree Main() { Sub() }\n`;
    const mainDoc =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        mainText,
        uri2
      );

    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(subDoc);
    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(mainDoc);

    await services.BtDsl.shared.workspace.DocumentBuilder.build(
      [subDoc, mainDoc],
      { validation: true }
    );

    const offset = mainText.indexOf("Sub");
    expect(offset).toBeGreaterThanOrEqual(0);

    const position = mainDoc.textDocument.positionAt(offset + 1);
    const params: DefinitionParams = {
      textDocument: { uri: mainDoc.uri.toString() },
      position,
    };

    const links = await services.BtDsl.lsp.DefinitionProvider!.getDefinition(
      mainDoc,
      params
    );
    expect(links && links.length > 0).toBe(true);
    expect(links?.some((l) => l.targetUri === subDoc.uri.toString())).toBe(
      true
    );
  });

  it("hover: manifest node should show ports", async () => {
    const text = `declare Action TestAction(\n  /// position\n  in pos: Int,\n  out found: Bool\n)\n\nTree Main() { TestAction() }\n`;
    const uri = URI.file(`/test/lsp-hover-${testId}.bt`);
    const doc = await buildDoc(text, uri);

    // Pick the usage site in the tree body (not the declaration).
    const offset = text.lastIndexOf("TestAction");
    const params: HoverParams = {
      textDocument: { uri: doc.uri.toString() },
      position: doc.textDocument.positionAt(offset + 1),
    };

    const hover = await services.BtDsl.lsp.HoverProvider!.getHoverContent(
      doc,
      params
    );
    expect(hover).toBeTruthy();

    let value = "";
    const contents = hover?.contents;
    if (typeof contents === "string") {
      value = contents;
    } else if (Array.isArray(contents)) {
      value = contents
        .map((c) => (typeof c === "string" ? c : c.value))
        .join("\n");
    } else if (contents) {
      value = contents.value;
    }

    expect(value).toContain("TestAction");
    expect(value).toContain("Ports");
    expect(value).toContain("pos");
  });

  it("documentHighlight: manifest node reference should not throw", async () => {
    const text = `declare Action TestAction()\nTree Main() { Sequence { TestAction() TestAction() } }\n`;
    const uri = URI.file(`/test/lsp-highlight-${testId}.bt`);
    const doc = await buildDoc(text, uri);

    const offset = text.indexOf("TestAction");
    expect(offset).toBeGreaterThanOrEqual(0);

    const params: DocumentHighlightParams = {
      textDocument: { uri: doc.uri.toString() },
      position: doc.textDocument.positionAt(offset + 1),
    };

    const highlights =
      await services.BtDsl.lsp.DocumentHighlightProvider!.getDocumentHighlight(
        doc,
        params
      );

    expect(highlights).toBeTruthy();
    expect((highlights ?? []).length).toBeGreaterThanOrEqual(2);
  });

  it("formatting: formatDocument should return edits for a valid document", async () => {
    const messy = `Tree Main(){\nSequence(){\n}\n}\n`;
    const uri = URI.file(`/test/lsp-format-${testId}.bt`);
    const doc = await buildDoc(messy, uri);
    const params: DocumentFormattingParams = {
      textDocument: { uri: doc.uri.toString() },
      options: {
        tabSize: 2,
        insertSpaces: true,
      },
    };

    const edits = await services.BtDsl.lsp.Formatter!.formatDocument(
      doc,
      params
    );
    expect(Array.isArray(edits)).toBe(true);
    // We expect at least one change for this compact style.
    expect(edits.length).toBeGreaterThan(0);

    const formatted = applyEdits(doc, edits);
    expect(formatted).toContain("Tree Main()");
    expect(formatted).toContain("Sequence()");
  });

  it("formatting: comments (doc/line/block) must never be omitted", async () => {
    const messy = `//! inner doc\n\n/// Tree doc\nTree Main(){\n  // line comment\n  Sequence( /* block comment */ target: "enemy" ){}\n}\n`;
    const uri = URI.file(`/test/lsp-format-comments-${testId}.bt`);
    const doc = await buildDoc(messy, uri);
    const params: DocumentFormattingParams = {
      textDocument: { uri: doc.uri.toString() },
      options: { tabSize: 2, insertSpaces: true },
    };

    const edits = await services.BtDsl.lsp.Formatter!.formatDocument(
      doc,
      params
    );
    const formatted =
      edits.length > 0 ? applyEdits(doc, edits) : doc.textDocument.getText();

    // Must keep exact comment markers.
    expect(formatted).toContain("//! inner doc");
    expect(formatted).toContain("/// Tree doc");
    expect(formatted).toContain("// line comment");
    expect(formatted).toContain("/* block comment */");

    // Must keep the program/tree structure too.
    expect(formatted).toContain("Tree Main()");
    expect(formatted).toContain("Sequence(");
  });

  it("completion: inside children block whitespace suggests nodes/trees", async () => {
    const text = `declare Action TestAction()\nTree Main() { Sequence {\n\n} }\n`;
    const uri = URI.file(`/test/lsp-completion-children-${testId}.bt`);
    const doc = await buildDoc(text, uri);

    const offset = text.indexOf("\n\n") + 1; // inside the empty line
    const params: CompletionParams = {
      textDocument: { uri: doc.uri.toString() },
      position: doc.textDocument.positionAt(offset),
    };

    const list = await services.BtDsl.lsp.CompletionProvider!.getCompletion(
      doc,
      params
    );
    expect(list).toBeTruthy();
    const labels = (list?.items ?? []).map((i) => i.label.toString());
    expect(labels.length).toBeGreaterThan(0);
    expect(labels).toContain("Main");
    expect(labels).toContain("TestAction");
  });

  it("completion: right after '(' suggests port names", async () => {
    const text = `declare Action TestAction(in pos: Int, out found: Bool)\nTree Main() {\n  TestAction(\n    pos: 1\n  )\n}\n`;
    const uri = URI.file(`/test/lsp-completion-props-${testId}.bt`);
    const doc = await buildDoc(text, uri);

    // Use the call site in the Tree, not the declaration.
    const treeStart = text.indexOf("Tree Main");
    const callStart = text.indexOf("TestAction(", treeStart);
    const offset = callStart + "TestAction(".length;
    const params: CompletionParams = {
      textDocument: { uri: doc.uri.toString() },
      position: doc.textDocument.positionAt(offset),
    };

    const list = await services.BtDsl.lsp.CompletionProvider!.getCompletion(
      doc,
      params
    );
    expect(list).toBeTruthy();
    const labels = (list?.items ?? []).map((i) => i.label.toString());
    expect(labels).toContain("pos");
    expect(labels).toContain("found");
  });

  it("completion: no synthetic suggestions inside comments", async () => {
    const text = `declare Action TestAction(in pos: Int)\nTree Main() {\n  // TestAction(\n  Sequence() {}\n}\n`;
    const uri = URI.file(`/test/lsp-completion-comment-${testId}.bt`);
    const doc = await buildDoc(text, uri);

    const offset = text.indexOf("TestAction") + 2;
    const params: CompletionParams = {
      textDocument: { uri: doc.uri.toString() },
      position: doc.textDocument.positionAt(offset),
    };

    const list = await services.BtDsl.lsp.CompletionProvider!.getCompletion(
      doc,
      params
    );
    const labels = (list?.items ?? []).map((i) => i.label.toString());

    // We only assert that our synthetic IDs don't leak into comment completion.
    expect(labels).not.toContain("TestAction");
    expect(labels).not.toContain("pos");
  });
});
