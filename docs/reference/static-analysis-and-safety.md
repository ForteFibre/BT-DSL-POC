# 6. 静的解析と安全性（Static Analysis and Safety）

本章は、コンパイル時に検証される整合性ルール（エラー/警告）を定義します。

---

## 6.1 初期化安全性（Initialization Safety）

### 6.1.1 原則

1. **書き込み保証**: `out` ポートに渡された変数は、そのノードが **`Success` を返した時点でのみ**
   `Init`（初期化済み）とみなされます。
   ただし、ここでいう `Success` は「**ノード本体が実行され**、その実行が `Success` で終了した」場合に限ります。
   事前条件（5.3.3）等によりノード本体の実行がスキップされた場合、当該呼び出しによる `out` への書き込みは発生せず、
   `out` に渡された変数を `Init` とみなしてはなりません。
2. **失敗時の扱い**: ノードが `Failure` の場合、`out` への書き込みは保証されません。
3. **静的解析**: コンパイラは `#[behavior]`
   に基づきデータフロー解析を行い、未初期化（`Uninit`）の参照をエラーとして報告します。

> [!NOTE]
> `mut` は読み書き両方を行うため、`in`/`ref` と同様に呼び出し時点で `Init` が必要です。

### 6.1.2 状態

| 状態     | 説明       |
| :------- | :--------- |
| `Uninit` | 未初期化   |
| `Init`   | 初期化済み |

### 6.1.3 DataPolicy / FlowPolicy による伝播

親ノードが `Success` した際、Blackboard が `Init` になるか（= 書き込み保証が伝播するか）は、親の
`#[behavior(DataPolicy, FlowPolicy)]` によって決定されます。

> [!IMPORTANT]
> `#[behavior]` を省略した場合のデフォルトは次のとおりです。
>
> - **DataPolicy のデフォルトは `All`**
> - **FlowPolicy のデフォルトは `Chained`**
>
> したがって、`extern control X(...);` / `extern decorator Y(...);` は `#[behavior(All, Chained)]`
> を暗黙に持つものとして解析されます。

#### DataPolicy（成功時の書き込み保証の集約）

DataPolicy は「親が成功した」事実から、子の成功ルートに関する情報をどこまで推論できるかを定義します。

##### `All`（和集合 / Union）

「親が成功したならば、**すべての子が成功している**」

- **意味**: どの子ノードで `out` が書かれたかに関わらず、親成功後には `Init` とみなされます。

```bt-dsl
Sequence {
  TaskA(out x);
  TaskB(out y);
}
// -> Sequence が Success で抜けたなら x, y は Init
```

##### `Any`（共通部分 / Intersection）

「親が成功したならば、**いずれかの子が成功している（どの子かは不明）** 」

- **意味**: すべての成功ルートで **共通して書き込まれる `out`** のみが、親成功後に `Init`
  とみなされます。
- **共通して書き込まれる**: 複数の子ノードが **同一の変数** を `out` として指定している場合に限り、
  その変数は親成功後に `Init` となります。異なる変数を出力する場合、いずれも `Init` とはみなされません。

```bt-dsl
Fallback {
  TaskA(out x);
  TaskB(out x, out y);
}
// -> Fallback が Success で抜けたなら x は Init
// -> y は Uninit の可能性がある
```

```bt-dsl
Fallback {
  TaskA(out x);
  TaskB(out y);
}
// -> x, y いずれも Init とはみなされない（共通の out がないため）
```

##### `None`（空集合 / Empty）

「親が成功しても、子の状態は保証できない」

- **意味**: 親成功後、子による書き込み保証は消滅します。

```bt-dsl
ForceSuccess {
  TaskA(out x);
}
// -> ForceSuccess が Success でも x は Init と保証できない
```

#### FlowPolicy（兄弟間の可視性）

FlowPolicy は「兄弟ノード間で、前のノードの書き込み結果を次のノードが参照できるか」を規定します。

##### `Chained`（デフォルト）

前のノードの書き込み結果が、次のノードの入力として有効になります。

```bt-dsl
Sequence {
  Calculate(out result);
  Use(in result);          // OK: result は Init
}
```

