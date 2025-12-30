# 構文ガイド

このページでは BT-DSL の構文をコード例を中心に解説します。

厳密な文法定義は [構文](/reference/syntax) を参照してください。

---

## 1. プログラム構造

BT-DSL ファイルは以下の順序で構成されます：

```bt-dsl
//! このファイルの説明（Inner Doc）

import "./other.bt"

extern action MyAction(port: int);

var GlobalVar: string

tree Main() {
  MyAction(port: 42)
}
```

> [!NOTE] 順序は固定です。`import` は `tree` より前に、`extern` は `var`
> より前に書く必要があります。

---

## 2. Import

他のファイルを取り込みます。

```bt-dsl
import "./lib/standard.bt"
import "../shared/common.bt"
```

- 相対パス（`./` または `../`）のみサポート
- 拡張子は省略不可

---

## 3. extern（ノード宣言）

カスタムノードの型を宣言します。

```bt-dsl
/// プレイヤーの位置を取得（Outer Doc）
extern action GetPlayerPosition(
  /// 取得した位置（ポートの Outer Doc）
  out position: Vector3
);

extern condition IsEnemyVisible(
  enemy_id: int32,
  out visible: bool
);

// Sequence: 全員成功が必要 (デフォルト: All, Chained)
extern control Sequence;

// Fallback: 誰か成功が必要
#[behavior(Any)]
extern control Fallback;

// ForceSuccess: 子の書き込みは保証されない
#[behavior(None)]
extern decorator ForceSuccess;

extern decorator Retry(n: int32);
```

`control` / `decorator` は `#[behavior(DataPolicy, FlowPolicy)]`
属性で振る舞いを定義します。詳細は[静的解析と安全性](/reference/static-analysis-and-safety)を参照してください。

### カテゴリ

| カテゴリ    | 用途                           |
| :---------- | :----------------------------- |
| `action`    | 動作を実行                     |
| `condition` | 条件を判定                     |
| `control`   | 子ノードを制御（`{...}` 必須） |
| `decorator` | 単一の子を修飾（`{...}` 必須） |
| `subtree`   | サブツリーの参照               |

### ポート方向

| 方向  | 意味                                           | 省略時     |
| :---- | :--------------------------------------------- | :--------- |
| `in`  | Input (Snapshot): 開始時の入力                 | デフォルト |
| `ref` | View (Live Read): 継続的な監視（読み取り）     | -          |
| `mut` | State (Live R/W): 状態の共有・更新（読み書き） | -          |
| `out` | Output: 結果の出力専用                         | -          |

