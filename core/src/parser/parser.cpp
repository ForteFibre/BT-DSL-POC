// parser.cpp - Tree-sitter to AST conversion implementation (C++17 compatible)
#include "bt_dsl/parser/parser.hpp"

#include <tree_sitter/api.h>

#include <charconv>
#include <cstring>

// C++17 compatible starts_with helper
namespace
{
inline bool starts_with(const std::string & str, const char * prefix)
{
  return str.size() >= std::strlen(prefix) && str.compare(0, std::strlen(prefix), prefix) == 0;
}
}  // namespace

// External declaration for Tree-sitter BT-DSL language
extern "C" const TSLanguage * tree_sitter_bt_dsl();

namespace bt_dsl
{

// ============================================================================
// Helper Functions
// ============================================================================

namespace
{

/**
 * Get text content of a TSNode from source.
 */
std::string get_node_text(TSNode node, std::string_view source)
{
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  return std::string(source.substr(start, end - start));
}

/**
 * Get SourceRange from TSNode.
 */
SourceRange get_source_range(TSNode node)
{
  const TSPoint start = ts_node_start_point(node);
  const TSPoint end = ts_node_end_point(node);
  SourceRange range;
  range.start_line = start.row + 1;  // 1-indexed
  range.start_column = start.column + 1;
  range.end_line = end.row + 1;
  range.end_column = end.column + 1;
  range.start_byte = ts_node_start_byte(node);
  range.end_byte = ts_node_end_byte(node);
  return range;
}

/**
 * Get child node by field name.
 */
TSNode get_child_by_field(TSNode node, const char * field_name)
{
  return ts_node_child_by_field_name(
    node, field_name, static_cast<uint32_t>(std::strlen(field_name)));
}

/**
 * Check if node type matches.
 */
bool is_node_type(TSNode node, const char * type)
{
  return std::strcmp(ts_node_type(node), type) == 0;
}

/**
 * Parse PortDirection from string.
 */
std::optional<PortDirection> parse_direction(std::string_view text)
{
  if (text == "in") return PortDirection::In;
  if (text == "out") return PortDirection::Out;
  if (text == "ref") return PortDirection::Ref;
  return std::nullopt;
}

/**
 * Parse BinaryOp from string.
 */
std::optional<BinaryOp> parse_binary_op(std::string_view text)
{
  if (text == "+") return BinaryOp::Add;
  if (text == "-") return BinaryOp::Sub;
  if (text == "*") return BinaryOp::Mul;
  if (text == "/") return BinaryOp::Div;
  if (text == "%") return BinaryOp::Mod;
  if (text == "==") return BinaryOp::Eq;
  if (text == "!=") return BinaryOp::Ne;
  if (text == "<") return BinaryOp::Lt;
  if (text == "<=") return BinaryOp::Le;
  if (text == ">") return BinaryOp::Gt;
  if (text == ">=") return BinaryOp::Ge;
  if (text == "&&") return BinaryOp::And;
  if (text == "||") return BinaryOp::Or;
  if (text == "&") return BinaryOp::BitAnd;
  if (text == "|") return BinaryOp::BitOr;
  return std::nullopt;
}

/**
 * Parse UnaryOp from string.
 */
std::optional<UnaryOp> parse_unary_op(std::string_view text)
{
  if (text == "!") return UnaryOp::Not;
  if (text == "-") return UnaryOp::Neg;
  return std::nullopt;
}

/**
 * Parse AssignOp from string.
 */
std::optional<AssignOp> parse_assign_op(std::string_view text)
{
  if (text == "=") return AssignOp::Assign;
  if (text == "+=") return AssignOp::AddAssign;
  if (text == "-=") return AssignOp::SubAssign;
  if (text == "*=") return AssignOp::MulAssign;
  if (text == "/=") return AssignOp::DivAssign;
  return std::nullopt;
}

/**
 * Unescape a string literal (remove quotes and handle escape sequences).
 */
std::string unescape_string(std::string_view text)
{
  // Remove surrounding quotes
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = text.substr(1, text.size() - 2);
  }

