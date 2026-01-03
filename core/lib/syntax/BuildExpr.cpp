// bt_dsl/syntax/BuildExpr.cpp - CST -> AST for expressions (literals)
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "bt_dsl/syntax/ast_builder.hpp"

namespace bt_dsl
{

static std::string_view strip_trailing_cr(std::string_view s)
{
  if (!s.empty() && s.back() == '\r') return s.substr(0, s.size() - 1);
  return s;
}

static bool is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c)
{
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(10 + (c - 'a'));
  return static_cast<uint32_t>(10 + (c - 'A'));
}

static void append_utf8(uint32_t cp, std::string & out)
{
  // Encode a Unicode scalar value into UTF-8.
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static bool parse_integer_strict(std::string_view lit, int64_t & out)
{
  // lexical-structure.md 1.4.1
  // integer = [ "-" ] , ( "0" | /[1-9][0-9]*/ | "0x" /hex+/ | "0b" /01+/ | "0o" /0-7+/ )
  lit = strip_trailing_cr(lit);
  if (lit.empty()) return false;

  bool neg = false;
  if (lit.front() == '-') {
    neg = true;
    lit.remove_prefix(1);
    if (lit.empty()) return false;
  }

  int base = 10;
  if (lit.size() >= 2 && lit[0] == '0' && (lit[1] == 'x' || lit[1] == 'X')) {
    base = 16;
    lit.remove_prefix(2);
  } else if (lit.size() >= 2 && lit[0] == '0' && (lit[1] == 'b' || lit[1] == 'B')) {
    base = 2;
    lit.remove_prefix(2);
  } else if (lit.size() >= 2 && lit[0] == '0' && (lit[1] == 'o' || lit[1] == 'O')) {
    base = 8;
    lit.remove_prefix(2);
  } else {
    base = 10;
  }

  if (lit.empty()) return false;

  // from_chars doesn't accept + sign here (we already rejected by grammar).
  uint64_t u = 0;
  auto res = std::from_chars(lit.data(), lit.data() + lit.size(), u, base);
  if (res.ec != std::errc{} || res.ptr != lit.data() + lit.size()) {
    return false;
  }

  if (!neg) {
    if (u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
    out = static_cast<int64_t>(u);
    return true;
  }

  // Allow -2^63.
  const uint64_t max_abs = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
  if (u > max_abs) return false;
  if (u == max_abs) {
    out = std::numeric_limits<int64_t>::min();
    return true;
  }
  out = -static_cast<int64_t>(u);
  return true;
}

static bool parse_float_strict(std::string_view lit, double & out)
{
  // lexical-structure.md 1.4.1
  // float = [ "-" ] , ( /[0-9]+/ "." /[0-9]+/ [ exponent ] | /[0-9]+/ exponent )
  lit = strip_trailing_cr(lit);
  if (lit.empty()) return false;

  // strtod requires a null-terminated string.
  const std::string tmp(lit);
  char * end = nullptr;
  errno = 0;
  const double v = std::strtod(tmp.c_str(), &end);
  if (end != tmp.c_str() + tmp.size()) return false;
  if (errno == ERANGE) {
    // Still a float token, but out of range.
    // Treat as parse failure to force a diagnostic.
    return false;
  }
  out = v;
  return true;
}

static bool unescape_string(
  std::string_view token_with_quotes, std::string & out, std::string & err)
{
  token_with_quotes = strip_trailing_cr(token_with_quotes);
  if (
    token_with_quotes.size() < 2 || token_with_quotes.front() != '"' ||
    token_with_quotes.back() != '"') {
    err = "Invalid string token";
    return false;
  }

  const std::string_view s = token_with_quotes.substr(1, token_with_quotes.size() - 2);
  out.clear();

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }

    // Escape
    if (i + 1 >= s.size()) {
      err = "Unterminated escape sequence";
      return false;
    }

    const char e = s[++i];
    switch (e) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '0':
        out.push_back('\0');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'u': {
        // \u{XXXX} (1..6 hex)
        if (i + 1 >= s.size() || s[i + 1] != '{') {
          err = "Expected '{' after \\u";
          return false;
        }
        i += 2;  // skip '{'
        if (i >= s.size()) {
          err = "Unterminated \\u{...} escape";
          return false;
        }

        uint32_t cp = 0;
        int digits = 0;
        for (; i < s.size(); ++i) {
          const char h = s[i];
          if (h == '}') break;
          if (!is_hex_digit(h)) {
            err = "Non-hex digit in \\u{...} escape";
            return false;
          }
          if (digits >= 6) {
            err = "Too many hex digits in \\u{...} escape (max 6)";
            return false;
          }
          cp = (cp << 4) | hex_value(h);
          ++digits;
        }

        if (i >= s.size() || s[i] != '}') {
          err = "Unterminated \\u{...} escape";
          return false;
        }

        if (digits < 1) {
          err = "Empty \\u{...} escape";
          return false;
        }
        if (cp > 0x10FFFF) {
          err = "Unicode codepoint out of range in \\u{...} escape";
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
          err = "Surrogate codepoint is not a valid Unicode scalar value";
          return false;
        }

        append_utf8(cp, out);
        break;
      }
      default:
        // Spec says: implementation-defined. We'll treat as error (safe side).
        err = "Unknown escape sequence";
        return false;
    }
  }

  return true;
}

