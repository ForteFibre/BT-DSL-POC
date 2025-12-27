import { URI, UriUtils } from "langium";
import type { LangiumDocument } from "langium";
import type { BtDslServices } from "./bt-dsl-module.js";
import { isProgram } from "./generated/ast.js";

/**
 * Compute the transitive `.bt` import closure starting from `start`.
 *
 * Notes:
 * - This is intentionally synchronous (no filesystem reads). It only traverses documents that
 *   already exist in `LangiumDocuments`.
 * - The language server's WorkspaceManager is expected to have created/imported documents.
 * - The built-in nodes document (builtin-nodes.bt) is always included when present.
 */
export function computeBtImportClosureUris(
  services: BtDslServices,
  start: LangiumDocument | URI
): Set<string> {
  const documents = services.shared.workspace.LangiumDocuments;
  const startDoc = start instanceof URI ? documents.getDocument(start) : start;

  const out = new Set<string>();

  // Always include builtin-nodes.bt if it exists.
  for (const doc of documents.all.toArray()) {
    if (
      services.manifest.ManifestManager.isBuiltinDocument(doc.uri.toString())
    ) {
      out.add(doc.uri.toString());
    }
  }

  if (!startDoc) {
    return out;
  }

  const queue: LangiumDocument[] = [startDoc];
  out.add(startDoc.uri.toString());

  while (queue.length > 0) {
    const doc = queue.shift()!;
    const root = doc.parseResult.value;
    if (!isProgram(root)) {
      continue;
    }

    const baseDir = UriUtils.dirname(doc.uri);

    for (const imp of root.imports ?? []) {
      const importPath = (imp.path ?? "").trim();
      if (!importPath) continue;

      const candidates = normalizeImportCandidates(importPath);
      for (const candidate of candidates) {
        if (!candidate.toLowerCase().endsWith(".bt")) continue;

        const resolved = UriUtils.resolvePath(baseDir, candidate);
        const resolvedKey = resolved.toString();
        if (out.has(resolvedKey)) break;

        const imported = documents.getDocument(resolved);
        if (!imported) continue;

        out.add(resolvedKey);
        queue.push(imported);
        break; // first candidate that exists wins
      }
    }
  }

  return out;
}

function normalizeImportCandidates(pathText: string): string[] {
  // Support extension-less imports as convenience: import "./foo" -> try "./foo.bt".
  if (/[.][A-Za-z0-9]+$/.test(pathText)) {
    return [pathText];
  }
  return [pathText, `${pathText}.bt`];
}
