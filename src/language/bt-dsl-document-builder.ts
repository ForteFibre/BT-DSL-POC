import {
  DefaultDocumentBuilder,
  UriUtils,
  type LangiumDocument,
} from "langium";
import type { URI } from "langium";
import type { CancellationToken } from "vscode-jsonrpc";

/**
 * DocumentBuilder that makes `import "..."` act like a real dependency edge during incremental builds.
 *
 * Why this exists:
 * - Our WorkspaceManager loads transitive imports at startup, but incremental updates (typing a new
 *   import, changing an import path, etc.) need to ensure imported documents are created and built
 *   before linking/validation of the importer runs.
 * - Langium's default builder only considers the changed URIs it is given.
 */
export class BtDslDocumentBuilder extends DefaultDocumentBuilder {
  override async update(
    changed: URI[],
    deleted: URI[],
    cancelToken?: CancellationToken
  ): Promise<void> {
    const extraChanged: URI[] = [];
    const extraChangedSet = new Set<string>();
    const deletedSet = new Set(deleted.map((u) => u.toString()));

    for (const uri of changed) {
      if (cancelToken?.isCancellationRequested) break;

      const doc = await this.safeGetOrCreateDocument(uri, cancelToken);
      if (!doc) continue;

      // Only support filesystem-relative imports.
      if (doc.uri.scheme !== "file") continue;

      const text = doc.textDocument.getText();
      const importPaths = extractImportPaths(text);
      if (importPaths.length === 0) continue;

      const baseDir = UriUtils.dirname(doc.uri);
      for (const imp of importPaths) {
        const candidates = normalizeImportCandidates(imp).filter((p) =>
          p.toLowerCase().endsWith(".bt")
        );

        for (const candidate of candidates) {
          const resolved = UriUtils.resolvePath(baseDir, candidate);
          if (resolved.scheme !== "file") continue;

          const resolvedKey = resolved.toString();
          if (deletedSet.has(resolvedKey)) continue;
          if (extraChangedSet.has(resolvedKey)) continue;

          try {
            // Ensure the imported document exists in LangiumDocuments.
            await this.langiumDocuments.getOrCreateDocument(
              resolved,
              cancelToken
            );

            extraChanged.push(resolved);
            extraChangedSet.add(resolvedKey);

            // First candidate that resolves wins.
            break;
          } catch {
            // ignore here; diagnostics for missing imports are produced by our validator
          }
        }
      }
    }

    const mergedChanged = dedupeUris([...changed, ...extraChanged]);
    return super.update(mergedChanged, deleted, cancelToken);
  }

  private async safeGetOrCreateDocument(
    uri: URI,
    cancelToken?: CancellationToken
  ): Promise<LangiumDocument | undefined> {
    const existing = this.langiumDocuments.getDocument(uri);
    if (existing) return existing;

    try {
      return await this.langiumDocuments.getOrCreateDocument(uri, cancelToken);
    } catch {
      return undefined;
    }
  }
}

function dedupeUris(uris: URI[]): URI[] {
  const seen = new Set<string>();
  const out: URI[] = [];
  for (const u of uris) {
    const key = u.toString();
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(u);
  }
  return out;
}

function extractImportPaths(text: string): string[] {
  // Matches: import "..."
  // - Keep this intentionally simple and fast.
  // - The STRING terminal in our grammar is: /"([^"\\]|\\.)*"/
  const re = /^\s*import\s+("(?:[^"\\\\]|\\\\.)*")\s*$/gm;
  const out: string[] = [];

  let m: RegExpExecArray | null;
  while ((m = re.exec(text)) !== null) {
    const raw = m[1];
    out.push(unescapeStringLiteral(raw));
  }

  return out;
}

function unescapeStringLiteral(raw: string): string {
  // raw includes quotes.
  try {
    return JSON.parse(raw) as string;
  } catch {
    return raw.length >= 2 && raw.startsWith('"') && raw.endsWith('"')
      ? raw.slice(1, -1)
      : raw;
  }
}

function normalizeImportCandidates(pathText: string): string[] {
  const trimmed = pathText.trim();
  if (trimmed.length === 0) return [];

  // Support extension-less imports as a convenience: import "./foo" -> try "./foo.bt".
  if (/[.][A-Za-z0-9]+$/.test(trimmed)) {
    return [trimmed];
  }

  return [trimmed, `${trimmed}.bt`];
}
