# コンパイラ仕様 (Compiler Specification)

本ドキュメントでは、本プロジェクトにおいて開発される **BT-DSL コンパイラ (`btc`)** の具体的な仕様を定義します。

---

## 1. デザインゴール (Design Goals)

1.  **Zero Configuration Start**: 単一ファイルであれば設定なしで即座にコンパイル可能であること。
2.  **Project-based**: 大規模な BehaviorTree 開発を想定し、プロジェクト設定ファイルによる依存関係管理とビルド設定をサポートすること。
3.  **Developer Experience (DX)**: 高速なビルド、明確なエラーメッセージ、LSP (Language Server Protocol) との統合を重視すること。
4.  **XML-Only Output**: コンパイラの責務を「XML生成」に限定し、ホスト(C++)側との結合を疎に保つこと。

---

## 2. プロジェクト構成 (Project Configuration)

コンパイラは、プロジェクトルートにある設定ファイル `btc.yaml` を認識します。

### 設定ファイル: `btc.yaml`

```yaml
package:
  name: 'my_behavior_tree'
  version: '0.1.0'

compiler:
  entry_points: ['./src/main.bt', './src/recovery.bt']
  output_dir: './generated'
  target: 'btcpp_v4' # ターゲット環境

dependencies:
  - path: '../common_bt_library' # ローカルパス依存
  - ros_package: 'nav2_bt_navigator' # ROS パッケージ依存
```

### 依存関係の種類

| キー          | 説明                                                                                                                               |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `path`        | ローカルファイルシステム上のパスを指定します。相対パスは `btc.yaml` からの相対として解決されます。                                 |
| `ros_package` | ROS パッケージ名を指定します。コンパイラは `ament_index` (ROS 2) または `rospack` (ROS 1) を使用してパッケージのパスを解決します。 |

> [!NOTE]
> `ros_package` を使用する場合、コンパイラは ROS 環境がソースされていることを前提とします。
> パッケージが見つからない場合はコンパイルエラーとなります。

---

### ターゲット設定

`compiler.target` により、コード生成時のターゲット環境を指定します。指定されたターゲットでサポートされない言語機能を使用した場合、コンパイルエラーとなります。

| ターゲット        | 説明                                                                               | カスタムノード依存                                |
| ----------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------- |
| `btcpp_v4`        | BehaviorTree.CPP v4 向け。（デフォルト）                                           | Blackboard の `null` 判定用カスタムノードのみ必要 |
| `btcpp_v4_strict` | BehaviorTree.CPP v4 向け厳格モード。標準ノードのみで構成可能なコードを強制します。 | なし                                              |

#### 機能制限の詳細

| 言語機能                        | `btcpp_v4` | `btcpp_v4_strict` |
| ------------------------------- | :--------: | :---------------: |
| Blackboard の `null` 代入・比較 |     ✓      |         ✗         |
| 標準的な式・制御構造            |     ✓      |         ✓         |

> [!IMPORTANT]
> 現時点では、全ての BT-DSL 言語機能をサポートするターゲットは存在しません。
> 将来的なバージョンで拡張される可能性があります。

> [!TIP]
> `btcpp_v4_strict` は、BT-DSL ランタイムライブラリに一切依存せず、純粋な BehaviorTree.CPP のみで動作させたい場合に有用です。

## 3. コマンドラインインターフェース (CLI)

コマンド名は `btc` (BehaviorTree Compiler) とします。

### サブコマンド

#### `btc build`

プロジェクトまたはファイルをビルドし、アーティファクトを生成します。

```bash
# カレントディレクトリの btc.yaml を読み込んでビルド
$ btc build

# 特定のファイルをビルド（設定ファイル無視/未指定時）
$ btc build src/main.bt -o ./output

# 監視モード
$ btc build --watch
```

#### `btc check`

コード生成を行わず、構文チェックと静的解析のみを実行します。CI/CD パイプラインでの利用を想定します。

```bash
$ btc check
```

#### `btc init`

新しい BT-DSL プロジェクトを初期化し、`btc.yaml` とディレクトリ構造を生成します。

```bash
$ btc init my_new_project
```

---

## 4. ビルドプロセス (Build Process)

ビルドは以下のパイプラインで実行されます。

1.  **Load & Parse**:
    - エントリポイント（`btc.yaml` または引数指定）からパースを開始。
    - `import` 文を検出し、依存ファイルを再帰的に探索・パース。
    - **キャッシング**: 変更のないファイルはパースをスキップ（インクリメンタルビルド）。
2.  **Resolve & Validate**:
    - シンボル解決：全ファイルに渡る識別子のリンク。
    - **型チェック**: `expression-typing.md` に基づく厳格な型検証。
    - **安全性検証**: `static-analysis-and-safety.md` に基づく循環参照や無限ループの検出。
    - エラーがある場合、ここでビルドを停止し、詳細な診断メッセージを表示。
3.  **Generate**:
    - XML形式のコード生成。

---

## 5. 生成アーティファクト仕様 (Artifacts)

### 5.1 XML Output (BehaviorTree.CPP v4)

実行時にロードされるBehaviorTree定義ファイルです。詳細は [XML マッピング仕様](./xml-mapping.md) を参照してください。

- **単一出力**: 各エントリーポイントにつき、依存関係を結合した単一の XML ファイルを生成します。
- **Source Map**: デバッグ情報の埋め込みをサポートします。

---

## 6. エラーメッセージ仕様

開発者が即座に修正できるよう、コンパイラは以下の情報を含むリッチなエラーメッセージを出力しなければなりません。

- **File Path & Location**: `src/behavior.bt:15:4`
- **Error Code**: `E1024` (ドキュメント検索用の一意なID)
- **Code Snippet**: エラー箇所のソースコード行と、問題のあるトークンを指すアンダーライン。
- **Hint/Suggestion**: 「もしかして `RunAction` ですか？」のような修正提案。

```text
Error: undefined variable 'target_pos'
   --> src/main.bt:12:15
    |
 12 |     Output(target_pos)
    |             ^^^^^^^^^^
    |
    = hint: declared variable is 'target_point'
```
