; Syntax highlighting for BT DSL

; Keywords
["import" "declare" "var" "Tree"] @keyword

; Port directions
["in" "out" "ref"] @keyword.modifier

; Node categories (validated in semantics)
(declare_stmt category: (identifier) @type)

; Tree and declare names
(tree_def name: (identifier) @function.definition)
(declare_stmt name: (identifier) @function.definition)

; Node calls
(node_stmt name: (identifier) @function.call)

; Decorators
(decorator "@" @punctuation.special)
(decorator name: (identifier) @attribute)

; Parameters and variables
(param_decl name: (identifier) @variable.parameter)
(local_var_decl name: (identifier) @variable)
(global_var_decl name: (identifier) @variable)

; Type annotations
(declare_port type: (identifier) @type)
(param_decl type: (identifier) @type)
(local_var_decl type: (identifier) @type)
(global_var_decl type: (identifier) @type)

; Arguments
(argument name: (identifier) @property)

; Blackboard references
(blackboard_ref name: (identifier) @variable)

; Literals
(string) @string
(integer) @number
(float) @number.float
(boolean) @constant.builtin

; Operators
["=" "+=" "-=" "*=" "/="] @operator
["+" "-" "*" "/" "%"] @operator
["==" "!=" "<" "<=" ">" ">="] @operator
["&&" "||" "&" "|" "!"] @operator

; Punctuation
["(" ")" "{" "}" ","] @punctuation.bracket
":" @punctuation.delimiter

; Comments
(line_comment) @comment
(block_comment) @comment
(outer_doc) @comment.documentation
(inner_doc) @comment.documentation
