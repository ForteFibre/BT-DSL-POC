# 意味論 — 補足情報

本ドキュメントは [意味論（言語仕様）](../reference/semantics.md) の補足情報です。

---

## 設計根拠

### 名前空間の分離

同名でも名前空間が異なれば共存できます（例: `extern type A;` と `tree A()`）。
これにより、型名とノード名の命名が独立し、既存ライブラリとの統合が容易になります。

### シャドウイング禁止

シャドウイング禁止は「入れ子（祖先関係）にあるスコープ間で同名を許さない」ことを意味します。
これにより、変数の参照先が曖昧になることを防ぎます。

ただし、互いに祖先関係にない別スコープ（例: 異なる `children_block`）で同名を用いることは許容されます。

### Blackboard のモデル

Blackboard 名の一意性は要求されません。
これは BehaviorTree.CPP の blackboard モデルに整合させる設計です。

### Blackboard の可視性とライフタイム

**可視性（名前解決）はブロックに従い、寿命（保存される値）はツリーに従います。**

- `children_block` を抜けるとその `var` 名は参照できない
- 対応する Blackboard エントリの値は Tree の寿命の間保持される

### Skip ステータスの導入

`Skip` の導入は BehaviorTree.CPP v4 のスキップ概念に整合します。
典型的には親（control/decorator）は `Skip` を受け取った場合、
`Success` と同様に「次の子へ進む」等の処理を行います。

---

## 実装定義事項

| 事項 | 説明 |
| :--- | :--- |
| パッケージ形式の import 解決 | 検索パス、パッケージ名解決は実装定義 |
| 事前条件の評価順序 | 複数の事前条件が指定された場合の順序は実装定義 |
| ポート方向の最適化 | 実メモリのコピー有無は最適化により変更可能 |
| 実行時評価の詳細 | 評価順序、オーバーフロー/丸め等は実装定義 |

---

## コード例

### import 曖昧性

```bt-dsl
// file_a.bt
extern type Pose;

// file_b.bt  
extern type Pose;

// main.bt
import "./file_a.bt"
import "./file_b.bt"

tree Main() {
  var p: Pose;  // エラー: Pose が曖昧
}
```

### 暗黙の構造: Decorator の複数子

`decorator` ノードの `children_block` に 2 つ以上の子が書かれた場合、
暗黙に `Sequence` でラップされます。

```bt-dsl
// 記述
Retry(n: 3) {
  TaskA();
  TaskB();
}

// 等価な振る舞い
Retry(n: 3) {
  Sequence {
    TaskA();
    TaskB();
  }
}
```

### 暗黙の構造: Blackboard 操作

`var x = expr;` および `x = expr;` は、
「Blackboard 書き込みを行い常に `Success` を返すアクション」と等価です。

```bt-dsl
Sequence {
  var count = 0;     // Success を返す
  count = count + 1; // Success を返す
  DoWork();
}
```

特性:
- 返り値は常に `Success`
- 実行順序は `children_block` 内での記述順
- 実行された時点で Blackboard に値が書き込まれる

---

## 処理系実装者向け

### @guard の実装

`@guard(cond)` は BT.CPP の標準属性に直接対応するものがありません。
複合変換によって同等の挙動を実現します。

詳細は [XMLマッピング仕様](./xml-mapping.md) を参照してください。

### 事前条件のスキップ

事前条件によりノード本体の実行がスキップされた場合:
- `out` への書き込みは発生しない
- そのノードの副作用は一切発生しない
