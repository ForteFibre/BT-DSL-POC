; completion_context.scm
; Query patterns used to classify completion context (ports/vars/nodes/decorators/keywords)
;
; NOTE: The C++ core embeds a copy of this query string. Keep them in sync.

; ---------------------------------------------------------------------------
; Top-level / tree
; ---------------------------------------------------------------------------

(tree_def
  "Tree"
  name: (identifier) @bt.tree.name
  "{" @bt.tree.lbrace
  "}" @bt.tree.rbrace)

(import_stmt
  "import"
  path: (string) @bt.import.path)

; ---------------------------------------------------------------------------
; Node / decorator names
; ---------------------------------------------------------------------------

(decorator
  "@" @bt.decorator.at
  name: (identifier) @bt.decorator.name)

(node_stmt
  name: (identifier) @bt.node.name)

; When cursor is inside a property_block, we also want to know which callable
; (node_stmt vs decorator) owns it.

(node_stmt
  name: (identifier) @bt.call.node.name
  (property_block "(" @bt.call.args.lparen ")" @bt.call.args.rparen))

(decorator
  "@"
  name: (identifier) @bt.call.decorator.name
  (property_block "(" @bt.call.args.lparen ")" @bt.call.args.rparen))

; ---------------------------------------------------------------------------
; Arguments
; ---------------------------------------------------------------------------

(property_block
  "(" @bt.args.lparen
  ")" @bt.args.rparen)

(argument
  name: (identifier) @bt.arg.name
  ":" @bt.arg.colon
  value: (_) @bt.arg.value)

; Named arg while value is not formed yet (common while typing)
(argument
  name: (identifier) @bt.arg.name
  ":" @bt.arg.colon)

; Blackboard refs / directions
(blackboard_ref
  (port_direction) @bt.port.direction
  name: (identifier) @bt.bb.name)

(blackboard_ref
  name: (identifier) @bt.bb.name)

; Punctuation tokens (filtered by range in the classifier)
":" @bt.punct.colon
"," @bt.punct.comma
"(" @bt.punct.lparen
")" @bt.punct.rparen
