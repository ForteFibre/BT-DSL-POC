; Local scope definitions for BT DSL

; Tree definitions create a scope
(tree_def) @scope

; Parameters are definitions in tree scope
(param_decl
  name: (identifier) @definition.parameter)

; Local blackboard vars / inline decls
(blackboard_decl
  name: (identifier) @definition.var)

(inline_blackboard_decl
  name: (identifier) @definition.var)

; Local consts
(local_const_decl
  name: (identifier) @definition.constant)

; Global vars / consts
(global_blackboard_decl
  name: (identifier) @definition.var)

(global_const_decl
  name: (identifier) @definition.constant)

; Assignment targets are references
(assignment_stmt
  target: (lvalue
    base: (identifier) @reference))

; Variable references in expressions
(primary_expr
  (identifier) @reference)
