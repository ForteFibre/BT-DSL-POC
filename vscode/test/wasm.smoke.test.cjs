const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');

function loadFactory(jsPath) {
  // Emscripten output is CJS-friendly with EXPORT_ES6=0 + MODULARIZE=1.
  // It may export the factory as module.exports or as { default }.
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const mod = require(jsPath);
  return mod?.default ?? mod;
}

function withCString(m, s, fn) {
  const n = m.lengthBytesUTF8(s) + 1;
  const ptr = m._malloc(n);
  try {
    m.stringToUTF8(s, ptr, n);
    return fn(ptr);
  } finally {
    m._free(ptr);
  }
}

function callJson(m, ptr) {
  assert.ok(ptr, 'expected non-null pointer from wasm');
  const s = m.UTF8ToString(ptr);
  m._bt_free(ptr);
  return s;
}

test('bt_dsl_wasm loads and basic APIs return JSON', async () => {
  // In this package ("type": "module"), `.js` is treated as ESM.
  // The Emscripten output is copied as CommonJS (`.cjs`) during prebuild.
  const jsPath = path.join(__dirname, '..', 'wasm', 'bt_dsl_wasm.cjs');
  const wasmPath = path.join(__dirname, '..', 'wasm', 'bt_dsl_wasm.wasm');

  const factory = loadFactory(jsPath);
  assert.equal(typeof factory, 'function', 'expected wasm module factory function');

  const m = await factory({
    locateFile: (p) => (p.endsWith('.wasm') ? wasmPath : p),
  });

  // Sanity check required exports exist
  for (const sym of [
    '_malloc',
    '_free',
    'lengthBytesUTF8',
    'stringToUTF8',
    'UTF8ToString',
    '_bt_workspace_create',
    '_bt_workspace_destroy',
    '_bt_workspace_set_document',
    '_bt_workspace_diagnostics_json',
    '_bt_workspace_completion_json',
    '_bt_workspace_hover_json',
    '_bt_workspace_definition_json',
    '_bt_workspace_document_symbols_json',
    '_bt_free',
  ]) {
    assert.equal(typeof m[sym], 'function', `missing export: ${sym}`);
  }

  const handle = m._bt_workspace_create();
  assert.ok(handle > 0, 'workspace handle should be non-zero');

  const uri = 'file:///smoke.bt';
  const text = [
    'var Ammo: Int\n',
    '/// main\n',
    'Tree Main() {\n',
    '  Sequence {\n',
    '    ForceResult(result: "SUCCESS")\n',
    '  }\n',
    '}\n',
  ].join('');

  // Set document
  withCString(m, uri, (uriPtr) =>
    withCString(m, text, (textPtr) => {
      m._bt_workspace_set_document(handle, uriPtr, textPtr);
    }),
  );

  // Diagnostics
  const diagJson = withCString(m, uri, (uriPtr) =>
    callJson(m, m._bt_workspace_diagnostics_json(handle, uriPtr)),
  );
  const diags = JSON.parse(diagJson);
  assert.ok(Array.isArray(diags.items), 'diagnostics.items should be an array');

  // Completion (pick a byte offset inside "ForceResult")
  const byteOff = Buffer.byteLength(text.slice(0, text.indexOf('ForceResult') + 2), 'utf8');
  const compJson = withCString(m, uri, (uriPtr) =>
    callJson(m, m._bt_workspace_completion_json(handle, uriPtr, byteOff)),
  );
  const comp = JSON.parse(compJson);
  assert.ok(Array.isArray(comp.items), 'completion.items should be an array');

  // Hover
  const hoverJson = withCString(m, uri, (uriPtr) =>
    callJson(m, m._bt_workspace_hover_json(handle, uriPtr, byteOff)),
  );
  const hover = JSON.parse(hoverJson);
  assert.ok('contents' in hover, 'hover should have contents');

  // Definition
  const defJson = withCString(m, uri, (uriPtr) =>
    callJson(m, m._bt_workspace_definition_json(handle, uriPtr, byteOff)),
  );
  const def = JSON.parse(defJson);
  assert.ok(Array.isArray(def.locations), 'definition.locations should be an array');

  // Document symbols
  const symJson = withCString(m, uri, (uriPtr) =>
    callJson(m, m._bt_workspace_document_symbols_json(handle, uriPtr)),
  );
  const syms = JSON.parse(symJson);
  assert.ok(Array.isArray(syms.symbols), 'documentSymbols.symbols should be an array');

  m._bt_workspace_destroy(handle);
});