  std::string result;
  result.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      switch (text[i + 1]) {
        case 'n':
          result += '\n';
          ++i;
          break;
        case 't':
          result += '\t';
          ++i;
          break;
        case 'r':
          result += '\r';
          ++i;
          break;
        case '\\':
          result += '\\';
          ++i;
          break;
        case '"':
          result += '"';
          ++i;
          break;
        default:
          result += text[i];
          break;
      }
    } else {
      result += text[i];
    }
  }

  return result;
}

/**
 * Collect all children of specific type.
 */
std::vector<TSNode> collect_children_of_type(TSNode node, const char * type)
{
  std::vector<TSNode> result;
  const uint32_t child_count = ts_node_child_count(node);
  for (uint32_t i = 0; i < child_count; ++i) {
    const TSNode child = ts_node_child(node, i);
    if (is_node_type(child, type)) {
      result.emplace_back(child);
    }
  }
  return result;
}

/**
 * Create ParseError helper.
 */
ParseError make_error(
  const std::string & message, SourceRange range,
  ParseError::Severity severity = ParseError::Severity::Error)
{
  ParseError err;
  err.message = message;
  err.range = range;
  err.severity = severity;
  return err;
}

/**
 * Create VarRef helper.
 */
VarRef make_var_ref(
  const std::string & name, std::optional<PortDirection> direction, SourceRange range)
{
  VarRef ref;
  ref.name = name;
  ref.direction = direction;
  ref.range = range;
  return ref;
}

/**
 * Create IntLiteral helper.
 */
IntLiteral make_int_literal(int64_t value, SourceRange range)
{
  IntLiteral lit;
  lit.value = value;
  lit.range = range;
  return lit;
}

/**
 * Create StringLiteral helper.
 */
StringLiteral make_string_literal(const std::string & value, SourceRange range)
{
  StringLiteral lit;
  lit.value = value;
  lit.range = range;
  return lit;
}

/**
 * Create FloatLiteral helper.
 */
FloatLiteral make_float_literal(double value, SourceRange range)
{
  FloatLiteral lit;
  lit.value = value;
  lit.range = range;
  return lit;
}

/**
 * Create BoolLiteral helper.
 */
BoolLiteral make_bool_literal(bool value, SourceRange range)
{
  BoolLiteral lit;
  lit.value = value;
  lit.range = range;
  return lit;
}

/**
 * Create BinaryExpr helper.
 */
BinaryExpr make_binary_expr(Expression left, BinaryOp op, Expression right, SourceRange range)
{
  BinaryExpr expr;
  expr.left = std::move(left);
  expr.op = op;
  expr.right = std::move(right);
  expr.range = range;
  return expr;
}

/**
 * Create UnaryExpr helper.
 */
UnaryExpr make_unary_expr(UnaryOp op, Expression operand, SourceRange range)
{
  UnaryExpr expr;
  expr.op = op;
  expr.operand = std::move(operand);
  expr.range = range;
  return expr;
}

}  // anonymous namespace

// ============================================================================
// Parser Implementation
// ============================================================================

struct Parser::Impl
{
  TSParser * parser_ = nullptr;
  std::string_view source_;
  std::vector<ParseError> errors_;

  Impl()
  {
    parser_ = ts_parser_new();
    ts_parser_set_language(parser_, tree_sitter_bt_dsl());
  }

  ~Impl()
  {
    if (parser_) {
      ts_parser_delete(parser_);
    }
  }

  void reset()
  {
    errors_.clear();
    ts_parser_reset(parser_);
  }

