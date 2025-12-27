import {
  createConnection,
  ProposedFeatures,
} from "vscode-languageserver/node.js";
import { NodeFileSystem } from "langium/node";
import { startLanguageServer } from "langium/lsp";
import { createBtDslServices } from "../language/bt-dsl-module.js";

// Create LSP connection
const connection = createConnection(ProposedFeatures.all);

// Create shared services (filesystem + LSP connection)
const services = createBtDslServices({
  ...NodeFileSystem,
  connection,
});

// IMPORTANT:
// Load builtin node declarations before workspace initialization kicks in.
// Otherwise, the first workspace build may run without builtin-nodes.bt indexed,
// causing false "unresolved reference" diagnostics.
//
// Note: This file is bundled to CJS for VS Code, so we can't use top-level await.
(async () => {
  await services.BtDsl.manifest.ManifestManager.initialize();
  // Start the language server (this also calls connection.listen() internally)
  startLanguageServer(services.shared);
})().catch((err) => {
  // eslint-disable-next-line no-console
  console.error("Failed to start BT DSL language server:", err);
  process.exit(1);
});