##### `Isolated`

孤立している。すべての子ノードは「親開始時点の Blackboard 状態」のみを参照できます。

```bt-dsl
ParallelAll {
  Calculate(out result);
  Use(in result);          // Error: result は ParallelAll 開始時点では Uninit
}
```

### 6.1.4 ノード呼び出し時の判定

呼び出し時点での事前条件は次のとおりです。

| ポート方向 | 事前条件                         | 備考   |
| :--------- | :------------------------------- | :----- |
| `in`       | 変数は `Init` でなければならない | エラー |
| `ref`      | 変数は `Init` でなければならない | エラー |
| `mut`      | 変数は `Init` でなければならない | エラー |
| `out`      | 変数は `Uninit` でも可           | —      |

### 6.1.5 推論ロジック（高水準）

処理系は、ツリーをデータフロー解析し、パスごとに Blackboard の `Init/Uninit`
を推論します。概略は次です。

1. **Action / Subtree 呼び出し**: `out`
   引数に指定された Blackboard を、そのパスの「書き込み済み集合」に追加します。
   ただし、当該パスにおいてノード本体が実行されず（例: `@success_if` / `@failure_if` / `@skip_if` によるスキップ）、
   あるいは `Success` 以外で終了する場合、その呼び出しは書き込み済み集合に寄与しません。
2. **Control / Decorator**: 子パスごとの集合を DataPolicy（`All`/`Any`/`None`）でマージします。
3. **FlowPolicy**: `Chained` は順次更新、`Isolated` は親開始状態を全子へ配布します。

### 6.1.6 失敗時にも値が必要な場合（パターン）

失敗時にもエラーコード等が必要な場合は、**事前初期化**により `Init` を確保します。

```bt-dsl
extern action TryConnect(out error_code: int32);

tree Main() {
  var code: int32 = 0;      // 事前に Init
  TryConnect(error_code: out code);
  Log(code);                // OK: code は常に Init
}
```

---

## 6.2 Null安全性と型の絞り込み（Null Safety and Narrowing）

### 6.2.1 Flow-Sensitive Typing

`@guard(expr)` / `@run_while(expr)` の条件式 `expr`
が「真である」と保証されるスコープ（対象ノードの内部）において、Nullable 変数 `T?` を `T`
に絞り込む（narrowing）ことがあります。

```bt-dsl
var target: Pose? = null;

Fallback {
  @guard(target != null)
  Sequence {
    // このブロック内では target: Pose として扱える
    MoveTo(target);
  }
}
```

### 6.2.2 条件式の分解規則（必要条件の抽出）

処理系は、式 `expr` が真となるために必要となる条件（Necessary
Conditions）を抽出し、型の絞り込みに用います。

#### null 比較

- `x != null` が真である場合、`x: T?` はそのスコープ内で `x: T` に絞り込まれます。

#### 連言（Conjunction）

- `(a && b)` が真であるためには `a` と `b` が共に真である必要があるため、両辺に再帰的に適用します。

```bt-dsl
@guard(a != null && b != null)
UseBoth(a, b); // a: T, b: U（ブロック内）
```

#### 否定（Negation）

- `!p` が真である場合、`p` は偽です。処理系は `p`
  の否定に相当する条件を解析に用いても構いません（例: `!(x == null)` を `x != null` として扱う）。

### 6.2.3 Nullable 変数の `out` への接続

`T?` 型の変数を `out T`（非Nullable を出力するポート）に渡すことを許可します。

| ノード結果 | 変数の状態                                  |
| :--------- | :------------------------------------------ |
| `Success`  | 変数に値 `T` が書き込まれ、非 `null` になる |
| `Failure`  | 変数は元の値（`null` など）のまま維持される |

```bt-dsl
extern action FindTarget(out result: Pose);

tree Main() {
  var target = null;  // Pose?

  Sequence {
    FindTarget(result: out target);

    @guard(target != null)
    Use(target);
  }
}
```

---

## 6.3 構造的制約（Structural Constraints）

### 6.3.1 再帰呼び出しの禁止

