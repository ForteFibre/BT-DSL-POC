# 意味制約

本ドキュメントは BT-DSL の**意味制約**（静的検査ルール）を厳密に定義します。

---

## 1. スコープと名前解決

### 1.1 スコープの種類

| スコープ      | 含まれる宣言                              |
| :------------ | :---------------------------------------- |
| グローバル    | `declare` 文、グローバル変数、`Tree` 定義 |
| Tree ローカル | Tree パラメータ、ローカル変数             |

### 1.2 名前解決の優先順位

識別子の解決は以下の順序で行われます（先に見つかったものが優先）：

1. Tree パラメータ
2. ローカル変数
3. グローバル変数

### 1.3 重複の禁止

同一スコープ内で同名の宣言は禁止されます：

- グローバルスコープ:
  - 同名の `declare` が複数: **エラー**
  - 同名のグローバル変数が複数: **エラー**
  - 同名の `Tree` が複数: **エラー**
  - `declare` と `Tree` が同名: **エラー**
- `declare` 内:
  - 同名のポートが複数: **エラー**
- Tree 内:
  - 同名のパラメータが複数: **エラー**
  - 同名のローカル変数が複数: **エラー**
  - パラメータとローカル変数が同名: **エラー**

---

## 2. ノードカテゴリ

### 2.1 有効なカテゴリ

`declare` 文の `category` は以下のいずれかでなければなりません：

```
Action, Condition, Control, Decorator, SubTree
```

### 2.2 カテゴリ別の制約

| カテゴリ    | 子ブロック `{...}`         |
| :---------- | :------------------------- |
| `Control`   | **必須**                   |
| `Action`    | 禁止                       |
| `Condition` | 禁止                       |
| `Decorator` | 適用先ノードの子として機能 |
| `SubTree`   | 禁止                       |

### 2.3 デコレータの使用位置

`@` 記法で使用できるのは `Decorator` カテゴリのノードのみです。

---

## 3. ポート方向の整合性

### 3.1 方向の定義

| 方向  | 意味                       |
| :---- | :------------------------- |
| `in`  | 読み取り専用（デフォルト） |
| `out` | 書き込み専用               |
| `ref` | 読み書き両用               |

### 3.2 引数とポートの方向整合性

ノード呼び出し時、引数に指定された方向（`arg_dir`）と宣言ポートの方向（`port_dir`）は整合していなければなりません。

#### 整合性マトリクス

| `arg_dir` \ `port_dir` |   `in`    |   `out`   |  `ref`  |
| :--------------------- | :-------: | :-------: | :-----: |
| `in`（または省略）     |     ✓     |  ✗ Error  | ✗ Error |
| `out`                  |  ✗ Error  |     ✓     | ✗ Error |
| `ref`                  | ⚠ Warning | ⚠ Warning |    ✓    |

#### 詳細

- **`arg_dir = in`, `port_dir = out`**: Error
  - ポートは書き込みを要求するが、引数は読み取り専用
- **`arg_dir = in`, `port_dir = ref`**: Error
  - ポートは書き込み権限を要求するが、引数は読み取り専用
- **`arg_dir = out`, `port_dir = in`**: Error
  - ポートは読み取り専用だが、引数は書き込みを宣言
- **`arg_dir = out`, `port_dir = ref`**: Error
  - 厳密な一致が必要
- **`arg_dir = ref`, `port_dir = in`**: Warning
  - 書き込み意図があるが、ポートは読み取り専用のため無視される
- **`arg_dir = ref`, `port_dir = out`**: Warning
  - `out` を使用することを推奨

### 3.3 Tree パラメータの権限

引数が Tree パラメータを参照する場合：

| パラメータ方向     | 引数に `out`/`ref` を付与 |
| :----------------- | :------------------------ |
| `in`（または省略） | **エラー**                |
| `out`              | 許可                      |
| `ref`              | 許可                      |

---

## 4. 引数の制約

### 4.1 位置引数（positional argument）

- 位置引数は**最大1個**まで
- 位置引数を使用する場合、対象ノードのポート数は**ちょうど1**でなければならない

### 4.2 リテラル引数の方向

リテラル値に `out` または `ref` を付与することはできません。

```bt-dsl
NodeName(port: out 123)  // エラー: リテラルに out は不可
```

---

## 5. 代入の制約

### 5.1 代入対象

代入文の左辺は以下のいずれかでなければなりません：

