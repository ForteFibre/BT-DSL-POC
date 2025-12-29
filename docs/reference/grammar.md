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
in  out  ref
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

#### 3.3.1 基本構文

```ebnf
extern_stmt       = { outer_doc } , "extern" , ( simple_extern | decorator_def | control_def ) ;

simple_extern     = category , identifier , "(" , [ extern_port_list ] , ")" , ";" ;

extern_type_stmt  = { outer_doc } , "extern" , "type" , identifier , ";" ;

type_alias_stmt   = { outer_doc } , "type" , identifier , "=" , type , ";" ;

category          = identifier ;
                    (* 有効な値: action, condition, subtree *)

extern_port_list  = extern_port , { "," , extern_port } ;

extern_port       = { outer_doc } , [ port_direction ] , identifier , ":" , type , [ "=" , const_expr ] ;

port_direction    = "in" | out_direction | "ref" ;

out_direction     = "out" , [ guarantee_modifier ] ;

guarantee_modifier = "always" | "on_failure" ;
```

<WithBaseImage src="/railroad/extern_stmt.svg" alt="Railroad diagram for extern_stmt" />

<WithBaseImage src="/railroad/extern_port.svg" alt="Railroad diagram for extern_port" />

> [!NOTE] 書き込み保証修飾子（`always`,
> `on_failure`）の詳細は[初期化安全性](./initialization-safety.md)を参照してください。

#### 3.3.2 Decorator 定義（状態変換）

Decorator ノードは子ノードの結果を変換するルールを記述できます。

```ebnf
decorator_def     = "decorator" , identifier , [ "(" , [ extern_port_list ] , ")" ] ,
                    "{" , { mapping_stmt } , "}" ;

mapping_stmt      = condition , "=>" , result_status , ";" ;

condition         = status_kind | "_" ;

status_kind       = "success" | "failure" | "running" ;

result_status     = status_kind | "ambiguous" ;
```

**例:**

```bt-dsl
extern decorator ForceSuccess {
    failure => success;
}

extern decorator Invert {
    success => failure;
    failure => success;
}
```

#### 3.3.3 Control 定義（論理集約）

Control ノードは複数の子ノードの結果を集約するルールを記述できます。

```ebnf
control_def       = "control" , identifier , [ "(" , [ extern_port_list ] , ")" ] ,
                    "{" , [ exec_attr ] , { mapping_stmt } , "}" ;

exec_attr         = "execution" , "=" , exec_mode , ";" ;

exec_mode         = "sequential" | "parallel" ;

condition         = status_kind
                  | "any(" , status_kind , ")"
                  | "all(" , status_kind , ")" ;
```

**例:**

```bt-dsl
extern control Sequence {
    any(failure) => failure;
    all(success) => success;
}

// ParallelAll: 全員成功なら成功、1つでも失敗なら失敗（静的解析可能）
extern control ParallelAll {
    execution = parallel;
    all(success) => success;
    any(failure) => failure;
}

// Parallel: 動的閾値による並列実行（静的解析不可）
extern control Parallel(
    in success_threshold: int32,
    in failure_threshold: int32
) {
    execution = parallel;
    _ => ambiguous;
}
```

> [!TIP]
> ノードロジック定義の詳細と解析への影響は[初期化安全性](./initialization-safety.md)を参照してください。

### 3.4 グローバル変数・定数宣言

```ebnf
global_var_decl   = "var" , identifier ,
                    [ ":" , type ] ,
                    [ "=" , expression ] ;

global_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

<WithBaseImage src="/railroad/global_var_decl.svg" alt="Railroad diagram for global_var_decl" />

> [!NOTE]
>
> - グローバル変数の型推論は**同一ファイル内**で完結する必要があります。
> - グローバル定数は初期化が必須であり、値はコンパイル時に評価可能な `const_expr`
>   でなければなりません。

### 3.5 Tree 定義

```ebnf
tree_def        = { outer_doc } , "tree" , identifier ,
                  "(" , [ param_list ] , ")" ,
                  "{" , { local_var_decl | local_const_decl } , [ node_stmt ] , "}" ;

param_list      = param_decl , { "," , param_decl } ;

param_decl      = [ port_direction ] , identifier , [ ":" , type ] , [ "=" , const_expr ] ;

(* 型も初期値も省略可能（推論に依存） *)
local_var_decl  = "var" , identifier ,
                  [ ":" , type ] ,
                  [ "=" , expression ] ;

(* ローカル定数もコンパイル時定数のみ *)
local_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

<WithBaseImage src="/railroad/tree_def.svg" alt="Railroad diagram for tree_def" />

<WithBaseImage src="/railroad/param_decl.svg" alt="Railroad diagram for param_decl" />

<WithBaseImage src="/railroad/local_var_decl.svg" alt="Railroad diagram for local_var_decl" />

### 3.6 ノード文

```ebnf
node_stmt       = { outer_doc } , [ decorator_list ] , identifier , node_body ;

node_body       = property_block , [ children_block ]
                | children_block ;

decorator_list  = "@[" , decorator_entry , { "," , decorator_entry } , "]" ;

decorator_entry = identifier , [ property_block ] ;

property_block  = "(" , [ argument_list ] , ")" ;

argument_list   = argument , { "," , argument } ;

argument        = identifier , [ "as" , type ] , ":" , argument_expr    (* named argument, optional CICO *)
                | argument_expr ;                                        (* positional argument *)

argument_expr   = [ port_direction ] , expression ;

children_block  = "{" , { node_stmt | expression_stmt } , "}" ;
```

<WithBaseImage src="/railroad/node_stmt.svg" alt="Railroad diagram for node_stmt" />

<WithBaseImage src="/railroad/decorator.svg" alt="Railroad diagram for decorator" />

<WithBaseImage src="/railroad/argument.svg" alt="Railroad diagram for argument" />

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

array_literal        = "[" , ( element_list | repeat_init ) , "]" ;     (* → 2.8 参照 *)

element_list         = [ expression , { "," , expression } ] ;

repeat_init          = expression , ";" , expression ;

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
