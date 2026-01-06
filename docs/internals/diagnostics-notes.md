# 診断仕様 — 補足情報

本ドキュメントは [診断仕様（言語仕様）](../reference/diagnostics.md) の補足情報です。

---

## 設計根拠

### 初期化安全性の目的

BT-DSL の初期化安全性は、未初期化変数へのアクセスをコンパイル時に検出することを目的としています。
これにより、実行時の未定義動作やエラーを防ぎます。

### DataPolicy の設計

| Policy | 意味 | 典型的なノード |
| :----- | :--- | :------------- |
| `All`  | すべての子が成功している | `Sequence` |
| `Any`  | いずれかの子が成功 | `Fallback` |
| `None` | 子の状態は保証されない | `ForceSuccess` |

### Warning（過剰権限）の意図

Warning は「必要以上に強い参照」を意味します。
例: `ref` で十分な場所に `mut` を使用している場合。
これは機能的には動作しますが、意図を明確にするため警告を発します。

---

## コード例: 初期化安全性

### DataPolicy: All（和集合）

```bt-dsl
Sequence {
  TaskA(result: out x);
  TaskB(result: out y);
}
// -> Sequence が Success で抜けたなら x, y は Init
```

### DataPolicy: Any（共通部分）

```bt-dsl
Fallback {
  TaskA(result: out x);
  TaskB(result1: out x, result2: out y);
}
// -> Fallback が Success で抜けたなら x は Init
// -> y は Uninit の可能性がある（共通 out ではない）
```

```bt-dsl
Fallback {
  TaskA(result: out x);
  TaskB(result: out y);
}
// -> x, y いずれも Init とはみなされない（共通の out がない）
```

### DataPolicy: None（空集合）

```bt-dsl
ForceSuccess {
  TaskA(result: out x);
}
// -> ForceSuccess が Success でも x は Init と保証できない
```

### FlowPolicy: Chained

```bt-dsl
Sequence {
  Calculate(value: out result);
  Use(value: in result);   // OK: result は Init
}
```

### FlowPolicy: Isolated

```bt-dsl
ParallelAll {
  Calculate(value: out result);
  Use(value: in result);   // Error: result は ParallelAll 開始時点では Uninit
}
```

---

## コード例: Null 安全性

### Flow-Sensitive Typing

```bt-dsl
var target: Pose? = null;

Fallback {
  @guard(target != null)
  Sequence {
    // このブロック内では target: Pose として扱える
    MoveTo(goal: target);
  }
}
```

### 連言での絞り込み

```bt-dsl
@guard(a != null && b != null)
UseBoth(a, b); // a: T, b: U（ブロック内で非 Nullable）
```

### Nullable 変数の out 接続

```bt-dsl
extern action FindTarget(out result: Pose);

tree Main() {
  var target = null;  // Pose?

  Sequence {
    FindTarget(result: out target);
    // Success 後: target は非 null

    @guard(target != null)
    Use(value: target);
  }
}
```

---

## コード例: 設計パターン

### 失敗時にも値が必要な場合

失敗時にもエラーコード等が必要な場合は、事前初期化により `Init` を確保します。

```bt-dsl
extern action TryConnect(out error_code: int32);

tree Main() {
  var code: int32 = 0;      // 事前に Init
  TryConnect(error_code: out code);
  Log(message: code);       // OK: code は常に Init
}
```

---

## ポート方向と引数制約

### tree パラメータの権限

| パラメータ方向     | `ref` を付与 | `mut`/`out` を付与 |
| :----------------- | :----------- | :----------------- |
| `in`（または省略） | 許可         | **エラー**         |
| `ref`              | 許可         | **エラー**         |
| `mut`              | 許可         | 許可               |
| `out`              | **エラー**   | 許可               |

### 引数の省略規則

| ポート方向         | デフォルト値あり | デフォルト値なし |
| :----------------- | :--------------- | :--------------- |
| `in`（または省略） | 省略可能         | 省略不可         |
| `ref`              | （設定不可）     | **省略不可**     |
| `mut`              | （設定不可）     | **省略不可**     |
| `out`              | （設定不可）     | **常に省略可能** |

`out` 引数が省略された場合、その出力は未接続として扱われ、評価結果は破棄されます。

### デフォルト値の制約

- `ref` / `mut` / `out` ポートにはデフォルト値を指定できません
- デフォルト値は `const_expr` でなければなりません
