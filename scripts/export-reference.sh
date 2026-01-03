#!/bin/bash
# export-reference.sh
# referenceディレクトリ内のmarkdownファイルを順番に結合して1つのファイルにエクスポート

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
REFERENCE_DIR="$PROJECT_ROOT/docs/reference"
OUTPUT_FILE="${1:-$PROJECT_ROOT/docs/reference-combined.md}"

# ファイルの順序（index.mdの目次に基づく）
# サブディレクトリがある場合はディレクトリパスを含める
FILES=(
    "index.md"
    "lexical-structure.md"
    "syntax.md"
    # type-system ディレクトリ（3章）
    "type-system/index.md"
    "type-system/type-definitions.md"
    "type-system/inference-and-resolution.md"
    "type-system/compatibility-and-conversion.md"
    "type-system/expression-typing.md"
    # 4章以降
    "declarations-and-scopes.md"
    "execution-model.md"
    "static-analysis-and-safety.md"
)

# 出力ファイルを初期化
> "$OUTPUT_FILE"

echo "Exporting reference documents to: $OUTPUT_FILE"

for file in "${FILES[@]}"; do
    filepath="$REFERENCE_DIR/$file"
    
    if [[ -f "$filepath" ]]; then
        echo "  Adding: $file"
        
        # ファイル間にセパレータを追加（最初のファイル以外）
        if [[ -s "$OUTPUT_FILE" ]]; then
            echo -e "\n\n---\n" >> "$OUTPUT_FILE"
        fi
        
        # ファイル内容を追加
        cat "$filepath" >> "$OUTPUT_FILE"
    else
        echo "  Warning: $file not found, skipping..."
    fi
done

echo ""
echo "Done! Combined markdown exported to: $OUTPUT_FILE"
echo "Total size: $(wc -c < "$OUTPUT_FILE") bytes"
