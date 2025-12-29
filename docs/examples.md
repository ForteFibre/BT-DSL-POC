# Examples（実践例）

ここではサンプルを題材に、BT-DSLの典型的な使い方（グローバル黒板、SubTree、ポート方向、Control/Decoratorの組み合わせ）を説明します。

---

## 1. サンプル全体

（ここでは要点に絞って紹介します。）

ポイント:

- `import` により標準ノード定義（標準ライブラリ）を導入
- グローバル黒板変数:
  - `TargetPos: Vector3`, `Ammo: int`, `IsAlerted: bool`
- メインループは `@[Repeat]` + `Sequence { ... }`

---

## 2. グローバル変数（黒板）

```bt-dsl
var TargetPos: Vector3
var Ammo: int
var IsAlerted: bool
```

- グローバル変数は全 tree から参照可能。

---

## 3. tree を SubTree として呼び出す

`SearchAndDestroy` は `tree SearchAndDestroy(ref target, ref ammo, ref alert) { ... }`
として定義され、別 tree からはノード呼び出しとして使います。

```bt-dsl
SearchAndDestroy(
  target: ref TargetPos,
  ammo: ref Ammo,
  alert: ref IsAlerted
)
```

- `ref` を付けることで「書き込み意図」を明示。
- 参照先が `tree` パラメータではなくグローバル変数であっても、`ref/out` 指定自体は構文上可能。
- ただし、宣言側ポートが `in` の場合に `ref`
  を付けると警告（入力専用ポートに対して書き込み意図を示しているため）。

---

## 4. out ポートへの受け渡し

サンプル内には以下があります:

```bt-dsl
FindEnemy(pos: out target, found: out alert)
```

- `FindEnemy` の宣言（例）は `pos` / `found` が `out` として定義される想定。
- `out target` のように、引数側で `out` を付ける必要があります。

---

## 5. Control ノード + children ブロック

```bt-dsl
Fallback {
  AlwaysFailure()
  AlwaysSuccess()
}
```

- `Fallback` は Control なので `{ ... }` が必須。
- `AlwaysFailure()` / `AlwaysSuccess()` は引数なしの Action ノード（子ブロックを持たない）。

---

## 6. デコレータの適用

```bt-dsl
@[Repeat(num_cycles: 3)]
Sequence {
  DoSomething()
}

// 複数デコレータの適用
@[Timeout(duration: 5.0), Retry(max_attempts: 3)]
TryConnect()

// 引数なしデコレータ
@[ForceSuccess]
MayFail()
```

- `@[...]` でデコレータを適用
- 複数デコレータはカンマ区切りで指定