- ローカル変数
- グローバル変数
- `out` または `ref` として宣言された Tree パラメータ

### 5.2 入力専用パラメータへの代入

`in` パラメータ（方向省略を含む）への代入は禁止されます。

---

## 6. 未使用警告

### 6.1 `out`/`ref` パラメータの未使用

`out` または `ref` として宣言された Tree パラメータが、Tree 内で一度も書き込み操作（代入または
`out`/`ref` として渡す）に使用されない場合、警告が発生します。

---

## 7. 診断メッセージ一覧

### 7.1 エラー

| ID   | 条件                             | メッセージ                                                                                               |
| :--- | :------------------------------- | :------------------------------------------------------------------------------------------------------- |
| E001 | 無効なカテゴリ                   | `Invalid category: '{name}'. Valid categories are: Action, Condition, Control, Decorator, SubTree`       |
| E002 | Control ノードに子ブロックがない | `Control node '{name}' requires a children block`                                                        |
| E003 | 非 Control ノードに子ブロック    | `Node '{name}' is not a Control node and cannot have children`                                           |
| E004 | 未知のノード                     | `Unknown node: '{name}'`                                                                                 |
| E005 | 未知のデコレータ                 | `Unknown decorator: '{name}'`                                                                            |
| E006 | 非 Decorator ノードを `@` で使用 | `'{name}' is not a Decorator`                                                                            |
| E007 | 未知の変数                       | `Unknown variable: '{name}'`                                                                             |
| E008 | 未知のポート                     | `Unknown port: '{name}' on node '{node}'`                                                                |
| E009 | ポート方向不整合（in → out/ref） | `Port '{name}' requires 'out' or 'ref' but argument is 'in'. Add 'out' or 'ref' to enable write access.` |
| E010 | ポート方向不整合（out → in/ref） | `Port '{name}' is declared as 'out' but argument uses '{arg_dir}'`                                       |
| E011 | in パラメータを out/ref で渡す   | `Parameter '{name}' is input-only and cannot be passed as '{arg_dir}'`                                   |
| E012 | in パラメータへの代入            | `Parameter '{name}' is input-only and cannot be assigned`                                                |
| E013 | リテラルに out/ref 付与          | `Cannot apply '{dir}' to literal value`                                                                  |
| E014 | 位置引数が2個以上                | `Multiple positional arguments are not allowed`                                                          |
| E015 | 位置引数使用時にポートが複数     | `Positional argument requires exactly one port, but '{node}' has {count}`                                |
| E016 | 型不整合（代入）                 | `Cannot assign {rhs_type} to {lhs_type}`                                                                 |
| E017 | 型不整合（演算）                 | `Operator '{op}' cannot be applied to {lhs_type} and {rhs_type}`                                         |
| E018 | 型不整合（単項演算）             | `Operator '{op}' cannot be applied to {type}`                                                            |
| E019 | ローカル変数に型も初期値もない   | `Local variable '{name}' must have either a type annotation or initial value`                            |
| E020 | 重複定義                         | `Duplicate definition: '{name}'`                                                                         |

### 7.2 警告

| ID   | 条件                                 | メッセージ                                                                         |
| :--- | :----------------------------------- | :--------------------------------------------------------------------------------- |
| W001 | `ref` を `in` ポートに渡す           | `Port '{name}' is 'in' but argument uses 'ref'. Write operations will be ignored.` |
| W002 | `ref` を `out` ポートに渡す          | `Port '{name}' is 'out'. Consider using 'out' instead of 'ref'.`                   |
| W003 | out/ref パラメータが書き込みに未使用 | `Parameter '{name}' is declared as '{dir}' but never used for write access`        |
| W004 | 未使用の変数                         | `Variable '{name}' is declared but never used`                                     |
| W005 | 未使用のパラメータ                   | `Parameter '{name}' is declared but never used`                                    |

---

## 8. Import の解決

### 8.1 パス形式

import 文字列は以下の形式でなければなりません：

- `./` または `../` で始まる相対パス

絶対パスやパッケージ名形式はサポートされません。

### 8.2 解決規則

1. import 文を含むファイルのディレクトリを基準とする
2. `.` および `..` セグメントを正規化する
3. 解決されたパスのファイルを読み込む

### 8.3 推移的 import

import 先のファイルがさらに import を持つ場合、それも再帰的に解決されます。同一ファイルが複数回参照されても、実質的な取り込みは1回です。
