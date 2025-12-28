; Local scope definitions for BT DSL

; Tree definitions create a scope
(tree_def) @scope

; Parameters are definitions in tree scope
(param_decl
  name: (identifier) @definition.parameter)

; Local variables are definitions in tree scope
(local_var_decl
  name: (identifier) @definition.var)

; Global variables are definitions at program scope
(global_var_decl
  name: (identifier) @definition.var)

; Blackboard references are references
(blackboard_ref
  name: (identifier) @reference)

; Assignment targets are references
(assignment_expr
  target: (identifier) @reference)

; Variable references in expressions
(primary_expr
  (identifier) @reference)
