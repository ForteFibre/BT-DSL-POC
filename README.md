# BT-DSL

BehaviorTree.CPP v4向けの独自DSL（Domain-Specific Language）。XMLの可読性と保守性を改善し、コンパイル時エラー検出を提供します。

## 特徴

- 🔍 **コンパイル時検証**: 変数のタイポ、型不一致、ref権限エラーをコンパイル時に検出
- 📝 **可読性向上**: XMLより簡潔で、IDEのシンタックスハイライト対応
- 🔗 **シンボル解決**: Blackboard変数とTree引数の自動解決
- 📖 **ドキュメントコメント**: `///`コメントがXMLの`_description`属性へ出力

## インストール

```bash
cd bt-dsl
npm install
npm run langium:generate
npm run build
```

## 使用方法

### コンパイル

```bash
# XMLへコンパイル
node out/cli/index.js generate <file.bt> -o <output.xml>

# 検証のみ
node out/cli/index.js validate <file.bt>
```

### 例

```
//! Soldier AI Definition v1.0

var TargetPos: Vector3
var Ammo: Int

/// メインツリー
Tree Main() {
    @Loop
    Sequence {
        AttackEnemy(target: TargetPos, ammo: ref Ammo)
        ForceResult(result: "SUCCESS")
    }
}
```

出力XML:

```xml
<BehaviorTree ID="Main">
    <Loop>
        <Sequence>
            <AttackEnemy target="{TargetPos}" ammo="{Ammo}" />
            <ForceResult result="SUCCESS" />
        </Sequence>
    </Loop>
</BehaviorTree>
```

## 文法

### 型

- リテラル: `"string"`, `42`, `3.14`, `true`/`false`
- 変数参照: `varName` (読み取り), `ref varName` (書き込み)

## 開発

```bash
# Lint
npm run lint
npm run lint:fix

# テスト
npm test
npm run test:watch

# ビルド
npm run build
```

## ライセンス

MIT
