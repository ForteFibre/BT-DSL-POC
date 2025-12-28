# 文法リファレンス

本ドキュメントは BT-DSL の**文法**を厳密に定義します。

---

## 1. 記法

本ドキュメントでは [ISO/IEC 14977 EBNF](https://www.w3.org/TR/REC-xml/#sec-notation) に準拠した記法を使用します。

| 記法 | 意味 |
|:---|:---|
| `=` | 定義 |
| `;` | 定義の終了 |
| `\|` | 選択（OR） |
| `( )` | グループ化 |
| `[ ]` | オプション（0回または1回） |
| `{ }` | 繰り返し（0回以上） |
| `"..."` | 終端記号（リテラル） |
| `'...'` | 終端記号（リテラル） |
| `/regex/` | 正規表現パターン |
| `(*...*)` | コメント |

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
identifier = /[a-zA-Z_][a-zA-Z0-9_]*/ ;
```

<WithBaseImage src="/railroad/identifier.svg" alt="Railroad diagram for identifier" />

### 2.5 キーワード

以下の識別子はキーワードとして予約されています：

```
import  declare  var  Tree
in  out  ref
true  false
```

### 2.6 リテラル

```ebnf
string  = '"' , /([^"\\]|\\.)*/ , '"' ;
float   = [ "-" ] , /[0-9]+/ , "." , /[0-9]+/ ;
integer = [ "-" ] , /[0-9]+/ ;
boolean = "true" | "false" ;
literal = string | float | integer | boolean ;
```

<WithBaseImage src="/railroad/literal.svg" alt="Railroad diagram for literal" />

> [!NOTE]
> 字句解析において `float` は `integer` より優先されます。`123.456` は `float` として、`123` は `integer` として解析されます。

---

## 3. 構文規則（Syntactic Grammar）

### 3.1 プログラム

```ebnf
program = { inner_doc }
          { import_stmt }
          { declare_stmt }
          { global_var_decl }
          { tree_def } ;
```

<WithBaseImage src="/railroad/program.svg" alt="Railroad diagram for program" />

> [!IMPORTANT]
> 各セクションの順序は固定です。`import` 文が `Tree` の後に現れることは構文エラーです。

### 3.2 Import 文

```ebnf
import_stmt = "import" , string ;
```

<WithBaseImage src="/railroad/import_stmt.svg" alt="Railroad diagram for import_stmt" />

### 3.3 Declare 文

```ebnf
declare_stmt      = { outer_doc } , "declare" , category , identifier , 
                    "(" , [ declare_port_list ] , ")" ;

category          = identifier ;
                    (* 有効な値: Action, Condition, Control, Decorator, SubTree *)

declare_port_list = declare_port , { "," , declare_port } ;

declare_port      = { outer_doc } , [ port_direction ] , identifier , ":" , type ;

port_direction    = "in" | "out" | "ref" ;

type              = identifier ;
```

<WithBaseImage src="/railroad/declare_stmt.svg" alt="Railroad diagram for declare_stmt" />

<WithBaseImage src="/railroad/declare_port.svg" alt="Railroad diagram for declare_port" />

### 3.4 グローバル変数宣言

```ebnf
global_var_decl = "var" , identifier , ":" , type ;
```

<WithBaseImage src="/railroad/global_var_decl.svg" alt="Railroad diagram for global_var_decl" />

### 3.5 Tree 定義

```ebnf
tree_def       = { outer_doc } , "Tree" , identifier , 
                 "(" , [ param_list ] , ")" ,
                 "{" , { local_var_decl } , [ node_stmt ] , "}" ;

param_list     = param_decl , { "," , param_decl } ;

param_decl     = [ port_direction ] , identifier , [ ":" , type ] ;

local_var_decl = "var" , identifier , 
                 [ ":" , type ] , 
                 [ "=" , expression ] ;
```

<WithBaseImage src="/railroad/tree_def.svg" alt="Railroad diagram for tree_def" />

<WithBaseImage src="/railroad/param_decl.svg" alt="Railroad diagram for param_decl" />

<WithBaseImage src="/railroad/local_var_decl.svg" alt="Railroad diagram for local_var_decl" />

### 3.6 ノード文

```ebnf
node_stmt       = { outer_doc } , { decorator } , identifier , node_body ;

node_body       = property_block , [ children_block ]
                | children_block ;

decorator       = "@" , identifier , [ property_block ] ;

property_block  = "(" , [ argument_list ] , ")" ;

argument_list   = argument , { "," , argument } ;

argument        = identifier , ":" , value_expr     (* named argument *)
                | value_expr ;                       (* positional argument *)

children_block  = "{" , { node_stmt | expression_stmt } , "}" ;
```

<WithBaseImage src="/railroad/node_stmt.svg" alt="Railroad diagram for node_stmt" />

<WithBaseImage src="/railroad/decorator.svg" alt="Railroad diagram for decorator" />

<WithBaseImage src="/railroad/argument.svg" alt="Railroad diagram for argument" />

### 3.7 値式

```ebnf
value_expr     = blackboard_ref | literal ;

blackboard_ref = [ port_direction ] , identifier ;
```

### 3.8 代入文

```ebnf
expression_stmt = assignment_expr ;

assignment_expr = identifier , assignment_op , expression ;

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

multiplicative_expr  = unary_expr , { ( "*" | "/" | "%" ) , unary_expr } ;

unary_expr           = ( "!" | "-" ) , unary_expr
                     | primary_expr ;

primary_expr         = "(" , expression , ")"
                     | literal
                     | identifier ;
```

<WithBaseImage src="/railroad/expression.svg" alt="Railroad diagram for expression" />

---

## 4. 演算子の優先順位

優先順位が高いほど強く結合します。すべて左結合です。

| 優先順位 | 演算子 | 説明 |
|---:|:---|:---|
| 9 | `!` `-` (単項) | 論理否定、算術否定 |
| 8 | `*` `/` `%` | 乗算、除算、剰余 |
| 7 | `+` `-` | 加算、減算 |
| 6 | `<` `<=` `>` `>=` | 比較 |
| 5 | `==` `!=` | 等価 |
| 4 | `&` | ビット AND |
| 3 | `\|` | ビット OR |
| 2 | `&&` | 論理 AND |
| 1 | `\|\|` | 論理 OR |
