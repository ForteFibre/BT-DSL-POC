# 診断ガイド

本ドキュメントは、BT-DSL コンパイラが報告する診断（警告・情報）について説明します。

---

## 1. 警告

### 1.1 未使用の `out` パラメータ

`out` 方向として宣言された tree パラメータが一度も書き込みに使用されない場合、警告を報告します。

```bt-dsl
tree Example(out result: int32) {  // 警告: result は未使用
  root DoSomething();
}
```

### 1.2 引数方向の昇格

`in` ポートに対して `inout` 方向の引数を渡した場合、警告を報告します。書き込み意図があるにもかかわらず、ポートは読み取り専用です。

```bt-dsl
extern action ReadOnly(in value: int32);

tree Example() {
  var x: int32 = 0;
  root ReadOnly(value: inout x);  // 警告: in ポートに inout を指定
}
```

---

## 2. データフロー診断

以下の診断は [データフロー安全性](./data-flow-safety.md) の実装に基づきます。

### 2.1 Unset 変数の読み取り

読み取り権限を持つポート（`in` / `inout`）に Unset 状態の変数を渡した場合、警告またはエラーを報告します。

```bt-dsl
tree Example() {
  var x: int32;  // Unset
  root ReadValue(value: x);  // 警告/エラー: x は未初期化
}
```

### 2.2 Maybe 変数の読み取り

条件分岐等により未設定の可能性がある変数を読み取った場合、警告を報告します。

```bt-dsl
tree Example() {
  var x: int32;
  root Sequence {
    @success_if(some_condition)
    SetValue(result: out x);

    UseValue(value: x);  // 警告: x は未設定の可能性あり
  }
}
```

---

## 3. リテラル検査

### 3.1 範囲外リテラル

型が決定した時点で、整数リテラルがその型の表現可能な範囲を超える場合、警告またはエラーを報告します。

```bt-dsl
var x: int8 = 256;  // 警告/エラー: int8 の範囲外
var y: uint32 = -1;  // 警告/エラー: 符号なし型に負値
```

---

## 4. 実装上の注意

- 1.x の警告は基本的な実装として含めることを想定
- 2.x の診断は [データフロー安全性](./data-flow-safety.md) の実装が前提
- 診断の重大度（警告/エラー）は処理系のオプションで制御可能にできる
