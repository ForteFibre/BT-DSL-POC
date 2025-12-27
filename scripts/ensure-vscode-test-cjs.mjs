import * as fs from "node:fs";
import * as path from "node:path";

// VS Code extension tests are executed inside an extension host that (today) is much
// happier loading CommonJS test files. Because this repo is "type": "module",
// plain ".js" in out/ would otherwise be treated as ESM.
//
// Solution: compile tests to CommonJS and drop a nested package.json that marks
// out/test as CommonJS so Node will load those .js files via require() correctly.

const outTestDir = path.resolve("out", "test");
fs.mkdirSync(outTestDir, { recursive: true });

const pkgPath = path.join(outTestDir, "package.json");
const pkg = {
  type: "commonjs",
};

fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + "\n", "utf8");
