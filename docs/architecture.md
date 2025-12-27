# Architecture

BT-DSL コンパイラのアーキテクチャ概要。

## 技術スタック

- **[Langium](https://langium.org/)**: TypeScript製DSLフレームワーク
- **Node.js**: ランタイム
- **Vitest**: テストフレームワーク
- **ESLint**: 静的解析

## コンポーネント

```
┌────────────────────────────────────────────────────────────┐
│                        CLI (index.ts)                       │
└────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┴─────────────────────┐
        ▼                                           ▼
┌───────────────────┐                    ┌───────────────────┐
│   Langium Parser  │                    │   XML Generator   │
│  (bt-dsl.langium) │                    │ (xml-generator.ts)│
└───────────────────┘                    └───────────────────┘
        │                                           ▲
        ▼                                           │
┌───────────────────┐                               │
│       AST         │───────────────────────────────┘
│   (generated/)    │
└───────────────────┘
        │
        ▼
┌───────────────────────────────────────────────────────────┐
│                    Semantic Services                       │
│  ┌─────────────────────┐  ┌─────────────────────────────┐ │
│  │   Scope Provider    │  │       Validator             │ │
│  │ (bt-dsl-scope-      │  │   (bt-dsl-validator.ts)     │ │
│  │   provider.ts)      │  │                             │ │
│  └─────────────────────┘  └─────────────────────────────┘ │
└───────────────────────────────────────────────────────────┘
```

## ファイル構成

```
src/
├── cli/
│   ├── index.ts         # CLIエントリポイント
│   └── cli-util.ts      # ドキュメント抽出ユーティリティ
├── generator/
│   └── xml-generator.ts # AST→XML変換
└── language/
    ├── bt-dsl.langium   # 文法定義
    ├── bt-dsl-module.ts # サービス構成
    ├── bt-dsl-scope-provider.ts  # シンボル解決
    ├── bt-dsl-validator.ts       # セマンティック検証
    └── generated/       # Langium自動生成
        ├── ast.ts       # AST型定義
        ├── grammar.ts   # パーサ
        └── module.ts    # 生成モジュール
```

## 処理フロー

1. **パース**: `.bt`ファイル → AST
2. **検証**: シンボル解決 + セマンティックチェック
3. **生成**: AST → BehaviorTree.CPP XML

## 拡張ポイント

### 新しい検証ルールの追加

`bt-dsl-validator.ts`に追加:

```typescript
checkMyRule(node: MyNode, accept: ValidationAcceptor): void {
    if (/* 条件 */) {
        accept('error', 'エラーメッセージ', { node, property: 'propName' });
    }
}
```

### XML出力のカスタマイズ

`xml-generator.ts`の該当メソッドを修正。
