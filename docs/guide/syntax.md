# 構文ガイド

このページでは BT-DSL の構文をコード例を中心に解説します。

厳密な文法定義は [構文](/reference/syntax) を参照してください。

---

## 1. プログラム構造

BT-DSL ファイルは典型的には次のように書きます（例）：

```bt-dsl
//! このファイルの説明（Inner Doc）

import "./other.bt"

extern action MyAction(port: int32);

var GlobalVar: int32;

tree Main() {
  root MyAction(port: 42);
}
```

> [!NOTE]
> トップレベル定義（`import`/`extern`/`type`/グローバル `var`/`const`/`tree`）の**出現順序は自由**です。
> ただし可読性のため、`import` を先頭付近にまとめることを推奨します。

---

## 2. Import

他のファイルを取り込みます。

```bt-dsl
import "./lib/standard.bt"
import "../shared/common.bt"
```

- `./` / `../` で始まる相対パスは **import 元ファイルのディレクトリ基準**で解決されます。
- 絶対パスは認めません。
- 拡張子は必須です。
- import は **非推移的**です（import したファイルのみ見え、import 先がさらに import したものは見えません）。

> [!NOTE]
> `"aaa/bbb.bt"` のようなパッケージ形式の解決（検索パス等）は **implementation-defined** です。

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
extern control Sequence();

// Fallback: 誰か成功が必要
#[behavior(Any)]
extern control Fallback();

// ForceSuccess: 子の書き込みは保証されない
#[behavior(None)]
extern decorator ForceSuccess();

extern decorator Retry(n: int32);
```

`control` / `decorator` は `#[behavior(DataPolicy, FlowPolicy)]`
属性で振る舞いを定義します。

### カテゴリ

| カテゴリ    | 用途                                |
| :---------- | :---------------------------------- |
| `action`    | 動作を実行                          |
| `condition` | 条件を判定                          |
| `control`   | 子ノードを制御（`{...}` 必須）      |
| `decorator` | 単一の子を修飾（`{...}` 必須）      |
| `subtree`   | DSL 外で定義された SubTree への参照 |

> [!NOTE]
> `extern subtree` は、他の XML ファイルや BT.CPP コードで定義された SubTree を参照するために使用します。
> DSL 内で定義する SubTree は `tree` キーワードを使用してください。
>
> ```bt-dsl
> // 外部定義の SubTree を宣言
> extern subtree NavigationTree(in goal: Vector3);
>
> // DSL 内で定義する SubTree
> tree MyTree() { ... }
> ```

### ポート方向

| 方向    | 意味   | 省略時     |
| :------ | :----- | :--------- |
| `in`    | 入力   | デフォルト |
| `inout` | 入出力 | -          |
| `out`   | 出力   | -          |

---

## 4. グローバル変数・定数

```bt-dsl
/// プレイヤーの体力
var PlayerHealth: int32;
var TargetPosition: Vector3;
var IsAlerted: bool;

/// 最大リトライ回数
const MAX_HEALTH = 100;
const DEFAULT_SPEED: float32 = 1.5;
```

- グローバル変数は型注釈または初期値が必要
- すべての tree から参照可能
- `const` は不変値（初期化後に変更できません）

