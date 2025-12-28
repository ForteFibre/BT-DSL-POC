import * as path from 'node:path';
import { globSync } from 'glob';
import Mocha from 'mocha';

export function run(): Promise<void> {
  const mocha = new Mocha({
    ui: 'tdd',
    color: true,
    timeout: 60000,
  });

  const testsRoot = path.resolve(__dirname);

  return new Promise((resolve, reject) => {
    const files = globSync('**/*.test.js', { cwd: testsRoot });
    for (const f of files) {
      mocha.addFile(path.resolve(testsRoot, f));
    }

    try {
      mocha.run((failures) => {
        if (failures > 0) reject(new Error(`${failures} tests failed.`));
        else resolve();
      });
    } catch (e) {
      reject(e);
    }
  });
}