Expr * AstBuilder::build_expr(ts_ll::Node expr_node)
{
  if (expr_node.is_null()) {
    diags_.error({}, "Missing expression node");
    return ast_.create<MissingExpr>();
  }

  auto build_literal_token = [&](ts_ll::Node n) -> Expr * {
    const auto k = n.kind();
    if (k == "integer") {
      int64_t v = 0;
      auto txt = strip_trailing_cr(n.text(sm_));
      if (!parse_integer_strict(txt, v)) {
        diags_.error(node_range(n), "Invalid integer literal: '" + std::string(txt) + "'");
        return ast_.create<MissingExpr>(node_range(n));
      }
      return ast_.create<IntLiteralExpr>(v, node_range(n));
    }

    if (k == "float") {
      double v = 0.0;
      auto txt = strip_trailing_cr(n.text(sm_));
      if (!parse_float_strict(txt, v)) {
        diags_.error(node_range(n), "Invalid float literal: '" + std::string(txt) + "'");
        return ast_.create<MissingExpr>(node_range(n));
      }
      return ast_.create<FloatLiteralExpr>(v, node_range(n));
    }

    if (k == "string") {
      std::string unescaped;
      std::string err;
      auto txt = strip_trailing_cr(n.text(sm_));
      if (!unescape_string(txt, unescaped, err)) {
        diags_.error(node_range(n), "Invalid string literal: " + err);
        return ast_.create<MissingExpr>(node_range(n));
      }
      // Intern the unescaped content.
      auto sv = ast_.intern(unescaped);
      return ast_.create<StringLiteralExpr>(sv, node_range(n));
    }

    if (k == "boolean") {
      auto txt = strip_trailing_cr(n.text(sm_));
      if (txt == "true") return ast_.create<BoolLiteralExpr>(true, node_range(n));
      if (txt == "false") return ast_.create<BoolLiteralExpr>(false, node_range(n));
      diags_.error(node_range(n), "Invalid boolean literal: '" + std::string(txt) + "'");
      return ast_.create<MissingExpr>(node_range(n));
    }

    if (k == "null") {
      return ast_.create<NullLiteralExpr>(node_range(n));
    }

    return nullptr;
  };

  auto parse_unary_op = [&](ts_ll::Node tok) -> std::optional<UnaryOp> {
    const std::string_view t = node_text(tok);
    if (t == "!") return UnaryOp::Not;
    if (t == "-") return UnaryOp::Neg;
    return std::nullopt;
  };

  auto parse_binary_op = [&](std::string_view tok) -> std::optional<BinaryOp> {
    if (tok == "+") return BinaryOp::Add;
    if (tok == "-") return BinaryOp::Sub;
    if (tok == "*") return BinaryOp::Mul;
    if (tok == "/") return BinaryOp::Div;
    if (tok == "%") return BinaryOp::Mod;
    if (tok == "==") return BinaryOp::Eq;
    if (tok == "!=") return BinaryOp::Ne;
    if (tok == "<") return BinaryOp::Lt;
    if (tok == "<=") return BinaryOp::Le;
    if (tok == ">") return BinaryOp::Gt;
    if (tok == ">=") return BinaryOp::Ge;
    if (tok == "&&") return BinaryOp::And;
    if (tok == "||") return BinaryOp::Or;
    if (tok == "&") return BinaryOp::BitAnd;
    if (tok == "^") return BinaryOp::BitXor;
    if (tok == "|") return BinaryOp::BitOr;
    return std::nullopt;
  };

  // Generic left-associative binary chain builder for nodes shaped like:
  //   seq(operand, repeat(seq(opToken, operand)))
  auto build_left_assoc_chain =
    [&](ts_ll::Node n, const std::vector<std::string_view> & allowedOps) -> Expr * {
    if (n.named_child_count() == 0) {
      return missing_expr(n, "Expected operand in expression");
    }

    Expr * lhs = nullptr;
    bool have_lhs = false;
    std::optional<BinaryOp> pending_op;

    for (uint32_t i = 0; i < n.child_count(); ++i) {
      const ts_ll::Node ch = n.child(i);
      if (ch.is_null()) continue;

      if (!ts_node_is_named(ch.raw())) {
        const std::string_view t = node_text(ch);
        bool ok = false;
        for (auto a : allowedOps) {
          if (t == a) {
            ok = true;
            break;
          }
        }
        if (!ok) continue;

        auto op = parse_binary_op(t);
        if (!op) {
          diags_.error(
            node_range(ch), "Unsupported binary operator token: '" + std::string(t) + "'");
          continue;
        }
        pending_op = op;
        continue;
      }

      // Named child (operand).
      Expr * operand = build_expr(ch);
      if (!have_lhs) {
        lhs = operand;
        have_lhs = true;
        continue;
      }

      if (!pending_op) {
        // We have an operand but no operator; this can happen under error recovery.
        diags_.error(node_range(ch), "Missing operator between operands");
        // Keep going, but treat as if it were just the lhs.
        continue;
      }
      lhs = ast_.create<BinaryExpr>(lhs, pending_op.value(), operand, node_range(n));
      pending_op.reset();
    }

    if (!have_lhs) {
      return missing_expr(n, "Expected operand in expression");
    }
    return lhs;
  };

  auto build_non_chained_binary =
    [&](ts_ll::Node n, const std::vector<std::string_view> & allowedOps) -> Expr * {
    if (n.named_child_count() == 0) {
      return missing_expr(n, "Expected operand in expression");
    }
    if (n.named_child_count() == 1) {
      return build_expr(n.named_child(0));
    }
    if (n.named_child_count() >= 2) {
      // Find operator token.
      std::optional<BinaryOp> op;
      for (uint32_t i = 0; i < n.child_count(); ++i) {
        const ts_ll::Node ch = n.child(i);
        if (ch.is_null()) continue;
        if (ts_node_is_named(ch.raw())) continue;
        const std::string_view t = node_text(ch);
        for (auto a : allowedOps) {
          if (t == a) {
            op = parse_binary_op(t);
            break;
          }
        }
        if (op) break;
      }
      if (!op) {
        return missing_expr(n, "Expected operator in expression");
      }
      Expr * lhs = build_expr(n.named_child(0));
      Expr * rhs = build_expr(n.named_child(1));
      return ast_.create<BinaryExpr>(lhs, *op, rhs, node_range(n));
    }
    return missing_expr(n, "Invalid expression");
  };

  const auto k = expr_node.kind();

  // Wrapper nodes.
  if (k == "expression" || k == "const_expr" || k == "literal") {
    if (expr_node.named_child_count() == 1) {
      return build_expr(expr_node.named_child(0));
    }
    if (k == "literal" && expr_node.named_child_count() == 0) {
      return missing_expr(expr_node, "Empty literal");
    }
    // Fall through for error recovery.
  }

  // Identifier
  if (k == "identifier") {
    return ast_.create<VarRefExpr>(intern_text(expr_node), node_range(expr_node));
  }

  // Literal leaf tokens
  if (Expr * lit = build_literal_token(expr_node)) {
    return lit;
  }

  // Expression precedence levels
  if (k == "or_expr") {
    return build_left_assoc_chain(expr_node, {"||"});
  }
  if (k == "and_expr") {
    return build_left_assoc_chain(expr_node, {"&&"});
  }
  if (k == "bitwise_or_expr") {
    return build_left_assoc_chain(expr_node, {"|"});
  }
  if (k == "bitwise_xor_expr") {
    return build_left_assoc_chain(expr_node, {"^"});
  }
  if (k == "bitwise_and_expr") {
    return build_left_assoc_chain(expr_node, {"&"});
  }
  if (k == "equality_expr") {
    return build_non_chained_binary(expr_node, {"==", "!="});
  }
  if (k == "comparison_expr") {
    return build_non_chained_binary(expr_node, {"<", "<=", ">", ">="});
  }
  if (k == "additive_expr") {
    return build_left_assoc_chain(expr_node, {"+", "-"});
  }
  if (k == "multiplicative_expr") {
    return build_left_assoc_chain(expr_node, {"*", "/", "%"});
  }

  if (k == "cast_expr") {
    if (expr_node.named_child_count() == 0) {
      return missing_expr(expr_node, "Empty cast_expr");
    }

    // cast_expr: unary_expr, repeat('as', type)  (left-assoc)
    Expr * cur = build_expr(expr_node.named_child(0));

    // The remaining named children are types (in order). We still scan for 'as' tokens
    // to be robust under error recovery.
    uint32_t type_idx = 1;
    for (uint32_t i = 0; i < expr_node.child_count(); ++i) {
      const ts_ll::Node ch = expr_node.child(i);
      if (ch.is_null()) continue;
      if (ts_node_is_named(ch.raw())) continue;
      if (node_text(ch) != "as") continue;

      if (type_idx >= expr_node.named_child_count()) {
        return missing_expr(expr_node, "cast_expr missing type after 'as'");
      }
      const ts_ll::Node type_node = expr_node.named_child(type_idx++);
      TypeExpr * ty = build_type(type_node);
      cur = ast_.create<CastExpr>(cur, ty, node_range(expr_node));
    }

    // If there were types but 'as' tokens were not found (unexpected), still build casts.
    while (type_idx < expr_node.named_child_count()) {
      const ts_ll::Node type_node = expr_node.named_child(type_idx++);
      TypeExpr * ty = build_type(type_node);
      cur = ast_.create<CastExpr>(cur, ty, node_range(expr_node));
    }

    return cur;
  }

  if (k == "unary_expr") {
    // unary_expr: ('!'|'-') unary_expr | primary_expr
    // In the operator case, there is an unnamed token for the operator and one named child (unary_expr).
    for (uint32_t i = 0; i < expr_node.child_count(); ++i) {
      const ts_ll::Node ch = expr_node.child(i);
      if (ch.is_null()) continue;
      if (ts_node_is_named(ch.raw())) continue;
      auto op = parse_unary_op(ch);
      if (!op) continue;
      if (expr_node.named_child_count() < 1) {
        return missing_expr(expr_node, "unary_expr missing operand");
      }
      Expr * operand = build_expr(expr_node.named_child(0));
      return ast_.create<UnaryExpr>(*op, operand, node_range(expr_node));
    }

    if (expr_node.named_child_count() == 1) {
      return build_expr(expr_node.named_child(0));
    }

    return missing_expr(expr_node, "Invalid unary_expr");
  }

  if (k == "primary_expr") {
    if (expr_node.named_child_count() == 0) {
      return missing_expr(expr_node, "Empty primary_expr");
    }

    // Base part: either ( '(' expression ')' ) or literal/array_literal/vec_macro/identifier.
    const ts_ll::Node base_node = expr_node.named_child(0);
    Expr * base = build_expr(base_node);

    // Apply index_suffix chain.
    for (uint32_t i = 1; i < expr_node.named_child_count(); ++i) {
      const ts_ll::Node suf = expr_node.named_child(i);
      if (suf.kind() != "index_suffix") {
        // Under error recovery, tolerate unexpected nodes.
        continue;
      }
      if (suf.named_child_count() != 1) {
        base = ast_.create<IndexExpr>(
          base, ast_.create<MissingExpr>(node_range(suf)), node_range(expr_node));
        continue;
      }
      Expr * idx = build_expr(suf.named_child(0));
      base = ast_.create<IndexExpr>(base, idx, node_range(expr_node));
    }

    return base;
  }

  if (k == "array_literal") {
    // array_literal: '[' optional(choice(repeat_init, element_list)) ']'
    if (expr_node.named_child_count() == 0) {
      // Empty array literal: []
      return ast_.create<ArrayLiteralExpr>(
        ast_.copy_to_arena(std::vector<Expr *>{}), node_range(expr_node));
    }

    const ts_ll::Node inner = expr_node.named_child(0);
    if (inner.kind() == "repeat_init") {
      if (inner.named_child_count() != 2) {
        return missing_expr(inner, "repeat_init must have two expressions");
      }
      Expr * v = build_expr(inner.named_child(0));
      Expr * c = build_expr(inner.named_child(1));
      return ast_.create<ArrayRepeatExpr>(v, c, node_range(expr_node));
    }

    if (inner.kind() == "element_list") {
      std::vector<Expr *> elems;
      elems.reserve(inner.named_child_count());
      for (uint32_t i = 0; i < inner.named_child_count(); ++i) {
        elems.push_back(build_expr(inner.named_child(i)));
      }
      return ast_.create<ArrayLiteralExpr>(ast_.copy_to_arena(elems), node_range(expr_node));
    }

    // Under error recovery, allow direct expressions as if it were element_list.
    if (inner.kind() == "expression" || inner.kind() == "or_expr") {
      std::vector<Expr *> elems;
      elems.push_back(build_expr(inner));
      return ast_.create<ArrayLiteralExpr>(ast_.copy_to_arena(elems), node_range(expr_node));
    }

    return missing_expr(expr_node, "Invalid array_literal");
  }

  if (k == "element_list") {
    // element_list is normally only used inside array_literal.
    std::vector<Expr *> elems;
    elems.reserve(expr_node.named_child_count());
    for (uint32_t i = 0; i < expr_node.named_child_count(); ++i) {
      elems.push_back(build_expr(expr_node.named_child(i)));
    }
    return ast_.create<ArrayLiteralExpr>(ast_.copy_to_arena(elems), node_range(expr_node));
  }

  if (k == "repeat_init") {
    if (expr_node.named_child_count() != 2) {
      return missing_expr(expr_node, "repeat_init must have two expressions");
    }
    Expr * v = build_expr(expr_node.named_child(0));
    Expr * c = build_expr(expr_node.named_child(1));
    return ast_.create<ArrayRepeatExpr>(v, c, node_range(expr_node));
  }

  if (k == "vec_macro") {
    // vec_macro: 'vec' '!' array_literal
    if (expr_node.named_child_count() != 1) {
      return missing_expr(expr_node, "vec_macro must have an array_literal");
    }
    Expr * inner = build_expr(expr_node.named_child(0));
    return ast_.create<VecMacroExpr>(inner, node_range(expr_node));
  }

  // Parenthesized expression: comes as a named child 'expression' under primary_expr.
  // If buildExpr is called directly on that 'expression' node it is handled above.

  diags_.error(
    node_range(expr_node),
    "Expression kind not implemented in core CST->AST builder yet: '" + std::string(k) + "'");
  return ast_.create<MissingExpr>(node_range(expr_node));
}

}  // namespace bt_dsl
