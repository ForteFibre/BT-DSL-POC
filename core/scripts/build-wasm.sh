#!/bin/bash
set -e

# Build WASM using Emscripten
# Requires: emsdk activated

cd "$(dirname "$0")/.."

if ! command -v emcmake &> /dev/null; then
    echo "Error: Emscripten not found. Please install and activate emsdk."
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    exit 1
fi

echo "Building WASM..."

mkdir -p build-wasm
cd build-wasm

emcmake cmake .. -DBUILD_WASM=ON -DBUILD_TESTS=OFF -DBUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc) bt_dsl_wasm

echo "✓ WASM build complete: build-wasm/"