- `tree` の直接・間接の再帰呼び出しは **禁止** です。

### 6.3.2 未使用の `mut`/`out` の警告

- `mut` または `out`
  として宣言された tree パラメータが、一度も書き込みに使用されない場合、**警告**となります。

---

## 6.4 ポート方向と引数制約（Port Direction and Argument Constraints）

本節は、ノード呼び出しにおける **ポート方向** と **引数の形（LValue/RValue）**
の整合性を規定します。

### 6.4.1 方向の定義

| 方向  | 意味（Semantics） | 役割                               |
| :---- | :---------------- | :--------------------------------- |
| `in`  | Input (Snapshot)  | 開始時の入力（論理的には値渡し）   |
| `ref` | View (Live Read)  | ライブ参照（読み取り専用）         |
| `mut` | State (Live R/W)  | ライブ参照（読み書き）             |
| `out` | Output            | 成功時のみ書き込みが保証される出力 |

### 6.4.2 引数方向とポート方向の整合性

ノード呼び出し時、引数に指定された方向（`arg_dir`）と、宣言ポートの方向（`port_dir`）は整合していなければなりません。

| `arg_dir` \ `port_dir` |   `in`    |   `ref`   |  `mut`  |  `out`  |
| :--------------------- | :-------: | :-------: | :-----: | :-----: |
| `in`（または省略）     |     ✓     |  ✗ Error  | ✗ Error | ✗ Error |
| `ref`                  | ⚠ Warning |     ✓     | ✗ Error | ✗ Error |
| `mut`                  | ⚠ Warning | ⚠ Warning |    ✓    | ✗ Error |
| `out`                  |  ✗ Error  |  ✗ Error  | ✗ Error |    ✓    |

> [!NOTE]
> Warning は「過剰権限（必要以上に強い参照）」を意味します。処理系は警告として報告できます。

### 6.4.3 引数の種類（LValue / RValue）

- **`in`**: 任意の式（RValue）を指定できます。
- **`ref`**: 左辺値（LValue）でなければなりません。
- **`mut` / `out`**: 左辺値（LValue）でなければなりません。

左辺値（LValue）とは、少なくとも次のいずれかです。

- 変数（Blackboard）
- tree パラメータ
- 配列要素（例: `arr[0]`）

### 6.4.4 tree パラメータの権限

引数が tree パラメータを参照する場合の追加制約を次に示します。

| パラメータ方向     | 引数に `ref` を付与 | 引数に `mut`/`out` を付与 |
| :----------------- | :------------------ | :------------------------ |
| `in`（または省略） | 許可                | **エラー**                |
| `ref`              | 許可                | **エラー**                |
| `mut`              | 許可                | 許可                      |
| `out`              | **エラー**          | 許可                      |

### 6.4.5 位置引数（positional argument）

- 位置引数は **最大1個**まで。
- 位置引数を使用する場合、対象ノードのポート数は **ちょうど1** でなければなりません。

### 6.4.6 引数の省略規則

| ポート方向         | デフォルト値あり | デフォルト値なし |
| :----------------- | :--------------- | :--------------- |
| `in`（または省略） | 省略可能         | 省略不可         |
| `ref`              | （設定不可）     | **省略不可**     |
| `mut`              | （設定不可）     | **省略不可**     |
| `out`              | （設定不可）     | **常に省略可能** |

`out` 引数が省略された場合、その出力は **未接続**として扱われ、評価結果は破棄されます。
このとき、当該 `out` ポートによる書き込み保証は存在しないため、初期化安全性の解析（6.1）においても
`Init` 伝播には寄与しません。

### 6.4.7 デフォルト値の制約

- `ref` / `mut` / `out` ポートにはデフォルト値を指定できません。
- デフォルト値は `const_expr`（コンパイル時評価可能）でなければなりません。

> [!NOTE]
> `const_expr` は構文上は `expression` ですが、意味論として「コンパイル時に評価可能」である必要があります。
> 詳細は [意味論: 宣言とスコープ - 定数評価](./declarations-and-scopes.md#_4-3-定数評価constant-evaluation) を参照してください。
