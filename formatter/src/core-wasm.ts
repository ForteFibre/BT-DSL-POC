/* eslint-disable @typescript-eslint/no-unsafe-member-access -- Required for dynamic Emscripten module loading */
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';

// Emscripten MODULARIZE+EXPORT_ES6 output: default export is a factory.
type WasmFactory = (opts?: Record<string, unknown>) => Promise<unknown>;

interface BtDslCoreWasm {
  parseToAstJson: (source: string) => string;
}

let wasmModule: BtDslCoreWasm | null = null;
let configuredWasmPath: string | null = null;

/**
 * Configure the directory that contains `formatter_wasm.wasm`.
 * Useful for environments where the plugin is bundled/relocated (e.g. VSCode).
 */
export function setCoreWasmPath(wasmDir: string): void {
  // Accept either a directory or a direct path to the .wasm/.js file.
  // When configured, we will load BOTH:
  // - formatter_wasm.js (emscripten factory)
  // - formatter_wasm.wasm
  // from this directory.
  if (wasmDir.endsWith('.wasm') || wasmDir.endsWith('.js')) {
    configuredWasmPath = dirname(wasmDir);
  } else {
    configuredWasmPath = wasmDir;
  }
  // Reset cached module so that initCoreWasm() reloads from the new path.
  wasmModule = null;
}

function defaultWasmDirFromImportMeta(): string {
  // We are in dist/src/, wasm lives in dist/.
  const __filename = fileURLToPath(import.meta.url);
  const __dirname = dirname(__filename);
  return join(__dirname, '..');
}

export async function initCoreWasm(): Promise<BtDslCoreWasm> {
  if (wasmModule) return wasmModule;

  const wasmDir = configuredWasmPath ?? defaultWasmDirFromImportMeta();

  // Load the generated Emscripten factory.
  // - Default: resolve relative to this file (works for normal installs: dist/src -> dist/).
  // - Configured: load from the configured directory (needed for VSCode packaging / relocation).
  const factory: WasmFactory = configuredWasmPath
    ? ((await import(pathToFileURL(join(wasmDir, 'formatter_wasm.js')).href))
        .default as WasmFactory)
    : // @ts-expect-error - This module is generated at build time by @bt-dsl/core (Emscripten output).
      ((await import('../formatter_wasm.js')).default as WasmFactory);

  wasmModule = (await factory({
    locateFile: (p: string) => {
      // Emscripten will request the .wasm by filename.
      return join(wasmDir, p);
    },
  })) as BtDslCoreWasm;

  return wasmModule;
}
