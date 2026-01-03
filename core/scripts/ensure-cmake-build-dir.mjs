import fs from 'node:fs/promises';
import path from 'node:path';

async function exists(p) {
  try {
    await fs.access(p);
    return true;
  } catch {
    return false;
  }
}

function usageAndExit() {
  // Keep output minimal; this is a helper invoked by package scripts.
  console.error('Usage: node scripts/ensure-cmake-build-dir.mjs <build-dir>');
  process.exit(2);
}

const buildDirArg = process.argv[2];
if (!buildDirArg) usageAndExit();

const projectDir = await fs.realpath(process.cwd());
const buildDir = path.resolve(projectDir, buildDirArg);
const cachePath = path.join(buildDir, 'CMakeCache.txt');

if (!(await exists(cachePath))) {
  process.exit(0);
}

const cacheText = await fs.readFile(cachePath, 'utf8');

// CMake writes this entry in the cache and it’s a reliable indicator of the
// source directory used to create the build directory.
const m = cacheText.match(/^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$/m);
if (!m || !m[1]) {
  // If the cache is weird/unexpected, do nothing rather than deleting user data.
  process.exit(0);
}

let cachedSource = m[1].trim();
try {
  cachedSource = await fs.realpath(cachedSource);
} catch {
  // If cached source path no longer exists, treat as stale.
}

// If build dir was generated from a different source directory,
// delete it so a fresh configure can proceed.
if (cachedSource !== projectDir) {
  await fs.rm(buildDir, { recursive: true, force: true });
  console.log(
    `[core] Removed stale CMake build dir: ${path.relative(projectDir, buildDir)} (cached source: ${cachedSource})`,
  );
}
