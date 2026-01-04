# XML マッピング仕様（XML Mapping Specification）

本ドキュメントは、BT-DSL ソースコードを BehaviorTree.CPP (v4.x) 互換の XML 形式へコンパイルするための**変換仕様**を規定します。

---

## 1. 原則とアーキテクチャ (Principles and Architecture)

### 1.1 BT.CPP との対応関係

BT-DSL は BehaviorTree.CPP (BT.CPP) の DSL フロントエンドとして設計されています。

| BT-DSL 構文        | BT.CPP 概念                                 |
| :----------------- | :------------------------------------------ |
| `extern action`    | Action ノード                               |
| `extern condition` | Condition ノード                            |
| `extern control`   | Control ノード（Sequence, Fallback 等）     |
| `extern decorator` | Decorator ノード                            |
| `extern subtree`   | 外部定義 SubTree                            |
| `tree`             | SubTree 定義（`<BehaviorTree>` として出力） |
| `extern type`      | **DSL固有**（後述）                         |

> [!IMPORTANT]
> **`extern` 定義は BT.CPP ノードに直接対応します。**
> DSL の `extern action MoveTo(...)` は、BT.CPP に登録された `MoveTo` ノードをそのまま呼び出します。

> [!NOTE]
> **`extern type` は DSL 固有の概念です。** BT.CPP の Blackboard には型システムがなく、ポート間で型が完全一致すれば動作します。コンパイラは `extern type` を静的解析にのみ使用し、XML 出力時には型情報を特別扱いしません。

### 1.2 XML 出力モードの制限

本仕様では、BT.CPP 標準機能のみで動作する XML を生成するため、以下の**コンパイル時制限**を適用します。

| 制限                 | 説明                                                                          |
| :------------------- | :---------------------------------------------------------------------------- |
| 標準型のみ           | `int32`, `float64`, `string` のみ許可。他の数値型（`int8`, `int16` 等）は禁止 |
| 配列型禁止           | 固定長配列 `[T; N]`、動的配列 `vec<T>` は使用不可                             |
| `as` キャスト禁止    | 明示的型変換 `expr as T` は使用不可                                           |
| グローバル変数初期化 | `null` のみ許可。ホスト側ランタイムで初期化する                               |
| nullable 型          | 許可。`null` は Blackboard エントリの不在で表現                               |

> [!NOTE]
> これらの制限により、BT.CPP 標準ノードのみで動作します。カスタムノードは **`BlackboardExists`（存在チェック用 Condition）** の1つのみ必要です。

### 1.3 カスタム型の使用

`float32`, `int16` など制限対象の型を使用したい場合は、`extern type` でカスタム型として定義できます。

**DSL:**

```bt-dsl
extern type Float32;  // float32 相当のカスタム型
extern type Int16;    // int16 相当のカスタム型
```

> [!IMPORTANT]
> カスタム型間の暗黙変換・明示変換はサポートされません。型の一致は完全一致が必要です。

### 1.4 単一出力 (Single Output)

コンパイラは、エントリーポイントとなる `.bt` ファイルを出発点とし、そこから到達可能な定義を含んだ **単一の XML ファイル** を生成します。

### 1.5 出力 XML 構造

```xml
<?xml version="1.0" encoding="UTF-8"?>
<root BTCPP_format="4">
    <BehaviorTree ID="Main">
        <!-- ツリー本体 -->
    </BehaviorTree>

    <!-- 依存する SubTree 定義 -->
    <BehaviorTree ID="_ImportedTree_xyz">
        ...
    </BehaviorTree>

    <TreeNodesModel>
        <!-- ノードマニフェスト -->
    </TreeNodesModel>
</root>
```

### 1.6 Tree の命名と可視性 (Tree Naming)

XML 上の `<BehaviorTree ID="...">` はフラットな名前空間を持ちます。名前衝突を防ぐため、以下の規則を適用します。

1. **エントリーポイント**: メインファイル内の `tree` 定義は、DSL 識別子をそのまま XML ID として使用。
2. **内部依存（Imported）**: `import` で取り込まれた `tree` 定義は、`_` で始まる一意な名前にマングリング。
   - 例: `_SubTree_A1b2C`

