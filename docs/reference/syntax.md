# 2. 構文（Syntax）

本章は、トークン列の並び（EBNF）によって、BT-DSL の**文法的に正しい構造**を定義します。

---

## 2.1 記法（Notation）

本ドキュメントでは ISO/IEC 14977 EBNF に準拠した記法を使用します。

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

## 2.2 モジュール構造（Module Structure）

### 2.2.1 program

```ebnf
program = { inner_doc
          | import_stmt
          | extern_type_stmt
          | type_alias_stmt
          | extern_stmt
          | global_blackboard_decl
          | global_const_decl
          | tree_def
          } ;
```

### 2.2.2 import

```ebnf
import_stmt = "import" , string ;
```

---

## 2.3 型の構文（Type Syntax）

```ebnf
type         = base_type ;

base_type    = identifier ;
```

---

## 2.4 式（Expressions）

### 2.4.1 EBNF

```ebnf
expression           = or_expr ;

or_expr              = and_expr , { "||" , and_expr } ;

and_expr             = bitwise_or_expr , { "&&" , bitwise_or_expr } ;

bitwise_or_expr      = bitwise_xor_expr , { "|" , bitwise_xor_expr } ;

bitwise_xor_expr     = bitwise_and_expr , { "^" , bitwise_and_expr } ;

bitwise_and_expr     = equality_expr , { "&" , equality_expr } ;

equality_expr        = comparison_expr , [ ( "==" | "!=" ) , comparison_expr ] ;

comparison_expr      = additive_expr , [ ( "<" | "<=" | ">" | ">=" ) , additive_expr ] ;

additive_expr        = multiplicative_expr , { ( "+" | "-" ) , multiplicative_expr } ;

multiplicative_expr  = cast_expr , { ( "*" | "/" | "%" ) , cast_expr } ;

cast_expr            = unary_expr , { "as" , type } ;

unary_expr           = ( "!" | "-" ) , unary_expr
                     | primary_expr ;

primary_expr         = "(" , expression , ")"
                     | "is_set" , "(" , identifier , ")"
                     | literal
                     | identifier ;
```

### 2.4.2 演算子の優先順位と結合規則

以下のテーブルは演算子の優先順位と結合規則を定義します。2.4.1 の EBNF 文法はこのテーブルを実装しています。

| 優先順位 | 演算子            | 結合規則 |
| -------: | :---------------- | :------- |
|       11 | `!` `-` (単項)    | 右結合   |
|       10 | `as` (キャスト)   | 左結合   |
|        9 | `*` `/` `%`       | 左結合   |
|        8 | `+` `-`           | 左結合   |
|        7 | `<` `<=` `>` `>=` | 非結合   |
|        6 | `==` `!=`         | 非結合   |
|        5 | `&`               | 左結合   |
|        4 | `^`               | 左結合   |
|        3 | `\|`              | 左結合   |
|        2 | `&&`              | 左結合   |
|        1 | `\|\|`            | 左結合   |

比較演算子（`<` `<=` `>` `>=`）および等価演算子（`==` `!=`）は連鎖を禁止します。`a < b < c` は構文エラーです。

---

## 2.5 文（Statements）

### 2.5.1 ノード文

```ebnf
statement
    = do_block
    | { outer_doc } , [ precondition ] , identifier , statement_after_identifier ;

statement_after_identifier
    = property_block , ";"
    | property_block , children_block
    | children_block ;
```

### 2.5.2 do ブロック（スクリプト文）

```ebnf
do_block      = "do" , "{" , { do_statement } , "}" ;

do_statement  = identifier , assignment_op , expression , ";"
              | blackboard_decl , ";" ;

assignment_op = "=" | "+=" | "-=" | "*=" | "/=" ;
```

---

## 2.6 定義（Definitions）

### 2.6.1 extern / type

```ebnf
extern_stmt       = { outer_doc } , { attribute } , "extern" , extern_def ;

extern_def        = "action"    , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "subtree"   , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "condition" , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "control"   , identifier , "(" , [ extern_port_list ] , ")" , ";"
                  | "decorator" , identifier , "(" , [ extern_port_list ] , ")" , ";" ;

attribute         = "#[" , attr_name , [ "(" , attr_args , ")" ] , "]" ;

attr_name         = identifier ;

attr_args         = attr_arg , { "," , attr_arg } ;

attr_arg          = identifier
                  | literal ;

extern_type_stmt  = { outer_doc } , "extern" , "type" , identifier , ";" ;

type_alias_stmt   = { outer_doc } , "type" , identifier , "=" , type , ";" ;

extern_port_list  = extern_port , { "," , extern_port } ;

extern_port       = { outer_doc } , [ port_direction ] , identifier , ":" , type , [ "=" , expression ] ;

port_direction    = "in" | "out" | "inout" ;
```

### 2.6.2 グローバル Blackboard・定数

```ebnf
global_blackboard_decl = { outer_doc } , "var" , identifier ,
                         [ ":" , type ] ,
                         [ "=" , expression ] , ";" ;

global_const_decl = { outer_doc } , "const" , identifier , [ ":" , type ] , "=" , expression , ";" ;
```

### 2.6.3 tree 定義

```ebnf
tree_def          = { outer_doc } , "tree" , identifier ,
                    "(" , [ param_list ] , ")" ,
                    tree_body ;

tree_body         = "{" , { local_decl } , root_clause , "}" ;

local_decl        = blackboard_decl , ";"
                  | local_const_decl , ";" ;

root_clause       = "root" , node_call ;

node_call         = identifier , statement_after_identifier ;

param_list        = param_decl , { "," , param_decl } ;

param_decl        = [ port_direction ] , identifier , [ ":" , type ] , [ "=" , expression ] ;

blackboard_decl   = { outer_doc } , "var" , identifier ,
                    [ ":" , type ] ,
                    [ "=" , expression ] ;

local_const_decl  = { outer_doc } , "const" , identifier , [ ":" , type ] , "=" , expression ;
```

### 2.6.4 ノード呼び出し

```ebnf
precondition      = "@" , precond_kind , "(" , expression , ")" ;

precond_kind      = "success_if" | "failure_if" | "skip_if" | "run_while" | "guard" ;

property_block    = "(" , [ argument_list ] , ")" ;

argument_list     = argument , { "," , argument } ;

argument          = identifier , ":" , argument_expr ;

argument_expr     = [ port_direction ] , expression
                  | "out" , inline_blackboard_decl ;

inline_blackboard_decl = "var" , identifier ;

children_block    = "{" , { statement } , "}" ;
```
