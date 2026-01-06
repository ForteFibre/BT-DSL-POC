# BT-DSL ドキュメント

BT-DSL は [BehaviorTree.CPP](https://www.behaviortree.dev/) v4 向けのドメイン固有言語です。

---

## ガイド

初めて使う方、実践的な使い方を知りたい方向け。

- [クイックスタート](/guide/quick-start) — 5分で最初のツリーを書く
- [構文ガイド](/guide/syntax) — コード例で学ぶ構文
- [実践例](/examples) — 実際のユースケース

---

## リファレンス

厳密な言語仕様。ツール開発や詳細な仕様確認に。

- [言語仕様書](/reference/) — 仕様全体（1〜5章）
- [字句構造](/reference/lexical-structure) — トークン化規則
- [構文](/reference/syntax) — EBNF 形式の構文定義
- [型システム](/reference/type-system) — 型規則と互換性
- [意味論](/reference/semantics) — 名前空間・スコープ・実行モデル
- [診断仕様](/reference/diagnostics) — 初期化安全性 / Null 安全性 / 構造制約

---

## ライブラリ

- [標準ノード](/standard-library) — 組み込みノード一覧

---

## スコープ

- 本仕様は **ソースコードとしての BT-DSL** の規則を規定します
- ノードの実行時挙動は、BT-DSL が規定する範囲（Blackboard/事前条件/脱糖規則など）を仕様として定義します
- `import` の読み込みなど環境依存要素については「言語として要求される前提」を定めます
