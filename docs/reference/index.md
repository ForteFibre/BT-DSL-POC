# BT-DSL 言語仕様書（Reference）

本ディレクトリのドキュメントは、BT-DSL の**規範的（normative）仕様**を定義します。

- ここでは「読みやすさ」よりも、実装・ツール開発（パーサ、LSP、フォーマッタ、コード生成器）に耐える**厳密性**を優先します。
- 仕様が未確定の箇所は、明示的に **未規定（TBD）** または **実装定義（implementation-defined）**
  として扱います。

---

## 目次

1. [字句構造（Lexical Structure）](./lexical-structure.md)
2. [構文（Syntax）](./syntax.md)
3. [型システム（Type System）](./type-system/)
4. [意味論: 宣言とスコープ（Semantics: Declarations and Scopes）](./declarations-and-scopes.md)
5. [意味論: 実行モデル（Semantics: Execution Model）](./execution-model.md)
6. [静的解析と安全性（Static Analysis and Safety）](./static-analysis-and-safety.md)

---

## 適用範囲（Scope）

本仕様は、次を規定します。

- ソーステキストからトークン列へ変換する規則（字句構造）
- トークン列から構文木を構成する規則（EBNF による文法）
- 型付け、互換性、推論、ならびに静的制約（エラー/警告条件）
- BT-DSL が規定する範囲での実行モデル（Blackboard、ノード呼び出し、事前条件、脱糖規則）

次は、本仕様の範囲外、または **実装定義** です。

- `import` のファイル探索規則（検索パス、パッケージ解決）
- ホスト（BehaviorTree.CPP 側）のノード実装が持つ内部副作用
- スレッドモデル、並列実行、リアルタイム性などの詳細（必要に応じて別途規定）
