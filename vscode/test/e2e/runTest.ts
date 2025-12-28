import * as path from 'node:path';
import { runTests } from '@vscode/test-electron';

async function main(): Promise<void> {
  try {
    // Enable lightweight debug logs inside the extension during e2e.
    process.env.BT_DSL_E2E = '1';

    // NOTE: this file is compiled to out-test/e2e/runTest.js
    // __dirname => <pkg>/out-test/e2e
    const extensionDevelopmentPath = path.resolve(__dirname, '../..');
    // When the extension runs as ESM, the extension host loads the test entry via import(),
    // which requires an explicit file extension.
    const extensionTestsPath = path.resolve(__dirname, './suite/index.js');
    const workspacePath = path.resolve(extensionDevelopmentPath, 'test', 'fixture-workspace');

    await runTests({
      extensionDevelopmentPath,
      extensionTestsPath,
      launchArgs: [workspacePath, '--disable-extensions'],
    });
  } catch (err) {
    console.error('Failed to run VS Code extension tests');
    console.error(err);
    process.exit(1);
  }
}

void main();
