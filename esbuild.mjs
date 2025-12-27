import * as esbuild from "esbuild";
import * as fs from "fs";
import * as path from "path";

const watch = process.argv.includes("--watch");

const buildConfigs = [
  // CLI bundle (ESM): used by bin/cli.js
  {
    entryPoints: ["src/cli/index.ts"],
    outfile: "out/cli/index.js",
    bundle: true,
    platform: "node",
    format: "esm",
    external: ["vscode"],
    sourcemap: true,
    minify: !watch,
    banner: {
      js: "import { createRequire } from 'module'; const require = createRequire(import.meta.url);",
    },
  },
  // Language server bundle (CJS): required for vscode-languageclient IPC (child_process.fork)
  {
    entryPoints: ["src/lsp-server/main.ts"],
    outfile: "out/lsp-server/main.cjs",
    bundle: true,
    platform: "node",
    format: "cjs",
    external: ["vscode", "prettier"],
    sourcemap: true,
    minify: !watch,
  },
  // VS Code extension entry (CJS)
  {
    entryPoints: ["src/vscode-extension/extension.ts"],
    outfile: "out/vscode-extension/extension.cjs",
    bundle: true,
    platform: "node",
    format: "cjs",
    external: ["vscode"],
    sourcemap: true,
    minify: !watch,
  },
];

const ctxs = await Promise.all(buildConfigs.map((cfg) => esbuild.context(cfg)));

// Copy builtin-nodes.bt to out directory
function copyBuiltinNodes() {
  const srcPath = "src/manifest/builtin-nodes.bt";
  const destDir = "out/manifest";
  const destPath = path.join(destDir, "builtin-nodes.bt");

  if (!fs.existsSync(destDir)) {
    fs.mkdirSync(destDir, { recursive: true });
  }
  fs.copyFileSync(srcPath, destPath);
}

if (watch) {
  await Promise.all(ctxs.map((ctx) => ctx.watch()));
  copyBuiltinNodes();
  console.log("Watching...");
} else {
  await Promise.all(ctxs.map((ctx) => ctx.rebuild()));
  copyBuiltinNodes();
  await Promise.all(ctxs.map((ctx) => ctx.dispose()));
}
