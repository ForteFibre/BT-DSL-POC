# 意味制約

本ドキュメントは BT-DSL の**意味制約**（静的検査ルール）を厳密に定義します。

---

## 1. スコープと名前解決

### 1.1 スコープの種類

| スコープ      | 含まれる宣言                                                                                                |
| :------------ | :---------------------------------------------------------------------------------------------------------- |
| グローバル    | `extern` 文 (Node), `extern type` 文, `type` 文 (Alias), グローバル Blackboard, グローバル定数, `tree` 定義 |
| Tree ローカル | tree パラメータ、ローカル Blackboard、ローカル定数                                                          |
| ブロック      | `children_block` 内で宣言された Blackboard、定数                                                            |

### 1.2 名前解決の優先順位

識別子の解決は以下の順序で行われます：

1. ブロックスコープ（`children_block` 内の Blackboard・定数）
2. Tree ローカル（パラメータ、ローカル Blackboard、ローカル定数）
3. グローバル Blackboard・グローバル定数

### 1.3 重複の禁止

同一スコープ内で同名の識別子を複数宣言することはできません。

| スコープ                       | 禁止される重複                                                                                                                                                 |
| :----------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| グローバル（ノード）           | `extern` (Node) 同士 / `tree` 同士 / `extern` (Node) と `tree`                                                                                                 |
| グローバル（Blackboard・定数） | グローバル Blackboard 同士 / グローバル定数同士 / グローバル Blackboard とグローバル定数                                                                       |
| グローバル（型）               | `extern type` 同士 / `type` (Alias) 同士 / `extern type` と `type` / 基本型・エイリアスとの衍突                                                                |
| `extern` 内                    | ポート同士                                                                                                                                                     |
| `tree` 内                      | パラメータ同士 / ローカル Blackboard 同士 / ローカル定数同士 / パラメータとローカル Blackboard / パラメータとローカル定数 / ローカル Blackboard とローカル定数 |
| ブロック内                     | 同一ブロック内での同名の Blackboard / 定数 / 親スコープの同名識別子の隠蔽（シャドウイング）                                                                    |

> [!NOTE]
>
> - ノード名（`extern`/`tree`）、Blackboard・定数名、型名はそれぞれ独立した名前空間を持つため、同名でも衍突しません（例:
>   `extern type A;` と `tree A` は共存可能）。
> - グローバル Blackboard・定数と tree 内の Blackboard・定数（パラメータ/ローカル Blackboard/ローカル定数）はスコープが異なるため、同名でも衍突しません（内側が優先されます）。

### 1.4 可視性（Visibility）

ファイル間の参照（インポート）におけるシンボルの可視性は、識別子の命名規則によって制御されます。

| 識別子の先頭文字     | 可視性      | 説明                                                       |
| :------------------- | :---------- | :--------------------------------------------------------- |
| `_` (アンダースコア) | **Private** | 定義されたファイル内でのみ参照可能。外部からは見えません。 |
| その他               | **Public**  | `import` した他のファイルから参照可能。                    |

> [!IMPORTANT] 型エイリアスにおける循環定義は禁止されます（例: `type A = B; type B = A;`）。

例：

- `tree _MyInternalTree` → Private（外部から隠蔽される）
- `tree MyPublicTree` → Public（外部から使用可能）

### 1.5 再帰呼び出しの禁止

`tree`
が自分自身を呼び出すこと（再帰呼び出し）は**禁止**されます。これには**間接的な再帰**も含まれます。

```bt-dsl
// 直接再帰: エラー
tree A() {
    A()  // エラー: tree A は自分自身を呼び出せません
}

// 間接再帰: エラー
tree B() {
    C()
}

tree C() {
    B()  // エラー: tree C -> tree B -> tree C の循環呼び出し
}
```

> [!NOTE] 再帰が禁止される理由：
>
> - `tree` の `out`
>   ポートの書き込み保証推論（Infer）において、再帰があると固定点が求まらない可能性がある
> - 型推論においても、再帰によって無限ループが発生する可能性がある
>
> コンパイラは呼び出しグラフを解析し、循環が検出された場合はコンパイルエラーを報告します。

### 1.6 Blackboard のライフタイムとスコープ

Blackboard（`var`
で宣言されるエントリ）は、一般的なプログラミング言語における「変数」とは異なる意味論を持ちます。

#### 1.6.1 永続ライフタイム

Blackboard エントリの値は **Tree 全体で保持** されます。tick 間でも値は維持され、破棄されません。

