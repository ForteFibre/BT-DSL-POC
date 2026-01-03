; Syntax highlighting for BT DSL (new spec)

; ---------------------------------------------------------------------------
; Keywords
; ---------------------------------------------------------------------------
["import" "extern" "type" "var" "const" "tree" "as"] @keyword

; Port directions
["in" "out" "ref" "mut"] @keyword.modifier

; Attribute-like constructs
(precondition "@" @punctuation.special)
(precondition kind: (precond_kind) @attribute)

(behavior_attr "#[" @punctuation.special)
(behavior_attr "behavior" @attribute)
(data_policy) @constant.builtin
(flow_policy) @constant.builtin

; ---------------------------------------------------------------------------
; Definitions
; ---------------------------------------------------------------------------
(tree_def name: (identifier) @function.definition)

(extern_def name: (identifier) @function.definition)

(extern_type_stmt name: (identifier) @type.definition)
(type_alias_stmt name: (identifier) @type.definition)

; ---------------------------------------------------------------------------
; Calls
; ---------------------------------------------------------------------------
(leaf_node_call name: (identifier) @function.call)
(compound_node_call name: (identifier) @function.call)

; ---------------------------------------------------------------------------
; Parameters / vars / consts
; ---------------------------------------------------------------------------
(param_decl name: (identifier) @variable.parameter)
(global_blackboard_decl name: (identifier) @variable)
(blackboard_decl name: (identifier) @variable)
(inline_blackboard_decl name: (identifier) @variable)

(global_const_decl name: (identifier) @constant)
(local_const_decl name: (identifier) @constant)

; ---------------------------------------------------------------------------
; Type names
; ---------------------------------------------------------------------------
(primary_type (identifier) @type)
(extern_port type: (_) @type)
(param_decl type: (_) @type)
(global_blackboard_decl type: (_) @type)
(global_const_decl type: (_) @type)
(blackboard_decl type: (_) @type)
(local_const_decl type: (_) @type)

; Arguments (named keys)
(argument name: (identifier) @property)

; ---------------------------------------------------------------------------
; Literals
; ---------------------------------------------------------------------------
(string) @string
(integer) @number
(float) @number.float
(boolean) @constant.builtin
(null) @constant.builtin

; ---------------------------------------------------------------------------
; Operators
; ---------------------------------------------------------------------------
["=" "+=" "-=" "*=" "/="] @operator
["+" "-" "*" "/" "%"] @operator
["==" "!=" "<" "<=" ">" ">="] @operator
["&&" "||" "&" "|" "!"] @operator
"as" @operator

; ---------------------------------------------------------------------------
; Punctuation
; ---------------------------------------------------------------------------
["(" ")" "{" "}" "[" "]" ","] @punctuation.bracket
[":" ";" "=" "<" ">"] @punctuation.delimiter

; ---------------------------------------------------------------------------
; Comments
; ---------------------------------------------------------------------------
(line_comment) @comment
(block_comment) @comment
(outer_doc) @comment.documentation
(inner_doc) @comment.documentation