  ParseResult<Program> parse(std::string_view source)
  {
    source_ = source;
    errors_.clear();

    TSTree * tree =
      ts_parser_parse_string(parser_, nullptr, source.data(), static_cast<uint32_t>(source.size()));

    if (!tree) {
      errors_.emplace_back(make_error("Failed to parse source", SourceRange{}));
      return ParseResult<Program>{std::move(errors_)};
    }

    const TSNode root_node = ts_tree_root_node(tree);

    // Check for parse errors
    collect_errors(root_node);

    Program program = build_program(root_node);

    ts_tree_delete(tree);

    if (!errors_.empty()) {
      return ParseResult<Program>{std::move(errors_)};
    }

    return program;
  }

  std::pair<Program, std::vector<ParseError>> parse_with_recovery(std::string_view source)
  {
    source_ = source;
    errors_.clear();

    TSTree * tree =
      ts_parser_parse_string(parser_, nullptr, source.data(), static_cast<uint32_t>(source.size()));

    Program program;

    if (tree) {
      const TSNode root_node = ts_tree_root_node(tree);
      collect_errors(root_node);
      program = build_program(root_node);
      ts_tree_delete(tree);
    } else {
      errors_.emplace_back(make_error("Failed to parse source", SourceRange{}));
    }

    return {std::move(program), std::move(errors_)};
  }

private:
  void collect_errors(TSNode node)
  {
    if (ts_node_has_error(node)) {
      if (ts_node_is_error(node) || ts_node_is_missing(node)) {
        const std::string msg =
          ts_node_is_missing(node) ? "Missing expected syntax" : "Syntax error";
        errors_.emplace_back(make_error(msg, get_source_range(node)));
      }

      // Recurse into children
      const uint32_t child_count = ts_node_child_count(node);
      for (uint32_t i = 0; i < child_count; ++i) {
        collect_errors(ts_node_child(node, i));
      }
    }
  }

  [[nodiscard]] Program build_program(TSNode node) const
  {
    Program program;
    program.range = get_source_range(node);

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      const char * type = ts_node_type(child);

      if (std::strcmp(type, "inner_doc") == 0) {
        std::string doc = get_node_text(child, source_);
        // Remove "//!" prefix
        if (starts_with(doc, "//!")) {
          doc = doc.substr(3);
        }
        program.inner_docs.push_back(std::move(doc));
      } else if (std::strcmp(type, "import_stmt") == 0) {
        program.imports.emplace_back(build_import_stmt(child));
      } else if (std::strcmp(type, "declare_stmt") == 0) {
        program.declarations.emplace_back(build_declare_stmt(child));
      } else if (std::strcmp(type, "global_var_decl") == 0) {
        program.global_vars.emplace_back(build_global_var_decl(child));
      } else if (std::strcmp(type, "tree_def") == 0) {
        program.trees.emplace_back(build_tree_def(child));
      }
    }

