# 1. 字句構造（Lexical Structure）

本章は、ソースコードのテキストを**トークン列**に変換するための規則を定義します。

---

## 1.1 ソースコード表現

### 1.1.1 文字エンコーディング

ソースコードは **UTF-8** として解釈されなければなりません。不正な UTF-8 バイト列を含む場合、処理系は**字句エラー**を報告しなければなりません。

### 1.1.2 改行コード

行終端は **LF (`\n`)** または **CRLF (`\r\n`)** を許容します。

---

## 1.2 空白とコメント（Whitespace and Comments）

### 1.2.1 空白

```ebnf
whitespace = /\s+/ ;
```

空白は構文要素間で無視されます。

### 1.2.2 コメント

```ebnf
line_comment  = "//" , /[^\n]*/ ;
block_comment = "/*" , (* ネスト可能なコメント本体 *) , "*/" ;
comment       = line_comment | block_comment ;
```

- コメントは空白と同様に無視されます。
- ブロックコメントは**ネスト可能**です。`/*` の出現でネストレベルが増加し、`*/` の出現で減少します。ネストレベルが 0 になった時点で終了します。

### 1.2.3 ドキュメンテーションコメント

```ebnf
inner_doc = "//!" , /[^\n]*/ ;
outer_doc = "///" , /[^\n]*/ ;
```

処理系は `inner_doc` / `outer_doc` を `line_comment` より優先して認識しなければなりません。

---

## 1.3 識別子とキーワード（Identifiers and Keywords）

### 1.3.1 識別子

```ebnf
identifier = /[a-zA-Z_][a-zA-Z0-9_]*/ - keyword ;
```

識別子はキーワードと一致してはなりません。

### 1.3.2 予約語（Keywords）

以下はキーワードとして予約されます。

```text
import  extern  type  var  const  tree  as  root  do
in  out  inout
true  false
action  subtree  condition  control  decorator
```

---

## 1.4 リテラル（Literals）

### 1.4.1 字句規則

```ebnf
float    = /[0-9]+/ , "." , /[0-9]+/ , [ exponent ]
         | /[0-9]+/ , exponent ;
exponent = ( "e" | "E" ) , [ "+" | "-" ] , /[0-9]+/ ;
integer  = "0" | /[1-9][0-9]*/ ;
string   = '"' , { string_char | escape_seq } , '"' ;
string_char = /[^"\\\n]/ ;
escape_seq  = "\\" , ( '"' | "\\" | "n" | "r" | "t" | "0" | "b" | "f" )
            | "\\u{" , /[0-9A-Fa-f]{1,6}/ , "}" ;
boolean  = "true" | "false" ;
literal  = float | integer | string | boolean ;
```

### 1.4.2 トークン化の優先順位

`float` は `integer` より優先してトークン化されなければなりません。

---

## 1.5 記号と演算子（Punctuation and Operators）

### 1.5.1 区切り文字（Punctuation）

- 区切り: `;` `,` `:`
- 括弧: `{` `}` `(` `)`
- 属性: `#` `[` `]`（構文上は `#[ ... ]` として使用）
- 事前条件: `@`

### 1.5.2 演算子トークン

- 代入: `=` `+=` `-=` `*=` `/=`
- 論理: `||` `&&` `!`
- ビット: `|` `&` `^`
- 比較: `==` `!=` `<` `<=` `>` `>=`
- 算術: `+` `-` `*` `/` `%`
- キャスト: `as`