---

## 2. ノード変換 (Node Translation)

### 2.1 基本規則

`extern` 定義されたノードは、対応する BT.CPP ノードへ直接変換されます。

**DSL:**

```bt-dsl
extern action MoveTo(in target: Point, out result: bool);
MoveTo(target: goal, result: out success);
```

**XML:**

```xml
<MoveTo target="{goal}" result="{success}" />
```

### 2.2 SubTree 呼び出し

`tree` 定義の呼び出しは `<SubTree>` ノードへ変換されます。

**DSL:**

```bt-dsl
tree Navigate(in goal: Point) { ... }

// 呼び出し
Navigate(goal: destination);
```

**XML:**

```xml
<SubTree ID="Navigate" goal="{destination}" />
```

### 2.3 Control/Decorator ノード

`extern control` / `extern decorator` は子ノードを持つ XML 要素として出力されます。

**DSL:**

```bt-dsl
extern control Sequence();
extern decorator Inverter();

Sequence {
    Inverter {
        CheckBattery();
    }
    MoveForward();
}
```

**XML:**

```xml
<Sequence>
    <Inverter>
        <CheckBattery />
    </Inverter>
    <MoveForward />
</Sequence>
```

### 2.4 暗黙のシーケンス (Implicit Sequence)

`tree` 定義のルート、または `decorator` の子が **2つ以上** の場合、コンパイラは自動的に `Sequence` でラップします。

**DSL:**

```bt-dsl
tree Main() {
    ActionA();
    ActionB();
}
```

**XML:**

```xml
<BehaviorTree ID="Main">
    <Sequence>  <!-- 自動挿入 -->
        <ActionA />
        <ActionB />
    </Sequence>
</BehaviorTree>
```

---

## 3. 引数と変数の変換 (Arguments and Variables)

### 3.1 ポート方向と値のマッピング

| ポート方向            | DSL 指定値     | XML 属性値               |
| :-------------------- | :------------- | :----------------------- |
| `in`                  | リテラル `10`  | `"10"`                   |
| `in`                  | 変数 `x`       | `"{x}"` または `"{x#1}"` |
| `in`                  | グローバル `g` | `"@{g}"`                 |
| `out` / `ref` / `mut` | 変数 `x`       | `"{x}"` または `"{x#1}"` |
| `out` / `ref` / `mut` | グローバル `g` | `"@{g}"`                 |

> [!NOTE]
> BT.CPP のポートは `in` / `out` の区別を厳密には行いません。DSL における `ref` は論理的に `in`（読み取り参照）、`mut` は論理的に `inout`（読み書き参照）に相当しますが、XML 出力上は同じ `{...}` / `@{...}` 形式になります。

### 3.2 省略された `out` 引数

`out` 引数が省略された場合、コンパイラはダミー変数 (`_discard_N`) を割り当てます。

### 3.3 スコープと名前マングリング

ブロックスコープ変数は、XML のフラット Blackboard 上で衝突しないようリネームされます。

- **ルール**: `{original_name}#{unique_id}`
- コンパイル時に静的解決され、ランタイム側でのスコープ解決は不要。

---

## 4. グローバル定義 (Global Definitions)

### 4.1 グローバル変数 (Global Blackboard)

DSL の `var g_param;` は、XML 上では **`@{g_param}`** 形式で参照されます。

- **初期化**: `null` のみ許可。XML に出力しない。ホスト側ランタイムの責務。

### 4.2 定数 (Constants)

DSL の `const MAX = 10;`（グローバル・ローカル問わず）はコンパイル時定数として、使用箇所で**リテラル値にインライン展開**されます。XML に定数定義は出力されません。

---

## 5. 事前条件 (Preconditions)

DSL の事前条件構文は BT.CPP の Precondition 属性にマップされます。

| DSL 構文            | XML 属性                |
| :------------------ | :---------------------- |
| `@success_if(expr)` | `_successIf="expr_str"` |
| `@failure_if(expr)` | `_failureIf="expr_str"` |
| `@skip_if(expr)`    | `_skipIf="expr_str"`    |
| `@run_while(expr)`  | `_while="expr_str"`     |
| `@guard(expr)`      | （複合変換、後述）      |

