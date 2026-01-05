import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

function run(cmd, args, opts = {}) {
  const res = spawnSync(cmd, args, {
    stdio: 'inherit',
    cwd: opts.cwd ?? join(__dirname, '..'),
    env: { ...process.env, ...(opts.env ?? {}) },
  });
  if (res.status !== 0) {
    throw new Error(`Command failed: ${cmd} ${args.join(' ')}`);
  }
}

const coreDir = join(__dirname, '..');
const buildDir = join(coreDir, 'build-wasm');
mkdirSync(buildDir, { recursive: true });

// Configure with emcmake so CMake uses the Emscripten toolchain.
run('emcmake', [
  'cmake',
  '-S',
  coreDir,
  '-B',
  buildDir,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DBUILD_WASM=ON',
  '-DBT_DSL_MINIMAL_CORE=ON',
  '-DBUILD_TESTS=OFF',
  '-DBUILD_CLI=OFF',
  '-DBUILD_LSP_SERVER=OFF',
]);

// Build the wasm target.
run('cmake', ['--build', buildDir, '--target', 'formatter_wasm', '-j'], { cwd: coreDir });
