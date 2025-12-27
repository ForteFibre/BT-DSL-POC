import type { LangiumCoreServices, LangiumDocument } from "langium";
import { URI } from "langium";
import * as fs from "node:fs";
import * as path from "node:path";
import type { Program } from "../language/generated/ast.js";
import { isProgram } from "../language/generated/ast.js";

/**
 * Extract the LangiumDocument from a file path.
 */
export async function extractDocument(
  filePath: string,
  services: LangiumCoreServices
): Promise<LangiumDocument> {
  const content = fs.readFileSync(filePath, "utf-8");
  const uri = URI.file(filePath);

  const document = services.shared.workspace.LangiumDocumentFactory.fromString(
    content,
    uri
  );
  services.shared.workspace.LangiumDocuments.addDocument(document);

  await services.shared.workspace.DocumentBuilder.build([document], {
    validation: true,
  });

  return document;
}

/**
 * Extract the LangiumDocument from a file path, additionally loading all transitive `.bt` imports.
 *
 * This is mainly for the CLI, where we don't have a WorkspaceManager that indexes the whole workspace.
 */
export async function extractDocumentWithBtImports(
  filePath: string,
  services: LangiumCoreServices
): Promise<LangiumDocument> {
  const documents = services.shared.workspace.LangiumDocuments;
  const builder = services.shared.workspace.DocumentBuilder;

  const visited = new Set<string>();
  const loaded: LangiumDocument[] = [];

  const loadOne = async (
    fsPath: string
  ): Promise<LangiumDocument | undefined> => {
    const resolved = path.resolve(fsPath);
    if (visited.has(resolved)) {
      return documents.getDocument(URI.file(resolved));
    }
    visited.add(resolved);

    if (!fs.existsSync(resolved)) {
      return undefined;
    }

    const uri = URI.file(resolved);
    let document = documents.getDocument(uri);
    if (!document) {
      const content = fs.readFileSync(resolved, "utf-8");
      document = services.shared.workspace.LangiumDocumentFactory.fromString(
        content,
        uri
      );
      documents.addDocument(document);
    }

    loaded.push(document);

    // NOTE:
    // We intentionally do NOT call DocumentBuilder here.
    // LangiumDocumentFactory.fromString already parses the document, which is sufficient to read imports.
    // Building would also perform linking, which can cache unresolved references before imports are loaded.

    const root = document.parseResult.value;
    if (isProgram(root)) {
      const program = root as Program;
      const baseDir = path.dirname(resolved);
      for (const imp of program.imports) {
        const importPath = imp.path;
        if (!importPath.toLowerCase().endsWith(".bt")) continue;
        const child = path.resolve(baseDir, importPath);
        await loadOne(child);
      }
    }

    return document;
  };

  const entry = await loadOne(filePath);
  if (!entry) {
    throw new Error(`File not found: ${filePath}`);
  }

  // Validate everything we loaded as a group.
  await builder.build(loaded, { validation: true });

  return entry;
}

/**
 * Extract the parsed AST node from a file path.
 */
export async function extractAstNode<T>(
  filePath: string,
  services: LangiumCoreServices
): Promise<T> {
  const document = await extractDocument(filePath, services);
  return document.parseResult.value as T;
}
