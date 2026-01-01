; completion_context.scm
; Query patterns used to classify completion context (ports/vars/nodes/decorators/keywords)
;
; NOTE: The C++ core embeds a copy of this query string. Keep them in sync.

; ---------------------------------------------------------------------------
; Top-level / tree
; ---------------------------------------------------------------------------

(tree_def
  "tree"
  name: (identifier) @bt.tree.name
  (tree_body
    "{" @bt.tree.lbrace
    "}" @bt.tree.rbrace))

(import_stmt
  "import"
  path: (string) @bt.import.path)

; ---------------------------------------------------------------------------
; Node / precondition names
; ---------------------------------------------------------------------------

; '@' token (common during incomplete typing, may not yet be part of a precondition node)
"@" @bt.precondition.at

; Precondition '@' token (useful while the kind is not formed yet)
(precondition
  "@" @bt.precondition.at)

(precondition
  "@" @bt.precondition.at
  kind: (precond_kind) @bt.precondition.kind)

(leaf_node_call
  name: (identifier) @bt.node.name)

(compound_node_call
  name: (identifier) @bt.node.name)

; When cursor is inside a property_block, we also want to know which callable
; (leaf_node_call vs compound_node_call) owns it.

(leaf_node_call
  name: (identifier) @bt.call.node.name
  args: (property_block) @bt.call.args)

(compound_node_call
  name: (identifier) @bt.call.node.name
  body: (node_body_with_children
    (property_block) @bt.call.args))

; ---------------------------------------------------------------------------
; Arguments
; ---------------------------------------------------------------------------

(property_block) @bt.args

(argument
  name: (identifier) @bt.arg.name
  ":" @bt.arg.colon
  value: (_) @bt.arg.value)

; Named arg while value is not formed yet (common while typing)
(argument
  name: (identifier) @bt.arg.name
  ":" @bt.arg.colon)

; Blackboard refs / directions

(argument_expr
  (port_direction) @bt.port.direction)

(inline_blackboard_decl
  name: (identifier) @bt.bb.name)

; Punctuation tokens (filtered by range in the classifier)
":" @bt.punct.colon
"," @bt.punct.comma
"(" @bt.punct.lparen
")" @bt.punct.rparen
