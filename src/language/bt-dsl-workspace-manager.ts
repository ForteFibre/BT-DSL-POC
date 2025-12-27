import type { LangiumDocument } from "langium";
import { DefaultWorkspaceManager } from "langium";
import type { WorkspaceFolder } from "langium";
import { URI, UriUtils } from "langium";

/**
 * WorkspaceManager that additionally loads transitive `.bt` imports, even when the imported files
 * are located outside of the current workspace folder(s).
 *
 * Why this exists:
 * - Langium's default workspace indexing only traverses workspace folders.
 * - Our DSL has an `import "..."` statement and users expect it to bring symbols into scope.
 * - The CLI already supports this via `extractDocumentWithBtImports`, but the language server
 *   needs a similar capability at workspace initialization time.
 */
export class BtDslWorkspaceManager extends DefaultWorkspaceManager {
  protected override async performStartup(
    folders: WorkspaceFolder[]
  ): Promise<LangiumDocument[]> {
    // 1) Collect documents from workspace folders (default behavior).
    const documents = await super.performStartup(folders);

    // 2) Add any imported `.bt` files (transitively), even if they are outside the workspace.
    await this.loadImportedBtDocuments(documents);

    return documents;
  }

  private async loadImportedBtDocuments(
    seedDocuments: LangiumDocument[]
  ): Promise<void> {
    const queue: LangiumDocument[] = [...seedDocuments];
    const seen = new Set<string>(seedDocuments.map((d) => d.uri.toString()));

    while (queue.length > 0) {
      const doc = queue.shift()!;
      const baseUri = doc.uri;

      // We only support filesystem imports here.
      if (baseUri.scheme !== "file") continue;

      const text = doc.textDocument.getText();
      const importPaths = extractImportPaths(text);
      if (importPaths.length === 0) continue;

      const baseDir = UriUtils.dirname(baseUri);
      for (const imp of importPaths) {
        const candidates = normalizeImportCandidates(imp).filter((p) =>
          p.toLowerCase().endsWith(".bt")
        );

        for (const candidate of candidates) {
          const resolved = UriUtils.resolvePath(baseDir, candidate);
          if (resolved.scheme !== "file") continue;

          const key = resolved.toString();
          if (seen.has(key)) continue;

          try {
            const imported =
              await this.langiumDocuments.getOrCreateDocument(resolved);
            seen.add(key);

            // Ensure the document participates in the upcoming build.
            if (!this.langiumDocuments.hasDocument(imported.uri)) {
              this.langiumDocuments.addDocument(imported);
            }

            seedDocuments.push(imported);
            queue.push(imported);
          } catch {
            // If the file doesn't exist or cannot be read, we just skip.
            // The validator will still show unresolved reference errors later.
          }

          // First candidate that resolves wins.
          if (seen.has(key)) break;
        }
      }
    }
  }
}

function extractImportPaths(text: string): string[] {
  // Matches: import "..."
  // - We keep this intentionally simple and fast.
  // - The STRING terminal in our grammar is: /"([^"\\]|\\.)*"/
  const re = /^\s*import\s+("(?:[^"\\]|\\.)*")\s*$/gm;
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
  // Try JSON first (covers common escapes), then fall back to a conservative unquote.
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
  // We keep the original candidate first.
  if (/[.][A-Za-z0-9]+$/.test(trimmed)) {
    return [trimmed];
  }

  return [trimmed, `${trimmed}.bt`];
}
