# クイックスタート

このガイドでは、5分で最初の BT-DSL ファイルを書く方法を説明します。

---

## 1. 最小限のファイル

```bt-dsl
// main.bt - 最も単純なビヘイビアツリー

Tree Main() {
  AlwaysSuccess()
}
```

これだけで有効な BT-DSL ファイルです。

---

## 2. 標準ノードを使う

BT-DSL には標準ノードが用意されています。import で読み込みます。

```bt-dsl
import "./std.bt"

Tree Main() {
  Sequence {
    Log(message: "Starting...")
    AlwaysSuccess()
  }
}
```

---

## 3. 基本パターン

### 3.1 ノードを順番に実行（Sequence）

```bt-dsl
Sequence {
  DoFirst()
  DoSecond()
  DoThird()
}
```

すべて SUCCESS になるまで順番に実行します。1つでも FAILURE になると全体が FAILURE です。

### 3.2 成功するまで試す（Fallback）

```bt-dsl
Fallback {
  TryThis()
  TryThat()
  DoFallback()
}
```

最初に SUCCESS になったノードで停止します。すべて FAILURE なら全体が FAILURE です。

### 3.3 デコレータで修飾

```bt-dsl
@Repeat(num_cycles: 3)
DoSomething()
```

`@` に続けてデコレータ名を書きます。

---

## 4. 変数（Blackboard）

### 4.1 グローバル変数

```bt-dsl
var TargetPos: Vector3
var Health: int

Tree Main() {
  FindEnemy(pos: out TargetPos)
  MoveTo(goal: TargetPos)
}
```

- `var` でグローバル変数を宣言
- ノードへの引数で `out` を付けると書き込み用

### 4.2 ローカル変数

```bt-dsl
Tree Counter() {
  var count: int = 0

  Sequence {
    count += 1
    Log(message: "Count is updated")
  }
}
```

Tree 内で `var` を宣言すると、その Tree だけで使えます。

---

## 5. カスタムノードの宣言

独自のノードを使うには、先に宣言が必要です。

```bt-dsl
/// 敵を探して位置を返す
declare Action FindEnemy(
  /// 見つけた敵の位置
  out pos: Vector3
)

/// 指定位置に移動
declare Action MoveTo(
  goal: Vector3
)
```

---

## 6. 次のステップ

- 詳しい構文 → [構文ガイド](/guide/syntax)
- 厳密な仕様 → [文法リファレンス](/reference/grammar)
- 標準ノード一覧 → [標準ライブラリ](/standard-library)
