// Tree-sitter grammar for BT DSL
// Generates parser for BehaviorTree DSL files (.bt)

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

module.exports = grammar({
  name: 'bt_dsl',

  extras: ($) => [/\s/, $.comment],

  word: ($) => $.identifier,

  rules: {
    // ========================================================
    // 1. Program (entry point)
    // ========================================================
    program: ($) =>
      seq(
        repeat($.inner_doc),
        repeat($.import_stmt),
        repeat($.extern_type_stmt),
        repeat($.type_alias_stmt),
        repeat($.extern_stmt),
        repeat(choice($.global_blackboard_decl, $.global_const_decl)),
        repeat($.tree_def),
      ),

    // ========================================================
    // 2. Import statement
    // ========================================================
    import_stmt: ($) => seq('import', field('path', $.string)),

    // ========================================================
    // 3. Types
    // ========================================================
    type: ($) => seq($.base_type, optional('?')),

    base_type: ($) =>
      choice($.primary_type, $.static_array_type, $.dynamic_array_type, $.infer_type),

    primary_type: ($) => choice($.identifier, $.bounded_string),

    bounded_string: ($) => seq('string', '<=', field('max_len', $.integer)),

    infer_type: ($) => '_',

    static_array_type: ($) =>
      seq('[', field('element', $.type), ';', field('size', $.array_size_spec), ']'),

    array_size_spec: ($) =>
      choice(field('exact', $.array_size), seq('<=', field('max', $.array_size))),

    array_size: ($) => choice($.integer, $.identifier),

    dynamic_array_type: ($) => seq('vec', '<', field('element', $.type), '>'),

    // ========================================================
    // 4. Extern / type alias
    // ========================================================
    extern_type_stmt: ($) =>
      seq(repeat($.outer_doc), 'extern', 'type', field('name', $.identifier), ';'),

    type_alias_stmt: ($) =>
      seq(
        repeat($.outer_doc),
        'type',
        field('name', $.identifier),
        '=',
        field('value', $.type),
        ';',
      ),

    extern_stmt: ($) =>
      seq(repeat($.outer_doc), optional($.behavior_attr), 'extern', field('def', $.extern_def)),

    behavior_attr: ($) =>
      seq(
        '#[',
        'behavior',
        '(',
        field('data', $.data_policy),
        optional(seq(',', field('flow', $.flow_policy))),
        ')',
        ']',
      ),

    data_policy: ($) => choice('All', 'Any', 'None'),

    flow_policy: ($) => choice('Chained', 'Isolated'),

    extern_def: ($) =>
      choice(
        seq('action', field('name', $.identifier), '(', optional($.extern_port_list), ')', ';'),
        seq('subtree', field('name', $.identifier), '(', optional($.extern_port_list), ')', ';'),
        seq('condition', field('name', $.identifier), '(', optional($.extern_port_list), ')', ';'),
        seq(
          'control',
          field('name', $.identifier),
          optional(seq('(', optional($.extern_port_list), ')')),
          ';',
        ),
        seq(
          'decorator',
          field('name', $.identifier),
          optional(seq('(', optional($.extern_port_list), ')')),
          ';',
        ),
      ),

    extern_port_list: ($) => seq($.extern_port, repeat(seq(',', $.extern_port))),

    extern_port: ($) =>
      seq(
        repeat($.outer_doc),
        optional($.port_direction),
        field('name', $.identifier),
        ':',
        field('type', $.type),
        optional(seq('=', field('default', $.const_expr))),
      ),

    port_direction: ($) => choice('in', 'out', 'ref', 'mut'),

    // ========================================================
    // 5. Global declarations
    // ========================================================
    global_blackboard_decl: ($) =>
      seq(
        'var',
        field('name', $.identifier),
        optional(seq(':', field('type', $.type))),
        optional(seq('=', field('init', $.expression))),
      ),

    global_const_decl: ($) =>
      seq(
        'const',
        field('name', $.identifier),
        optional(seq(':', field('type', $.type))),
        '=',
        field('value', $.const_expr),
      ),

    // ========================================================
    // 6. Tree definition
    // ========================================================
    tree_def: ($) =>
      seq(
        repeat($.outer_doc),
        'tree',
        field('name', $.identifier),
        '(',
        optional($.param_list),
        ')',
        field('body', $.tree_body),
      ),

    tree_body: ($) => seq('{', repeat($.statement), '}'),

    param_list: ($) => seq($.param_decl, repeat(seq(',', $.param_decl))),

    param_decl: ($) =>
      seq(
        optional($.port_direction),
        field('name', $.identifier),
        ':',
        field('type', $.type),
        optional(seq('=', field('default', $.const_expr))),
      ),

    // ========================================================
    // 7. Statements
    // ========================================================
    statement: ($) => choice(seq($.simple_stmt, ';'), $.block_stmt),

    simple_stmt: ($) =>
      choice($.leaf_node_call, $.assignment_stmt, $.blackboard_decl, $.local_const_decl),

    block_stmt: ($) => $.compound_node_call,

    blackboard_decl: ($) =>
      seq(
        'var',
        field('name', $.identifier),
        optional(seq(':', field('type', $.type))),
        optional(seq('=', field('init', $.expression))),
      ),

    local_const_decl: ($) =>
      seq(
        'const',
        field('name', $.identifier),
        optional(seq(':', field('type', $.type))),
        '=',
        field('value', $.const_expr),
      ),

    leaf_node_call: ($) =>
      seq(
        repeat($.outer_doc),
        optional($.precondition_list),
        field('name', $.identifier),
        field('args', $.property_block),
      ),

    compound_node_call: ($) =>
      seq(
        repeat($.outer_doc),
        optional($.precondition_list),
        field('name', $.identifier),
        field('body', $.node_body_with_children),
      ),

    node_body_with_children: ($) =>
      choice(seq($.property_block, $.children_block), $.children_block),

    precondition_list: ($) => seq($.precondition, repeat($.precondition)),

    precondition: ($) =>
      seq('@', field('kind', $.precond_kind), '(', field('cond', $.expression), ')'),

    precond_kind: ($) => choice('success_if', 'failure_if', 'skip_if', 'run_while', 'guard'),

    // ========================================================
    // 8. Node arguments / children
    // ========================================================
    property_block: ($) => seq('(', optional($.argument_list), ')'),

    argument_list: ($) => seq($.argument, repeat(seq(',', $.argument))),

    argument: ($) =>
      choice(
        seq(field('name', $.identifier), ':', field('value', $.argument_expr)),
        field('value', $.argument_expr),
      ),

    argument_expr: ($) =>
      choice(
        seq('out', field('inline_decl', $.inline_blackboard_decl)),
        seq(optional($.port_direction), field('value', $.expression)),
      ),

    inline_blackboard_decl: ($) => seq('var', field('name', $.identifier)),

    children_block: ($) => seq('{', repeat($.statement), '}'),

    // ========================================================
    // 9. Expressions
    // ========================================================
    assignment_stmt: ($) =>
      seq(field('target', $.lvalue), field('op', $.assignment_op), field('value', $.expression)),

    lvalue: ($) => seq(field('base', $.identifier), repeat(field('index', $.index_suffix))),

    assignment_op: ($) => choice('=', '+=', '-=', '*=', '/='),

    expression: ($) => $.or_expr,

    or_expr: ($) => prec.left(3, seq($.and_expr, repeat(seq('||', $.and_expr)))),

    and_expr: ($) => prec.left(4, seq($.bitwise_or_expr, repeat(seq('&&', $.bitwise_or_expr)))),

    bitwise_or_expr: ($) =>
      prec.left(5, seq($.bitwise_and_expr, repeat(seq('|', $.bitwise_and_expr)))),

    bitwise_and_expr: ($) => prec.left(6, seq($.equality_expr, repeat(seq('&', $.equality_expr)))),

    equality_expr: ($) =>
      prec.left(7, seq($.comparison_expr, optional(seq(choice('==', '!='), $.comparison_expr)))),

    comparison_expr: ($) =>
      prec.left(
        8,
        seq($.additive_expr, optional(seq(choice('<', '<=', '>', '>='), $.additive_expr))),
      ),

    additive_expr: ($) =>
      prec.left(
        9,
        seq($.multiplicative_expr, repeat(seq(choice('+', '-'), $.multiplicative_expr))),
      ),

    multiplicative_expr: ($) =>
      prec.left(10, seq($.cast_expr, repeat(seq(choice('*', '/', '%'), $.cast_expr)))),

    cast_expr: ($) => prec.left(11, seq($.unary_expr, optional(seq('as', $.type)))),

    unary_expr: ($) => choice(seq(choice('!', '-'), $.unary_expr), $.primary_expr),

    primary_expr: ($) =>
      seq(
        choice(seq('(', $.expression, ')'), $.literal, $.array_literal, $.vec_macro, $.identifier),
        repeat($.index_suffix),
      ),

    vec_macro: ($) => seq('vec!', $.array_literal),

    index_suffix: ($) => seq('[', $.expression, ']'),

    array_literal: ($) => seq('[', optional(choice($.repeat_init, $.element_list)), ']'),

    element_list: ($) => seq($.expression, repeat(seq(',', $.expression)), optional(',')),

    repeat_init: ($) => seq($.expression, ';', $.expression),

    // ========================================================
    // 10. Constant expressions
    // ========================================================
    const_expr: ($) => $.expression,

    // ========================================================
    // 11. Literals
    // ========================================================
    literal: ($) => choice($.string, $.float, $.integer, $.boolean, $.null),

    string: ($) => /"([^"\\]|\\.)*"/,

    float: ($) => token(prec(2, /-?[0-9]+\.[0-9]+/)),

    integer: ($) => token(prec(1, /-?(0x[0-9a-fA-F]+|0b[01]+|0o[0-7]+|0|[1-9][0-9]*)/)),

    boolean: ($) => choice('true', 'false'),

    null: ($) => 'null',

    // ========================================================
    // 12. Comments and documentation
    // ========================================================
    comment: ($) => choice($.line_comment, $.block_comment),

    line_comment: ($) => token(seq('//', /[^/!][^\n]*/)),

    block_comment: ($) => token(seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/')),

    outer_doc: ($) => token(seq('///', /[^\n]*/)),

    inner_doc: ($) => token(seq('//!', /[^\n]*/)),

    // ========================================================
    // 13. Identifier
    // ========================================================
    identifier: ($) => /[a-zA-Z_][a-zA-Z0-9_]*/,
  },
});
