# BT DSL Formatter

Prettier plugin for BT DSL using tree-sitter parser.

## 概要

このパッケージは、tree-sitter-bt-dslを使用したBT
DSL言語のPrettierプラグインです。Langiumに依存せず、スタンドアロンで動作します。

## インストール

```bash
cd /home/kota/Documents/bt-dsl/formatter
npm install
npm run build
```

## 使用方法

### プログラムから使用

```typescript
import { formatBtDslText } from 'bt-dsl-formatter';

const source = `
Tree Main() {
  Sequence{AlwaysSuccess()}
}
`;

const formatted = await formatBtDslText(source, {
  filepath: 'example.bt',
  tabWidth: 2,
  useTabs: false,
  endOfLine: 'lf',
});

console.log(formatted);
// Tree Main() {
//   Sequence {
//     AlwaysSuccess()
//   }
// }
```

### Prettierプラグインとして使用

```typescript
import prettier from 'prettier';
import btDslPrettierPlugin from 'bt-dsl-formatter';

const formatted = await prettier.format(source, {
  parser: 'bt-dsl',
  plugins: [btDslPrettierPlugin],
});
```

## 機能

- ✅ BT DSL構文の完全なパース対応
- ✅ インデント、スペース、改行の自動調整
- ✅ Direction keywords (`ref`, `in`, `out`) の保持
- ✅ **全てのコメントの保持** (通常のコメント `//`, `/* */` も含む)
- ✅ Documentation comments (`///`, `//!`) の保持
- ✅ 空行の保持（元のコードの空行を維持）
- ✅ tree-sitter-bt-dsl WASMパーサーを使用（プラットフォーム非依存）

## フォーマット例

### 入力

```bt-dsl
// Comment before import
import "StandardNodes.bt"

// Comment before var
var X:int

Tree Main(){
  // Comment in tree
  Sequence{
    // Comment in children
    AlwaysSuccess()
  }
}
```

### 出力

```bt-dsl
// Comment before import
import "StandardNodes.bt"

// Comment before var
var X: int

Tree Main() {
  // Comment in tree
  Sequence {
    // Comment in children
    AlwaysSuccess()
  }
}
```

## テスト

テストはTypeScriptで記述されています。

```bash
# ビルドしてすべてのテストを実行
npm run build
npm test
```

## ライセンス

MIT