**DSL:**

```bt-dsl
@skip_if(battery < 20)
Navigate(goal);
```

**XML:**

```xml
<Navigate goal="{goal}" _skipIf="{battery} < 20" />
```

> [!NOTE]
> 式 `expr_str` は変数参照が解決された文字列になります（`{x#1} > 10` 等）。

### 5.1 `@guard` の複合変換

`@guard(expr)` は BT.CPP の標準属性に直接対応するものがないため、**複合変換**によって同等の挙動を実現します。

**DSL:**

```bt-dsl
@guard(sensor_ok)
DoWork();
```

**XML:**

```xml
<Sequence>
    <DoWork _while="{sensor_ok}" />
    <AlwaysSuccess _failureIf="!({sensor_ok})" />
</Sequence>
```

**変換規則:**

1. 対象ノードに `_while="expr"` 属性を付与（条件が偽になった時点で中断）
2. 直後に `AlwaysSuccess` ノードを配置し、`_failureIf="!(expr)"` を付与
3. 全体を `Sequence` でラップ

この変換により、条件が偽になった場合は `AlwaysSuccess` が `Failure` を返し、全体として `Failure` となります。

---

## 6. 式と代入 (Expressions and Assignments)

### 6.1 BT.CPP Script ノードの使用

代入文や変数宣言など、直接ノード呼び出しで表現できない処理は **BT.CPP 標準の `<Script>` ノード**を使用します。

**DSL:**

```bt-dsl
var x: int32 = 10;
x = x + 5;
x += 3;
```

**XML:**

```xml
<Script code="x#1 := 10" />
<Script code="x#1 = x#1 + 5" />
<Script code="x#1 = x#1 + 3" />
```

> [!NOTE]
> BT.CPP Script では `:=` は新規エントリ作成、`=` は既存エントリへの代入です。変数宣言時は `:=` を使用します。

> [!IMPORTANT]
> **複合代入演算子**（`+=`, `-=`, `*=`, `/=`）は、コンパイラによって**単純な代入に展開**されます。例: `x += 3` → `x = x + 3`

### 6.2 Script の演算子

BT.CPP Script は以下の演算子をサポートします：

| カテゴリ | 演算子                           |
| :------- | :------------------------------- |
| 算術     | `+`, `-`, `*`, `/`, `%`          |
| 複合代入 | `+=`, `-=`, `*=`, `/=`           |
| 比較     | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| 論理     | `&&`, `\|\|`, `!`                |
| ビット   | `&`, `\|`, `^`                   |
| 三項     | `cond ? a : b`                   |

### 6.3 前処理を伴うノード呼び出し

以下のケースでは、ノード呼び出しの前に `<Script>` で前処理を行い、全体を `<Sequence>` でラップします。

#### 6.3.1 デフォルト引数省略時

`extern` 定義でデフォルト引数が指定されている場合、呼び出し時に引数を省略すると、コンパイラはデフォルト値を一時変数に格納してからノードを呼び出します。

**DSL:**

```bt-dsl
extern action Foo(in x: int32 = 10);
Foo();  // x を省略
```

**XML:**

```xml
<Sequence>
    <Script code="_default#1 := 10" />
    <Foo x="{_default#1}" />
</Sequence>
```

#### 6.3.2 `out var x` インライン宣言

`out var x` 構文で変数をインライン宣言する場合、コンパイラは事前に変数を宣言します。

**DSL:**

```bt-dsl
DoWork(result: out var x);
```

**XML:**

```xml
<Sequence>
    <Script code="x#1 := 0" />
    <DoWork result="{x#1}" />
</Sequence>
```

> [!NOTE]
> 初期値は型に応じたデフォルト値（`int32` → `0`, `float64` → `0.0`, `string` → `''`）を使用します。

#### 6.3.3 in ポートへの右辺値（式）渡し

`in` ポートに式を渡す場合、式を事前に評価して一時変数に格納します。

**DSL:**

```bt-dsl
MoveTo(target: start + offset);
```

