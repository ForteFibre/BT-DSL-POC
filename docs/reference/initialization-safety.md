# 初期化安全性

本ドキュメントは BT-DSL における**変数の初期化安全性**と、それを実現するための**拡張ノード定義**を定義します。

---

## 1. 概要

本言語仕様は、Behavior
Tree における変数の安全なデータフローを保証するための仕組みを定義します。コンパイラは以下の情報を利用して、変数が参照される時点で必ず初期化されていること（`Init`
状態）を静的に検証します。

1. **ポートの書き込み保証**: `extern` ノードがどのタイミングで変数に書き込むか
2. **ノードの状態遷移**: Decorator や Control ノードが、子の結果をどう親の結果に変換するか
3. **データフロー解析**: ユーザー定義 `tree` の内部グラフに基づく、実効的な書き込み保証の推論

> [!IMPORTANT] **初期化安全性はすべての変数（グローバル変数を含む）に対して必須です。**
>
> - 非Nullable型（`T`）の変数は使用前に必ず初期化されている必要があります
> - Nullable型（`T?`）の変数は `null` で初期化することで未初期化状態を明示的に表現できます
>
> 詳細は[型システム - Nullable型](./type-system.md#_1-7-nullable型)を参照してください。

---

## 2. 変数状態

### 2.1 状態の定義

| 状態     | 説明                                                                         |
| :------- | :--------------------------------------------------------------------------- |
| `Uninit` | 未初期化。宣言直後（初期値なし）、または初期化が保証されないパスを通った状態 |
| `Init`   | 初期化済み。初期値あり、または `out` ポートへの書き込みが保証された状態      |

### 2.2 状態遷移

```
var x: int32;        // x は Uninit

Compute(out x);      // Compute が成功すれば x は Init

Log(msg: x);         // x が Init でなければコンパイルエラー
```

---

## 3. 書き込み保証修飾子

`out` ポートが値を書き込むタイミング（事後条件）を制御します。

### 3.1 構文

```ebnf
port_direction    = "in" | out_direction | "ref" ;

out_direction     = "out" , [ guarantee_modifier ] ;

guarantee_modifier = "always" | "on_failure" ;
```

> [!IMPORTANT] 書き込み保証修飾子は **`extern` 宣言でのみ**使用可能です。`tree` 定義の `out`
> ポートはコンパイラが内部を解析して自動推論するため、修飾子は不要です。

### 3.2 修飾子の意味

| 修飾子           | 意味                     | 書き込み保証         | 用途                            |
| :--------------- | :----------------------- | :------------------- | :------------------------------ |
| `out`（省略）    | 成功時のみ書き込む       | `{Success}`          | 通常のアクション、計算処理      |
| `out always`     | 成功・失敗両方で書き込む | `{Success, Failure}` | ステータス取得、ハードウェア IO |
| `out on_failure` | 失敗時のみ書き込む       | `{Failure}`          | エラー詳細情報の出力            |

### 3.3 使用例

```bt-dsl
// 成功時のみ result を書き込む（デフォルト）
extern action Compute(out result: int32);

// 成功・失敗に関わらず常に status を書き込む
extern action GetStatus(out always status: int32);

// 失敗時のみ error_code を書き込む
extern action TryConnect(out on_failure error_code: int32);
```

### 3.4 ref ポートの扱い

`ref` ポートは「読み書き両用」であるため、呼び出し前に変数が `Init` である必要があります。

---

## 4. ノードロジック定義

Decorator や Control ノードの振る舞いを DSL 内で定義します。これにより、コンパイラは任意の拡張ノードに対しても正しい初期化解析を行えます。

### 4.1 共通構文

- **アロー記法 (`=>`)**: 状態遷移を定義。記述がない遷移は**パススルー**（変更なし）
- **ワイルドカード (`_`)**: 任意のステータスにマッチ
- **Ambiguous (`ambiguous`)**: 結果が一意に定まらない（非決定性がある）ことを示す

**ステータス定数**: `success`, `failure`, `running`

### 4.2 Decorator（状態変換）

子ノード（1つ）の結果を変換するルールを記述します。

**構文**:

```ebnf
decorator_def     = "decorator" , identifier , [ "(" , [ extern_port_list ] , ")" ] ,
                    "{" , { mapping_stmt } , "}" ;

mapping_stmt      = ( status_kind | "_" ) , "=>" , result_status , ";" ;

status_kind       = "success" | "failure" | "running" ;
result_status     = status_kind | "ambiguous" ;
```

**例: 標準デコレータ**

```bt-dsl
// 子が失敗しても成功を返す
extern decorator ForceSuccess {
    failure => success;
}

// 成功と失敗を反転
extern decorator Invert {
    success => failure;
    failure => success;
}

// Running を Failure として扱う（タイムアウト）
extern decorator Timeout(in duration: float64) {
    running => failure;
}

// 失敗を継続として扱う（リトライ）
extern decorator Retry(in max_attempts: int32) {
    failure => running;
}
```

### 4.3 Control（論理集約）

複数の子ノードの結果を集約するルールを記述します。

**構文**:

```ebnf
control_def       = "control" , identifier , [ "(" , [ extern_port_list ] , ")" ] ,
                    "{" , [ "execution" , "=" , exec_mode , ";" ] , { mapping_stmt } , "}" ;

exec_mode         = "sequential" | "parallel" ;

(* 条件式 *)
condition         = status_kind
                  | "any(" , status_kind , ")"
                  | "all(" , status_kind , ")" ;
```

**条件式**:

| 条件          | 意味                                                              |
| :------------ | :---------------------------------------------------------------- |
| `any(status)` | 子のどれか1つでも `status` なら、即座にその結果を返す（短絡評価） |
| `all(status)` | 子のすべてが `status` なら、その結果を返す                        |

**実行属性**:

| 属性                       | 説明                                                 |
| :------------------------- | :--------------------------------------------------- |
| `sequential`（デフォルト） | 順次実行。前のノードの出力を後のノードの入力に使える |
| `parallel`                 | 並列実行。                                           |

**例: 標準コントロール**

```bt-dsl
// Sequence: 誰か失敗したら即失敗、全員成功なら成功
extern control Sequence {
    any(failure) => failure;
    all(success) => success;
}

// Fallback: 誰か成功したら即成功、全員失敗なら失敗
extern control Fallback {
    any(success) => success;
    all(failure) => failure;
}

// ParallelAll: 全員成功なら成功、1つでも失敗なら失敗（静的解析可能）
extern control ParallelAll {
    execution = parallel;
    all(success) => success;
    any(failure) => failure;
}

// Parallel: 動的閾値による並列実行（静的解析不可）
extern control Parallel(
    in success_threshold: int32,
    in failure_threshold: int32
) {
    execution = parallel;
    _ => ambiguous;
}
```

### 4.4 Ambiguous（非決定性）

ランダムノードなど、静的に結果を特定できない場合に使用します。動的パラメータを持つ `Parallel`
も静的解析が不可能なため `ambiguous` として定義されます。

```bt-dsl
extern control RandomSelector {
    _ => ambiguous;
}
```

`ambiguous`
への遷移が含まれるパスでは、**「変数は初期化されない可能性がある」**という最悪のケースが適用されます。

---

## 5. 静的解析ロジック

コンパイラは以下のアルゴリズムで変数の状態を追跡します。

### 5.1 ノード呼び出し時の判定

| ポート方向 | 事前条件                         | 備考       |
| :--------- | :------------------------------- | :--------- |
| `in`       | 変数は `Init` でなければならない | **エラー** |
| `ref`      | 変数は `Init` でなければならない | **エラー** |
| `out`      | 変数は `Uninit` でも可           | —          |

### 5.2 実行後の状態更新

呼び出したノードの保証セット `G(node)` と、現在のパス `P` に基づいて判定：

- `P ∈ G(node)` ならば、変数は `Init` に遷移
- それ以外なら、変数は `Uninit` のまま

### 5.3 状態の合流

親ノードの結果ステータス `S` に至るすべての内部パスを検査：

- すべてのパスにおいて変数が `Init` となる場合のみ、親の `S` において変数は `Init`
- 一つでも `Uninit` のパス（例: ForceSuccess による失敗の隠蔽、ambiguous）があれば、`Uninit`

---

## 6. tree の書き込み保証推論

ユーザー定義の `tree` はホワイトボックスであるため、コンパイラが内部のグラフを解析し、その `out`
ポートの保証レベルを自動的に推論します。

```bt-dsl
// コンパイラが自動的に推論する
tree MyTask(out result: int32) {
    // GetStatus が "always" で書き込むなら、
    // MyTask も "always" として推論される
    GetStatus(code: out result);
}
```

> [!NOTE] `tree` の `out` ポートには修飾子を指定できません。保証レベルは内部実装から推論されます。

---

## 7. 使用例

### 例1: 基本的な安全性チェック

```bt-dsl
extern action Compute(out res: int32);
extern action Log(in msg: int32);

tree Main() {
    var x: int32;  // Uninit

    Sequence {
        // x は成功時のみ Init
        Compute(res: out x),

        // Sequence は前のノードが成功しないとここに来ない
        // したがって、ここでは x は Init であることが保証される
        Log(msg: x)  // OK
    }
}
```

### 例2: ForceSuccess によるエラー検出

```bt-dsl
extern decorator ForceSuccess { failure => success; }

tree Main() {
    var x: int32;

    Sequence {
        // Compute が失敗(Uninit)しても、ForceSuccess により成功扱いになる
        // 親の Success ルートには「初期化された状態」と「初期化されていない状態」が混在
        // -> 結果として Uninit
        @[ForceSuccess] Compute(res: out x),

        // コンパイルエラー: x は初期化されていない可能性があります
        Log(msg: x)
    }
}
```

### 例3: ParallelAll（静的解析可能）

```bt-dsl
tree Main() {
    var x: int32;
    var y: int32;

    Sequence {
        // ParallelAll は「全員成功なら成功」という静的に決定可能なルール
        // 親が成功 ＝ 全員成功 ＝ 全員書き込み済み、と証明される
        ParallelAll {
            Compute(res: out x),
            Compute(res: out y)
        },

        // OK: x と y は両方とも初期化済み
        Log(msg: x + y)
    }
}
```

> [!NOTE] 動的閾値を使用する `Parallel` は `ambiguous` として扱われるため、`out`
> で書き込まれた変数は「初期化されていない可能性がある」とみなされます。静的解析が必要な場合は
> `ParallelAll` を使用してください。

### 例4: Ambiguous（確率的ノード）

```bt-dsl
extern decorator CoinFlip { _ => ambiguous; }
extern action GetLucky(out always val: int32);

tree Main() {
    var x: int32;

    Sequence {
        // CoinFlip の結果は ambiguous だが、
        // 子の GetLucky は "always" 書き込む
        // 成功しようが失敗しようが、実行さえされれば x は書かれる
        @[CoinFlip] GetLucky(val: out x),

        // 推論結果: OK (x is Init)
        Log(msg: x)
    }
}
```
