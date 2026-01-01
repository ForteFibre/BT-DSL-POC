// parser.cpp - Tree-sitter to AST conversion implementation (C++17 compatible)
#include "bt_dsl/parser/parser.hpp"

#include <tree_sitter/api.h>

#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>

// C++17 compatible starts_with helper
namespace
{
inline bool starts_with(const std::string & str, const char * prefix)
{
  return str.size() >= std::strlen(prefix) && str.compare(0, std::strlen(prefix), prefix) == 0;
}

// Validate UTF-8 per reference lexical-structure.md 1.1.1.
// Returns the index of the first invalid byte (or npos if valid).
size_t first_invalid_utf8_byte(std::string_view s)
{
  const auto is_cont = [](unsigned char c) { return (c & 0b1100'0000) == 0b1000'0000; };
  size_t i = 0;
  while (i < s.size()) {
    const auto c0 = static_cast<unsigned char>(s[i]);
    if (c0 <= 0x7F) {
      i++;
      continue;
    }

    // 2-byte
    if (c0 >= 0xC2 && c0 <= 0xDF) {
      if (i + 1 >= s.size()) return i;
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      if (!is_cont(c1)) return i;
      i += 2;
      continue;
    }

    // 3-byte
    if (c0 >= 0xE0 && c0 <= 0xEF) {
      if (i + 2 >= s.size()) return i;
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      const auto c2 = static_cast<unsigned char>(s[i + 2]);
      if (!is_cont(c1) || !is_cont(c2)) return i;
      // Reject overlongs and surrogates
      if (c0 == 0xE0 && c1 < 0xA0) return i;
      if (c0 == 0xED && c1 >= 0xA0) return i;
      i += 3;
      continue;
    }

    // 4-byte
    if (c0 >= 0xF0 && c0 <= 0xF4) {
      if (i + 3 >= s.size()) return i;
      const auto c1 = static_cast<unsigned char>(s[i + 1]);
      const auto c2 = static_cast<unsigned char>(s[i + 2]);
      const auto c3 = static_cast<unsigned char>(s[i + 3]);
      if (!is_cont(c1) || !is_cont(c2) || !is_cont(c3)) return i;
      // Reject overlongs and > U+10FFFF
      if (c0 == 0xF0 && c1 < 0x90) return i;
      if (c0 == 0xF4 && c1 > 0x8F) return i;
      i += 4;
      continue;
    }

    // Invalid leading byte
    return i;
  }
  return std::string_view::npos;
}

bt_dsl::SourceRange range_at_byte(std::string_view src, size_t byte_index)
{
  bt_dsl::SourceRange r;
  r.start_byte = static_cast<uint32_t>(byte_index);
  r.end_byte = static_cast<uint32_t>(std::min(byte_index + 1, src.size()));

  uint32_t line = 1;
  uint32_t col = 1;
  for (size_t i = 0; i < byte_index && i < src.size(); ++i) {
    const char ch = src[i];
    if (ch == '\n') {
      line++;
      col = 1;
      continue;
    }
    // Treat CRLF as single newline (count at '\n')
    if (ch == '\r') {
      if (i + 1 < byte_index && src[i + 1] == '\n') {
        continue;
      }
      line++;
      col = 1;
      continue;
    }
    col++;
  }

  r.start_line = line;
  r.end_line = line;
  r.start_column = col;
  r.end_column = col;
  return r;
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

void strip_trailing_cr(std::string & s)
{
  // Support CRLF sources: tree-sitter tokens for doc comments use /[^\n]*/ so the
  // captured text may include a trailing '\r'. The reference allows CRLF line
  // endings, and doc/comment contents should not include the line terminator.
  if (!s.empty() && s.back() == '\r') {
    s.pop_back();
  }
}

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
  if (text == "mut") return PortDirection::Mut;
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
  if (text == "^") return BinaryOp::BitXor;
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
  if (text == "%=") return AssignOp::ModAssign;
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

  auto append_utf8 = [](std::string & out, uint32_t cp) {
    // Encode a Unicode scalar value as UTF-8.
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };

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
        case '0':
          result += '\0';
          ++i;
          break;
        case 'b':
          result += '\b';
          ++i;
          break;
        case 'f':
          result += '\f';
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
        case 'u': {
          // Unicode scalar escape: \u{HEX+}
          // Reference: docs/reference/lexical-structure.md 1.4
          if (i + 2 < text.size() && text[i + 2] == '{') {
            size_t j = i + 3;
            uint32_t cp = 0;
            size_t digits = 0;
            for (; j < text.size() && text[j] != '}'; ++j) {
              const char c = text[j];
              uint32_t v = 0;
              if (c >= '0' && c <= '9') {
                v = static_cast<uint32_t>(c - '0');
              } else if (c >= 'a' && c <= 'f') {
                v = static_cast<uint32_t>(10 + (c - 'a'));
              } else if (c >= 'A' && c <= 'F') {
                v = static_cast<uint32_t>(10 + (c - 'A'));
              } else {
                digits = 0;
                break;
              }

              // Prevent overflow beyond Unicode range.
              if (cp > 0x10FFFFU / 16U) {
                digits = 0;
                break;
              }
              cp = (cp << 4) | v;
              ++digits;
            }

            const bool has_closing = (j < text.size() && text[j] == '}');
            const bool is_surrogate = (cp >= 0xD800U && cp <= 0xDFFFU);
            if (has_closing && digits >= 1 && digits <= 6 && cp <= 0x10FFFFU && !is_surrogate) {
              append_utf8(result, cp);
              i = j;
              break;
            }
          }

          // Fallback: keep the backslash literally.
          result += text[i];
          break;
        }
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
 * Create MissingExpr helper.
 */
MissingExpr make_missing_expr(SourceRange range)
{
  MissingExpr m;
  m.range = range;
  return m;
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
  mutable std::vector<ParseError> errors_;

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

    // Reference: docs/reference/lexical-structure.md 1.1.1
    // Reject invalid UTF-8 input up-front as a lexical error.
    if (const size_t bad = first_invalid_utf8_byte(source_); bad != std::string_view::npos) {
      errors_.emplace_back(make_error("Invalid UTF-8 in source", range_at_byte(source_, bad)));
      return ParseResult<Program>{std::move(errors_)};
    }

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

    if (const size_t bad = first_invalid_utf8_byte(source_); bad != std::string_view::npos) {
      errors_.emplace_back(make_error("Invalid UTF-8 in source", range_at_byte(source_, bad)));
      return {Program{}, std::move(errors_)};
    }

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

  // Extract the first named child that is one of the given types.
  [[nodiscard]] static TSNode find_first_named_child_of_any(
    TSNode node, const std::vector<const char *> & types)
  {
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) {
        continue;
      }
      const char * t = ts_node_type(child);
      for (const auto * wanted : types) {
        if (std::strcmp(t, wanted) == 0) {
          return child;
        }
      }
    }
    return TSNode{};
  }

  // Recursively search for the first named descendant of one of the given types.
  [[nodiscard]] static TSNode find_first_named_descendant_of_any(
    TSNode node, const std::vector<const char *> & types, uint32_t depth_limit = 32)
  {
    if (depth_limit == 0) {
      return TSNode{};
    }
    const TSNode direct = find_first_named_child_of_any(node, types);
    if (!ts_node_is_null(direct)) {
      return direct;
    }
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) {
        continue;
      }
      const TSNode found = find_first_named_descendant_of_any(child, types, depth_limit - 1);
      if (!ts_node_is_null(found)) {
        return found;
      }
    }
    return TSNode{};
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
        strip_trailing_cr(doc);
        program.inner_docs.push_back(std::move(doc));
      } else if (std::strcmp(type, "import_stmt") == 0) {
        program.imports.emplace_back(build_import_stmt(child));
      } else if (std::strcmp(type, "extern_type_stmt") == 0) {
        program.extern_types.emplace_back(build_extern_type_stmt(child));
      } else if (std::strcmp(type, "type_alias_stmt") == 0) {
        program.type_aliases.emplace_back(build_type_alias_stmt(child));
      } else if (std::strcmp(type, "extern_stmt") == 0) {
        program.declarations.emplace_back(build_extern_stmt(child));
      } else if (std::strcmp(type, "global_blackboard_decl") == 0) {
        program.global_vars.emplace_back(build_global_var_decl(child));
      } else if (std::strcmp(type, "global_const_decl") == 0) {
        program.global_consts.emplace_back(build_global_const_decl(child));
      } else if (std::strcmp(type, "tree_def") == 0) {
        program.trees.emplace_back(build_tree_def(child));
      }
    }

    return program;
  }

  [[nodiscard]] ExternTypeStmt build_extern_type_stmt(TSNode node) const
  {
    ExternTypeStmt stmt;
    stmt.range = get_source_range(node);

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      stmt.docs.push_back(std::move(doc));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      stmt.name = get_node_text(name_node, source_);
    }

    return stmt;
  }

  [[nodiscard]] TypeAliasStmt build_type_alias_stmt(TSNode node) const
  {
    TypeAliasStmt stmt;
    stmt.range = get_source_range(node);

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      stmt.docs.push_back(std::move(doc));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      stmt.name = get_node_text(name_node, source_);
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      stmt.value = get_node_text(value_node, source_);
    }

    return stmt;
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

  [[nodiscard]] DeclareStmt build_extern_stmt(TSNode node) const
  {
    DeclareStmt stmt;
    stmt.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      stmt.docs.push_back(std::move(doc));
    }

    // Optional: #[behavior(DataPolicy[, FlowPolicy])]
    for (auto behavior_node : collect_children_of_type(node, "behavior_attr")) {
      const TSNode data_node = get_child_by_field(behavior_node, "data");
      if (!ts_node_is_null(data_node)) {
        stmt.data_policy = get_node_text(data_node, source_);
      }
      const TSNode flow_node = get_child_by_field(behavior_node, "flow");
      if (!ts_node_is_null(flow_node)) {
        stmt.flow_policy = get_node_text(flow_node, source_);
      }
      break;
    }

    TSNode def = get_child_by_field(node, "def");
    if (ts_node_is_null(def)) {
      // Fallback: find first extern_def child
      for (auto d : collect_children_of_type(node, "extern_def")) {
        def = d;
        break;
      }
    }

    // Category keyword is an anonymous token within extern_def.
    std::string category_kw;
    if (!ts_node_is_null(def)) {
      const uint32_t child_count = ts_node_child_count(def);
      for (uint32_t i = 0; i < child_count; ++i) {
        const TSNode ch = ts_node_child(def, i);
        if (ts_node_is_named(ch)) {
          continue;
        }
        const std::string t = get_node_text(ch, source_);
        if (
          t == "action" || t == "condition" || t == "control" || t == "decorator" ||
          t == "subtree") {
          category_kw = t;
          break;
        }
      }
    }

    // Store the spec keyword as-is (lowercase).
    stmt.category = category_kw.empty() ? "action" : category_kw;

    const TSNode name_node = get_child_by_field(def, "name");
    if (!ts_node_is_null(name_node)) {
      stmt.name = get_node_text(name_node, source_);
    }

    for (auto port_list : collect_children_of_type(def, "extern_port_list")) {
      for (auto port_node : collect_children_of_type(port_list, "extern_port")) {
        stmt.ports.emplace_back(build_extern_port(port_node));
      }
    }

    return stmt;
  }

  [[nodiscard]] DeclarePort build_extern_port(TSNode node) const
  {
    DeclarePort port;
    port.range = get_source_range(node);

    // Direction
    for (auto dir_node : collect_children_of_type(node, "port_direction")) {
      port.direction = parse_direction(get_node_text(dir_node, source_));
      break;
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      port.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      port.type_name = get_node_text(type_node, source_);
    }

    const TSNode default_node = get_child_by_field(node, "default");
    if (!ts_node_is_null(default_node)) {
      port.default_value = build_expression(default_node);
    }

    return port;
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
      strip_trailing_cr(doc);
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
      strip_trailing_cr(doc);
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

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      decl.docs.push_back(std::move(doc));
    }

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

  [[nodiscard]] ConstDeclStmt build_global_const_decl(TSNode node) const
  {
    ConstDeclStmt decl;
    decl.range = get_source_range(node);

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      decl.docs.push_back(std::move(doc));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      decl.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      decl.type_name = get_node_text(type_node, source_);
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      decl.value = build_expression(value_node);
    } else {
      // Should not happen for valid input.
      decl.value = make_missing_expr(decl.range);
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
      strip_trailing_cr(doc);
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

    // New DSL: tree body contains statements.
    const TSNode body_node = get_child_by_field(node, "body");
    if (!ts_node_is_null(body_node)) {
      parse_statement_block(body_node, tree.body);
    }

    return tree;
  }

  void parse_statement_block(TSNode node, std::vector<Statement> & out) const
  {
    // tree_body / children_block := '{' { statement } '}'
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) {
        continue;
      }
      if (std::strcmp(ts_node_type(child), "statement") != 0) {
        continue;
      }
      auto stmt = build_statement(child);
      if (stmt.has_value()) {
        out.emplace_back(std::move(*stmt));
      }
    }
  }

  [[nodiscard]] std::optional<Statement> build_statement(TSNode statement_node) const
  {
    // statement := simple_stmt ';' | block_stmt
    // simple_stmt := leaf_node_call | assignment_stmt | blackboard_decl | local_const_decl
    // block_stmt  := compound_node_call
    const TSNode inner = find_first_named_descendant_of_any(
      statement_node, {"leaf_node_call", "compound_node_call", "assignment_stmt", "blackboard_decl",
                       "local_const_decl"});
    if (ts_node_is_null(inner)) {
      return std::nullopt;
    }

    const char * type = ts_node_type(inner);
    if (std::strcmp(type, "leaf_node_call") == 0 || std::strcmp(type, "compound_node_call") == 0) {
      return Statement{Box<NodeStmt>(build_node_stmt(inner))};
    }
    if (std::strcmp(type, "assignment_stmt") == 0) {
      return Statement{build_assignment_stmt(inner)};
    }
    if (std::strcmp(type, "blackboard_decl") == 0) {
      return Statement{build_blackboard_decl_stmt(inner)};
    }
    if (std::strcmp(type, "local_const_decl") == 0) {
      return Statement{build_const_decl_stmt(inner)};
    }

    return std::nullopt;
  }

  [[nodiscard]] BlackboardDeclStmt build_blackboard_decl_stmt(TSNode node) const
  {
    BlackboardDeclStmt decl;
    decl.range = get_source_range(node);

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      decl.docs.push_back(std::move(doc));
    }

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

  [[nodiscard]] ConstDeclStmt build_const_decl_stmt(TSNode node) const
  {
    ConstDeclStmt decl;
    decl.range = get_source_range(node);

    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      decl.docs.push_back(std::move(doc));
    }

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      decl.name = get_node_text(name_node, source_);
    }

    const TSNode type_node = get_child_by_field(node, "type");
    if (!ts_node_is_null(type_node)) {
      decl.type_name = get_node_text(type_node, source_);
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      decl.value = build_expression(value_node);
    } else {
      decl.value = make_missing_expr(decl.range);
    }

    return decl;
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

    const TSNode default_node = get_child_by_field(node, "default");
    if (!ts_node_is_null(default_node)) {
      param.default_value = build_expression(default_node);
    }

    return param;
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
      strip_trailing_cr(doc);
      stmt.docs.push_back(std::move(doc));
    }

    const std::string_view node_type = ts_node_type(node);

    // Precondition list (optional)
    const TSNode precond_list = get_child_by_field(node, "preconds");
    const TSNode precond_list_fallback =
      ts_node_is_null(precond_list) ? find_first_named_child_of_any(node, {"precondition_list"})
                                    : precond_list;
    if (!ts_node_is_null(precond_list_fallback)) {
      const uint32_t pc_count = ts_node_child_count(precond_list_fallback);
      for (uint32_t i = 0; i < pc_count; ++i) {
        const TSNode pc = ts_node_child(precond_list_fallback, i);
        if (!ts_node_is_named(pc) || std::strcmp(ts_node_type(pc), "precondition") != 0) {
          continue;
        }
        stmt.preconditions.emplace_back(build_precondition(pc));
      }
    }

    if (node_type == "leaf_node_call") {
      const TSNode name_node = get_child_by_field(node, "name");
      if (!ts_node_is_null(name_node)) {
        stmt.node_name = get_node_text(name_node, source_);
      }
      const TSNode args_node = get_child_by_field(node, "args");
      if (!ts_node_is_null(args_node)) {
        stmt.has_property_block = true;
        parse_property_block(args_node, stmt.args);
      }
      return stmt;
    }

    if (node_type == "compound_node_call") {
      const TSNode name_node = get_child_by_field(node, "name");
      if (!ts_node_is_null(name_node)) {
        stmt.node_name = get_node_text(name_node, source_);
      }
      const TSNode body_node = get_child_by_field(node, "body");
      if (!ts_node_is_null(body_node)) {
        // node_body_with_children := (property_block children_block) | children_block
        // IMPORTANT: only consider *direct* children of node_body_with_children.
        // Using descendant search will accidentally pick up a child node's property_block,
        // e.g. `Inverter { Repeat(3) { ... } }` would incorrectly assign `(3)` to `Inverter`.
        const TSNode prop = find_first_named_child_of_any(body_node, {"property_block"});
        if (!ts_node_is_null(prop)) {
          stmt.has_property_block = true;
          parse_property_block(prop, stmt.args);
        }
        const TSNode children_block = find_first_named_child_of_any(body_node, {"children_block"});
        if (!ts_node_is_null(children_block)) {
          stmt.has_children_block = true;
          parse_children_block(children_block, stmt.children);
        }
      }
      return stmt;
    }

    return stmt;
  }

  [[nodiscard]] Precondition build_precondition(TSNode node) const
  {
    Precondition pc;
    pc.range = get_source_range(node);

    const TSNode kind_node = get_child_by_field(node, "kind");
    if (!ts_node_is_null(kind_node)) {
      pc.kind = get_node_text(kind_node, source_);
    }

    const TSNode cond_node = get_child_by_field(node, "cond");
    if (!ts_node_is_null(cond_node)) {
      pc.condition = build_expression(cond_node);
    } else {
      pc.condition = make_missing_expr(pc.range);
    }

    return pc;
  }

  void parse_property_block(TSNode node, std::vector<Argument> & args) const
  {
    // property_block := '(' [argument_list] ')'
    for (auto arg_list : collect_children_of_type(node, "argument_list")) {
      for (auto arg_node : collect_children_of_type(arg_list, "argument")) {
        args.emplace_back(build_argument(arg_node));
      }
    }
  }

  [[nodiscard]] Argument build_argument(TSNode node) const
  {
    Argument arg;
    arg.range = get_source_range(node);

    const TSNode name_node = get_child_by_field(node, "name");
    if (!ts_node_is_null(name_node)) {
      arg.name = get_node_text(name_node, source_);
    }

    // Spec: value is argument_expr
    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      parse_argument_expr(value_node, arg);
      return arg;
    }

    // Fallback: treat as empty expression.
    arg.value = Expression{make_missing_expr(get_source_range(node))};

    return arg;
  }

  void parse_argument_expr(TSNode node, Argument & arg) const
  {
    // argument_expr :=
    //   | 'out' inline_blackboard_decl
    //   | [port_direction] expression

    const TSNode inline_decl = get_child_by_field(node, "inline_decl");
    if (!ts_node_is_null(inline_decl)) {
      arg.direction = PortDirection::Out;
      InlineBlackboardDecl decl;
      decl.range = get_source_range(inline_decl);
      const TSNode name_node = get_child_by_field(inline_decl, "name");
      if (!ts_node_is_null(name_node)) {
        decl.name = get_node_text(name_node, source_);
      }
      arg.value = std::move(decl);
      return;
    }

    // Optional direction prefix
    const TSNode dir_node = find_first_named_descendant_of_any(node, {"port_direction"});
    if (!ts_node_is_null(dir_node)) {
      arg.direction = parse_direction(get_node_text(dir_node, source_));
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      arg.value = build_expression(value_node);
      return;
    }

    arg.value = Expression{make_missing_expr(get_source_range(node))};
  }

  void parse_children_block(TSNode node, std::vector<Statement> & children) const
  {
    parse_statement_block(node, children);
  }

  [[nodiscard]] AssignmentStmt build_assignment_stmt(TSNode node) const
  {
    AssignmentStmt stmt;
    stmt.range = get_source_range(node);

    // Collect outer docs
    for (auto doc_node : collect_children_of_type(node, "outer_doc")) {
      std::string doc = get_node_text(doc_node, source_);
      if (starts_with(doc, "///")) {
        doc = doc.substr(3);
      }
      strip_trailing_cr(doc);
      stmt.docs.push_back(std::move(doc));
    }

    // Precondition list (optional)
    const TSNode precond_list = get_child_by_field(node, "preconds");
    const TSNode precond_list_fallback =
      ts_node_is_null(precond_list) ? find_first_named_child_of_any(node, {"precondition_list"})
                                    : precond_list;
    if (!ts_node_is_null(precond_list_fallback)) {
      const uint32_t pc_count = ts_node_child_count(precond_list_fallback);
      for (uint32_t i = 0; i < pc_count; ++i) {
        const TSNode pc = ts_node_child(precond_list_fallback, i);
        if (!ts_node_is_named(pc) || std::strcmp(ts_node_type(pc), "precondition") != 0) {
          continue;
        }
        stmt.preconditions.emplace_back(build_precondition(pc));
      }
    }

    const TSNode lvalue_node = get_child_by_field(node, "target");
    if (!ts_node_is_null(lvalue_node)) {
      const TSNode base_node = get_child_by_field(lvalue_node, "base");
      if (!ts_node_is_null(base_node)) {
        stmt.target = get_node_text(base_node, source_);
      }

      const uint32_t lv_children = ts_node_child_count(lvalue_node);
      for (uint32_t i = 0; i < lv_children; ++i) {
        const TSNode ch = ts_node_child(lvalue_node, i);
        if (!ts_node_is_named(ch) || std::strcmp(ts_node_type(ch), "index_suffix") != 0) {
          continue;
        }
        // index_suffix := '[' expression ']'
        const TSNode expr_node = find_first_named_descendant_of_any(ch, {"expression"});
        if (!ts_node_is_null(expr_node)) {
          stmt.indices.emplace_back(build_expression(expr_node));
        }
      }
    }

    const TSNode op_node = get_child_by_field(node, "op");
    if (!ts_node_is_null(op_node)) {
      const std::string op_text = get_node_text(op_node, source_);
      stmt.op = parse_assign_op(op_text).value_or(AssignOp::Assign);
    }

    const TSNode value_node = get_child_by_field(node, "value");
    if (!ts_node_is_null(value_node)) {
      stmt.value = build_expression(value_node);
    } else {
      stmt.value = make_missing_expr(stmt.range);
    }

    return stmt;
  }

  [[nodiscard]] Expression build_expression(TSNode node) const
  {
    const char * type = ts_node_type(node);

    // In recovery mode, tree-sitter may attach following tokens as children of an ERROR node.
    // Treat missing/error nodes as MissingExpr to avoid accidentally consuming unrelated input.
    if (ts_node_is_missing(node) || ts_node_is_error(node)) {
      return make_missing_expr(get_source_range(node));
    }

    // Handle wrapper nodes (expression, or_expr, and_expr, etc.)
    if (
      std::strcmp(type, "expression") == 0 || std::strcmp(type, "or_expr") == 0 ||
      std::strcmp(type, "and_expr") == 0 || std::strcmp(type, "bitwise_or_expr") == 0 ||
      std::strcmp(type, "bitwise_xor_expr") == 0 || std::strcmp(type, "bitwise_and_expr") == 0 ||
      std::strcmp(type, "equality_expr") == 0 || std::strcmp(type, "comparison_expr") == 0 ||
      std::strcmp(type, "additive_expr") == 0 || std::strcmp(type, "multiplicative_expr") == 0) {
      // If this expression subtree contains any parse errors (missing tokens / ERROR nodes),
      // prefer representing it as a MissingExpr. This prevents recovery parses like
      // `@success_if() Action();` or `Action(x:);` from accidentally consuming the following
      // statement tokens as the missing expression.
      if (ts_node_has_error(node)) {
        return make_missing_expr(get_source_range(node));
      }
      return build_binary_or_passthrough(node);
    }

    if (std::strcmp(type, "cast_expr") == 0) {
      return build_cast_expr(node);
    }

    if (std::strcmp(type, "unary_expr") == 0) {
      return build_unary_expr(node);
    }

    if (std::strcmp(type, "primary_expr") == 0) {
      return build_primary_expr(node);
    }

    if (std::strcmp(type, "array_literal") == 0) {
      return Box<ArrayLiteralExpr>(build_array_literal_expr(node));
    }

    if (std::strcmp(type, "vec_macro") == 0) {
      return Box<VecMacroExpr>(build_vec_macro_expr(node));
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
    return make_missing_expr(get_source_range(node));
  }

  [[nodiscard]] Expression build_cast_expr(TSNode node) const
  {
    // Spec (docs/reference/syntax.md 2.4): cast_expr := unary_expr , { 'as' , type } ;
    // Tree-sitter grammar allows multiple chained casts. We model this as
    // left-associative nested CastExpr: ((a as T1) as T2) ...
    TSNode unary_node = find_first_named_descendant_of_any(node, {"unary_expr"});
    if (ts_node_is_null(unary_node)) {
      unary_node = find_first_named_descendant_of_any(node, {"primary_expr"});
    }

    Expression expr = build_expression(unary_node);

    // Collect type nodes in source order (direct children of cast_expr).
    std::vector<TSNode> type_nodes;
    const uint32_t child_count = ts_node_child_count(node);
    type_nodes.reserve(2);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode ch = ts_node_child(node, i);
      if (!ts_node_is_named(ch)) {
        continue;
      }
      if (std::strcmp(ts_node_type(ch), "type") == 0) {
        type_nodes.emplace_back(ch);
      }
    }

    if (type_nodes.empty()) {
      return expr;
    }

    for (const TSNode & type_node : type_nodes) {
      CastExpr c;
      c.range = get_source_range(node);
      c.expr = std::move(expr);
      c.type_name = get_node_text(type_node, source_);
      expr = Box<CastExpr>(std::move(c));
    }

    return expr;
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

    // Spec (docs/reference/syntax.md 2.4):
    // - comparison operators (<, <=, >, >=) are non-associative (chaining forbidden)
    // - equality operators (==, !=) are non-associative (chaining forbidden)
    // Therefore, `a < b < c` and `a == b == c` must be syntax errors.
    if (
      (std::strcmp(ts_node_type(node), "comparison_expr") == 0 ||
       std::strcmp(ts_node_type(node), "equality_expr") == 0) &&
      operators.size() > 1) {
      const std::string kind =
        (std::strcmp(ts_node_type(node), "comparison_expr") == 0) ? "comparison" : "equality";
      errors_.emplace_back(
        make_error("Chained " + kind + " operators are not allowed", get_source_range(node)));

      // Return a best-effort partial expression to avoid cascading failures in
      // recovery consumers. The parse() entry point will still fail due to errors_.
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

    return make_missing_expr(get_source_range(node));
  }

  [[nodiscard]] Expression build_primary_expr(TSNode node) const
  {
    // primary_expr := ( '(' expression ')' | literal | array_literal | vec_macro | identifier ) { index_suffix }
    Expression base = make_missing_expr(get_source_range(node));

    // Find base expression
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child)) {
        continue;
      }
      const char * type = ts_node_type(child);

      if (std::strcmp(type, "literal") == 0) {
        base = build_literal(child);
        break;
      }
      if (std::strcmp(type, "identifier") == 0) {
        base = make_var_ref(get_node_text(child, source_), std::nullopt, get_source_range(child));
        break;
      }
      if (std::strcmp(type, "expression") == 0) {
        base = build_expression(child);
        break;
      }
      if (std::strcmp(type, "array_literal") == 0) {
        base = Box<ArrayLiteralExpr>(build_array_literal_expr(child));
        break;
      }
      if (std::strcmp(type, "vec_macro") == 0) {
        base = Box<VecMacroExpr>(build_vec_macro_expr(child));
        break;
      }
    }

    // Apply index_suffix chain
    for (uint32_t i = 0; i < child_count; ++i) {
      const TSNode child = ts_node_child(node, i);
      if (!ts_node_is_named(child) || std::strcmp(ts_node_type(child), "index_suffix") != 0) {
        continue;
      }
      const TSNode idx_expr = find_first_named_descendant_of_any(child, {"expression"});
      if (ts_node_is_null(idx_expr)) {
        continue;
      }
      IndexExpr ie;
      ie.range = get_source_range(child);
      ie.base = std::move(base);
      ie.index = build_expression(idx_expr);
      base = Box<IndexExpr>(std::move(ie));
    }

    return base;
  }

  [[nodiscard]] ArrayLiteralExpr build_array_literal_expr(TSNode node) const
  {
    ArrayLiteralExpr arr;
    arr.range = get_source_range(node);

    const TSNode repeat_node = find_first_named_descendant_of_any(node, {"repeat_init"});
    if (!ts_node_is_null(repeat_node)) {
      // repeat_init := expression ';' expression
      std::vector<TSNode> exprs;
      const uint32_t cc = ts_node_child_count(repeat_node);
      for (uint32_t i = 0; i < cc; ++i) {
        const TSNode ch = ts_node_child(repeat_node, i);
        if (ts_node_is_named(ch) && std::strcmp(ts_node_type(ch), "expression") == 0) {
          exprs.emplace_back(ch);
        }
      }
      if (!exprs.empty()) {
        arr.repeat_value = build_expression(exprs[0]);
      }
      if (exprs.size() >= 2) {
        arr.repeat_count = build_expression(exprs[1]);
      }
      return arr;
    }

    const TSNode list_node = find_first_named_descendant_of_any(node, {"element_list"});
    if (!ts_node_is_null(list_node)) {
      const uint32_t cc = ts_node_child_count(list_node);
      for (uint32_t i = 0; i < cc; ++i) {
        const TSNode ch = ts_node_child(list_node, i);
        if (ts_node_is_named(ch) && std::strcmp(ts_node_type(ch), "expression") == 0) {
          arr.elements.emplace_back(build_expression(ch));
        }
      }
      return arr;
    }

    // Empty array literal: []
    return arr;
  }

  [[nodiscard]] VecMacroExpr build_vec_macro_expr(TSNode node) const
  {
    VecMacroExpr vec;
    vec.range = get_source_range(node);
    const TSNode arr_node = find_first_named_descendant_of_any(node, {"array_literal"});
    if (!ts_node_is_null(arr_node)) {
      vec.value = build_array_literal_expr(arr_node);
    }
    return vec;
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
        // Reference: docs/reference/lexical-structure.md 1.4 (float)
        // Enforce strict parsing (no partial parses, no silent fallback).
        double value = 0.0;
        const char * begin = text.data();
        const char * end = text.data() + text.size();
        const auto res = std::from_chars(begin, end, value);
        if (res.ec != std::errc() || res.ptr != end) {
          errors_.emplace_back(make_error("Invalid float literal: " + text, child_range));
          return make_float_literal(0.0, child_range);
        }
        return make_float_literal(value, child_range);
      }
      if (std::strcmp(type, "integer") == 0) {
        // Supports: 0x.., 0b.., 0o.., decimal (with optional -)
        // Reference: docs/reference/lexical-structure.md 1.4 (integer)
        // Enforce strict parsing + range checks.
        const bool neg = !text.empty() && text.front() == '-';
        std::string_view s = text;
        if (neg) {
          s = s.substr(1);
        }

        int base = 10;
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
          base = 16;
          s = s.substr(2);
        } else if (s.size() >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
          base = 2;
          s = s.substr(2);
        } else if (s.size() >= 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
          base = 8;
          s = s.substr(2);
        } else {
          // Spec: decimal integer is either "0" or [1-9][0-9]* (no leading zeros).
          if (s.size() > 1 && !s.empty() && s.front() == '0') {
            errors_.emplace_back(
              make_error("Invalid integer literal (leading zeros): " + text, child_range));
            return make_int_literal(0, child_range);
          }
        }

        if (s.empty()) {
          errors_.emplace_back(make_error("Invalid integer literal: " + text, child_range));
          return make_int_literal(0, child_range);
        }

        uint64_t magnitude = 0;
        const char * begin = s.data();
        const char * end = s.data() + s.size();
        const auto res = std::from_chars(begin, end, magnitude, base);
        if (res.ec != std::errc() || res.ptr != end) {
          errors_.emplace_back(make_error("Invalid integer literal: " + text, child_range));
          return make_int_literal(0, child_range);
        }

        // Range-check into int64_t (supporting INT64_MIN for negative literals).
        int64_t value = 0;
        const auto max_pos = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        const auto max_neg = max_pos + 1ULL;  // abs(INT64_MIN)
        if (neg) {
          if (magnitude > max_neg) {
            errors_.emplace_back(make_error("Integer literal out of range: " + text, child_range));
            return make_int_literal(0, child_range);
          }
          if (magnitude == max_neg) {
            value = std::numeric_limits<int64_t>::min();
          } else {
            value = -static_cast<int64_t>(magnitude);
          }
        } else {
          if (magnitude > max_pos) {
            errors_.emplace_back(make_error("Integer literal out of range: " + text, child_range));
            return make_int_literal(0, child_range);
          }
          value = static_cast<int64_t>(magnitude);
        }

        return make_int_literal(value, child_range);
      }
      if (std::strcmp(type, "boolean") == 0) {
        return make_bool_literal(text == "true", child_range);
      }
      if (std::strcmp(type, "null") == 0) {
        NullLiteral nl;
        nl.range = child_range;
        return nl;
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