**XML:**

```xml
<Sequence>
    <Script code="_expr#1 := {start#1} + {offset#2}" />
    <MoveTo target="{_expr#1}" />
</Sequence>
```

> [!NOTE]
> 複数の前処理が必要な場合（例: 複数の引数で式評価が必要）、すべての `<Script>` を同一の `<Sequence>` 内に順番に配置します。

---

## 7. Nullable 型と存在チェック (Nullable Types)

### 7.1 null の表現

DSL の nullable 型 (`T?`) における `null` 値は、**Blackboard エントリの不在**として表現されます。

**DSL:**

```bt-dsl
var maybeValue: int32?;  // 初期状態は null
```

**XML:**
（エントリを作成しない = Blackboard に存在しない状態）

### 7.2 null 代入

変数に `null` を代入する場合、BT.CPP 標準の `UnsetBlackboard` ノードを使用して Blackboard エントリを削除します。

**DSL:**

```bt-dsl
var maybeValue: int32? = 10;
maybeValue = null;  // null に戻す
```

**XML:**

```xml
<Script code="maybeValue#1 := 10" />
<UnsetBlackboard key="maybeValue#1" />
```

### 7.3 BlackboardExists ノード

nullable 型の存在チェックには、カスタム Condition ノード `BlackboardExists` を使用します。

**DSL:**

```bt-dsl
@guard(maybeValue != null)
Process(value: maybeValue!);
```

**XML:**

```xml
<Sequence>
    <BlackboardExists key="maybeValue#1" _while="true" />
    <Process value="{maybeValue#1}" _while="true" />
    <AlwaysSuccess _failureIf="!(true)" />
</Sequence>
```

> [!NOTE]
> `BlackboardExists` は、指定されたキーが Blackboard に存在すれば `Success`、存在しなければ `Failure` を返す Condition ノードです。

### 7.4 BlackboardExists ノード仕様

```xml
<BlackboardExists key="variable_name" />
```

| 属性  | 必須 | 説明                           |
| :---- | :--: | :----------------------------- |
| `key` |  ✓   | チェックする Blackboard キー名 |

**戻り値:**

- `Success`: キーが存在する
- `Failure`: キーが存在しない

**実装例（C++）:**

```cpp
class BlackboardExists : public ConditionNode {
public:
    static PortsList providedPorts() {
        return { InputPort<std::string>("key") };
    }

    NodeStatus tick() override {
        auto key = getInput<std::string>("key").value();
        return config().blackboard->getEntry(key)
            ? NodeStatus::SUCCESS
            : NodeStatus::FAILURE;
    }
};
```

### 7.5 複合条件での null チェック

null チェックと他の条件が `&&` で結合されている場合、短絡評価をシミュレートする変換が必要です。

> [!NOTE]
> BT.CPP Script では存在しないキーにアクセスすると例外が発生するため、存在チェックを先に行う必要があります。

**DSL:**

```bt-dsl
@skip_if(x != null && x > 10)
DoWork();
```

**変換パターン:**

1. ヘルパー変数を初期化（デフォルト: スキップしない）
2. `ForceSuccess` 内で存在チェックと条件評価を行う
3. ヘルパー変数を使って最終的なスキップ判定

**XML:**

```xml
<Sequence>
    <!-- 初期値: スキップしない -->
    <Script code="_should_skip#1 := false" />
    <!-- x が存在する場合のみ条件を評価 -->
    <ForceSuccess>
        <Sequence>
            <BlackboardExists key="x#1" />
            <Script code="_should_skip#1 := ({x#1} > 10)" />
        </Sequence>
    </ForceSuccess>
    <!-- ヘルパー変数でスキップ判定 -->
    <DoWork _skipIf="{_should_skip#1}" />
</Sequence>
```

**動作:**

| 条件             | `_should_skip` の値     | 結果            |
| :--------------- | :---------------------- | :-------------- |
| x が存在しない   | `false`（初期値のまま） | DoWork 実行     |
| x が存在、x > 10 | `true`                  | DoWork スキップ |
| x が存在、x ≤ 10 | `false`                 | DoWork 実行     |