> [!NOTE]
> `const` はコンパイル時定数です。初期化式にはリテラル、他の `const`、基本的な演算のみが使用できます。
> `var` やパラメータは参照できません。詳細は[型システム - 定数式](/reference/type-system#344-定数式constant-expression)を参照してください。

---

## 5. tree 定義

```bt-dsl
/// このツリーの説明
tree SearchAndDestroy(out target: Vector3, inout ammo: int32) {
  var localCounter: int32 = 0;

  root Sequence {
    FindEnemy(pos: out target);
    AttackEnemy(ammo: inout ammo);
    do {
      localCounter += 1;
    }
  }
}
```

### パラメータ

```bt-dsl
tree Example(
  inputOnly: int32,          // in（デフォルト）
  in explicitIn: int32,      // in（明示）
  out outputOnly: int32,     // out
  inout readWrite: int32     // inout（入出力）
) {
  // ...
}
```

### ローカル変数・定数

```bt-dsl
tree Example() {
  /// カウンタ
  var count: int32 = 0;       // 型と初期値
  var value = 1.0;            // 型推論（float64）
  var flag: bool;           // 初期値なし（Unset: 値なし）

  /// ローカル上限
  const LOCAL_MAX = 10;     // ローカル定数

  root Sequence {
    DoSomething();
  }

  // var x                  ← エラー: 型か初期値が必要
}
```

> [!NOTE]
> `var` / `const` 宣言は `root` より前に配置します。

---

## 6. ノード文

### 基本形

```bt-dsl
// 引数なし
AlwaysSuccess();

// 名前付き引数
MoveTo(goal: TargetPos, speed: 1.5);

// Blackboard 参照に方向を指定
GetPosition(pos: out CurrentPos);
UpdateValue(val: inout Counter);
```

> [!IMPORTANT]
> `children_block`（`{ ... }`）内では、Leaf ノード呼び出し（子を持たない呼び出し）は `;` が必要です: `Action(...);`
>
> `Sequence { ... }` のような Compound ノード呼び出し（子ブロックを持つもの）自体には `;` は不要です。
>
> なお、ローカル宣言（`var x = ...;` / `const X = ...;`）は `tree` 本体の `root` より**前**でのみ記述可能です。`children_block` 内では宣言できません。

### Control ノード

```bt-dsl
Sequence {
  DoFirst();
  DoSecond();
}

Fallback {
  TryPrimary();
  TryFallback();
}

Parallel(failure_threshold: 1, success_threshold: -1) {
  MonitorCondition();
  ExecuteAction();
}
```

> [!IMPORTANT]
> Control ノードは `{...}` が必須です。`Sequence()` のように `()`
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

> [!NOTE]
> 複数の子を記述した場合、暗黙的に `Sequence` でラップされます。
> 以下の2つは等価です：
>
> ```bt-dsl
> Retry(n: 3) {
>   ActionA();
>   ActionB();
> }
>
> // 上記は以下と等価
> Retry(n: 3) {
>   Sequence {
>     ActionA();
>     ActionB();
>   }
> }
> ```

### ポート引数の評価

ポート引数に渡した式は**毎 tick 評価**されます。これにより、Blackboard の値が変化すると自動的に追従します。

```bt-dsl
// targetPos が外部で更新されると、MoveTo も新しい値を使う
MoveTo(goal: targetPos);

// 式も毎tick再評価される
MoveTo(goal: targetPos + offset);
```

> [!IMPORTANT]
> 式（`targetPos + 1` など）も毎tick評価されます。
> 「計算結果を固定したい」場合は、事前にローカル変数にコピーしてください。

```bt-dsl
Sequence {
  // 開始時の値を固定
  do { cachedGoal = targetPos + 1; }

  // cachedGoal は変化しない
  MoveTo(goal: cachedGoal);
}
```

### 事前条件（Precondition）

ノードの実行前に評価される条件を `@` 構文で記述します。

```bt-dsl
// 条件が真なら実行せずに成功
@success_if(cache_valid)
FetchData(result: out data);

// 条件が偽なら実行しない（Failure）
@guard(is_set(target))
MoveTo(goal: target);

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

> [!NOTE]
> `Skip` は多くの場合 `Success` と同様に扱われ、親が「次の子へ進む」等の進行を行います。
> 最終的な伝播はノード実装に依存します（詳細は [意味論](/reference/semantics)）。

> [!IMPORTANT]
> 事前条件は1つのノードに対して**1つだけ**付与できます。
> 複数の条件が必要な場合は、論理演算子で結合してください。
>
> ```bt-dsl
> // OK: 1つの事前条件に結合
> @guard(is_set(x) && is_set(y))
> DoSomething();
> ```

---

## 7. 型

### 基本型

```bt-dsl
var b: bool;            // 真偽値
var i: int32;           // 32ビット符号付き整数
var u: uint64;          // 64ビット符号なし整数
var f: float64;         // 64ビット浮動小数点
var s: string;          // 文字列
```

> [!NOTE]
> 整数型は `int8`〜`int64`、`uint8`〜`uint64`、浮動小数点型は `float32`/`float64` が利用可能です。

### Unset 変数と存在チェック

```bt-dsl
var target: Pose;          // 初期値なし = Unset（値がない）

// target = null;          // エラー: null リテラルは使用できません

@guard(is_set(target))
MoveTo(target);            // このスコープ内では target は Set として扱われる
```

初期化されていない変数は **Unset** 状態であり、Blackboard 上にエントリが存在しません。
Unset 変数を読み取ろうとすると（`in` ポートへの引数渡しなど）、実行時エラーとなります。

`is_set(x)` 関数を使用することで、変数が設定済みかどうかをチェックできます。
`@guard` 内でチェックを行うと、そのノードのスコープ内で変数を安全に使用できます（[データフロー安全性](/internals/data-flow-safety)）。

詳細は[型システム](/reference/type-system)を参照してください。

---

## 8. 式と代入

### do ブロック（代入文・変数宣言）

```bt-dsl
tree Counter() {
  var counter: int32 = 0;
  var health: int32 = 100;
  var damage: int32 = 10;
  var score: int32 = 0;

  root Sequence {
    do {
      counter = 0;
      counter += 1;
      health -= damage;
      score *= 2;
      var temp: int32 = counter * 2;  // do 内での変数宣言も可能
    }
    DoWork();
  }
}
```

代入文と `var` 宣言は `do { ... }` ブロック内で記述可能です。`do` ブロックは常に `Success` を返します。
`do` 内で宣言された変数は Tree ローカルスコープを持ち、Tree 全体から参照可能です。

### 式の演算子

```bt-dsl
tree Expressions() {
  var a: int32 = 5;
  var b: int32 = 3;
  var result: int32 = 0;
  var value: int32 = 1;
  var isPositive: bool = false;
  var isReady: bool = true;
  var isPaused: bool = false;
  var shouldAct: bool = false;
  var mask: int32 = 15;
  var flags: int32 = 0;
  var large: float64 = 3.7;
  var small: int32 = 0;

  root Sequence {
    do {
      // 算術
      result = a + b * 2;

      // 比較
      isPositive = value > 0;

      // 論理
      shouldAct = isReady && !isPaused;

      // ビット
      flags = mask & 255;

      // キャスト
      small = large as int32;
    }
  }
}
```

詳細な演算子の優先順位は
[構文](/reference/syntax#242-演算子の優先順位と結合規則)
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
