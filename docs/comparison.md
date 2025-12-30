# XML vs BT-DSL 比較

BehaviorTree.CPP v4 の XML 形式と BT-DSL の構文を比較します。

この例は [Nav2](https://docs.nav2.org/) の `navigate_through_poses_w_replanning_and_recovery`
ツリーを題材にしています。

---

## 概要

このビヘイビアツリーは以下の機能を持ちます：

- 経由点を通るグローバルパスを 0.333Hz で再計画
- プランニング / コントロールのリカバリアクション
- 全般的なシステム問題に対するリカバリ（スピン、待機、バックアップ）

---

## XML 形式（BehaviorTree.CPP v4）

<<< @/../shared/examples/navigate_through_poses_w_replanning_and_recovery.xml

---

## BT-DSL 形式

<<< @/../shared/examples/navigate_through_poses_w_replanning_and_recovery.bt

---
