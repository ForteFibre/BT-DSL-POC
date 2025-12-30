# 文法リファレンス

本ドキュメントは BT-DSL の**文法**を厳密に定義します。

---

## 1. 記法

本ドキュメントでは [ISO/IEC 14977 EBNF](https://www.w3.org/TR/REC-xml/#sec-notation)
に準拠した記法を使用します。

| 記法      | 意味                       |
| :-------- | :------------------------- |
| `=`       | 定義                       |
| `;`       | 定義の終了                 |
| `\|`      | 選択（OR）                 |
| `( )`     | グループ化                 |
| `[ ]`     | オプション（0回または1回） |
| `{ }`     | 繰り返し（0回以上）        |
| `"..."`   | 終端記号（リテラル）       |
| `'...'`   | 終端記号（リテラル）       |
| `/regex/` | 正規表現パターン           |
| `(*...*)` | コメント                   |

---

## 2. 字句規則（Lexical Grammar）

### 2.1 空白

```ebnf
whitespace = /\s+/ ;
```

空白文字は構文要素間で無視されます。

### 2.2 コメント

```ebnf
line_comment  = "//" , /[^/!][^\n]*/ ;
block_comment = "/*" , /[^*]*\*+([^/*][^*]*\*+)*/ , "/" ;
comment       = line_comment | block_comment ;
```

コメントは空白と同様に無視されます。

### 2.3 ドキュメンテーションコメント

```ebnf
inner_doc = "//!" , /[^\n]*/ ;
outer_doc = "///" , /[^\n]*/ ;
```

- **Inner doc** (`//!`): プログラム先頭に記述するモジュールレベルのドキュメント
- **Outer doc** (`///`): 宣言・定義・ノードに付与するドキュメント

### 2.4 識別子

```ebnf
identifier = /[a-zA-Z_][a-zA-Z0-9_]*/ - keyword ;
```

<WithBaseImage src="/railroad/identifier.svg" alt="Railroad diagram for identifier" />

### 2.5 キーワード

以下の識別子はキーワードとして予約されています：

```
import  extern  type  var  const  tree  as
in  out  ref  mut
true  false  null
vec
```

### 2.6 型 (Types)

Rustスタイルの静的配列構文と、`vec` による動的配列構文、および型推論ワイルドカードを採用します。

```ebnf
type               = base_type , [ "?" ] ;         (* ? で Nullable *)

base_type          = primary_type
                   | static_array_type
                   | dynamic_array_type
                   | infer_type ;

primary_type       = identifier
                   | bounded_string ;

(* 静的配列: [int32; 4] または [int32; <=4] *)
static_array_type  = "[" , type , ";" , array_size_spec , "]" ;

array_size_spec    = array_size                    (* 固定サイズ: [T; 5] *)
                   | "<=" , array_size ;           (* 上限付き: [T; <=5] *)

array_size         = integer | identifier ;

(* 動的配列: vec<int32> *)
dynamic_array_type = "vec" , "<" , type , ">" ;

(* 上限付き文字列: string<=10 *)
bounded_string     = "string" , "<=" , integer ;

(* 型推論ワイルドカード: _ または _? *)
infer_type         = "_" , [ "?" ] ;
```

**例:**

- `int32` : 基本型（非Nullable）
- `int32?` : Nullable型（`null` を許容）
- `[int32; 10]` : 静的配列（固定サイズ10）
- `[int32; <=10]` : 静的配列（最大10要素）
- `vec<float64>` : 動的配列（可変長）
- `vec<_>` : 要素型推論付き動的配列
- `[_; 4]` : 要素型推論付き静的配列
- `[_; <=4]` : 要素型推論付き上限付き静的配列
- `string<=32` : 上限付き文字列（最大32文字）
- `_?` : Nullable型推論（ベース型は後続の使用から推論）

### 2.7 リテラル

```ebnf
string  = '"' , /([^"\\]|\\.)*/ , '"' ;
float   = [ "-" ] , /[0-9]+/ , "." , /[0-9]+/ ;
integer = "0" | [ "-" ] , /[1-9][0-9]*/ ;
boolean = "true" | "false" ;
null    = "null" ;
literal = string | float | integer | boolean | null ;
```

<WithBaseImage src="/railroad/literal.svg" alt="Railroad diagram for literal" />

> [!NOTE] 字句解析において `float` は `integer` より優先されます。`123.456` は `float` として、`123`
> は `integer` として解析されます。

### 2.8 配列リテラル

配列リテラルに「繰り返し初期化」構文を追加します。

```ebnf
array_literal = "[" , ( element_list | repeat_init ) , "]" ;

element_list  = [ expression , { "," , expression } ] ;

repeat_init   = expression , ";" , expression ;
```

**例:**

- `[1, 2, 3]` : 通常初期化
- `[0; 10]` : 繰り返し初期化（0を10個）

---

## 3. 構文規則（Syntactic Grammar）

### 3.1 プログラム

```ebnf
program = { inner_doc }
          { import_stmt }
          { extern_type_stmt }
          { type_alias_stmt }
          { extern_stmt }
          { global_var_decl | global_const_decl }
          { tree_def } ;
```

<WithBaseImage src="/railroad/program.svg" alt="Railroad diagram for program" />

> [!IMPORTANT] 各セクションの順序は固定です。`import` 文が `tree` の後に現れることは構文エラーです。

### 3.2 Import 文

```ebnf
import_stmt = "import" , string ;
```

<WithBaseImage src="/railroad/import_stmt.svg" alt="Railroad diagram for import_stmt" />

### 3.3 Extern 文

ノードの外部定義には、その振る舞い（データフローと実行順序）を記述するための属性 `#[behavior(...)]`
を付与します。

#### 3.3.1 基本構文

```ebnf
extern_stmt       = { outer_doc } , [ behavior_attr ] , "extern" , extern_def ;

extern_def        = "action"    , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "subtree"   , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "condition" , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "control"   , identifier , [ "(" , [ extern_port_list ] , ")" ] , ";"
                  | "decorator" , identifier , [ "(" , [ extern_port_list ] , ")" ] , ";" ;

behavior_attr     = "#[" , "behavior" , "(" , data_policy , [ "," , flow_policy ] , ")" , "]" ;

data_policy       = "All" | "Any" | "None" ;

flow_policy       = "Chained" | "Isolated" ;

extern_type_stmt  = { outer_doc } , "extern" , "type" , identifier , ";" ;

type_alias_stmt   = { outer_doc } , "type" , identifier , "=" , type , ";" ;

extern_port_list  = extern_port , { "," , extern_port } ;

extern_port       = { outer_doc } , [ port_direction ] , identifier , ":" , type , [ "=" , const_expr ] ;

port_direction    = "in" | "out" | "ref" | "mut" ;
```

<WithBaseImage src="/railroad/extern_stmt.svg" alt="Railroad diagram for extern_stmt" />

<WithBaseImage src="/railroad/extern_port.svg" alt="Railroad diagram for extern_port" />

#### 3.3.2 Policies（振る舞い定義）

`#[behavior]`
は、ノードが成功した際の**「子の実行結果（書き込み保証）の集約方法」**と、**「子の実行順序」**を定義します。

**デフォルト値と省略:**

- `DataPolicy` のデフォルトは `All`、`FlowPolicy` のデフォルトは `Chained`
- デフォルト値の場合は属性全体を省略可能（`#[behavior(All, Chained)]` と同等）
- `FlowPolicy` のみ省略可能: `#[behavior(Any)]` = `#[behavior(Any, Chained)]`

| DataPolicy | 意味（成功時の保証）                             | 論理的解釈               | 典型的なノード                     |
| :--------- | :----------------------------------------------- | :----------------------- | :--------------------------------- |
| **`All`**  | **すべて**の子が成功したことを保証（デフォルト） | 和集合（Union）          | `Sequence`, `ParallelAll`, `Retry` |
| **`Any`**  | **いずれか**の子が成功したことを保証             | 共通部分（Intersection） | `Fallback`（Selector）             |
| **`None`** | 子の実行結果を保証しない                         | 空集合（Empty）          | `ForceSuccess`, `Invert`, `Random` |

| FlowPolicy     | 意味（データの可視性）                                                           |
| :------------- | :------------------------------------------------------------------------------- |
| **`Chained`**  | 前のノードの書き込み結果が、次のノードの入力として有効になる（デフォルト）       |
| **`Isolated`** | 孤立している。ノード実行時に前のノードの書き込みが終了していることは保証されない |

#### 3.3.3 定義例

```bt-dsl
// Sequence: 全員成功が必要 (All = デフォルト)、Chained (デフォルト)
extern control Sequence;

// Fallback: 誰か成功が必要 (Any)、Chained (デフォルト)
#[behavior(Any)]
extern control Fallback;

// ParallelAll: 全員成功が必要 (All = デフォルト)、Isolated
#[behavior(All, Isolated)]
extern control ParallelAll;

// Retry: 最終的に成功したなら、子も成功しているとみなす (All = デフォルト)
extern decorator Retry(n: int32);

// ForceSuccess: 失敗を成功に書き換えるため、子の書き込みは保証されない
#[behavior(None)]
extern decorator ForceSuccess;
```

> [!TIP]
> 書き込み保証の伝播ルールの詳細は[初期化安全性](./initialization-safety.md)を参照してください。

### 3.4 グローバル Blackboard・定数宣言

```ebnf
global_blackboard_decl = "var" , identifier ,
                         [ ":" , type ] ,
                         [ "=" , expression ] ;

global_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

<WithBaseImage src="/railroad/global_var_decl.svg" alt="Railroad diagram for global_blackboard_decl" />

> [!NOTE]
>
> - グローバル Blackboard の型は、明示的な型注釈または初期値から決定される必要があります。
> - グローバル定数は初期化が必須であり、値はコンパイル時に評価可能な `const_expr`
>   でなければなりません。

### 3.5 Tree 定義

```ebnf
tree_def        = { outer_doc } , "tree" , identifier ,
                  "(" , [ param_list ] , ")" ,
                  tree_body ;

tree_body       = "{" , { statement } , "}" ;

param_list      = param_decl , { "," , param_decl } ;

param_decl      = [ port_direction ] , identifier , ":" , type , [ "=" , const_expr ] ;

(* Blackboard 宣言: 初期値は省略可能 *)
blackboard_decl   = "var" , identifier ,
                    [ ":" , type ] ,
                    [ "=" , expression ] ;

(* ローカル定数もコンパイル時定数のみ *)
local_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

<WithBaseImage src="/railroad/tree_def.svg" alt="Railroad diagram for tree_def" />

<WithBaseImage src="/railroad/param_decl.svg" alt="Railroad diagram for param_decl" />

<WithBaseImage src="/railroad/local_var_decl.svg" alt="Railroad diagram for blackboard_decl" />

### 3.6 文（Statement）

```ebnf
(* 文の定義 *)
statement         = simple_stmt , ";"       (* 単一行はセミコロン必須 *)
                  | block_stmt ;            (* ブロック系はセミコロン不要 *)

(* セミコロンが必要なもの *)
simple_stmt       = leaf_node_call          (* Action(); *)
                  | assignment_stmt         (* x = 1; *)
                  | blackboard_decl ;       (* var x; *)

(* セミコロンが不要なもの（波括弧で終わるもの） *)
block_stmt        = compound_node_call ;    (* Sequence { ... } *)

(* ノード呼び出しの詳細 *)
leaf_node_call    = { outer_doc } , [ precondition_list ] , identifier , property_block ;

compound_node_call = { outer_doc } , [ precondition_list ] , identifier , node_body_with_children ;

node_body_with_children = property_block , children_block
                        | children_block ;

precondition_list = precondition , { precondition } ;

precondition      = "@" , precond_kind , "(" , expression , ")" ;

precond_kind      = "success_if" | "failure_if" | "skip_if" | "run_while" | "guard" ;

property_block    = "(" , [ argument_list ] , ")" ;

argument_list     = argument , { "," , argument } ;

argument          = identifier , ":" , argument_expr    (* named argument *)
                  | argument_expr ;                      (* positional argument *)

argument_expr     = [ port_direction ] , expression
                  | "out" , inline_blackboard_decl ;    (* out var x 構文 *)

inline_blackboard_decl = "var" , identifier ;

children_block    = "{" , { statement } , "}" ;
```

<WithBaseImage src="/railroad/node_stmt.svg" alt="Railroad diagram for node_stmt" />

<WithBaseImage src="/railroad/argument.svg" alt="Railroad diagram for argument" />

#### 3.6.1 インライン Blackboard 宣言

`out` ポートへの引数として、Blackboard を同時に宣言することができます。

```bt-dsl
// 事前に宣言するパターン
var result: int32;
Compute(res: out result);

// インライン宣言パターン（型はポートから推論）
Compute(res: out var result);


```

**制約:**

- インライン宣言は `out` 方向でのみ使用可能
- 宣言された Blackboard は、その宣言を含む `children_block` のスコープに属する
- 型注釈は使用不可（ポートの型から推論される）

### 3.7 事前条件（Precondition）

事前条件はノードの実行前に評価される組み込み構文です。`@`
記号を先頭に置き、条件式を括弧内に記述します。

| 構文                | 動作                                                       | 偽の時のステータス |
| :------------------ | :--------------------------------------------------------- | :----------------- |
| `@success_if(cond)` | 条件が真なら実行せず即座に終了                             | `Success`          |
| `@failure_if(cond)` | 条件が真なら実行せず即座に終了                             | `Failure`          |
| `@skip_if(cond)`    | 条件が真なら実行せずスキップ                               | `Skip`             |
| `@run_while(cond)`  | 実行前・実行中に条件を評価。偽になった場合、実行を中断する | `Skip`             |
| `@guard(cond)`      | 実行前・実行中に条件を評価。偽になった場合、実行を中断する | `Failure`          |

> [!NOTE] `@run_while` と `@guard`
> は、条件が満たされなくなった際の終了ステータスのみが異なります。どちらも「条件が真である間のみ実行を許可する」という動作は同一です。

**例:**

```bt-dsl
// 条件が真なら実行せずに成功
@success_if(cache_valid)
FetchData(out data);

// target が null でない場合のみ実行
@guard(target != null)
MoveTo(target);

// 条件が偽になったら Running 中でも中断
@run_while(is_active)
LongRunningTask();

// 複数の事前条件を組み合わせる
@guard(target != null)
@failure_if(!is_valid)
ApproachTarget(target);
```

> [!NOTE] 複数の事前条件が指定された場合の評価順序はランタイム依存です。

> [!IMPORTANT] `@guard` および `@run_while`
> の条件式内で参照される Nullable 変数は、そのノードのスコープ内で非Nullable型として扱われます（型の絞り込み）。詳細は[意味制約 - 制御フローによる型推論](./semantics.md#_8-1-制御フローによる型推論-flow-sensitive-typing)を参照してください。

### 3.8 Decorator の暗黙的 Sequence 挿入

`decorator` カテゴリのノードは、厳密には **1つの子ノードのみ**
を持つことができます。しかし、構文上は `control`
と同じ形式で複数の子ノードを記述することが許可されます。

**ルール:**

- `decorator` ノードの `children_block` に**2つ以上**のノードが記述された場合、コンパイラは自動的に
  `Sequence` ノードでそれらをラップします。
- `children_block` に**1つのみ**のノードが記述された場合、そのまま子として扱われます。

**例:**

```bt-dsl
// ユーザーが記述したコード
Retry(n: 3) {
    TaskA();
    TaskB();
}

// コンパイラが解釈するコード（暗黙のSequence挿入）
Retry(n: 3) {
    Sequence {
        TaskA();
        TaskB();
    }
}
```

> [!NOTE] この暗黙的変換は `decorator` カテゴリのノードにのみ適用されます。`control`
> ノードは複数の子を持つことが許可されているため、この変換は行われません。

### 3.8 代入文

```ebnf
expression_stmt = assignment_stmt ;

assignment_stmt = lvalue , assignment_op , expression ;

lvalue          = identifier , { index_suffix } ;

assignment_op   = "=" | "+=" | "-=" | "*=" | "/=" ;
```

<WithBaseImage src="/railroad/assignment_expr.svg" alt="Railroad diagram for assignment_expr" />

### 3.9 式

```ebnf
expression           = or_expr ;

or_expr              = and_expr , { "||" , and_expr } ;

and_expr             = bitwise_or_expr , { "&&" , bitwise_or_expr } ;

bitwise_or_expr      = bitwise_and_expr , { "|" , bitwise_and_expr } ;

bitwise_and_expr     = equality_expr , { "&" , equality_expr } ;

equality_expr        = comparison_expr , [ ( "==" | "!=" ) , comparison_expr ] ;

comparison_expr      = additive_expr , [ ( "<" | "<=" | ">" | ">=" ) , additive_expr ] ;

additive_expr        = multiplicative_expr , { ( "+" | "-" ) , multiplicative_expr } ;

multiplicative_expr  = cast_expr , { ( "*" | "/" | "%" ) , cast_expr } ;

(* キャスト式: 明示的な型変換 *)
cast_expr            = unary_expr , [ "as" , type ] ;

unary_expr           = ( "!" | "-" ) , unary_expr
                     | primary_expr ;

(* プライマリ式: 配列アクセスを含む *)
primary_expr         = ( "(" , expression , ")"
                       | literal
                       | array_literal
                       | vec_macro
                       | identifier
                       ) , { index_suffix } ;

vec_macro            = "vec!" , array_literal ;

(* array_literal, element_list, repeat_init は 2.8 配列リテラル を参照 *)

index_suffix         = "[" , expression , "]" ;
```

<WithBaseImage src="/railroad/expression.svg" alt="Railroad diagram for expression" />

---

## 4. 演算子の優先順位

優先順位が高いほど強く結合します。

| 優先順位 | 演算子              | 結合規則 | 説明               |
| -------: | :------------------ | :------- | :----------------- |
|       13 | `[]` (配列アクセス) | 左結合   | 配列インデックス   |
|       12 | `!` `-` (単項)      | 右結合   | 論理否定、算術否定 |
|       11 | `as` (キャスト)     | 左結合   | 明示的な型変換     |
|       10 | `*` `/` `%`         | 左結合   | 乗算、除算、剰余   |
|        9 | `+` `-`             | 左結合   | 加算、減算         |
|        8 | `<` `<=` `>` `>=`   | 左結合   | 比較               |
|        7 | `==` `!=`           | 左結合   | 等価               |
|        6 | `&`                 | 左結合   | ビット AND         |
|        5 | `\|`                | 左結合   | ビット OR          |
|        4 | `&&`                | 左結合   | 論理 AND           |
|        3 | `\|\|`              | 左結合   | 論理 OR            |

---

## 5. 定数式（Constant Expression）

定数式 (`const_expr`) はコンパイル時に評価可能な式です。グローバル定数の初期化に使用されます。

```ebnf
const_expr           = const_or_expr ;

const_or_expr        = const_and_expr , { "||" , const_and_expr } ;

const_and_expr       = const_equality_expr , { "&&" , const_equality_expr } ;

const_equality_expr  = const_comparison_expr , [ ( "==" | "!=" ) , const_comparison_expr ] ;

const_comparison_expr = const_additive_expr , [ ( "<" | "<=" | ">" | ">=" ) , const_additive_expr ] ;

const_additive_expr  = const_multiplicative_expr , { ( "+" | "-" ) , const_multiplicative_expr } ;

const_multiplicative_expr = const_cast_expr , { ( "*" | "/" | "%" ) , const_cast_expr } ;

const_cast_expr      = const_unary_expr , [ "as" , type ] ;

const_unary_expr     = ( "!" | "-" ) , const_unary_expr
                     | const_primary_expr ;

const_primary_expr   = "(" , const_expr , ")"
                     | literal
                     | const_array_literal
                     | identifier ;       (* 他の定数への参照 *)

const_array_literal  = "[" , ( const_element_list | const_repeat_init ) , "]" ;

const_element_list   = [ const_expr , { "," , const_expr } ] ;

const_repeat_init    = const_expr , ";" , const_expr ;
```

> [!NOTE] 定数式内の `identifier`
> は、既に定義済みの他の定数を参照できます。ただし、循環参照はコンパイルエラーとなります。