    return program;
  }

  [[nodiscard]] ImportStmt build_import_stmt(TSNode node) const
  {
    ImportStmt stmt;
    stmt.range = get_source_range(node);

    const TSNode path_node = get_child_by_field(node, "path");
    if (!ts_node_is_null(path_node)) {
      stmt.path = unescape_string(get_node_text(path_node, source_));
    }

    return stmt;
  }

  [[nodiscard]] DeclareStmt build_declare_stmt(TSNode node) const
  {
    DeclareStmt stmt;
    stmt.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      stmt.docs.push_back(std::move(doc));
    }

    const TSNode category_node = get_child_by_field(node, "category");
    if (!ts_node_is_null(category_node)) {
      stmt.category = get_node_text(category_node, source_);
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      stmt.name = get_node_text(name_node, source_);
    }

    // Find declare_port_list
    for (auto port_list : collect_children_of_type(node, "declare_port_list")) {
      for (auto port_node : collect_children_of_type(port_list, "declare_port")) {
        stmt.ports.emplace_back(build_declare_port(port_node));
      }
    }

    return stmt;
  }

  [[nodiscard]] DeclarePort build_declare_port(TSNode node) const
  {
    DeclarePort port;
    port.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      port.docs.push_back(std::move(doc));
    }

    // Check for direction
    for (auto dir_node : collect_children_of_type(node, "port_direction")) {
      port.direction = parse_direction(get_node_text(dir_node, source_));
      break;  // Only first one
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      port.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      port.type_name = get_node_text(type_node, source_);
    }

    return port;
  }

  [[nodiscard]] GlobalVarDecl build_global_var_decl(TSNode node) const
  {
    GlobalVarDecl decl;
    decl.range = get_source_range(node);

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      decl.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      decl.type_name = get_node_text(type_node, source_);
    }

    return decl;
  }

  [[nodiscard]] TreeDef build_tree_def(TSNode node) const
  {
    TreeDef tree;
    tree.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      tree.docs.push_back(std::move(doc));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      tree.name = get_node_text(name_node, source_);
    }

    // Parse param_list
    for (auto param_list : collect_children_of_type(node, "param_list")) {
      for (auto param_node : collect_children_of_type(param_list, "param_decl")) {
        tree.params.emplace_back(build_param_decl(param_node));
      }
    }

    // Parse local_var_decl
    for (auto var_node : collect_children_of_type(node, "local_var_decl")) {
      tree.local_vars.emplace_back(build_local_var_decl(var_node));
    }

    // Parse body (node_stmt)
    const TSNode body_node = get_child_by_field(node, "body");
    if (!ts_node_is_null(body_node)) {
      tree.body = build_node_stmt(body_node);
    }

    return tree;
  }

  [[nodiscard]] ParamDecl build_param_decl(TSNode node) const
  {
    ParamDecl param;
    param.range = get_source_range(node);

    // Check for direction
    for (auto dir_node : collect_children_of_type(node, "port_direction")) {
      param.direction = parse_direction(get_node_text(dir_node, source_));
      break;
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      param.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      param.type_name = get_node_text(type_node, source_);
    }

    return param;
  }

  [[nodiscard]] LocalVarDecl build_local_var_decl(TSNode node) const
  {
    LocalVarDecl decl;
    decl.range = get_source_range(node);

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      decl.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      decl.type_name = get_node_text(type_node, source_);
    }

    const TSNode init_node = get_child_by_field(node, "init");
    if (!ts_node_is_null(init_node)) {
      decl.initial_value = build_expression(init_node);
    }

    return decl;
  }

  [[nodiscard]] NodeStmt build_node_stmt(TSNode node) const
  {
    NodeStmt stmt;
    stmt.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      stmt.docs.push_back(std::move(doc));
    }

    // Collect decorators
    for (auto dec_node : collect_children_of_type(node, "decorator")) {
      stmt.decorators.emplace_back(build_decorator(dec_node));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      stmt.node_name = get_node_text(name_node, source_);
    }

    // Parse property_block (arguments)
    for (auto prop_node : collect_children_of_type(node, "property_block")) {
      for (auto arg_list : collect_children_of_type(prop_node, "argument_list")) {
        for (auto arg_node : collect_children_of_type(arg_list, "argument")) {
          stmt.args.emplace_back(build_argument(arg_node));
        }
      }
    }

    // Parse children_block
    for (auto children_node : collect_children_of_type(node, "children_block")) {
      stmt.has_children_block = true;
      parse_children_block(children_node, stmt.children);
    }

    return stmt;
  }

  [[nodiscard]] Decorator build_decorator(TSNode node) const
  {
    Decorator dec;
    dec.range = get_source_range(node);

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      dec.name = get_node_text(name_node, source_);
    }

    // Parse property_block (arguments)
    for (auto prop_node : collect_children_of_type(node, "property_block")) {
      for (auto arg_list : collect_children_of_type(prop_node, "argument_list")) {
        for (auto arg_node : collect_children_of_type(arg_list, "argument")) {
          dec.args.emplace_back(build_argument(arg_node));
        }
      }
    }

    return dec;
  }

  [[nodiscard]] Argument build_argument(TSNode node) const
  {
    Argument arg;
    arg.range = get_source_range(node);

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      arg.name = get_node_text(name_node, source_);
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      arg.value = build_value_expr(value_node);
    }

    return arg;
  }

  [[nodiscard]] ValueExpr build_value_expr(TSNode node) const
  {
    // value_expr can be: blackboard_ref | literal
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      const char * type = ts_node_type(child);

      if (std::strcmp(type, "blackboard_ref") == 0) {
        return build_blackboard_ref(child);
      }
      if (std::strcmp(type, "literal") == 0) {
        return build_literal(child);
      }
    }

    // Default: empty blackboard ref (shouldn't happen with valid input)
    return BlackboardRef{};
  }

  [[nodiscard]] BlackboardRef build_blackboard_ref(TSNode node) const
  {
    BlackboardRef ref;
    ref.range = get_source_range(node);

    // Check for direction
    for (auto dir_node : collect_children_of_type(node, "port_direction")) {
      ref.direction = parse_direction(get_node_text(dir_node, source_));
      break;
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      ref.name = get_node_text(name_node, source_);
    }

    return ref;
  }

  void parse_children_block(TSNode node, std::vector<ChildElement> & children) const
  {
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      const char * type = ts_node_type(child);

      if (std::strcmp(type, "node_stmt") == 0) {
        children.emplace_back(Box<NodeStmt>(build_node_stmt(child)));
      } else if (std::strcmp(type, "expression_stmt") == 0) {
        children.emplace_back(build_assignment_stmt(child));
      }
    }
  }

  [[nodiscard]] AssignmentStmt build_assignment_stmt(TSNode node) const
  {
    // expression_stmt contains assignment_expr
    AssignmentStmt stmt;
    stmt.range = get_source_range(node);

    // Find assignment_expr child
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (is_node_type(child, "assignment_expr")) {
        const TSNode target_node = get_child_by_field(child, "target");
        if (!ts_node_is_null(target_node)) {
          stmt.target = get_node_text(target_node, source_);
        }

        const TSNode op_node = get_child_by_field(child, "op");
        if (!ts_node_is_null(op_node)) {
          // assignment_op has the actual operator as child
          const std::string op_text = get_node_text(op_node, source_);
          stmt.op = parse_assign_op(op_text).value_or(AssignOp::Assign);
        }

        const TSNode value_node = get_child_by_field(child, "value");
        if (!ts_node_is_null(value_node)) {
          stmt.value = build_expression(value_node);
        }
        break;
      }
    }

    return stmt;
  }

  [[nodiscard]] Expression build_expression(TSNode node) const
  {
    const char * type = ts_node_type(node);

    // Handle wrapper nodes (expression, or_expr, and_expr, etc.)
    if (
      std::strcmp(type, "expression") == 0 || std::strcmp(type, "or_expr") == 0 ||
      std::strcmp(type, "and_expr") == 0 || std::strcmp(type, "bitwise_or_expr") == 0 ||
      std::strcmp(type, "bitwise_and_expr") == 0 || std::strcmp(type, "equality_expr") == 0 ||
      std::strcmp(type, "comparison_expr") == 0 || std::strcmp(type, "additive_expr") == 0 ||
      std::strcmp(type, "multiplicative_expr") == 0) {
      return build_binary_or_passthrough(node);
    }

    if (std::strcmp(type, "unary_expr") == 0) {
      return build_unary_expr(node);
    }

    if (std::strcmp(type, "primary_expr") == 0) {
      return build_primary_expr(node);
    }

    if (std::strcmp(type, "literal") == 0) {
      return build_literal(node);
    }

    if (std::strcmp(type, "identifier") == 0) {
      return make_var_ref(get_node_text(node, source_), std::nullopt, get_source_range(node));
    }

    // Fallback: try to find any child that can be an expression
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) continue;
      return build_expression(child);
    }

    // Default: empty literal
    return Literal{make_int_literal(0, get_source_range(node))};
  }

  [[nodiscard]] Expression build_binary_or_passthrough(TSNode node) const
  {
    // Check for binary operators
    std::vector<TSNode> operands;
    std::vector<std::string> operators;

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (ts_node_is_named(child)) {
        operands.emplace_back(child);
      } else {
        const std::string text = get_node_text(child, source_);
        if (parse_binary_op(text).has_value()) {
          operators.emplace_back(text);
        }
      }
    }

    if (operands.size() == 1) {
      // Passthrough - no binary operation at this level
      return build_expression(operands[0]);
    }

    // Build left-associative binary expression
    Expression result = build_expression(operands[0]);
    for (size_t i = 0; i < operators.size() && i + 1 < operands.size(); ++i) {
      auto op = parse_binary_op(operators[i]);
      Expression right = build_expression(operands[i + 1]);

      result = Box<BinaryExpr>(make_binary_expr(
        std::move(result), op.value_or(BinaryOp::Add), std::move(right), get_source_range(node)));
    }

    return result;
  }

  [[nodiscard]] Expression build_unary_expr(TSNode node) const
  {
    const uint32_t child_count = ts_node_child_count(node);

    std::optional<UnaryOp> op;
    TSNode operand_node = {};

    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) {
        const std::string text = get_node_text(child, source_);
        if (auto parsed_op = parse_unary_op(text)) {
          op = parsed_op;
        }
      } else {
        operand_node = child;
      }
    }

    if (op.has_value() && !ts_node_is_null(operand_node)) {
      return Box<UnaryExpr>(
        make_unary_expr(op.value(), build_expression(operand_node), get_source_range(node)));
    }

    // No operator found, pass through
    if (!ts_node_is_null(operand_node)) {
      return build_expression(operand_node);
    }

    return Literal{make_int_literal(0, get_source_range(node))};
  }

  [[nodiscard]] Expression build_primary_expr(TSNode node) const
  {
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      const char * type = ts_node_type(child);

      if (std::strcmp(type, "literal") == 0) {
        return build_literal(child);
      }
      if (std::strcmp(type, "identifier") == 0) {
        return make_var_ref(get_node_text(child, source_), std::nullopt, get_source_range(child));
      }
      if (std::strcmp(type, "expression") == 0) {
        return build_expression(child);
      }
    }

    return Literal{make_int_literal(0, get_source_range(node))};
  }

  [[nodiscard]] Literal build_literal(TSNode node) const
  {
    const SourceRange range = get_source_range(node);

    // literal has child: string | float | integer | boolean
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      const char * type = ts_node_type(child);
      const std::string text = get_node_text(child, source_);
      const SourceRange child_range = get_source_range(child);

      if (std::strcmp(type, "string") == 0) {
        return make_string_literal(unescape_string(text), child_range);
      }
      if (std::strcmp(type, "float") == 0) {
        double value = 0.0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return make_float_literal(value, child_range);
      }
      if (std::strcmp(type, "integer") == 0) {
        int64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return make_int_literal(value, child_range);
      }
      if (std::strcmp(type, "boolean") == 0) {
        return make_bool_literal(text == "true", child_range);
      }
    }

    return make_int_literal(0, range);
  }
};

// ============================================================================
// Parser Public Interface
// ============================================================================

Parser::Parser() : impl_(std::make_unique<Impl>()) {}

Parser::~Parser() = default;

Parser::Parser(Parser &&) noexcept = default;
Parser & Parser::operator=(Parser &&) noexcept = default;

ParseResult<Program> Parser::parse(std::string_view source) { return impl_->parse(source); }

std::pair<Program, std::vector<ParseError>> Parser::parse_with_recovery(std::string_view source)
{
  return impl_->parse_with_recovery(source);
}

void Parser::reset() { impl_->reset(); }

}  // namespace bt_dsl
