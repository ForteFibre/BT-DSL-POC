import * as fs from 'node:fs';
import * as path from 'node:path';

// VS Code extension e2e tests are executed by Node from within the `vscode/` package.
// That package is `"type": "module"`, so any `.js` file under it is treated as ESM
// unless a nearer package scope overrides it.
//
// Our e2e tests are compiled to CommonJS (`tsconfig.test.json` has `module: CommonJS`).
// Therefore we must mark the compiled output directory as CommonJS, otherwise Node 20+
// will throw `ReferenceError: exports is not defined in ES module scope`.
//
// Solution: after compiling, drop a nested `package.json` into `out-test/` with
// `{ "type": "commonjs" }`.

const outTestDirs = [
  // Current output directory for this repo.
  path.resolve('out-test'),
  // Back-compat for older layouts (kept intentionally).
  path.resolve('out', 'test'),
];

for (const outTestDir of outTestDirs) {
  fs.mkdirSync(outTestDir, { recursive: true });
  const pkgPath = path.join(outTestDir, 'package.json');
  const pkg = {
    type: 'commonjs',
  };
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n', 'utf8');
}
