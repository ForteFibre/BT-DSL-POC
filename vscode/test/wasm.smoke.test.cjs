const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');

function isExecutable(p) {
  try {
    const st = fs.statSync(p);
    if (!st.isFile()) return false;
    // On Windows, existence is enough.
    if (process.platform === 'win32') return true;
    return (st.mode & 0o111) !== 0;
  } catch {
    return false;
  }
}

test('bt_dsl_lsp_server is packaged (or configured via env)', () => {
  const exe = process.platform === 'win32' ? 'bt_dsl_lsp_server.exe' : 'bt_dsl_lsp_server';
  const fromEnv = process.env.BT_DSL_LSP_PATH;
  if (fromEnv) {
    assert.ok(isExecutable(fromEnv), `BT_DSL_LSP_PATH is set but not executable: ${fromEnv}`);
    return;
  }

  const packaged = path.join(__dirname, '..', 'server', exe);
  assert.ok(
    isExecutable(packaged),
    `Expected packaged LSP server at ${packaged}. Build core target bt_dsl_lsp_server and run vscode prebuild.`,
  );
});
