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
        repeat($.declare_stmt),
        repeat($.global_var_decl),
        repeat($.tree_def),
      ),

    // ========================================================
    // 2. Import statement
    // ========================================================
    import_stmt: ($) => seq('import', field('path', $.string)),

    // ========================================================
    // 3. Declare statement
    // ========================================================
    declare_stmt: ($) =>
      seq(
        repeat($.outer_doc),
        'declare',
        field('category', $.identifier),
        field('name', $.identifier),
        '(',
        optional($.declare_port_list),
        ')',
      ),

    declare_port_list: ($) => seq($.declare_port, repeat(seq(',', $.declare_port))),

    declare_port: ($) =>
      seq(
        repeat($.outer_doc),
        optional($.port_direction),
        field('name', $.identifier),
        ':',
        field('type', $.identifier),
      ),

    port_direction: ($) => choice('in', 'out', 'ref'),

    // ========================================================
    // 4. Global variable declaration
    // ========================================================
    global_var_decl: ($) =>
      seq('var', field('name', $.identifier), ':', field('type', $.identifier)),

    // ========================================================
    // 5. Tree definition
    // ========================================================
    tree_def: ($) =>
      seq(
        repeat($.outer_doc),
        'Tree',
        field('name', $.identifier),
        '(',
        optional($.param_list),
        ')',
        '{',
        repeat($.local_var_decl),
        optional(field('body', $.node_stmt)),
        '}',
      ),

    param_list: ($) => seq($.param_decl, repeat(seq(',', $.param_decl))),

    param_decl: ($) =>
      seq(
        optional($.port_direction),
        field('name', $.identifier),
        optional(seq(':', field('type', $.identifier))),
      ),

    local_var_decl: ($) =>
      seq(
        'var',
        field('name', $.identifier),
        optional(seq(':', field('type', $.identifier))),
        optional(seq('=', field('init', $.expression))),
      ),

    // ========================================================
    // 6. Node statement
    // ========================================================
    node_stmt: ($) =>
      seq(
        repeat($.outer_doc),
        repeat($.decorator),
        field('name', $.identifier),
        choice(
          // Node with children
          seq(optional($.property_block), $.children_block),
          // Node without children (must have parentheses)
          $.property_block,
        ),
      ),

    decorator: ($) => seq('@', field('name', $.identifier), optional($.property_block)),

    property_block: ($) => seq('(', optional($.argument_list), ')'),

    argument_list: ($) => seq($.argument, repeat(seq(',', $.argument))),

    argument: ($) =>
      choice(
        // Named argument
        seq(field('name', $.identifier), ':', field('value', $.value_expr)),
        // Positional argument
        field('value', $.value_expr),
      ),

    children_block: ($) => seq('{', repeat(choice($.node_stmt, $.expression_stmt)), '}'),

    // ========================================================
    // 7. Values and expressions
    // ========================================================
    value_expr: ($) => choice($.blackboard_ref, $.literal),

    blackboard_ref: ($) => seq(optional($.port_direction), field('name', $.identifier)),

    expression_stmt: ($) => $.assignment_expr,

    assignment_expr: ($) =>
      seq(
        field('target', $.identifier),
        field('op', $.assignment_op),
        field('value', $.expression),
      ),

    assignment_op: ($) => choice('=', '+=', '-=', '*=', '/='),

    expression: ($) => $.or_expr,

    or_expr: ($) => prec.left(1, seq($.and_expr, repeat(seq('||', $.and_expr)))),

    and_expr: ($) => prec.left(2, seq($.bitwise_or_expr, repeat(seq('&&', $.bitwise_or_expr)))),

    bitwise_or_expr: ($) =>
      prec.left(3, seq($.bitwise_and_expr, repeat(seq('|', $.bitwise_and_expr)))),

    bitwise_and_expr: ($) => prec.left(4, seq($.equality_expr, repeat(seq('&', $.equality_expr)))),

    equality_expr: ($) =>
      prec.left(5, seq($.comparison_expr, optional(seq(choice('==', '!='), $.comparison_expr)))),

    comparison_expr: ($) =>
      prec.left(
        6,
        seq($.additive_expr, optional(seq(choice('<', '<=', '>', '>='), $.additive_expr))),
      ),

    additive_expr: ($) =>
      prec.left(
        7,
        seq($.multiplicative_expr, repeat(seq(choice('+', '-'), $.multiplicative_expr))),
      ),

    multiplicative_expr: ($) =>
      prec.left(8, seq($.unary_expr, repeat(seq(choice('*', '/', '%'), $.unary_expr)))),

    unary_expr: ($) => choice(seq(choice('!', '-'), $.unary_expr), $.primary_expr),

    primary_expr: ($) => choice(seq('(', $.expression, ')'), $.literal, $.identifier),

    // ========================================================
    // 8. Literals
    // ========================================================
    literal: ($) => choice($.string, $.float, $.integer, $.boolean),

    string: ($) => /"([^"\\]|\\.)*"/,

    float: ($) => /-?[0-9]+\.[0-9]+/,

    integer: ($) => /-?[0-9]+/,

    boolean: ($) => choice('true', 'false'),

    // ========================================================
    // 9. Comments and documentation
    // ========================================================
    comment: ($) => choice($.line_comment, $.block_comment),

    line_comment: ($) => token(seq('//', /[^/!][^\n]*/)),

    block_comment: ($) => token(seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/')),

    outer_doc: ($) => token(seq('///', /[^\n]*/)),

    inner_doc: ($) => token(seq('//!', /[^\n]*/)),

    // ========================================================
    // 10. Identifier
    // ========================================================
    identifier: ($) => /[a-zA-Z_][a-zA-Z0-9_]*/,
  },
});
