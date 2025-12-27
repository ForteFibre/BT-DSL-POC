import { defineConfig } from "@vscode/test-cli";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

export default defineConfig({
  // Compiled mocha tests
  // (tsconfig.vscode-test.json outputs to out/test/..., preserving the test/ path)
  files: "out/test/test/vscode/**/*.test.js",

  // Open a small workspace that contains *.bt fixtures
  workspaceFolder: path.join(__dirname, "test", "vscode", "fixture-workspace"),

  // Snap版は拡張機能テストでハング/不安定になりやすいので、デフォルトでは
  // @vscode/test-electron がダウンロードするVS Codeを使用します。
  // どうしてもローカルのVS Codeを使いたい場合のみ環境変数で明示します。
  ...(process.env.VSCODE_TEST_USE_SNAP === "1"
    ? { useInstallation: { fromPath: "/snap/bin/code" } }
    : {}),

  // Isolate the test instance from your daily VS Code profile
  launchArgs: [
    "--user-data-dir",
    path.join(__dirname, ".vscode-test", "user-data"),
    "--extensions-dir",
    path.join(__dirname, ".vscode-test", "extensions"),
    "--disable-workspace-trust",
  ],

  // Be a bit more forgiving on first download/launch
  mocha: {
    timeout: 60000,
  },
});
