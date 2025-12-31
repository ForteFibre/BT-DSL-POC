# 2. 構文（Syntax）

本章は、トークン列の並び（EBNF）によって、BT-DSL の**文法的に正しい構造**を定義します。

- 型検査・スコープ解決・意味制約は本章の範囲外です（後続章で定義）。
- 字句構造は [字句構造](./lexical-structure.md) を参照してください。

---

## 2.1 記法（Notation）

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

## 2.2 モジュール構造（Module Structure）

### 2.2.1 program

```ebnf
program = { inner_doc }
          { import_stmt }
          { extern_type_stmt }
          { type_alias_stmt }
          { extern_stmt }
          { global_var_decl | global_const_decl }
          { tree_def } ;
```

> [!IMPORTANT]
> 各セクションの順序は固定です。`import` 文が `tree` の後に現れることは構文エラーです。

### 2.2.2 import

```ebnf
import_stmt = "import" , string ;
```

> [!NOTE]
> `import` の探索規則（検索パス、拡張子要件、循環 import の扱い等）は別途規定（TBD /
> implementation-defined）です。

### 2.2.3 extern / type / var / const / tree

（次節以降の各定義を参照）

---

## 2.3 型の構文（Type Syntax）

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

---

## 2.4 式（Expressions）

### 2.4.1 EBNF

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
cast_expr            = unary_expr , { "as" , type } ;

unary_expr           = ( "!" | "-" ) , unary_expr
                     | primary_expr ;

(* プライマリ式: 配列アクセスを含む *)
primary_expr         = ( "(" , expression , ")"
                       | literal
                       | array_literal
                       | vec_macro
                       | identifier
                       ) , { index_suffix } ;

array_literal = "[" , ( element_list | repeat_init ) , "]" ;

element_list  = [ expression , { "," , expression } , [ "," ] ] ;

repeat_init   = expression , ";" , expression ;

vec_macro            = "vec!" , array_literal ;

index_suffix         = "[" , expression , "]" ;
```

### 2.4.2 演算子の優先順位と結合規則（Precedence and Associativity）

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

> [!NOTE]
> 2.4.1 の EBNF と本表（2.4.2）は、通常は同一の優先順位/結合規則を表すことを意図します。
> `as` は左結合として扱われ、例えば `a as T1 as T2` は `(a as T1) as T2` として解釈されます。

---

## 2.5 文（Statements）

```ebnf
(*
    文（statement）は、BT-DSL の実行順序を構成する最小単位です。

    `statement` は「識別子から始まる文」が複数種類あるため（ノード呼び出し / 代入）、
    先頭の `identifier` を左くくり出しし、後続の形で分岐するように定義します。
*)

statement
    = blackboard_decl , ";"                        (* var x = ...; *)
    | local_const_decl , ";"                       (* const X = ...; *)
    | { outer_doc } , [ precondition_list ] , identifier , statement_after_identifier ;

statement_after_identifier
    = property_block , ";"                         (* Leaf node call: Action(...); *)
    | property_block , children_block                (* Compound node call (with args) *)
    | children_block                                 (* Compound node call (no args) *)
    | { index_suffix } , assignment_op , expression , ";" ;  (* Assignment: x = 1; / x[0] += 1; *)

assignment_op = "=" | "+=" | "-=" | "*=" | "/=" ;
```

---

## 2.6 定義（Definitions）

### 2.6.1 extern / type

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

### 2.6.2 グローバル Blackboard・定数

```ebnf
global_blackboard_decl = "var" , identifier ,
                         [ ":" , type ] ,
                         [ "=" , expression ] ;

global_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

### 2.6.3 tree 定義

```ebnf
tree_def        = { outer_doc } , "tree" , identifier ,
                  "(" , [ param_list ] , ")" ,
                  tree_body ;

tree_body       = "{" , { statement } , "}" ;

param_list      = param_decl , { "," , param_decl } ;

param_decl      = [ port_direction ] , identifier , ":" , type , [ "=" , const_expr ] ;

blackboard_decl   = "var" , identifier ,
                    [ ":" , type ] ,
                    [ "=" , expression ] ;

local_const_decl = "const" , identifier , [ ":" , type ] , "=" , const_expr ;
```

### 2.6.4 ノード呼び出し

```ebnf
(*
    ノード呼び出しは「Leaf（子なし）」と「Compound（子あり）」の 2 形態を持ちます。
    文としての終端規則（セミコロンの要否）は 2.5 の `statement` が規定します。
*)

node_call_prefix = { outer_doc } , [ precondition_list ] ;

leaf_node_call
    = node_call_prefix , identifier , property_block ;

compound_node_call
    = node_call_prefix , identifier , ( property_block , children_block | children_block ) ;

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

---

## 2.7 定数式（Constant Expressions）

定数式 (`const_expr`) はコンパイル時に評価可能な式です。

```ebnf
const_expr           = const_or_expr ;

const_or_expr        = const_and_expr , { "||" , const_and_expr } ;

const_and_expr       = const_equality_expr , { "&&" , const_equality_expr } ;

const_equality_expr  = const_comparison_expr , [ ( "==" | "!=" ) , const_comparison_expr ] ;

const_comparison_expr = const_additive_expr , [ ( "<" | "<=" | ">" | ">=" ) , const_additive_expr ] ;

const_additive_expr  = const_multiplicative_expr , { ( "+" | "-" ) , const_multiplicative_expr } ;

const_multiplicative_expr = const_cast_expr , { ( "*" | "/" | "%" ) , const_cast_expr } ;

const_cast_expr      = const_unary_expr , { "as" , type } ;

const_unary_expr     = ( "!" | "-" ) , const_unary_expr
                     | const_primary_expr ;

const_primary_expr   = "(" , const_expr , ")"
                     | literal
                     | const_array_literal
                     | identifier ;       (* 他の定数への参照 *)

const_array_literal  = "[" , ( const_element_list | const_repeat_init ) , "]" ;

const_element_list   = [ const_expr , { "," , const_expr } , [ "," ] ] ;

const_repeat_init    = const_expr , ";" , const_expr ;
```

> [!NOTE]
> 定数式内の `identifier`
> は、既に定義済みの他の定数を参照できます。ただし循環参照はコンパイルエラーです。
