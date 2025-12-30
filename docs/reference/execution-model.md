# 5. 意味論: 実行モデル（Semantics: Execution Model）

本章は、BT-DSL が規定する範囲でのランタイム挙動を定義します。

> [!IMPORTANT]
> ここでいう「実行」は、BT-DSL が生成する BehaviorTree 構造と、ノード呼び出し規約に関する意味論です。個々の
> `extern action` の内部副作用（ロボットを動かす等）はホスト実装に依存します。

---

## 5.1 データモデル（Data Model）

### 5.1.1 Blackboard

- `var` により宣言されるエントリを **Blackboard エントリ** と呼びます。
- Blackboard エントリは、一般的な言語の「スタック変数」とは異なり、**tick 間で値が保持**されます。

### 5.1.2 ライフタイム（Tick 間の永続性）

- `children_block` 内で宣言された `var`
  であっても、値は Tree 全体で保持され、tick 間で破棄されません。

> [!NOTE] これは BehaviorTree.CPP の blackboard モデルに整合させる設計です。

### 5.1.3 コピーと参照（Copy vs Reference）

- `in` は「スナップショット入力」を意味し、論理的には**値渡し**です。
- `ref` は「ライブ参照（読み取り）」を意味し、論理的には**参照渡し（読み取り専用）**です。
- `mut` は「ライブ参照（読み書き）」を意味し、論理的には**参照渡し（読み書き）**です。
- `out` は「出力（成功時のみ書き込み保証）」を意味し、論理的には**書き込み専用参照**です。

> [!NOTE]
> 実メモリのコピー有無は最適化により処理系が変更し得ますが、観測可能な意味論は上記に従います。

---

## 5.2 ノードインターフェース（Node Interface）

### 5.2.1 ポート方向の操作権限

| 方向  | 読み取り | 書き込み | 備考                               |
| :---- | :------: | :------: | :--------------------------------- |
| `in`  |    ✓     |    ✗     | 開始時点の入力（スナップショット） |
| `ref` |    ✓     |    ✗     | 実行中も変化を観測し得る（ライブ） |
| `mut` |    ✓     |    ✓     | 状態共有・更新                     |
| `out` |    ✗     |    ✓     | **成功時のみ**書き込みが保証される |

> [!IMPORTANT] `out`
> の書き込み保証は「静的解析と安全性」で厳密に扱います（[初期化安全性](./static-analysis-and-safety.md#_6-1-初期化安全性initialization-safety)）。

### 5.2.2 実行ステータス

BT-DSL のノード実行は、少なくとも次のステータスを持ちます。

- `Success`
- `Failure`
- `Running`
- `Skip`

> [!NOTE] `Skip` の導入は BehaviorTree.CPP
> v4 のスキップ概念に整合します。厳密な伝播則（親がどう扱うか）は、ノード実装（control/decorator の振る舞い）と
> `#[behavior]` に依存し得るため、現時点では **部分的に実装定義** です（TBD）。

---

## 5.3 制御フロー（Control Flow）

### 5.3.1 Tick

- Tree は外部から繰り返し **tick** されます。
- 1 tick で実行されるノードの順序は、構文上の子列と、`#[behavior(DataPolicy, FlowPolicy)]`
  によって規定されます。

### 5.3.2 Behavior Policies

`#[behavior(DataPolicy, FlowPolicy)]` は、主に次を規定します。

- **DataPolicy**: 親が成功したときに、子による書き込み保証をどう集約するか（静的解析に影響）
- **FlowPolicy**: 兄弟間で書き込み結果が次の子の入力として可視かどうか（静的解析に影響）

詳細な定義は [初期化安全性](./static-analysis-and-safety.md#_6-1-初期化安全性initialization-safety)
を参照してください。

### 5.3.3 事前条件（Preconditions）

事前条件はノードの実行前（および一部は実行中）に評価される組み込み構文です。

| 構文                | 動作                                                       | 偽の時のステータス |
| :------------------ | :--------------------------------------------------------- | :----------------- |
| `@success_if(cond)` | 条件が真なら実行せず即座に終了                             | `Success`          |
| `@failure_if(cond)` | 条件が真なら実行せず即座に終了                             | `Failure`          |
| `@skip_if(cond)`    | 条件が真なら実行せずスキップ                               | `Skip`             |
| `@run_while(cond)`  | 実行前・実行中に条件を評価。偽になった場合、実行を中断する | `Skip`             |
| `@guard(cond)`      | 実行前・実行中に条件を評価。偽になった場合、実行を中断する | `Failure`          |

> [!NOTE] 複数の事前条件が指定された場合の評価順序は **実装定義** です。

---

## 5.4 構文糖衣の脱糖（Desugaring）

本節は「同等変換（desugaring）」を規定します。処理系は、次の変換を行った結果と**等価**な意味論を持たなければなりません。

### 5.4.1 Decorator の暗黙 Sequence

- `decorator` ノードの `children_block` に 2 つ以上の子が書かれた場合、暗黙に `Sequence`
  でラップされます。

```bt-dsl
// 入力
Retry(n: 3) {
	TaskA();
	TaskB();
}

// 脱糖後（概念的）
Retry(n: 3) {
	Sequence {
		TaskA();
		TaskB();
	}
}
```

### 5.4.2 Blackboard 宣言・代入のアクション化

- `var x = expr;` および `x = expr;` は、概念的に「Blackboard 書き込みを行い常に `Success`
  を返すアクション」と等価に扱われます。

特性:

- 返り値は常に `Success`
- 実行順序は `children_block` 内での記述順
- 実行された時点で Blackboard に値が書き込まれる
