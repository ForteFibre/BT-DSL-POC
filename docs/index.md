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

- [文法](/reference/grammar) — EBNF 形式の構文定義
- [型システム](/reference/type-system) — 型規則と互換性
- [意味制約](/reference/semantics) — 静的検査ルールと診断メッセージ
- [初期化安全性](/reference/initialization-safety) — Blackboard の初期化と書き込み保証

---

## ライブラリ

- [標準ノード](/standard-library) — 組み込みノード一覧

---

## スコープ

- 本仕様は **ソースコードとしての BT-DSL** の規則を規定します
- ノードの実行時挙動やランタイムの動作は仕様の範囲外です
- `import` の読み込みなど環境依存要素については「言語として要求される前提」を定めます
