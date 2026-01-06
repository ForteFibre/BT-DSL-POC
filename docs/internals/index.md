# BT-DSL 補足資料（Internals）

本ディレクトリには、言語仕様の補足情報を配置しています。

言語仕様（規範的ドキュメント）は [docs/reference/](../reference/) を参照してください。

---

## ドキュメント構成

各補足資料は以下のカテゴリで構成されています：

| カテゴリ | 内容 |
| :--- | :--- |
| **設計根拠** | なぜそのように設計したかの説明 |
| **実装定義事項** | 処理系に委ねられる事項 |
| **未規定事項（TBD）** | 将来の仕様で決定される事項 |
| **コード例** | 仕様の具体的な使用例 |
| **処理系実装者向け** | コンパイラ/ツール実装者向けの情報 |

---

## コンパイラ・変換仕様

- [XMLマッピング仕様](./xml-mapping.md) — BT-DSL から BehaviorTree.CPP XML への変換規則
- [コンパイラ仕様](./compiler.md) — コンパイラアーキテクチャ

---

## 章別の補足資料

| 章 | 補足資料 |
| :- | :------- |
| 1. 字句構造 | [lexical-structure-notes.md](./lexical-structure-notes.md) |
| 2. 構文 | [syntax-notes.md](./syntax-notes.md) |
| 3. 型システム | [type-system-notes.md](./type-system-notes.md) |
| 4. 意味論 | [semantics-notes.md](./semantics-notes.md) |
| 5. 診断仕様 | [diagnostics-notes.md](./diagnostics-notes.md) |
