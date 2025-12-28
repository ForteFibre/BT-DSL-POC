# 構文ガイド

このページでは BT-DSL の構文をコード例を中心に解説します。

厳密な文法定義は [文法リファレンス](/reference/grammar) を参照してください。

---

## 1. プログラム構造

BT-DSL ファイルは以下の順序で構成されます：

```bt-dsl
//! このファイルの説明（Inner Doc）

import "./other.bt"

declare Action MyAction(port: int)

var GlobalVar: string

Tree Main() {
  MyAction(port: 42)
}
```

> [!NOTE] 順序は固定です。`import` は `Tree` より前に、`declare` は `var`
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

## 3. Declare（ノード宣言）

カスタムノードの型を宣言します。

```bt-dsl
/// プレイヤーの位置を取得（Outer Doc）
declare Action GetPlayerPosition(
  /// 取得した位置（ポートの Outer Doc）
  out position: Vector3
)

declare Condition IsEnemyVisible(
  enemy_id: int,
  out visible: bool
)

declare Control MySequence()

declare Decorator Timeout(
  timeout_ms: int
)
```

### カテゴリ

| カテゴリ    | 用途                           |
| :---------- | :----------------------------- |
| `Action`    | 動作を実行                     |
| `Condition` | 条件を判定                     |
| `Control`   | 子ノードを制御（`{...}` 必須） |
| `Decorator` | 別のノードを修飾（`@` で使用） |
| `SubTree`   | サブツリーの参照               |

### ポート方向

| 方向  | 意味         | 省略時     |
| :---- | :----------- | :--------- |
| `in`  | 読み取り専用 | デフォルト |
| `out` | 書き込み専用 | -          |
| `ref` | 読み書き両用 | -          |

---

## 4. グローバル変数

```bt-dsl
var PlayerHealth: int
var TargetPosition: Vector3
var IsAlerted: bool
```

- 型注釈は必須
- すべての Tree から参照可能

---

## 5. Tree 定義

```bt-dsl
/// このツリーの説明
Tree SearchAndDestroy(ref target: Vector3, ref ammo: int) {
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
Tree Example(
  inputOnly: int,           // in（デフォルト）
  in explicitIn: int,       // in（明示）
  out outputOnly: int,      // out
  ref readWrite: int        // ref
) {
  // ...
}
```

### ローカル変数

```bt-dsl
Tree Example() {
  var count: int = 0       // 型と初期値
  var name = "test"         // 型推論（string）
  var flag: bool            // 初期値なし

  // var x                   ← エラー: 型か初期値が必要
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

Parallel(failure_count: 1, success_count: -1) {
  MonitorCondition()
  ExecuteAction()
}
```

> [!IMPORTANT] Control ノードは `{...}` が必須です。`Sequence()` のように `()`
> だけで終わるとエラーになります。

### デコレータ

```bt-dsl
@Repeat(num_cycles: 3)
@Delay(delay_msec: 100)
Sequence {
  DoSomething()
}

// 引数なしのデコレータ
@ForceSuccess
MayFail()
```

- `@` に続けてデコレータ名
- 複数のデコレータを重ねられる（上から順に適用）

---

## 7. 式と代入

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
}
```

詳細な演算子の優先順位は [文法リファレンス](/reference/grammar#4-演算子の優先順位)
を参照してください。

---

## 8. コメント

```bt-dsl
// 行コメント

/* ブロックコメント
   複数行 OK */

/// Outer Doc（宣言・ノードに付与）
declare Action Example()

//! Inner Doc（ファイル先頭に記述）
```
