# 初期化安全性

本ドキュメントは BT-DSL における **Blackboard の初期化安全性**のルールを定義します。

---

## 1. 原則

1. **書き込み保証**: `out` ポートに渡された変数は、そのノードが **`success` を返した時点でのみ**
   `Init`（初期化済み）状態であるとみなされます。
2. **失敗時の扱い**: ノードが `failure` を返した場合、`out`
   ポートへの書き込みは保証されません。失敗時にも値が必要な場合（エラーコード等）は、変数を事前に初期化しておく必要があります。
3. **静的解析**: コンパイラは `#[behavior]`
   属性に基づいて、ツリー全体をデータフロー解析し、未初期化 Blackboard の参照（`Uninit` 状態での
   `in`/`ref`/`mut` 渡し）をエラーとして報告します。

> [!NOTE] `mut` ポートは読み書き両方を行うため、`in` や `ref`
> と同様に呼び出し時点で初期化されている必要があります。未初期化 Blackboard を受け入れるのは `out`
> ポートのみです。

> [!IMPORTANT]
> **初期化安全性はすべての Blackboard（グローバル Blackboard を含む）に対して必須です。**
>
> - 非Nullable型（`T`）の Blackboard は使用前に必ず初期化されている必要があります
> - Nullable型（`T?`）の Blackboard は `null` で初期化することで未初期化状態を明示的に表現できます
>
> 詳細は[型システム - Nullable型](./type-system.md#_1-7-nullable型)を参照してください。

---

## 2. Blackboard 状態

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

## 3. 書き込み保証の伝播ルール

親ノードが `success` した際、Blackboard が `Init` 状態になるかどうかは、親の `DataPolicy`
によって決定されます。

### 3.1 `All`（和集合 / Union）

「親が成功したならば、すべての子が成功している」

- **意味**: どの子ノードで書き込まれた Blackboard であっても、親ノード終了後には `Init`
  とみなされます。

```bt-dsl
// Sequence は全員成功が必要なため、子の書き込みがすべて保証される
Sequence {
    TaskA(out x); // xを書く
    TaskB(out y); // yを書く
}
// -> ここで x, y 両方が Init
```

### 3.2 `Any`（共通部分 / Intersection）

「親が成功したならば、いずれかの子が成功している（どの子かは不明）」

- **意味**: **すべての子ルートで共通して書き込まれる Blackboard のみ**が、親ノード終了後に `Init`
  とみなされます。

```bt-dsl
// Fallback はいずれかが成功すれば成功なので、共通部分のみ保証される
Fallback {
    TaskA(out x);       // xを書く
    TaskB(out x, out y); // xとyを書く
}
// -> ここで x は Init (共通しているため)
// -> y は Uninit (TaskAが成功したルートでは書かれないため)
```

### 3.3 `None`（空集合 / Empty）

「親が成功しても、子の状態は保証できない」

- **意味**: 親ノード終了後、子ノードによる書き込み保証はすべて消滅します（実行前の状態に戻ります）。

```bt-dsl
// ForceSuccess は behavior(None) を持つため、子の書き込みは保証されない
ForceSuccess {
    TaskA(out x);
}
// -> ここで x は Uninit (TaskAが失敗してForceSuccessが成功させた可能性があるため)
```

---

## 4. FlowPolicy とデータの可視性

`FlowPolicy` は、兄弟ノード間での Blackboard の受け渡し（データの可視性）を制御します。

### 4.1 `Chained`（デフォルト）

前のノードの書き込み結果が、次のノードの入力として有効になります。

```bt-dsl
Sequence {
    Calculate(out result);    // result: Uninit -> Init
    Use(in result);           // OK: Init済み
}
```

### 4.2 `Isolated`

孤立している。ノードは独立して実行されるとみなされます。すべての子ノードは、親ノード開始時点の Blackboard 状態のみを参照できます。

```bt-dsl
ParallelAll {
    Calculate(out result);
    Use(in result);            // Error: resultはParallel開始時点でUninit
}
```

---

## 5. 静的解析ロジック

### 5.1 ノード呼び出し時の判定

| ポート方向 | 事前条件                         | 備考       |
| :--------- | :------------------------------- | :--------- |
| `in`       | 変数は `Init` でなければならない | **エラー** |
| `ref`      | 変数は `Init` でなければならない | **エラー** |
| `mut`      | 変数は `Init` でなければならない | **エラー** |
| `out`      | 変数は `Uninit` でも可           | —          |

### 5.2 推論ロジック

コンパイラは以下の手順で Blackboard の状態を更新します。

1. **Action / Subtree 呼び出し**:
   - `out` 引数に指定された Blackboard を、そのパスにおける「書き込み済み集合」に追加します。

2. **Control / Decorator 評価**:
   - 子ノードのパスごとの「書き込み済み集合」を、`DataPolicy`
     (`All`/`Any`/`None`) に従ってマージし、親の書き込み保証とします。

3. **FlowPolicy 評価**:
   - `Chained` ならば、状態を順次更新して次の子へ渡します。
   - `Isolated` ならば、すべての子に元の状態を渡します。

---

## 6. エラーハンドリングパターン

失敗時にエラー情報を取得したい場合は、Blackboard の事前初期化（Default
Initialization）を利用します。

```bt-dsl
// 失敗時にもエラーコードを書き込む可能性があるアクション
extern action TryConnect(out error_code: int32);

tree Main() {
    // 1. 事前に初期化 (Uninit状態を回避)
    var code: int32 = 0;

    // 2. 実行
    // 成功時: codeには成功時の値が入る
    // 失敗時: codeにはTryConnectが書いた値(あるいは元の0)が入る
    TryConnect(error_code: out code);

    // 3. 安全に利用可能 (事前にInit済みなので解析上安全)
    Log(code);
}
```

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
        Compute(res: out x);

        // Sequence は前のノードが成功しないとここに来ない
        // したがって、ここでは x は Init であることが保証される
        Log(msg: x);  // OK
    }
}
```

### 例2: ForceSuccess によるエラー検出

```bt-dsl
// 属性 #[behavior(None)] を指定
extern decorator ForceSuccess;

tree Main() {
    var x: int32;

    Sequence {
        // Compute が失敗(Uninit)しても、ForceSuccess により成功扱いになる
        // 親の Success ルートには「初期化された状態」と「初期化されていない状態」が混在
        // -> 結果として Uninit
        ForceSuccess {
            Compute(res: out x);
        }

        // コンパイルエラー: x は初期化されていない可能性があります
        Log(msg: x);
    }
}
```

### 例3: ParallelAll（静的解析可能）

```bt-dsl
tree Main() {
    var x: int32;
    var y: int32;

    Sequence {
        // ParallelAll は「全員成功なら成功」という behavior(All, Isolated)
        // 親が成功 = 全員成功 = 全員書き込み済み、と証明される
        ParallelAll {
            Compute(res: out x);
            Compute(res: out y);
        }

        // OK: x と y は両方とも初期化済み
        Log(msg: x + y);
    }
}
```

### 例4: Fallback での共通変数

```bt-dsl
tree Main() {
    var x: int32;
    var y: int32;

    Sequence {
        // Fallback は behavior(Any)
        // どちらかが成功するが、どちらかは不明
        Fallback {
            TaskA(out x);           // x を書く
            TaskB(out x, out y);    // x と y を書く
        }

        // x は両方のルートで書かれるので Init
        Log(msg: x);  // OK

        // y は TaskB ルートでのみ書かれるので Uninit
        Log(msg: y);  // Error: y は初期化されていない可能性があります
    }
}
```
