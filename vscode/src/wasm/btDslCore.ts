import * as path from 'node:path';
import { pathToFileURL } from 'node:url';
import type * as vscode from 'vscode';

export interface EmscriptenModule {
  _malloc(n: number): number;
  _free(ptr: number): void;
  lengthBytesUTF8(s: string): number;
  stringToUTF8(s: string, ptr: number, maxBytes: number): void;
  UTF8ToString(ptr: number): string;

  _bt_workspace_create(): number;
  _bt_workspace_destroy(handle: number): void;
  _bt_workspace_set_document(handle: number, uriPtr: number, textPtr: number): void;
  _bt_workspace_remove_document(handle: number, uriPtr: number): void;

  _bt_workspace_diagnostics_json(handle: number, uriPtr: number): number;
  _bt_workspace_diagnostics_json_with_imports?: (
    handle: number,
    uriPtr: number,
    importsJsonPtr: number,
  ) => number;
  _bt_workspace_completion_json(handle: number, uriPtr: number, byteOffset: number): number;
  _bt_workspace_completion_json_with_imports?: (
    handle: number,
    uriPtr: number,
    byteOffset: number,
    importsJsonPtr: number,
  ) => number;
  _bt_workspace_hover_json(handle: number, uriPtr: number, byteOffset: number): number;
  _bt_workspace_hover_json_with_imports?: (
    handle: number,
    uriPtr: number,
    byteOffset: number,
    importsJsonPtr: number,
  ) => number;
  _bt_workspace_definition_json(handle: number, uriPtr: number, byteOffset: number): number;
  _bt_workspace_definition_json_with_imports?: (
    handle: number,
    uriPtr: number,
    byteOffset: number,
    importsJsonPtr: number,
  ) => number;
  _bt_workspace_document_symbols_json(handle: number, uriPtr: number): number;

  _bt_workspace_document_highlights_json(
    handle: number,
    uriPtr: number,
    byteOffset: number,
  ): number;
  _bt_workspace_document_highlights_json_with_imports?: (
    handle: number,
    uriPtr: number,
    byteOffset: number,
    importsJsonPtr: number,
  ) => number;

  _bt_workspace_semantic_tokens_json(handle: number, uriPtr: number): number;
  _bt_workspace_semantic_tokens_json_with_imports?: (
    handle: number,
    uriPtr: number,
    importsJsonPtr: number,
  ) => number;

  _bt_workspace_resolve_imports_json?: (
    handle: number,
    uriPtr: number,
    stdlibUriPtr: number,
  ) => number;

  _bt_free(ptr: number): void;
}

type WasmFactory = (opts?: {
  locateFile?: (p: string, prefix?: string) => string;
}) => Promise<EmscriptenModule> | EmscriptenModule;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function toWasmFactory(mod: unknown): WasmFactory {
  if (typeof mod === 'function') {
    return mod as WasmFactory;
  }
  if (isRecord(mod) && typeof mod.default === 'function') {
    return mod.default as WasmFactory;
  }
  throw new Error('Invalid WASM factory module');
}

async function loadFactory(jsPath: string): Promise<WasmFactory> {
  // The extension runs as ESM (package.json has "type": "module").
  // Importing a .cjs module is supported by Node and yields a namespace object
  // whose `default` is `module.exports`.
  const mod: unknown = await import(pathToFileURL(jsPath).href);
  return toWasmFactory(mod);
}

function withCString<T>(m: EmscriptenModule, s: string, fn: (ptr: number) => T): T {
  const n = m.lengthBytesUTF8(s) + 1;
  const ptr = m._malloc(n);
  try {
    m.stringToUTF8(s, ptr, n);
    return fn(ptr);
  } finally {
    m._free(ptr);
  }
}

function callJson(m: EmscriptenModule, ptr: number): string {
  if (!ptr) return '{}';
  const s = m.UTF8ToString(ptr);
  m._bt_free(ptr);
  return s;
}

export class BtDslCore {
  private handle: number;

  private constructor(
    private readonly mod: EmscriptenModule,
    handle: number,
  ) {
    this.handle = handle;
  }

  static async create(context: vscode.ExtensionContext): Promise<BtDslCore> {
    // The Emscripten output we ship is UMD/CJS; keep it as .cjs so it can be
    // imported from an ESM extension (package.json has "type": "module").
    const jsPath = context.asAbsolutePath(path.join('wasm', 'bt_dsl_wasm.cjs'));
    const wasmPath = context.asAbsolutePath(path.join('wasm', 'bt_dsl_wasm.wasm'));

    const factory = await loadFactory(jsPath);
    const mod = await factory({
      locateFile: (p: string) => (p.endsWith('.wasm') ? wasmPath : p),
    });

    const handle = mod._bt_workspace_create();
    return new BtDslCore(mod, handle);
  }

