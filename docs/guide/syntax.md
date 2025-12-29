# 構文ガイド

このページでは BT-DSL の構文をコード例を中心に解説します。

厳密な文法定義は [文法リファレンス](/reference/grammar) を参照してください。

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
  enemy_id: int,
  out visible: bool
);

extern control MySequence {
  any(failure) => failure;
  all(success) => success;
}

extern decorator Timeout(timeout_ms: int) {
  running => failure;
}
```

### カテゴリ

| カテゴリ    | 用途                                |
| :---------- | :---------------------------------- |
| `action`    | 動作を実行                          |
| `condition` | 条件を判定                          |
| `control`   | 子ノードを制御（`{...}` 必須）      |
| `decorator` | 別のノードを修飾（`@[...]` で使用） |
| `subtree`   | サブツリーの参照                    |

### ポート方向

| 方向  | 意味         | 省略時     |
| :---- | :----------- | :--------- |
| `in`  | 読み取り専用 | デフォルト |
| `out` | 書き込み専用 | -          |
| `ref` | 読み書き両用 | -          |

詳細な書き込み保証（`out always`,
`out on_failure`）については[初期化安全性](/reference/initialization-safety)を参照してください。

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
  inputOnly: int,           // in（デフォルト）
  in explicitIn: int,       // in（明示）
  out outputOnly: int,      // out
  ref readWrite: int        // ref
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
@[Repeat(num_cycles: 3), Delay(delay_msec: 100)]
Sequence {
  DoSomething()
}

// 引数なしのデコレータ
@[ForceSuccess]
MayFail()
```

- `@[...]` でデコレータを適用
- 複数のデコレータはカンマ区切り（上から順に適用）

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
var target: Pose?         // Nullable（null を許容）
target = null             // OK

@[Guard(target)]
Sequence {
  // このブロック内では target は Pose 型として扱える
  MoveTo(target)
}
```

詳細は[型システム](/reference/type-system)を参照してください。

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

詳細な演算子の優先順位は [文法リファレンス](/reference/grammar#4-演算子の優先順位)
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