#### `x == null || expr` パターン

`||` で結合された null チェックも同様に変換できます。

**DSL:**

```bt-dsl
@skip_if(x == null || x < 0)
DoWork();
```

**XML:**

```xml
<Sequence>
    <!-- 初期値: x が存在しない場合はスキップ -->
    <Script code="_should_skip#1 := true" />
    <!-- x が存在する場合のみ条件を評価 -->
    <ForceSuccess>
        <Sequence>
            <BlackboardExists key="x#1" />
            <Script code="_should_skip#1 := ({x#1} < 0)" />
        </Sequence>
    </ForceSuccess>
    <DoWork _skipIf="{_should_skip#1}" />
</Sequence>
```

---

## 8. 型のシリアライズ (Type Serialization)

| 型            | シリアライズ形式                         |
| :------------ | :--------------------------------------- |
| `bool`        | `"true"`, `"false"`                      |
| `string`      | エスケープ済み文字列（シングルクォート） |
| `int32`       | 整数リテラルをそのまま出力               |
| `float64`     | 浮動小数点リテラルをそのまま出力         |
| `extern type` | 変換不要（BT.CPP は型情報を使用しない）  |

---

## 9. TreeNodesModel 生成 (Manifest)

`<root>` 直下に `<TreeNodesModel>` を生成し、使用されるノードの定義を宣言します。

**DSL:**

```bt-dsl
extern action MoveTo(in target: Point, out success: bool);
extern condition IsBatteryOk();
```

**XML:**

```xml
<TreeNodesModel>
    <Action ID="MoveTo">
        <input_port name="target" type="Point" />
        <output_port name="success" type="bool" />
    </Action>
    <Condition ID="IsBatteryOk" />
    <Condition ID="BlackboardExists">
        <input_port name="key" type="string" />
    </Condition>
</TreeNodesModel>
```

> [!NOTE]
> `BlackboardExists` は自動的にマニフェストに追加されます。

---

## 10. 変換規則サマリー

| DSL 構文                    | XML 出力                                       |
| :-------------------------- | :--------------------------------------------- |
| `extern action Foo(...)`    | `<Foo ... />`                                  |
| `extern control Bar(...)`   | `<Bar>...</Bar>`                               |
| `extern decorator Baz(...)` | `<Baz>...</Baz>`                               |
| `tree MyTree(...)` 定義     | `<BehaviorTree ID="MyTree">...</BehaviorTree>` |
| `MyTree(...)` 呼び出し      | `<SubTree ID="MyTree" ... />`                  |
| `var x = 10;`               | `<Script code="x#1 := 10" />`                  |
| `x = expr;`                 | `<Script code="x#1 = expr" />`                 |
| `@skip_if(cond)`            | `_skipIf="..."` 属性                           |
| `@guard(cond)`              | 複合変換（5.1 参照）                           |
| グローバル変数 `g`          | `@{g}`                                         |
| ローカル変数 `x`            | `{x}` または `{x#id}`                          |
| デフォルト引数省略          | `<Sequence>` + `<Script>` + Node（6.3.1 参照） |
| `out var x` 構文            | `<Sequence>` + `<Script>` + Node（6.3.2 参照） |
| in ポートへの式渡し         | `<Sequence>` + `<Script>` + Node（6.3.3 参照） |
| nullable 存在チェック       | `<BlackboardExists key="..." />`（7.3 参照）   |

---

## 11. XML に出力されない要素 (Elements Not Output)

以下の DSL 要素は静的解析にのみ使用され、XML に出力されません。

| DSL 要素                                    | 説明                                          |
| :------------------------------------------ | :-------------------------------------------- |
| ドキュメンテーションコメント (`///`, `//!`) | メタ情報として使用。XML には含まれない        |
| `#[behavior(DataPolicy, FlowPolicy)]`       | 静的解析（初期化安全性）にのみ使用            |
| `extern type`                               | DSL 固有の型宣言。BT.CPP は型情報を使用しない |
| `const` 定義（グローバル・ローカル）        | リテラル値にインライン展開される              |
| `type` エイリアス                           | 透過的に展開され、XML には影響しない          |
