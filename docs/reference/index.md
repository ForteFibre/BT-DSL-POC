# BT-DSL 言語仕様書（Reference）

本ディレクトリのドキュメントは、BT-DSL の**規範的（normative）仕様**を定義します。

- 本仕様は**観測可能な振る舞い**のみを規定します。
- 補足情報、コード例、実装詳細は [docs/internals/](../internals/) を参照してください。

## 目次

1. [字句構造（Lexical Structure）](./lexical-structure.md)
2. [構文（Syntax）](./syntax.md)
3. [型システム（Type System）](./type-system.md)
4. [意味論（Semantics）](./semantics.md)

## 適用範囲（Scope）

本仕様は、次を規定します。

- ソーステキストからトークン列へ変換する規則（字句構造）
- トークン列から構文木を構成する規則（EBNF による文法）
- 型付け、互換性、推論
- BT-DSL が規定する範囲での実行モデル（Blackboard、ノード呼び出し、事前条件）

次は、本仕様の範囲外、または**実装定義**です。

- `import` のファイル探索規則（検索パス、パッケージ解決）
- ホスト（BehaviorTree.CPP 側）のノード実装が持つ内部副作用
- スレッドモデル、並列実行、リアルタイム性などの詳細
- XML 変換規則（[docs/internals/xml-mapping.md](../internals/xml-mapping.md) を参照）