  dispose(): void {
    if (!this.handle) return;
    this.mod._bt_workspace_destroy(this.handle);
    this.handle = 0;
  }

  setDocument(uri: string, text: string): void {
    if (!this.handle) return;
    withCString(this.mod, uri, (uriPtr) => {
      withCString(this.mod, text, (textPtr) => {
        this.mod._bt_workspace_set_document(this.handle, uriPtr, textPtr);
      });
    });
  }

  removeDocument(uri: string): void {
    if (!this.handle) return;
    withCString(this.mod, uri, (uriPtr) => {
      this.mod._bt_workspace_remove_document(this.handle, uriPtr);
    });
  }

  diagnosticsJson(uri: string): string {
    if (!this.handle) return '{"items":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_diagnostics_json(this.handle, uriPtr)),
    );
  }

  diagnosticsJsonWithImports(uri: string, importedUris: string[]): string {
    if (!this.handle) return '{"items":[]}';
    const fn = this.mod._bt_workspace_diagnostics_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.diagnosticsJson(uri);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, importsPtr)),
      ),
    );
  }

  completionJson(uri: string, byteOffset: number): string {
    if (!this.handle) return '{"items":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_completion_json(this.handle, uriPtr, byteOffset)),
    );
  }

  completionJsonWithImports(uri: string, byteOffset: number, importedUris: string[]): string {
    if (!this.handle) return '{"items":[]}';
    const fn = this.mod._bt_workspace_completion_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.completionJson(uri, byteOffset);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, byteOffset, importsPtr)),
      ),
    );
  }

  hoverJson(uri: string, byteOffset: number): string {
    if (!this.handle) return '{}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_hover_json(this.handle, uriPtr, byteOffset)),
    );
  }

  hoverJsonWithImports(uri: string, byteOffset: number, importedUris: string[]): string {
    if (!this.handle) return '{}';
    const fn = this.mod._bt_workspace_hover_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.hoverJson(uri, byteOffset);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, byteOffset, importsPtr)),
      ),
    );
  }

  definitionJson(uri: string, byteOffset: number): string {
    if (!this.handle) return '{"locations":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_definition_json(this.handle, uriPtr, byteOffset)),
    );
  }

  definitionJsonWithImports(uri: string, byteOffset: number, importedUris: string[]): string {
    if (!this.handle) return '{"locations":[]}';
    const fn = this.mod._bt_workspace_definition_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.definitionJson(uri, byteOffset);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, byteOffset, importsPtr)),
      ),
    );
  }

  documentSymbolsJson(uri: string): string {
    if (!this.handle) return '{"symbols":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_document_symbols_json(this.handle, uriPtr)),
    );
  }

  documentHighlightsJson(uri: string, byteOffset: number): string {
    if (!this.handle) return '{"items":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(
        this.mod,
        this.mod._bt_workspace_document_highlights_json(this.handle, uriPtr, byteOffset),
      ),
    );
  }

  documentHighlightsJsonWithImports(
    uri: string,
    byteOffset: number,
    importedUris: string[],
  ): string {
    if (!this.handle) return '{"items":[]}';
    const fn = this.mod._bt_workspace_document_highlights_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.documentHighlightsJson(uri, byteOffset);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, byteOffset, importsPtr)),
      ),
    );
  }

  semanticTokensJson(uri: string): string {
    if (!this.handle) return '{"tokens":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      callJson(this.mod, this.mod._bt_workspace_semantic_tokens_json(this.handle, uriPtr)),
    );
  }

  semanticTokensJsonWithImports(uri: string, importedUris: string[]): string {
    if (!this.handle) return '{"tokens":[]}';
    const fn = this.mod._bt_workspace_semantic_tokens_json_with_imports;
    if (!fn || importedUris.length === 0) {
      return this.semanticTokensJson(uri);
    }
    const importsJson = JSON.stringify(importedUris);
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, importsJson, (importsPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, importsPtr)),
      ),
    );
  }

  resolveImportsJson(uri: string, stdlibUri: string): string {
    if (!this.handle) return '{"uris":[]}';
    const fn = this.mod._bt_workspace_resolve_imports_json;
    if (!fn) return '{"uris":[]}';
    return withCString(this.mod, uri, (uriPtr) =>
      withCString(this.mod, stdlibUri, (stdlibPtr) =>
        callJson(this.mod, fn(this.handle, uriPtr, stdlibPtr)),
      ),
    );
  }
}