```bt-dsl
tree Main() {
    Sequence {
        var counter: int32 = 0;  // ブロック内で宣言
        Increment(out counter);
        Log(counter);            // 次の tick でも counter の値は保持される
    }
}
```

#### 1.6.2 レキシカルスコープ（可視性）

Blackboard エントリは、宣言されたブロック内の **宣言以降** でのみ参照可能です。

```bt-dsl
tree Main() {
    Sequence {
        var x: int32;
        TaskA(out x);            // OK: x は宣言済み
        Sequence {
            var y: int32;
            TaskB(x, out y);     // OK: x は親スコープ、y は現スコープ
        }
        TaskC(x);                // OK: x はスコープ内
        TaskD(y);                // Error: y はスコープ外
    }
}
```

#### 1.6.3 FlowPolicy: Isolated における制約

`FlowPolicy: Isolated` を持つノード（例: `ParallelAll`）の `children_block`
内では、Blackboard 宣言（`var`）は **禁止** されます。

**理由**:
Isolated では前のノードの書き込み結果が次のノードの入力として有効にならないため、そこで宣言しても後続ノードで利用できません。

```bt-dsl
// ParallelAll は behavior(All, Isolated)
ParallelAll {
    var x: int32;  // Error: Isolated ブロック内での var 宣言は禁止
    TaskA(out x);
    TaskB(x);
}
```

> [!NOTE] Tree ルートまたは `FlowPolicy: Chained` のブロックでは、Blackboard 宣言は許可されます。

#### 1.6.4 インライン Blackboard 宣言

`out` ポート引数として、Blackboard を同時に宣言できます（`out var identifier` 構文）。

```bt-dsl
Sequence {
    // インライン宣言（型はポートから推論）
    Compute(out var result);

    // result は以降の兄弟ノードで参照可能
    Log(result);
}
```

**スコープ規則:**

- インラインで宣言された Blackboard は、その宣言を含む `children_block` のスコープに属する
- 宣言以降の兄弟ノードおよび子孫ノードから参照可能
- `FlowPolicy: Isolated` のブロック内ではインライン宣言も禁止

#### 1.6.5 Blackboard 宣言・代入の実行タイミング

Tree 内に記述される Blackboard 宣言（`var x = ...`）および代入文（`x = ...`）は、**暗黙のアクションノード**として扱われます。

**意味論:**

```bt-dsl
Sequence {
    var counter = 0;     // ← これは暗黙のアクションとして実行される
    TaskA();
    counter = counter + 1;  // ← これも暗黙のアクションとして実行される
}
```

上記のコードは、概念的には以下と同等です：

```bt-dsl
Sequence {
    SetValue(target: out var counter, value: 0);  // 常に Success を返す暗黙のアクション
    TaskA();
    SetValue(target: out counter, value: counter + 1);
}
```

**特性:**

| 特性         | 説明                                                  |
| :----------- | :---------------------------------------------------- |
| 返り値       | 常に `Success`                                        |
| 実行順序     | 他のノードと同様、`children_block` 内での記述順に実行 |
| データフロー | 実行された時点で Blackboard に値が書き込まれる        |

> [!IMPORTANT]
> Blackboard 宣言や代入は、ノードと同じく Tree の**実行時**に評価されます。コンパイル時に評価されるのは
> `const` 宣言のみです。

---

## 2. ノードカテゴリ

### 2.1 有効なカテゴリ

`extern` 文の `category` は以下のいずれかでなければなりません：

```
action, condition, control, decorator, subtree
```

### 2.2 カテゴリ別の制約

各カテゴリのノードは、以下の構文規則に従う必要があります。

#### **action / subtree / condition**

- **構文**: `NodeName(...)`
- **丸括弧 `(...)`**: **必須**（引数がない場合でも `()` が必要）
- **子ブロック `{...}`**: **禁止**
- **返り値**: `Success` / `Failure` / `Running`

#### **control**

- **構文**: `NodeName(...) { ... }`
- **丸括弧 `(...)`**: **省略可能**（引数がない場合）
- **子ブロック `{...}`**: **必須**

#### **decorator**

