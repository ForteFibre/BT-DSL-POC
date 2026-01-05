#!/bin/bash
set -e

# Copy LSP server binary if available
if [ -f ../core/build-lsp/bt_dsl_lsp_server ]; then
  mkdir -p server
  cp -f ../core/build-lsp/bt_dsl_lsp_server server/
elif [ -f ../core/build/bt_dsl_lsp_server ]; then
  mkdir -p server
  cp -f ../core/build/bt_dsl_lsp_server server/
elif [ -f server/bt_dsl_lsp_server ]; then
  : # Already exists in target location
else
  echo 'Warning: LSP server not found (checked ../core/build-lsp/bt_dsl_lsp_server, ../core/build/bt_dsl_lsp_server, server/bt_dsl_lsp_server). Set BT_DSL_LSP_PATH for dev or build bt_dsl_lsp_server.'
fi

# Copy standard library
if [ -d ../core/std ]; then
  cp -rf ../core/std .
else
  echo 'Warning: std/ not found'
fi

# Build and copy WASM formatter
mkdir -p out
cp -f ../core/build-wasm/formatter_wasm.js out/
cp -f ../core/build-wasm/formatter_wasm.wasm out/
