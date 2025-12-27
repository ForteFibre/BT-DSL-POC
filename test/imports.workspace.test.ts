import { describe, it, expect, beforeEach, afterEach } from "vitest";
import { NodeFileSystem } from "langium/node";
import { createBtDslServices } from "../src/language/bt-dsl-module.js";
import { URI } from "langium";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

function getErrors(diags: any[] | undefined) {
  return (diags ?? []).filter((d) => d.severity === 1);
}

describe("Workspace imports", () => {
  let tempRoot: string;

  beforeEach(() => {
    tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), "bt-dsl-import-test-"));
  });

  afterEach(() => {
    try {
      fs.rmSync(tempRoot, { recursive: true, force: true });
    } catch {
      // ignore
    }
  });

  it("should load .bt imports outside workspace folder", async () => {
    const workspaceDir = path.join(tempRoot, "workspace");
    const sharedDir = path.join(tempRoot, "shared");
    fs.mkdirSync(workspaceDir, { recursive: true });
    fs.mkdirSync(sharedDir, { recursive: true });

    const sharedNodesPath = path.join(sharedDir, "nodes.bt");
    fs.writeFileSync(sharedNodesPath, `declare Action Foo()\n`, "utf-8");

    const mainPath = path.join(workspaceDir, "main.bt");
    fs.writeFileSync(
      mainPath,
      `import "../shared/nodes.bt"\n\nTree Main() {\n  Foo()\n}\n`,
      "utf-8"
    );

    const services = createBtDslServices(NodeFileSystem);
    await services.BtDsl.manifest.ManifestManager.initialize();

    // Ensure validation runs during workspace initialization.
    services.shared.workspace.WorkspaceManager.initialBuildOptions = {
      validation: true,
    };

    await services.shared.workspace.WorkspaceManager.initializeWorkspace([
      { uri: URI.file(workspaceDir).toString(), name: "ws" },
    ]);

    const mainDoc = services.shared.workspace.LangiumDocuments.getDocument(
      URI.file(mainPath)
    );
    expect(mainDoc, "main.bt should be loaded").toBeTruthy();

    const errors = getErrors(mainDoc?.diagnostics);
    expect(
      errors,
      `expected no errors, got: ${JSON.stringify(errors, null, 2)}`
    ).toHaveLength(0);
  });
});
