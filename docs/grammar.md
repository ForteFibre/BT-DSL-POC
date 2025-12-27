# BT-DSL Grammar Reference

完全な文法リファレンス。

## プログラム構造

```
program
    : inner_doc* import_stmt* blackboard_decl? tree_def+
```

### インポート

外部ファイルのインポート:

```
import "path/to/nodes.bt"
```

### Blackboard宣言

グローバル変数の定義（型指定必須）:

```
Blackboard {
    Position: Vector3
    Health: Int
    IsActive: Bool
}
```

## ツリー定義

```
Tree TreeName(param1, ref param2: Type) {
    // ノード定義
}
```

### パラメータ

| 定義             | 権限              | XML出力                   |
| ---------------- | ----------------- | ------------------------- |
| `name`           | 読み取り専用      | `<InputPort>`             |
| `name: Type`     | 読み取り専用 + 型 | `<InputPort type="Type">` |
| `ref name`       | 読み書き          | `<InOutPort>`             |
| `ref name: Type` | 読み書き + 型     | `<InOutPort type="Type">` |

## ノード

### 基本構文

```
NodeName(arg1: value1, arg2: value2) {
    Child1()
    Child2()
}
```

### デコレータ

ノードの前に`@`で指定:

```
@Repeat(times: 5)
@Inverter
Sequence {
    Action1()
    Action2()
}
```

生成XML:

```xml
<Inverter>
    <Repeat times="5">
        <Sequence>
            <Action1 />
            <Action2 />
        </Sequence>
    </Repeat>
</Inverter>
```

## 値

### リテラル

```
text: "string value"     // 文字列
count: 42                // 整数
rate: 3.14               // 浮動小数点
enabled: true            // ブール値
```

### 変数参照

```
pos: MyVariable          // 読み取り（Blackboard/引数から解決）
target: ref MyVariable   // 書き込み権限付き
```

## ドキュメントコメント

### 外部ドキュメント (`///`)

ツリーやノードの説明:

```
/// このツリーは敵を検索します
Tree SearchEnemy() {
    /// 検索アクション
    Search()
}
```

出力:

```xml
<BehaviorTree ID="SearchEnemy">
    <Metadata>
        <item key="description" value="このツリーは敵を検索します"/>
    </Metadata>
    <Search _description="検索アクション" />
</BehaviorTree>
```

### 内部ドキュメント (`//!`)

ファイル全体の説明（トップレベルに配置）:

```
//! AI System v1.0
//! Author: ...
```

## 検証ルール

### エラー

- 未定義変数の参照
- 重複したTreeName
- 重複したパラメータ名
- `ref`なしパラメータへの`ref`付き使用

### 警告

- `ref`ありパラメータの`ref`なし使用（読み取りのみ）
