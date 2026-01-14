# Examples（実践例）

ここではサンプルを題材に、BT-DSLの典型的な使い方（グローバル黒板、SubTree、ポート方向、Control/Decoratorの組み合わせ）を説明します。

---

## 1. サンプル全体

（ここでは要点に絞って紹介します。）

ポイント:

- `import` により標準ノード定義（標準ライブラリ）を導入
- グローバル黒板変数:
  - `TargetPos: Vector3`, `Ammo: int32`, `IsAlerted: bool`
- メインループは `Repeat { Sequence { ... } }` の形式

---

## 2. グローバル変数（黒板）

```bt-dsl
var TargetPos: Vector3;
var Ammo: int32;
var IsAlerted: bool;
```

- グローバル変数は全 tree から参照可能。

---

## 3. tree を SubTree として呼び出す

`SearchAndDestroy` は `tree SearchAndDestroy(out target: Vector3, inout ammo: int32) { ... }`
として定義され、別 tree からはノード呼び出しとして使います。

```bt-dsl
tree Main() {
  root SearchAndDestroy(
    target: out TargetPos,
    ammo: inout Ammo
  );
}
```

- `inout` を付けることで「入出力」を明示。
- 出力専用の場合は `out` を使用。
- 宣言側ポートが `in` の場合に `inout`
  を付けると警告（入力専用ポートに対して書き込み意図を示しているため）。

---

## 4. out ポートへの受け渡し

サンプル内には以下があります:

```bt-dsl
FindEnemy(pos: out target, found: out alert);
```

- `FindEnemy` の宣言（例）は `pos` / `found` が `out` として定義される想定。
- `out target` のように、引数側で `out` を付ける必要があります。

---

## 5. Control ノード + children ブロック

```bt-dsl
Fallback {
  AlwaysFailure();
  AlwaysSuccess();
}
```

- `Fallback` は Control なので `{ ... }` が必須。
- `AlwaysFailure()` / `AlwaysSuccess()` は引数なしの Action ノード（子ブロックを持たない）。

---

## 6. デコレータと事前条件

### デコレータ

```bt-dsl
Retry(n: 3) {
  DoSomething();
}

ForceSuccess {
  MayFail();
}
```

デコレータは `control` と同様に `{ ... }` ブロックで子を持ちます。

### 事前条件

```bt-dsl
// 存在チェックで安全なアクセス
@guard(is_set(target))
MoveTo(goal: target);

// 条件が真ならスキップ
@success_if(already_done)
DoTask();
```

`@guard`, `@success_if`, `@failure_if`, `@skip_if`, `@run_while` が使用できます。