- **構文**: `NodeName(...) { ... }`
- **丸括弧 `(...)`**: **省略可能**（引数がない場合）
- **子ブロック `{...}`**: **必須**
- **子の数**: 厳密には1つのみ。複数記述した場合は暗黙的に `Sequence`
  でラップされます（[文法リファレンス 3.8](./grammar.md#_3-8-decorator-の暗黙的-sequence-挿入)
  を参照）

---

## 3. ポート方向の整合性

### 3.1 方向の定義

| 方向  | 意味（Semantics） | 役割                                                           |
| :---- | :---------------- | :------------------------------------------------------------- |
| `in`  | Input (Snapshot)  | 開始時の入力。論理的には「その瞬間の値」を渡す（デフォルト）   |
| `ref` | View (Live Read)  | 継続的な監視。実行中も値の変化を見たいが、自分では書き換えない |
| `mut` | State (Live R/W)  | 状態の共有・更新。読み書き両方を行う                           |
| `out` | Output            | 結果の出力専用（成功時のみ書き込み）                           |

> [!NOTE] `out`
> ポートは成功時のみ書き込みが保証されます。詳細は[初期化安全性](./initialization-safety.md)を参照してください。

### 3.2 引数とポートの方向整合性

ノード呼び出し時、引数に指定された方向（`arg_dir`）と宣言ポートの方向（`port_dir`）は整合していなければなりません。

#### 整合性マトリクス

| `arg_dir` \ `port_dir` |   `in`    |   `ref`   |  `mut`  |  `out`  |
| :--------------------- | :-------: | :-------: | :-----: | :-----: |
| `in`（または省略）     |     ✓     |  ✗ Error  | ✗ Error | ✗ Error |
| `ref`                  | ⚠ Warning |     ✓     | ✗ Error | ✗ Error |
| `mut`                  | ⚠ Warning | ⚠ Warning |    ✓    | ✗ Error |
| `out`                  |  ✗ Error  |  ✗ Error  | ✗ Error |    ✓    |

#### 詳細

- **`arg_dir = in`, `port_dir = ref/mut/out`**: Error
  - ポートはライブ参照または書き込みを要求するが、引数はスナップショット（コピー）を渡すのみ
- **`arg_dir = ref`, `port_dir = in`**: Warning
  - ライブ参照を提供するが、ポートは開始時のスナップショットのみ必要
- **`arg_dir = ref`, `port_dir = mut/out`**: Error
  - ポートは書き込み権限を要求するが、`ref` は読み取り専用
- **`arg_dir = mut`, `port_dir = in`**: Warning
  - 書き込み権限を付与するが、ポートは読み取りのみ
- **`arg_dir = mut`, `port_dir = ref`**: Warning
  - 書き込み権限を付与するが、ポートは読み取りのみ
- **`arg_dir = mut`, `port_dir = out`**: Error
  - 厳密な一致が必要（`out` は出力専用、`mut` は双方向）
- **`arg_dir = out`, `port_dir = in/ref/mut`**: Error
  - ポートは出力専用ではない

### 3.3 引数の種類と制約（LValue / RValue）

- **`in` ポートへの引数**: 任意の式（RValue）を使用できます。
- **`ref` ポートへの引数**: **左辺値（LValue）**でなければなりません。
  - 左辺値とは、変数、パラメータ、または配列要素など、アドレスを持つ対象です。
  - リテラルや計算式（例: `a + b`）は右辺値であり、参照として指定できません。
  - **定数**（`const`）も `ref` 引数として使用可能です（読み取り専用のため）。
- **`mut` / `out` ポートへの引数**: **左辺値（LValue）**でなければなりません。
  - 左辺値とは、変数、パラメータ、または配列要素など、値を代入可能な対象です。
  - リテラルや計算式（例: `a + b`）は右辺値であり、書き込み先として指定できません。
  - **定数**（`const`）は書き込み不可のため、`mut` / `out` 引数には使用できません。

### 3.4 tree パラメータの権限

引数が tree パラメータを参照する場合：

| パラメータ方向     | 引数に `ref` を付与 | 引数に `mut`/`out` を付与 |
| :----------------- | :------------------ | :------------------------ |
| `in`（または省略） | 許可                | **エラー**                |
| `ref`              | 許可                | **エラー**                |
| `mut`              | 許可                | 許可                      |
| `out`              | **エラー**          | 許可                      |

---

## 4. 型変換と引数渡し

### 4.1 暗黙の拡大 (Implicit Widening for Output)

`out` ポートの型より、受け取る変数の型が大きい場合、暗黙的に接続可能です。

```bt-dsl
var large: int32;
// NodeOut8 (out: int8) -> large (int32) : 安全
NodeOut8(port: large);
```

---

## 5. 引数の制約

### 5.1 位置引数（positional argument）

- 位置引数は**最大1個**まで
- 位置引数を使用する場合、対象ノードのポート数は**ちょうど1**でなければならない

### 5.2 非左辺値（RValue）への方向指定

リテラルや計算結果などの右辺値（RValue）に対して、`ref`、`mut`、または `out`
を付与することはできません。

```bt-dsl
NodeName(port: out 123)      // エラー: リテラルはRValue
NodeName(port: mut (a + b))  // エラー: 計算結果はRValue
NodeName(port: ref (a + b))  // エラー: 計算結果はRValue
```

### 5.3 引数の省略規則

ノード呼び出し時、各ポートの省略可否は以下のルールに従います：

| ポート方向         | デフォルト値あり | デフォルト値なし |
| :----------------- | :--------------- | :--------------- |
| `in`（または省略） | 省略可能         | 省略不可         |
| `ref`              | （設定不可）     | **省略不可**     |
| `mut`              | （設定不可）     | **省略不可**     |
| `out`              | （設定不可）     | **常に省略可能** |

**動作の詳細:**

- **`in` ポート省略時**: デフォルト値が使用されます。
- **`ref` ポート**: 常に左辺値を指定する必要があります（読み取り用の参照）。
- **`mut` ポート**: 常に左辺値を指定する必要があります（読み書き用の参照）。
- **`out` ポート省略時**: 出力は破棄されます（一時変数に書き込まれ、無視される）。

```bt-dsl
extern action Example(
    in required: int32,
    in optional_in: int32 = 0,
    out result: int32,
    mut state: int32
);

tree Test() {
    var x: int32;
    var y: int32 = 0;

    // すべて指定
    Example(required: 1, optional_in: 2, result: out x, state: mut y);

    // in のデフォルト値と out の省略を使用
    Example(required: 1, state: mut y);  // optional_in=0, result は破棄

    // mut は省略不可
    // Example(required: 1);  // エラー: state が必須
}
```

### 5.4 デフォルト値の制約

- **`ref` / `mut` / `out` ポートにはデフォルト値を指定できません。**
- **デフォルト値は定数式（`const_expr`）でなければなりません。**
  実行時の値（変数やパラメータ）は使用できません。
- **デフォルト値の型はポートの型と互換性が必要です。** 暗黙の型変換ルールが適用されます。

```bt-dsl
const DEFAULT_SPEED = 1.0;

extern action MoveTo(
    target: Vector3,
    speed: float = DEFAULT_SPEED,  // OK: in ポートにデフォルト値
    out result: bool,              // デフォルト値なし（禁止）
    mut state: int32               // デフォルト値なし（禁止）
);
```

---

## 6. 代入の制約

### 6.1 代入対象

代入文の左辺は以下のいずれかでなければなりません：

- ローカル Blackboard
- グローバル Blackboard
- `out` または `ref` として宣言された tree パラメータ
- 配列要素（例: `arr[0]`）

### 6.2 入力専用パラメータへの代入

`in` パラメータ（方向省略を含む）への代入は禁止されます。

### 6.3 定数への代入

定数（`const`）への代入は禁止されます。定数は宣言時に初期化され、その後は変更できません。

```bt-dsl
const MAX = 100;
MAX = 200;  // エラー: 定数への代入は禁止
```

---

## 7. 未使用警告

### 7.1 `mut`/`out` パラメータの未使用

`mut` または `out` として宣言された tree パラメータが、tree 内で一度も書き込み操作（代入または
`mut`/`out` として渡す）に使用されない場合、警告が発生します。

---

## 8. Nullable型の安全な使用

Nullable 型 (`T?`) を安全に使用するための機構を定義します。

### 8.1 制御フローによる型推論（Flow-sensitive Typing）

コンパイラは、事前条件（特に `@guard` や
`@run_while`）に記述された条件式を解析し、その条件が「真」であると保証されるスコープ内（対象ノードの内部）において、変数の型をより具体的な型へ絞り込み（Narrowing）ます。

```bt-dsl
var target: Pose? = null;

Fallback {
    // @guard により条件式が真であることが保証される
    @guard(target != null)
    Sequence {
        // 【型の絞り込み有効】
        // このブロック内では target は Pose 型として扱える
        MoveTo(target);
    }

    // ここでは target は Pose?（null の可能性あり）のまま
    Log("Target is null");
}
```

### 8.2 条件式による型の絞り込み (Narrowing via Conditional Logic)

コンパイラは、`@guard(expr)` や `@run_while(expr)` に指定された条件式 `expr`
が 「真 (true) と評価される」 という前提に基づき、その論理構造を再帰的に解析して型の絞り込みを行います。

具体的には、式 `expr` が真となるために 「論理的に必須となる項（Necessary
Conditions）」 を特定します。解析は以下の抽象ルールに基づいて行われます：

#### null 比較 (Null Comparison)

式が `variable != null` の形式である場合、その式が真であることは「変数が `null`
でない」ことと等価であるため、変数は非Nullable型へ絞り込まれます。

```bt-dsl
@guard(target != null)  // target: T? → target: T（ブロック内）
UseTarget(target);
```

逆に、式が `variable == null` の形式である場合、その式が真であることは「変数が `null`
である」ことを意味するため、型の絞り込みは発生しません（ただしコンパイラはこの情報を使用してエラー検出に利用できます）。

#### 連言の分解 (Conjunctive Decomposition)

式が論理積 `Left && Right` の構造を持つ場合、式全体が真であるためには `Left` と `Right`
の双方が真でなければなりません。したがって、両辺それぞれに対して再帰的に絞り込み解析を適用し、その結果を統合します。

```bt-dsl
@guard(a != null && b != null)  // a: T?, b: U? → a: T, b: U（ブロック内）
UseBoth(a, b);
```

#### 否定の反転 (Negation Inversion)

式が論理否定 `!Operand` の構造を持つ場合、`Operand` が「偽 (false) である」ための条件を解析します。

```bt-dsl
// !(target == null) は target != null と等価
@guard(!(target == null))
UseTarget(target);  // target: T（絞り込み有効）
```

### 8.3 Nullable変数の `out` ポートへの接続

`T?` 型の変数を `out T`（非Nullable を出力するポート）に渡すことを許可します。

| アクション結果 | 変数の状態                                      |
| :------------- | :---------------------------------------------- |
| 成功           | 変数に値 `T` が書き込まれ、非 `null` 状態になる |
| 失敗           | 変数は元の値（`null` など）のまま維持される     |

```bt-dsl
extern action FindTarget(out result: Pose);

tree Main() {
    var target = null;  // Pose?

    Sequence {
        // 成功すれば target に Pose が入る
        // 失敗すれば target は null のまま
        FindTarget(result: out target);

        // null チェックしてから使用
        @guard(target != null)
        Use(target);
    }
}
```

> [!NOTE]
> 事前条件の詳細は[文法リファレンス - 事前条件](./grammar.md#_3-7-事前条件-precondition)を参照してください。

---

## 9. Import の解決

### 9.1 パス形式

import 文字列は以下のいずれかの形式でなければなりません：

- **相対パス**: `./` または `../` で始まるパス
- **パッケージパス**: 上記以外（例: `pkg_name/path/to/file.bt`）

### 9.2 解決規則

**相対パスの場合:**

1. import 文を含むファイルのディレクトリを基準とする
2. `.` および `..` セグメントを正規化する
3. 解決されたパスのファイルを読み込む

**パッケージパスの場合:**

1. パスの最初のセグメントをパッケージ名として扱う
2. **パッケージ名からルートディレクトリを特定する（Implementation Defined）**
3. 残りのパスをパッケージルートからの相対パスとして結合する
4. 解決されたパスのファイルを読み込む

> [!NOTE] パッケージ解決ロジックは **実装依存（Implementation Defined）**
> です。具体的な解決方法は環境やツールチェーンによって異なります。
>
> **実装例:**
>
> - **ROS 環境**: `rospack find <pkg>` などの ROS パッケージ解決メカニズムを使用
> - **スタンドアロン**: 環境変数やコンパイラ設定でパッケージディレクトリを指定

共通の処理として: 5. 読み込んだファイル内の **Public なシンボルのみ** を現在のスコープに取り込む

> [!IMPORTANT] Private なシンボル（`_` で始まる名前）はインポートされません。

### 9.3 インポートの非推移性

`import`
は推移的ではありません。すなわち、ファイル A がファイル B をインポートし、ファイル B がファイル C をインポートしている場合でも、ファイル A からファイル C のシンボルは参照できません。ファイル C のシンボルを使用するには、ファイル A でも明示的にファイル C をインポートする必要があります。

### 9.4 シンボルの衝突

インポートされたシンボルは、現在のファイル内で定義されたシンボルと同様に扱われます。そのため、以下の場合はコンパイルエラーとなります：

| 衝突の種類                       | 例                                                                        |
| :------------------------------- | :------------------------------------------------------------------------ |
| インポートと現在のファイルの定義 | ファイル A が `tree Foo` を定義し、インポート先にも `tree Foo` が存在する |
| 複数のインポート間               | ファイル B と C の両方に `tree Bar` があり、両ファイルをインポートした    |

> [!NOTE] 名前空間が異なるシンボル（例: `extern type X` と
> `tree X`）は衝突しません（[1.3 重複の禁止](#_1-3-重複の禁止)を参照）。
