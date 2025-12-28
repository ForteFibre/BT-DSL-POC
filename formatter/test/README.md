# Formatter Tests

このディレクトリには、BT DSL formatterのテストファイルが含まれています。

## テストファイル

すべてのテストはTypeScriptで記述されています：

- `test-format.ts` - soldier-ai.btのフォーマットテスト
- `test-standard-nodes.ts` - StandardNodes.btのフォーマットテスト
- `test-params.ts` - パラメータdirection keywordsのテスト
- `test-comment-preservation.ts` - コメント保持機能のテスト

## 実行方法

```bash
# rootディレクトリから実行
npm run build
npm test
```

## 期待される結果

すべてのテストは正常にフォーマットされた出力を表示し、エラーなく終了します。特に：

- コメントが元の位置に保持される
- Direction keywords (`ref`, `in`, `out`) が保持される
- 空行が適切に保持される
- インデントとスペースが正しく調整される
