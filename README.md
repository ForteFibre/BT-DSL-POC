# BT-DSL

BehaviorTree.CPP v4向けの独自DSL（Domain-Specific
Language）。XMLの可読性と保守性を改善し、強力な静的解析とエディタサポートを提供します。

## 特徴

- 🔍 **コンパイル時検証**: 変数のタイポ、型不一致、ref権限エラー、ポートの必須チェック等を検出
- 📝 **可読性向上**: 冗長なXMLタグを排除した、クリーンなC++風の構文
- 🔗 **シンボル解決**: Blackboard変数とTree引数の自動解決
- ⚡ **高速なパース**: C++実装のコアロジックで高速処理

## アーキテクチャ

本プロジェクトは以下のコンポーネントで構成されています：

- **core**: C++17で記述されたコンパイラコア（パーサー、意味解析、XML生成）
- **vscode**: VS Code拡張機能（LSPクライアント/サーバー）
- **formatter**: Prettierプラグインによるコードフォーマッター

## 必須要件

- Node.js (pnpm推奨)
- CMake 3.20以上
- C++17 コンパイラ
- Emscripten

## インストールとビルド

```bash
# 依存関係のインストール
pnpm install

# 文法と全パッケージのビルド
pnpm run build
```

## 使用方法

### CLI (Command Line Interface)

`core` ディレクトリでビルドされた `bt_dsl_cli` を使用して、検証や変換を行います。

```bash
# ビルドされたCLIのパス (例: Linux/macOS)
export CLI=./core/build/bt_dsl_cli

# 構文・意味解析のチェック
$CLI check main.bt

# XMLへのコンパイル
$CLI convert main.bt -o main.xml

# 既存のXMLマニフェストをBT-DSLへ変換
$CLI xml-to-bt explicit_manifest.xml -o converted.bt
```

### VS Code 拡張機能

`vscode` ディレクトリで開発用ビルドを実行できます。

1. VS Codeで本リポジトリを開く
2. デバッグビュー (F5) から "Run Extension" を実行

## 開発

### テスト

```bash
pnpm test
```

### プロジェクト構成

- `core/`: C++実装のコアライブラリとCLI
- `vscode/`: VS Code拡張機能
- `formatter/`: Prettierプラグイン
- `shared/examples/`: サンプルコード

## ライセンス

MIT
