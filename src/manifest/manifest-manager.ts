import type { LangiumDocument } from "langium";
import { URI } from "langium";
import * as fsSync from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import type { BtDslServices } from "../language/bt-dsl-module.js";

/**
 * Builtin nodes loader.
 *
 * This project has fully migrated manifest definitions to `.bt` `declare ...` statements.
 * Therefore, we only need to ensure `builtin-nodes.bt` is loaded and indexed.
 */
export class ManifestManager {
  private builtinDocument: LangiumDocument | undefined;
  private initPromise: Promise<void> | undefined;
  private builtinPath: string | undefined;

  constructor(private readonly services: BtDslServices) {
    this.initPromise = this.loadBuiltinNodes();
  }

  /**
   * Check if a given URI is the builtin-nodes.bt file.
   * This is used to skip duplicate declaration validation.
   */
  isBuiltinDocument(uri: string): boolean {
    if (!this.builtinPath) return false;
    const normalizedUri = URI.file(this.builtinPath).toString();
    return uri === normalizedUri;
  }

  /**
   * Wait for initialization to complete (builtin-nodes.bt indexed).
   */
  async initialize(): Promise<void> {
    await this.initPromise;
  }

  /**
   * Compatibility shim: XML manifest import problems no longer exist.
   */
  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  getProblems(_documentUri: string): [] {
    return [];
  }

  private async loadBuiltinNodes(): Promise<void> {
    this.builtinPath = this.getBuiltinNodesPath();
    if (!this.builtinPath || !fsSync.existsSync(this.builtinPath)) {
      return;
    }

    try {
      const content = fsSync.readFileSync(this.builtinPath, "utf-8");
      const uri = URI.file(this.builtinPath);

      this.builtinDocument =
        this.services.shared.workspace.LangiumDocumentFactory.fromString(
          content,
          uri
        );
      this.services.shared.workspace.LangiumDocuments.addDocument(
        this.builtinDocument
      );

      // Build the document so it gets indexed (no validation needed).
      await this.services.shared.workspace.DocumentBuilder.build(
        [this.builtinDocument],
        { validation: false }
      );
    } catch {
      // Silently ignore errors loading builtins
    }
  }

  private getBuiltinNodesPath(): string | undefined {
    let currentFile: string;
    if (typeof __filename === "string") {
      currentFile = __filename;
    } else if (import.meta.url) {
      currentFile = fileURLToPath(import.meta.url);
    } else {
      currentFile = process.cwd();
    }
    const currentDir = path.dirname(currentFile);

    const candidates = [
      // Same directory (for bundled output)
      path.join(currentDir, "builtin-nodes.bt"),
      // Relative to out/manifest/ -> out/manifest/builtin-nodes.bt
      path.join(currentDir, "..", "manifest", "builtin-nodes.bt"),
      // Relative to out/ -> src/manifest/builtin-nodes.bt
      path.join(currentDir, "..", "..", "src", "manifest", "builtin-nodes.bt"),
      // From project root (for tests running via vitest)
      path.join(process.cwd(), "src", "manifest", "builtin-nodes.bt"),
      // From project root -> out/manifest (if tests run after build)
      path.join(process.cwd(), "out", "manifest", "builtin-nodes.bt"),
    ];

    for (const candidate of candidates) {
      if (fsSync.existsSync(candidate)) {
        return candidate;
      }
    }

    return undefined;
  }
}