詳細は[静的解析と安全性 - ポート方向の整合性](/reference/static-analysis-and-safety#_6-4-ポート方向と引数制約port-direction-and-argument-constraints)を参照してください。

---

## 4. グローバル変数・定数

```bt-dsl
var PlayerHealth: int
var TargetPosition: Vector3
var IsAlerted: bool

const MAX_HEALTH = 100
const DEFAULT_SPEED: float = 1.5
```

- グローバル変数は型注釈または初期値が必要
- すべての tree から参照可能
- `const` はコンパイル時定数

---

## 5. tree 定義

```bt-dsl
/// このツリーの説明
tree SearchAndDestroy(ref target: Vector3, ref ammo: int) {
  var localCounter: int = 0

  Sequence {
    FindEnemy(pos: out target)
    AttackEnemy(ammo: ref ammo)
    localCounter += 1
  }
}
```

### パラメータ

```bt-dsl
tree Example(
  inputOnly: int32,          // in（デフォルト）
  in explicitIn: int32,      // in（明示）
  out outputOnly: int32,     // out
  ref readOnly: int32,       // ref（読み取り参照）
  mut readWrite: int32       // mut（読み書き両用）
) {
  // ...
}
```

### ローカル変数・定数

```bt-dsl
tree Example() {
  var count: int = 0       // 型と初期値
  var name = "test"        // 型推論（string）
  var flag: bool           // 初期値なし（Uninit）
  const LOCAL_MAX = 10     // ローカル定数

  // var x                  ← エラー: 型か初期値が必要
}
```

---

## 6. ノード文

### 基本形

```bt-dsl
// 引数なし
AlwaysSuccess()

// 名前付き引数
MoveTo(goal: TargetPos, speed: 1.5)

// 位置引数（ポートが1つの場合のみ）
Log("Hello, World!")

// Blackboard 参照に方向を指定
GetPosition(pos: out CurrentPos)
UpdateValue(val: ref Counter)
```

### Control ノード

```bt-dsl
Sequence {
  DoFirst()
  DoSecond()
}

Fallback {
  TryPrimary()
  TryFallback()
}

Parallel(failure_threshold: 1, success_threshold: -1) {
  MonitorCondition()
  ExecuteAction()
}
```

> [!IMPORTANT] Control ノードは `{...}` が必須です。`Sequence()` のように `()`
> だけで終わるとエラーになります。

### デコレータ

```bt-dsl
Retry(n: 3) {
  Sequence {
    DoSomething();
  }
}

ForceSuccess {
  MayFail();
}
```

デコレータは `control`
と同様の構文で使用しますが、厳密には子は1つのみです。複数記述した場合は暗黙的に `Sequence`
でラップされます。

### 事前条件（Precondition）

ノードの実行前に評価される条件を `@` 構文で記述します。

```bt-dsl
// 条件が真なら実行せずに成功
@success_if(cache_valid)
FetchData(out data);

// 条件が偽なら実行しない（Failure）
@guard(target != null)
MoveTo(target);

// 条件が偽になったら Running 中でも中断
@run_while(is_active)
LongRunningTask();
```

| 構文                | 動作                       |
| :------------------ | :------------------------- |
| `@success_if(cond)` | 真なら実行せず Success     |
| `@failure_if(cond)` | 真なら実行せず Failure     |
| `@skip_if(cond)`    | 真なら実行せず Skip        |
| `@run_while(cond)`  | 偽になったら中断 (Skip)    |
| `@guard(cond)`      | 偽になったら中断 (Failure) |

---

## 7. 型

### 基本型

```bt-dsl
var i: int32         // 32ビット整数
var f: float64       // 64ビット浮動小数点
var b: bool          // 真偽値
var s: string        // 文字列
```

### 配列型

```bt-dsl
var arr: [int32; 5]       // 静的配列（固定サイズ5）
var bounded: [int32; <=5] // 上限付き静的配列（最大5要素）
var dynamic: vec<int32>   // 動的配列

// 配列リテラル
var a = [1, 2, 3]         // [int32; 3]
var v = vec![1, 2, 3]     // vec<int32>
```

### Nullable型

```bt-dsl
var target: Pose?;         // Nullable（null を許容）
target = null;             // OK

@guard(target != null)
MoveTo(target);            // このスコープ内では target は Pose 型として扱える
```

`@guard`
内で null チェックを行うと、そのノードのスコープ内で型の絞り込み（Narrowing）が適用されます。

詳細は[型システム](/reference/type-system/)を参照してください。

---

## 8. 式と代入

### 代入文

```bt-dsl
Sequence {
  counter = 0
  counter += 1
  health -= damage
  score *= 2
}
```

代入は `{...}` ブロック内でのみ記述可能です。

### 式の演算子

```bt-dsl
Sequence {
  // 算術
  result = a + b * 2

  // 比較
  isPositive = value > 0

  // 論理
  shouldAct = isReady && !isPaused

  // ビット
  flags = mask & 0xFF

  // キャスト
  small = large as int8
}
```

詳細な演算子の優先順位は
[構文](/reference/syntax#_2-4-2-演算子の優先順位と結合規則precedence-and-associativity)
を参照してください。

---

## 9. コメント

```bt-dsl
// 行コメント

/* ブロックコメント
   複数行 OK */

/// Outer Doc（宣言・ノードに付与）
extern action Example();

//! Inner Doc（ファイル先頭に記述）
```
