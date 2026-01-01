// bt_dsl/xml_generator.cpp - BehaviorTree.CPP XML generation
#include "bt_dsl/codegen/xml_generator.hpp"

#include <functional>
#include <memory>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "bt_dsl/semantic/const_eval.hpp"
#include "tinyxml2.h"

namespace bt_dsl
{

// ============================================================================
// AstToBtCppModelConverter
// ============================================================================

struct AstToBtCppModelConverter::CodegenContext
{
  const Program & program;
  const AnalysisResult & analysis;

  // Resolve SubTree call target IDs (used for import tree ID mangling).
  std::function<std::string(std::string_view)> subtree_id_resolver;

  // Const evaluation (for const inlining)
  ConstEvalContext const_eval;
  DiagnosticBag const_eval_diags;

  // Variable mangling for BT.CPP flat blackboard
  uint32_t next_id = 1;

  struct Frame
  {
    Frame * parent = nullptr;
    std::unordered_map<std::string, std::string> var_keys;
    std::unique_ptr<Scope> const_scope;
  };

  std::vector<std::unique_ptr<Frame>> frames;
  Frame * current = nullptr;

  Scope * global_scope_mut() const
  {
    // We never mutate the parent scope through this pointer; Scope requires a
    // non-const parent type.
    return const_cast<Scope *>(analysis.symbols.global_scope());
  }

  explicit CodegenContext(
    const Program & p, const AnalysisResult & a,
    const std::vector<const Program *> & imported_programs = {})
  : program(p), analysis(a)
  {
    build_visible_global_const_map(program, imported_programs, const_eval.global_consts);

    // Root frame: const lookup starts from a fresh scope chained to the semantic global scope.
    auto root = std::make_unique<Frame>();
    root->parent = nullptr;
    root->var_keys.reserve(64);
    root->const_scope = std::make_unique<Scope>(global_scope_mut());
    current = root.get();
    frames.push_back(std::move(root));
  }

  std::string subtree_xml_id(std::string_view tree_name) const
  {
    if (subtree_id_resolver) {
      return subtree_id_resolver(tree_name);
    }
    return std::string(tree_name);
  }

  void push_block()
  {
    auto f = std::make_unique<Frame>();
    f->parent = current;
    f->var_keys.reserve(32);
    f->const_scope =
      std::make_unique<Scope>(current ? current->const_scope.get() : global_scope_mut());
    current = f.get();
    frames.push_back(std::move(f));
  }

  void pop_block()
  {
    if (current && current->parent) {
      current = current->parent;
    }
  }

  std::string declare_var(std::string_view name)
  {
    const std::string base(name);
    const std::string key = base + "#" + std::to_string(next_id++);
    current->var_keys[base] = key;
    return key;
  }

  void define_const(const ConstDeclStmt & decl, SymbolKind kind) const
  {
    Symbol s;
    s.name = decl.name;
    s.kind = kind;
    s.type_name = decl.type_name;
    s.direction = std::nullopt;
    s.definition_range = decl.range;
    s.ast_node = &decl;
    current->const_scope->upsert(std::move(s));
  }

  std::optional<std::string> lookup_local_var_key(std::string_view name) const
  {
    const std::string key(name);
    for (const Frame * f = current; f; f = f->parent) {
      if (auto it = f->var_keys.find(key); it != f->var_keys.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  bool is_global_var(std::string_view name) const
  {
    const Symbol * g = analysis.symbols.get_global(name);
    return g && g->kind == SymbolKind::GlobalVariable;
  }

  std::string var_ref(std::string_view name, ExprMode mode) const
  {
    if (auto local = lookup_local_var_key(name)) {
      if (mode == ExprMode::Script) {
        return *local;
      }
      return "{" + *local + "}";
    }

    if (is_global_var(name)) {
      return "@{" + std::string(name) + "}";
    }

    // Fallback (should not happen on successful semantic analysis)
    if (mode == ExprMode::Script) {
      return std::string(name);
    }
    return "{" + std::string(name) + "}";
  }

  const Scope * const_scope_ptr() const { return current->const_scope.get(); }
};

namespace
{

bool is_null_literal_expr(const Expression & expr)
{
  if (const auto * lit = std::get_if<Literal>(&expr)) {
    return std::holds_alternative<NullLiteral>(*lit);
  }
  return false;
}

bool is_simple_value_expr_for_in_port(const Expression & expr)
{
  return std::holds_alternative<Literal>(expr) || std::holds_alternative<VarRef>(expr);
}

btcpp::Node make_plain_script_node(std::string code)
{
  btcpp::Node script;
  script.tag = "Script";
  // Keep leading/trailing spaces for stable tinyxml2 attribute formatting.
  script.attributes.push_back(btcpp::Attribute{"code", " " + std::move(code) + " "});
  return script;
}

std::string default_init_for_type(std::optional<std::string> type_name)
{
  // xml-mapping.md §6.3.2: initialize out var with a default value.
  // Best-effort: map common scalar types; fall back to 0.
  if (!type_name) {
    return "0";
  }
  const std::string & t = *type_name;
  if (t == "string" || t.rfind("string<", 0) == 0) {
    return "''";
  }
  if (t.find("float") != std::string::npos || t.find("double") != std::string::npos) {
    return "0.0";
  }
  // Treat everything else as integer-like.
  return "0";
}

}  // namespace

btcpp::Node AstToBtCppModelConverter::apply_preconditions_and_guard(
  btcpp::Node node, const std::vector<Precondition> & preconditions, CodegenContext & ctx)
{
  std::vector<Expression> guard_conditions;
  guard_conditions.reserve(preconditions.size());

  for (const auto & pc : preconditions) {
    if (pc.kind == "guard") {
      guard_conditions.push_back(pc.condition);
      continue;
    }
    std::string attr_name;
    if (pc.kind == "success_if") {
      attr_name = "_successIf";
    } else if (pc.kind == "failure_if") {
      attr_name = "_failureIf";
    } else if (pc.kind == "skip_if") {
      attr_name = "_skipIf";
    } else if (pc.kind == "run_while") {
      attr_name = "_while";
    }
    if (!attr_name.empty()) {
      node.attributes.push_back(
        btcpp::Attribute{
          attr_name, serialize_expression(pc.condition, ctx, ExprMode::Precondition)});
    }
  }

  if (guard_conditions.empty()) {
    return node;
  }

  // xml-mapping.md §5.1: @guard(expr) ->
  // <Sequence>
  //   <Node _while="expr" ... />
  //   <AlwaysSuccess _failureIf="!(expr)" />
  // </Sequence>
  Expression combined = guard_conditions.front();
  for (size_t i = 1; i < guard_conditions.size(); ++i) {
    BinaryExpr b;
    b.op = BinaryOp::And;
    b.left = combined;
    b.right = guard_conditions[i];
    combined = Box<BinaryExpr>(std::move(b));
  }
  const std::string expr_str = serialize_expression(combined, ctx, ExprMode::Precondition);

  node.attributes.push_back(btcpp::Attribute{"_while", expr_str});

  btcpp::Node always;
  always.tag = "AlwaysSuccess";
  always.attributes.push_back(btcpp::Attribute{"_failureIf", "!(" + expr_str + ")"});

  btcpp::Node seq;
  seq.tag = "Sequence";
  seq.children.push_back(std::move(node));
  seq.children.push_back(std::move(always));
  return seq;
}

std::string AstToBtCppModelConverter::join_docs(const std::vector<std::string> & docs)
{
  std::string out;
  for (const auto & d : docs) {
    std::string_view sv(d);
    // Parser already removed leading "///", but keep trimming.
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
      sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
      sv.remove_suffix(1);
    }
    if (sv.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(sv);
  }
  return out;
}

static std::string to_string_lossless(double v)
{
  // Keep tinyxml2 output stable and avoid scientific notation for common
  // values.
  std::ostringstream oss;
  oss.precision(17);
  oss << v;
  return oss.str();
}

static std::string quote_script_string(std::string_view s)
{
  // BT.CPP Script uses single quotes.
  std::string escaped;
  escaped.reserve(s.size());
  for (const char c : s) {
    if (c == '\'') {
      escaped += "\\'";
    } else {
      escaped += c;
    }
  }
  return "'" + escaped + "'";
}

static std::string format_const_value_for_mode(const ConstValue & v, bool attribute_value_mode)
{
  if (const auto * i = std::get_if<int64_t>(&v)) {
    return std::to_string(*i);
  }
  if (const auto * f = std::get_if<double>(&v)) {
    return to_string_lossless(*f);
  }
  if (const auto * b = std::get_if<bool>(&v)) {
    return *b ? "true" : "false";
  }
  if (const auto * s = std::get_if<std::string>(&v)) {
    if (attribute_value_mode) {
      return *s;
    }
    return quote_script_string(*s);
  }
  if (std::holds_alternative<std::monostate>(v)) {
    return "null";
  }

  // Arrays are forbidden by xml-mapping.md output mode restrictions; keep best-effort.
  if (const auto * ap = std::get_if<ConstArrayPtr>(&v)) {
    if (!ap || !*ap) {
      return std::string{};
    }
    const ConstArrayValue & arr = **ap;
    std::string out = "[";
    const bool rep = arr.repeat_value.has_value();
    const uint64_t len = rep ? arr.repeat_count : static_cast<uint64_t>(arr.elements.size());
    const uint64_t lim = (len > 16) ? 16 : len;
    for (uint64_t i = 0; i < lim; ++i) {
      if (i > 0) out += ", ";
      const ConstValue & el = rep ? *arr.repeat_value : arr.elements[static_cast<size_t>(i)];
      out += format_const_value_for_mode(el, attribute_value_mode);
    }
    if (lim < len) {
      out += ", ...";
    }
    out += "]";
    return out;
  }

  return std::string{};
}

std::string AstToBtCppModelConverter::format_literal_for_script(const Literal & lit)
{
  return std::visit(
    [](const auto & l) -> std::string {
      using T = std::decay_t<decltype(l)>;
      if constexpr (std::is_same_v<T, StringLiteral>) {
        // BT.CPP Script uses single quotes.
        const std::string s = l.value;
        // Escape single quotes in script strings.
        std::string escaped;
        escaped.reserve(s.size());
        for (const char c : s) {
          if (c == '\'') {
            escaped += "\\'";
          } else {
            escaped += c;
          }
        }
        return "'" + escaped + "'";
      } else if constexpr (std::is_same_v<T, IntLiteral>) {
        return std::to_string(l.value);
      } else if constexpr (std::is_same_v<T, FloatLiteral>) {
        return to_string_lossless(l.value);
      } else if constexpr (std::is_same_v<T, BoolLiteral>) {
        return l.value ? "true" : "false";
      } else if constexpr (std::is_same_v<T, NullLiteral>) {
        return "null";
      } else {
        return std::string{};
      }
    },
    lit);
}

std::string AstToBtCppModelConverter::serialize_expression(
  const Expression & expr, CodegenContext & ctx, ExprMode mode)
{
  return std::visit(
    [&](const auto & e) -> std::string {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        if (mode == ExprMode::AttributeValue) {
          // Attribute values: string literals are unquoted.
          return std::visit(
            [&](const auto & lit) -> std::string {
              using L = std::decay_t<decltype(lit)>;
              if constexpr (std::is_same_v<L, StringLiteral>) {
                return lit.value;
              } else if constexpr (std::is_same_v<L, IntLiteral>) {
                return std::to_string(lit.value);
              } else if constexpr (std::is_same_v<L, FloatLiteral>) {
                return to_string_lossless(lit.value);
              } else if constexpr (std::is_same_v<L, BoolLiteral>) {
                return lit.value ? "true" : "false";
              } else if constexpr (std::is_same_v<L, NullLiteral>) {
                return "null";
              } else {
                return std::string{};
              }
            },
            e);
        }
        return format_literal_for_script(e);
      } else if constexpr (std::is_same_v<T, VarRef>) {
        // Attempt const inlining first.
        VarRef vr = e;
        vr.direction = std::nullopt;
        const Expression ve = vr;
        if (const Symbol * sym = ctx.analysis.symbols.resolve(vr.name, ctx.const_scope_ptr());
            sym && sym->is_const() && sym->kind != SymbolKind::Parameter) {
          if (
            auto cv = eval_const_value(
              ve, ctx.const_scope_ptr(), ctx.analysis.symbols, ctx.const_eval, ctx.const_eval_diags,
              /*type_env=*/nullptr, /*current_const_name=*/std::nullopt)) {
            return format_const_value_for_mode(*cv, mode == ExprMode::AttributeValue);
          }
        }

        return ctx.var_ref(e.name, mode);
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const auto & b = *e;
        const auto left = serialize_expression(b.left, ctx, mode);
        const auto right = serialize_expression(b.right, ctx, mode);
        return "(" + left + " " + std::string(to_string(b.op)) + " " + right + ")";
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const auto & u = *e;
        const auto operand = serialize_expression(u.operand, ctx, mode);
        return std::string(to_string(u.op)) + operand;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        // xml-mapping.md output mode forbids casts; best-effort: keep inner expression.
        return serialize_expression(e->expr, ctx, mode);
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        const auto base = serialize_expression(e->base, ctx, mode);
        const auto idx = serialize_expression(e->index, ctx, mode);
        return base + "[" + idx + "]";
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        if (e->repeat_value && e->repeat_count) {
          return "[" + serialize_expression(*e->repeat_value, ctx, mode) + "; " +
                 serialize_expression(*e->repeat_count, ctx, mode) + "]";
        }
        std::string out = "[";
        for (size_t i = 0; i < e->elements.size(); ++i) {
          if (i > 0) {
            out += ", ";
          }
          out += serialize_expression(e->elements[i], ctx, mode);
        }
        out += "]";
        return out;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        std::string out = "vec!";
        out += serialize_expression(Box<ArrayLiteralExpr>(e->value), ctx, mode);
        return out;
      } else {
        return std::string{};
      }
    },
    expr);
}

std::string AstToBtCppModelConverter::serialize_assignment_stmt(
  const AssignmentStmt & stmt, CodegenContext & ctx)
{
  std::string lhs = ctx.var_ref(stmt.target, ExprMode::Script);
  for (const auto & idx : stmt.indices) {
    lhs += "[";
    lhs += serialize_expression(idx, ctx, ExprMode::Script);
    lhs += "]";
  }

  const std::string rhs = serialize_expression(stmt.value, ctx, ExprMode::Script);

  auto expanded_assign = [&](std::string_view op) -> std::string {
    return lhs + " = (" + lhs + " " + std::string(op) + " " + rhs + ")";
  };

  switch (stmt.op) {
    case AssignOp::Assign:
      return lhs + " = " + rhs;
    case AssignOp::AddAssign:
      return expanded_assign("+");
    case AssignOp::SubAssign:
      return expanded_assign("-");
    case AssignOp::MulAssign:
      return expanded_assign("*");
    case AssignOp::DivAssign:
      return expanded_assign("/");
    case AssignOp::ModAssign:
      return expanded_assign("%");
  }
  return lhs + " = " + rhs;
}

std::string AstToBtCppModelConverter::serialize_expression(const Expression & expr)
{
  return std::visit(
    [&](const auto & e) -> std::string {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        return format_literal_for_script(e);
      } else if constexpr (std::is_same_v<T, VarRef>) {
        return e.name;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const auto & b = *e;
        const auto left = serialize_expression(b.left);
        const auto right = serialize_expression(b.right);
        return "(" + left + " " + std::string(to_string(b.op)) + " " + right + ")";
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const auto & u = *e;
        const auto operand = serialize_expression(u.operand);
        return std::string(to_string(u.op)) + operand;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        // BT.CPP Script has no formal cast syntax. Best-effort: keep the inner expression.
        return serialize_expression(e->expr);
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        const auto base = serialize_expression(e->base);
        const auto idx = serialize_expression(e->index);
        return base + "[" + idx + "]";
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        // Best-effort textual form.
        if (e->repeat_value && e->repeat_count) {
          return "[" + serialize_expression(*e->repeat_value) + "; " +
                 serialize_expression(*e->repeat_count) + "]";
        }
        std::string out = "[";
        for (size_t i = 0; i < e->elements.size(); ++i) {
          if (i > 0) {
            out += ", ";
          }
          out += serialize_expression(e->elements[i]);
        }
        out += "]";
        return out;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        // vec![...] is not a native BT.CPP Script construct; keep as text.
        std::string out = "vec!";
        out += serialize_expression(Box<ArrayLiteralExpr>(e->value));
        return out;
      } else {
        return std::string{};
      }
    },
    expr);
}

std::string AstToBtCppModelConverter::serialize_expression_for_precondition(const Expression & expr)
{
  return std::visit(
    [&](const auto & e) -> std::string {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        return format_literal_for_script(e);
      } else if constexpr (std::is_same_v<T, VarRef>) {
        return "{" + e.name + "}";
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const auto & b = *e;
        const auto left = serialize_expression_for_precondition(b.left);
        const auto right = serialize_expression_for_precondition(b.right);
        return "(" + left + " " + std::string(to_string(b.op)) + " " + right + ")";
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const auto & u = *e;
        const auto operand = serialize_expression_for_precondition(u.operand);
        return std::string(to_string(u.op)) + operand;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        // BT.CPP Script has no formal cast syntax. Best-effort: keep the inner expression.
        return serialize_expression_for_precondition(e->expr);
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        const auto base = serialize_expression_for_precondition(e->base);
        const auto idx = serialize_expression_for_precondition(e->index);
        return base + "[" + idx + "]";
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        // Best-effort textual form.
        if (e->repeat_value && e->repeat_count) {
          return "[" + serialize_expression_for_precondition(*e->repeat_value) + "; " +
                 serialize_expression_for_precondition(*e->repeat_count) + "]";
        }
        std::string out = "[";
        for (size_t i = 0; i < e->elements.size(); ++i) {
          if (i > 0) {
            out += ", ";
          }
          out += serialize_expression_for_precondition(e->elements[i]);
        }
        out += "]";
        return out;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        std::string out = "vec!";
        out += serialize_expression_for_precondition(Box<ArrayLiteralExpr>(e->value));
        return out;
      } else {
        return std::string{};
      }
    },
    expr);
}

btcpp::Node AstToBtCppModelConverter::convert_script_stmt(
  std::string code, const std::vector<Precondition> & preconditions,
  const std::vector<std::string> & docs, CodegenContext & ctx)
{
  btcpp::Node script;
  script.tag = "Script";
  script.attributes.push_back(btcpp::Attribute{"code", " " + std::move(code) + " "});

  // Preconditions (Reference: docs/reference/execution-model.md §5.3.3)
  // BT.CPP uses special attributes: _successIf, _failureIf, _skipIf, _while
  // NOTE: @guard is handled by desugaring into a Sequence (same as nodes).
  std::vector<Expression> guard_conditions;
  guard_conditions.reserve(preconditions.size());

  for (const auto & pc : preconditions) {
    if (pc.kind == "guard") {
      guard_conditions.push_back(pc.condition);
      continue;
    }
    std::string attr_name;
    if (pc.kind == "success_if") {
      attr_name = "_successIf";
    } else if (pc.kind == "failure_if") {
      attr_name = "_failureIf";
    } else if (pc.kind == "skip_if") {
      attr_name = "_skipIf";
    } else if (pc.kind == "run_while") {
      attr_name = "_while";
    }
    if (!attr_name.empty()) {
      script.attributes.push_back(
        btcpp::Attribute{
          attr_name, serialize_expression(pc.condition, ctx, ExprMode::Precondition)});
    }
  }

  (void)docs;  // docs are not emitted to XML (xml-mapping.md §11)

  if (guard_conditions.empty()) {
    return script;
  }

  // xml-mapping.md §5.1: @guard(expr) ->
  // <Sequence>
  //   <Script _while="expr" ... />
  //   <AlwaysSuccess _failureIf="!(expr)" />
  // </Sequence>
  Expression combined = guard_conditions.front();
  for (size_t i = 1; i < guard_conditions.size(); ++i) {
    BinaryExpr b;
    b.op = BinaryOp::And;
    b.left = combined;
    b.right = guard_conditions[i];
    combined = Box<BinaryExpr>(std::move(b));
  }
  const std::string expr_str = serialize_expression(combined, ctx, ExprMode::Precondition);

  script.attributes.push_back(btcpp::Attribute{"_while", expr_str});

  btcpp::Node always;
  always.tag = "AlwaysSuccess";
  always.attributes.push_back(btcpp::Attribute{"_failureIf", "!(" + expr_str + ")"});

  btcpp::Node seq;
  seq.tag = "Sequence";
  seq.children.push_back(std::move(script));
  seq.children.push_back(std::move(always));
  return seq;
}

std::string AstToBtCppModelConverter::serialize_argument_value_for_attribute(
  const ArgumentValue & value)
{
  return std::visit(
    [&](const auto & v) -> std::string {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, Expression>) {
        return std::visit(
          [&](const auto & n) -> std::string {
            using E = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<E, Literal>) {
              return std::visit(
                [](const auto & lit) -> std::string {
                  using L = std::decay_t<decltype(lit)>;
                  if constexpr (std::is_same_v<L, StringLiteral>) {
                    return lit.value;
                  } else if constexpr (std::is_same_v<L, IntLiteral>) {
                    return std::to_string(lit.value);
                  } else if constexpr (std::is_same_v<L, FloatLiteral>) {
                    return to_string_lossless(lit.value);
                  } else if constexpr (std::is_same_v<L, BoolLiteral>) {
                    return lit.value ? "true" : "false";
                  } else if constexpr (std::is_same_v<L, NullLiteral>) {
                    return "null";
                  } else {
                    return std::string{};
                  }
                },
                n);
            } else if constexpr (std::is_same_v<E, VarRef>) {
              // BT.CPP blackboard substitutions use {var}.
              return "{" + n.name + "}";
            } else {
              // Best-effort: serialize complex expressions as text.
              return serialize_expression(v);
            }
          },
          v);
      } else if constexpr (std::is_same_v<T, InlineBlackboardDecl>) {
        return "{" + v.name + "}";
      }
    },
    value);
}

std::string AstToBtCppModelConverter::serialize_argument_value_for_attribute(
  const ArgumentValue & value, CodegenContext & ctx)
{
  return std::visit(
    [&](const auto & v) -> std::string {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, Expression>) {
        return serialize_expression(v, ctx, ExprMode::AttributeValue);
      } else if constexpr (std::is_same_v<T, InlineBlackboardDecl>) {
        // xml-mapping.md §6.3.2: requires a pre-Script declaration; handled at node-conversion time.
        // Here we only serialize the resulting variable reference.
        return ctx.var_ref(v.name, ExprMode::AttributeValue);
      }
    },
    value);
}

std::vector<btcpp::Attribute> AstToBtCppModelConverter::convert_arguments_to_attributes(
  const std::vector<Argument> & args, std::string_view node_id, const NodeRegistry & nodes,
  CodegenContext & ctx)
{
  std::vector<btcpp::Attribute> out;
  out.reserve(args.size());

  for (const auto & arg : args) {
    std::optional<std::string> port_name = arg.name;
    if (!port_name) {
      port_name = nodes.get_single_port_name(node_id);
    }
    if (!port_name) {
      continue;  // unknown positional mapping
    }
    out.push_back(
      btcpp::Attribute{*port_name, serialize_argument_value_for_attribute(arg.value, ctx)});
  }

  return out;
}

btcpp::Node AstToBtCppModelConverter::convert_node_stmt(
  const NodeStmt & node, const Program & program, const NodeRegistry & nodes,
  CodegenContext & ctx) const
{
  (void)program;  // reserved for future (e.g., scope-aware formatting)

  // Pre-scripts required by xml-mapping.md §6.3 (defaults / out var / in-expr).
  std::vector<btcpp::Node> pre_scripts;
  pre_scripts.reserve(4);

  btcpp::Node element;
  const NodeInfo * info = nodes.get_node(node.node_name);
  if (info && info->category == NodeCategory::SubTree) {
    element.tag = "SubTree";
    element.attributes.push_back(btcpp::Attribute{"ID", ctx.subtree_xml_id(node.node_name)});
  } else {
    element.tag = node.node_name;
  }

  // ------------------------------------------------------------------------
  // Arguments -> attributes, inserting required pre-Scripts
  // ------------------------------------------------------------------------
  std::unordered_set<std::string> provided_ports;
  provided_ports.reserve(node.args.size());

  // Convert explicitly provided arguments.
  struct PreparedAttr
  {
    std::string port;
    std::string value;
  };
  std::vector<PreparedAttr> prepared;
  prepared.reserve(node.args.size());

  for (const auto & arg : node.args) {
    std::optional<std::string> port_name = arg.name;
    if (!port_name) {
      port_name = nodes.get_single_port_name(node.node_name);
    }
    if (!port_name) {
      continue;
    }
    provided_ports.insert(*port_name);

    const PortInfo * port = nodes.get_port(node.node_name, *port_name);

    std::string attr_value;
    std::visit(
      [&](const auto & v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, InlineBlackboardDecl>) {
          // xml-mapping.md §6.3.2: out var x -> pre-Script declaration.
          const std::string key = ctx.declare_var(v.name);
          pre_scripts.push_back(make_plain_script_node(
            key + " := " + default_init_for_type(port ? port->type_name : std::nullopt)));
          attr_value = ctx.var_ref(v.name, ExprMode::AttributeValue);
        } else if constexpr (std::is_same_v<T, Expression>) {
          // xml-mapping.md §6.3.3: in port with expression -> pre-Script.
          const bool is_in_port = (port && port->direction == PortDirection::In);
          if (is_in_port && !is_simple_value_expr_for_in_port(v)) {
            const std::string tmp_base = "_expr";
            const std::string key = ctx.declare_var(tmp_base);
            const std::string rhs = serialize_expression(v, ctx, ExprMode::Script);
            pre_scripts.push_back(make_plain_script_node(key + " := " + rhs));
            attr_value = ctx.var_ref(tmp_base, ExprMode::AttributeValue);
          } else {
            attr_value = serialize_expression(v, ctx, ExprMode::AttributeValue);
          }
        }
      },
      arg.value);

    prepared.push_back(PreparedAttr{*port_name, std::move(attr_value)});
  }

  // Now: synthesize omitted defaults (after seeing explicit args).
  if (info) {
    for (const auto & p : info->ports) {
      if (p.direction != PortDirection::In) {
        continue;
      }
      if (!p.default_value.has_value()) {
        continue;
      }
      if (provided_ports.count(p.name) > 0) {
        continue;
      }
      // xml-mapping.md §6.3.1: default argument omission -> pre-Script.
      const std::string tmp_base = "_default";
      const std::string key = ctx.declare_var(tmp_base);
      const std::string rhs = serialize_expression(*p.default_value, ctx, ExprMode::Script);
      pre_scripts.push_back(make_plain_script_node(key + " := " + rhs));
      prepared.push_back(PreparedAttr{p.name, ctx.var_ref(tmp_base, ExprMode::AttributeValue)});
    }
  }

  for (auto & pa : prepared) {
    element.attributes.push_back(btcpp::Attribute{std::move(pa.port), std::move(pa.value)});
  }

  // Children
  std::vector<btcpp::Node> converted_children;
  converted_children.reserve(node.children.size());

  // Enter a new block scope for the children block.
  ctx.push_block();
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & ch) {
        using T = std::decay_t<decltype(ch)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          converted_children.push_back(convert_node_stmt(*ch, program, nodes, ctx));
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          if (ch.op == AssignOp::Assign && ch.indices.empty() && is_null_literal_expr(ch.value)) {
            // xml-mapping.md §7.2: null assignment -> UnsetBlackboard.
            btcpp::Node unset;
            unset.tag = "UnsetBlackboard";
            unset.attributes.push_back(
              btcpp::Attribute{"key", ctx.var_ref(ch.target, ExprMode::Script)});
            converted_children.push_back(
              apply_preconditions_and_guard(std::move(unset), ch.preconditions, ctx));
          } else {
            converted_children.push_back(convert_script_stmt(
              serialize_assignment_stmt(ch, ctx), ch.preconditions, ch.docs, ctx));
          }
        } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
          const std::string key = ctx.declare_var(ch.name);
          if (ch.initial_value) {
            btcpp::Node script;
            script.tag = "Script";
            const std::string code =
              key + ":=" + serialize_expression(*ch.initial_value, ctx, ExprMode::Script);
            script.attributes.push_back(btcpp::Attribute{"code", " " + code + " "});
            converted_children.push_back(std::move(script));
          }
        } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
          // xml-mapping.md §4.2: const declarations are not emitted; they are inlined.
          ctx.define_const(ch, SymbolKind::BlockConst);
        }
      },
      child);
  }
  ctx.pop_block();

  // Reference grammar: decorator nodes may be written with multiple children,
  // which must be implicitly wrapped by a Sequence.
  if (info && info->category == NodeCategory::Decorator && converted_children.size() > 1) {
    btcpp::Node seq;
    seq.tag = "Sequence";
    seq.children = std::move(converted_children);
    element.children.push_back(std::move(seq));
  } else {
    element.children = std::move(converted_children);
  }

  // Apply preconditions (including @guard desugaring) to the node element.
  btcpp::Node with_preconds =
    apply_preconditions_and_guard(std::move(element), node.preconditions, ctx);

  // If we needed pre-scripts, wrap everything into a single Sequence.
  if (!pre_scripts.empty()) {
    btcpp::Node seq;
    seq.tag = "Sequence";
    for (auto & s : pre_scripts) {
      seq.children.push_back(std::move(s));
    }
    seq.children.push_back(std::move(with_preconds));
    return seq;
  }

  return with_preconds;
}

// ============================================================================
// Single-output conversion helpers
// ============================================================================

namespace
{

bool is_public_name(std::string_view name) { return !name.empty() && name.front() != '_'; }

struct TreeKey
{
  const Program * program = nullptr;
  std::string name;

  bool operator==(const TreeKey & other) const
  {
    return program == other.program && name == other.name;
  }
};

struct TreeKeyHash
{
  size_t operator()(const TreeKey & k) const noexcept
  {
    const auto h1 = std::hash<const Program *>{}(k.program);
    const auto h2 = std::hash<std::string>{}(k.name);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

const TreeDef * find_tree_def_in_program(const Program & p, std::string_view name)
{
  for (const auto & t : p.trees) {
    if (t.name == name) {
      return &t;
    }
  }
  return nullptr;
}

std::string choose_entry_tree_name(const Program & p)
{
  for (const auto & t : p.trees) {
    if (t.name == "Main") {
      return "Main";
    }
  }
  if (!p.trees.empty()) {
    return p.trees.front().name;
  }
  return "Main";
}

const std::vector<const Program *> & get_direct_imports(const ImportGraph & g, const Program * p)
{
  static const std::vector<const Program *> k_empty;
  if (!p) {
    return k_empty;
  }
  if (auto it = g.find(p); it != g.end()) {
    return it->second;
  }
  return k_empty;
}

// Resolve a tree name within a module's *direct* import visibility.
// - Prefer local definitions.
// - Otherwise search direct imports (public-only).
const TreeDef * resolve_tree_def(
  const ImportGraph & g, const Program * from, std::string_view tree_name,
  const Program *& out_owner)
{
  out_owner = nullptr;
  if (!from) {
    return nullptr;
  }

  if (const TreeDef * local = find_tree_def_in_program(*from, tree_name)) {
    out_owner = from;
    return local;
  }

  // Cross-file visibility: public-only.
  if (!is_public_name(tree_name)) {
    return nullptr;
  }

  for (const Program * imp : get_direct_imports(g, from)) {
    if (!imp) continue;
    if (const TreeDef * t = find_tree_def_in_program(*imp, tree_name)) {
      out_owner = imp;
      return t;
    }
  }

  return nullptr;
}

}  // namespace

btcpp::Document AstToBtCppModelConverter::convert(
  const Program & program, const AnalysisResult & analysis) const
{
  btcpp::Document doc;

  const std::string main_tree = choose_entry_tree_name(program);
  doc.main_tree_to_execute = main_tree;

  // TreeNodesModel manifest
  // - Declared nodes that are used in this program
  // - SubTrees with ports (tree params)
  // - Plus BlackboardExists (required by xml-mapping.md)
  {
    std::unordered_set<std::string> used_node_ids;
    used_node_ids.reserve(64);

    auto collect_used = [&](const std::vector<Statement> & stmts, const auto & self_ref) -> void {
      for (const auto & st : stmts) {
        std::visit(
          [&](const auto & s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
              used_node_ids.insert(s->node_name);
              self_ref(s->children, self_ref);
            }
          },
          st);
      }
    };

    for (const auto & tree : program.trees) {
      collect_used(tree.body, collect_used);
    }

    // Extern node models
    for (const auto & id : used_node_ids) {
      const NodeInfo * info = analysis.all_nodes.get_node(id);
      if (!info) {
        continue;
      }
      if (info->source != NodeSource::Declare) {
        continue;
      }

      btcpp::NodeModel nm;
      switch (info->category) {
        case NodeCategory::Action:
          nm.kind = btcpp::NodeModelKind::Action;
          break;
        case NodeCategory::Condition:
          nm.kind = btcpp::NodeModelKind::Condition;
          break;
        case NodeCategory::Control:
          nm.kind = btcpp::NodeModelKind::Control;
          break;
        case NodeCategory::Decorator:
          nm.kind = btcpp::NodeModelKind::Decorator;
          break;
        case NodeCategory::SubTree:
          continue;
      }
      nm.id = info->id;

      for (const auto & p : info->ports) {
        btcpp::PortModel pm;
        if (p.direction == PortDirection::Out) {
          pm.kind = btcpp::PortKind::Output;
        } else if (p.direction == PortDirection::Ref || p.direction == PortDirection::Mut) {
          pm.kind = btcpp::PortKind::InOut;
        } else {
          pm.kind = btcpp::PortKind::Input;
        }
        pm.name = p.name;
        pm.type = p.type_name;
        nm.ports.push_back(std::move(pm));
      }

      doc.node_models.push_back(std::move(nm));
    }

    // BlackboardExists: always add (spec requirement)
    {
      btcpp::NodeModel nm;
      nm.kind = btcpp::NodeModelKind::Condition;
      nm.id = "BlackboardExists";
      btcpp::PortModel pm;
      pm.kind = btcpp::PortKind::Input;
      pm.name = "key";
      pm.type = std::string{"string"};
      nm.ports.push_back(std::move(pm));
      doc.node_models.push_back(std::move(nm));
    }

    // SubTree models (only for trees with params)
    for (const auto & tree : program.trees) {
      if (tree.params.empty()) {
        continue;
      }

      btcpp::SubTreeModel st;
      st.id = tree.name;

      for (const auto & p : tree.params) {
        btcpp::PortModel pm;
        const auto dir = p.direction.value_or(PortDirection::In);
        if (dir == PortDirection::Out) {
          pm.kind = btcpp::PortKind::Output;
        } else if (dir == PortDirection::Ref || dir == PortDirection::Mut) {
          pm.kind = btcpp::PortKind::InOut;
        } else {
          pm.kind = btcpp::PortKind::Input;
        }
        pm.name = p.name;
        pm.type = p.type_name;
        st.ports.push_back(std::move(pm));
      }

      doc.subtree_models.push_back(std::move(st));
    }
  }

  // BehaviorTrees
  for (const auto & tree : program.trees) {
    btcpp::BehaviorTreeModel tm;
    tm.id = tree.name;

    CodegenContext ctx(program, analysis);

    // Convert Tree body statement block into a single BT.CPP root.
    std::vector<btcpp::Node> roots;
    roots.reserve(tree.body.size());

    auto push_script = [&](std::string code) {
      btcpp::Node script;
      script.tag = "Script";
      script.attributes.push_back(btcpp::Attribute{"code", " " + std::move(code) + " "});
      roots.push_back(std::move(script));
    };

    for (const auto & stmt : tree.body) {
      std::visit(
        [&](const auto & s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
            roots.push_back(convert_node_stmt(*s, program, analysis.nodes, ctx));
          } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
            if (s.op == AssignOp::Assign && s.indices.empty() && is_null_literal_expr(s.value)) {
              btcpp::Node unset;
              unset.tag = "UnsetBlackboard";
              unset.attributes.push_back(
                btcpp::Attribute{"key", ctx.var_ref(s.target, ExprMode::Script)});
              roots.push_back(
                apply_preconditions_and_guard(std::move(unset), s.preconditions, ctx));
            } else {
              roots.push_back(convert_script_stmt(
                serialize_assignment_stmt(s, ctx), s.preconditions, s.docs, ctx));
            }
          } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
            const std::string key = ctx.declare_var(s.name);
            if (s.initial_value) {
              push_script(
                key + ":=" + serialize_expression(*s.initial_value, ctx, ExprMode::Script));
            }
          } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
            // xml-mapping.md §4.2: const declarations are not emitted; they are inlined.
            ctx.define_const(s, SymbolKind::LocalConst);
          }
        },
        stmt);
    }

    if (roots.size() == 1) {
      tm.root = std::move(roots.front());
    } else if (roots.size() > 1) {
      btcpp::Node seq;
      seq.tag = "Sequence";
      seq.children = std::move(roots);
      tm.root = std::move(seq);
    }

    doc.behavior_trees.push_back(std::move(tm));
  }

  return doc;
}

btcpp::Document AstToBtCppModelConverter::convert_single_output(
  const Program & main_program, const AnalysisResult & main_analysis,
  const ImportGraph & direct_imports) const
{
  btcpp::Document doc;

  // Entry-point tree is in the main module and uses its DSL name as-is.
  const std::string entry_tree_name = choose_entry_tree_name(main_program);
  doc.main_tree_to_execute = entry_tree_name;

  // Cache per-module analyses (for correct global/private const + global var handling).
  std::unordered_map<const Program *, AnalysisResult> analysis_cache;
  analysis_cache.reserve(direct_imports.size() + 1);

  auto analysis_for = [&](const Program * p) -> const AnalysisResult & {
    if (p == &main_program) {
      return main_analysis;
    }
    auto it = analysis_cache.find(p);
    if (it != analysis_cache.end()) {
      return it->second;
    }
    const auto & imps = get_direct_imports(direct_imports, p);
    auto inserted = analysis_cache.emplace(p, Analyzer::analyze(*p, imps));
    return inserted.first->second;
  };

  // Discover reachable trees starting from the entry tree.
  std::vector<TreeKey> ordered_trees;
  ordered_trees.reserve(32);
  std::unordered_set<TreeKey, TreeKeyHash> seen;
  seen.reserve(64);

  std::unordered_set<std::string> used_node_ids;
  used_node_ids.reserve(128);

  std::vector<TreeKey> queue;
  queue.reserve(32);

  {
    const Program * owner = nullptr;
    const TreeDef * entry_def =
      resolve_tree_def(direct_imports, &main_program, entry_tree_name, owner);
    if (entry_def && owner) {
      TreeKey k{owner, entry_def->name};
      queue.push_back(k);
    }
  }

  auto visit_stmt_list = [&](
                           const Program * current_prog, const AnalysisResult & a,
                           const std::vector<Statement> & stmts, const auto & self_ref) -> void {
    for (const auto & st : stmts) {
      std::visit(
        [&](const auto & s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
            used_node_ids.insert(s->node_name);

            // Recurse into children first (node may contain nested subtree calls).
            self_ref(current_prog, a, s->children, self_ref);

            const NodeInfo * info = a.all_nodes.get_node(s->node_name);
            if (!info) {
              return;
            }
            if (info->source != NodeSource::Tree || info->category != NodeCategory::SubTree) {
              return;
            }

            const Program * callee_owner = nullptr;
            const TreeDef * callee_def =
              resolve_tree_def(direct_imports, current_prog, s->node_name, callee_owner);
            if (!callee_def || !callee_owner) {
              return;
            }
            TreeKey k{callee_owner, callee_def->name};
            if (seen.insert(k).second) {
              queue.push_back(k);
            }
          }
        },
        st);
    }
  };

  while (!queue.empty()) {
    TreeKey current = std::move(queue.back());
    queue.pop_back();
    if (!current.program) {
      continue;
    }
    if (seen.count(current) == 0) {
      // Entry tree may have been queued before seen insertion.
      (void)seen.insert(current);
    }
    ordered_trees.push_back(current);

    const Program * owner = nullptr;
    const TreeDef * def = resolve_tree_def(direct_imports, current.program, current.name, owner);
    if (!def || !owner) {
      // Should not happen on successful analysis; keep best-effort.
      continue;
    }

    const AnalysisResult & a = analysis_for(owner);
    visit_stmt_list(owner, a, def->body, visit_stmt_list);
  }

  // Assign mangled XML IDs.
  std::unordered_map<const Program *, uint32_t> program_ids;
  program_ids.reserve(ordered_trees.size());
  uint32_t next_prog_id = 1;

  std::unordered_set<std::string> used_tree_ids;
  used_tree_ids.reserve(ordered_trees.size() * 2);

  std::unordered_map<TreeKey, std::string, TreeKeyHash> tree_xml_ids;
  tree_xml_ids.reserve(ordered_trees.size());

  auto ensure_unique = [&](std::string id) -> std::string {
    if (used_tree_ids.insert(id).second) {
      return id;
    }
    for (uint32_t n = 2;; ++n) {
      std::string candidate = id + "_" + std::to_string(n);
      if (used_tree_ids.insert(candidate).second) {
        return candidate;
      }
    }
  };

  for (const auto & k : ordered_trees) {
    if (!k.program) continue;
    if (k.program == &main_program) {
      tree_xml_ids.emplace(k, ensure_unique(k.name));
      continue;
    }
    auto pit = program_ids.find(k.program);
    if (pit == program_ids.end()) {
      pit = program_ids.emplace(k.program, next_prog_id++).first;
    }
    const uint32_t pid = pit->second;
    std::string mangled = std::string{"_SubTree_"} + std::to_string(pid) + "_" + k.name;
    tree_xml_ids.emplace(k, ensure_unique(std::move(mangled)));
  }

  // TreeNodesModel manifest (extern node models + BlackboardExists + SubTree models)
  {
    // Extern node models for any used declared nodes.
    for (const auto & id : used_node_ids) {
      // Find NodeInfo in any reachable module analysis.
      const NodeInfo * info = nullptr;
      for (const auto & tk : ordered_trees) {
        const AnalysisResult & a = analysis_for(tk.program);
        info = a.all_nodes.get_node(id);
        if (info) {
          break;
        }
      }
      if (!info) {
        continue;
      }
      if (info->source != NodeSource::Declare) {
        continue;
      }
      if (info->category == NodeCategory::SubTree) {
        continue;
      }

      btcpp::NodeModel nm;
      switch (info->category) {
        case NodeCategory::Action:
          nm.kind = btcpp::NodeModelKind::Action;
          break;
        case NodeCategory::Condition:
          nm.kind = btcpp::NodeModelKind::Condition;
          break;
        case NodeCategory::Control:
          nm.kind = btcpp::NodeModelKind::Control;
          break;
        case NodeCategory::Decorator:
          nm.kind = btcpp::NodeModelKind::Decorator;
          break;
        case NodeCategory::SubTree:
          continue;
      }
      nm.id = info->id;

      for (const auto & p : info->ports) {
        btcpp::PortModel pm;
        if (p.direction == PortDirection::Out) {
          pm.kind = btcpp::PortKind::Output;
        } else if (p.direction == PortDirection::Ref || p.direction == PortDirection::Mut) {
          pm.kind = btcpp::PortKind::InOut;
        } else {
          pm.kind = btcpp::PortKind::Input;
        }
        pm.name = p.name;
        pm.type = p.type_name;
        nm.ports.push_back(std::move(pm));
      }

      doc.node_models.push_back(std::move(nm));
    }

    // BlackboardExists: always add (spec requirement)
    {
      btcpp::NodeModel nm;
      nm.kind = btcpp::NodeModelKind::Condition;
      nm.id = "BlackboardExists";
      btcpp::PortModel pm;
      pm.kind = btcpp::PortKind::Input;
      pm.name = "key";
      pm.type = std::string{"string"};
      nm.ports.push_back(std::move(pm));
      doc.node_models.push_back(std::move(nm));
    }

    // SubTree models (only for included trees with params)
    for (const auto & k : ordered_trees) {
      const Program * owner = nullptr;
      const TreeDef * def = resolve_tree_def(direct_imports, k.program, k.name, owner);
      if (!def || !owner) {
        continue;
      }
      if (def->params.empty()) {
        continue;
      }

      btcpp::SubTreeModel st;
      st.id = tree_xml_ids.at(k);

      for (const auto & p : def->params) {
        btcpp::PortModel pm;
        const auto dir = p.direction.value_or(PortDirection::In);
        if (dir == PortDirection::Out) {
          pm.kind = btcpp::PortKind::Output;
        } else if (dir == PortDirection::Ref || dir == PortDirection::Mut) {
          pm.kind = btcpp::PortKind::InOut;
        } else {
          pm.kind = btcpp::PortKind::Input;
        }
        pm.name = p.name;
        pm.type = p.type_name;
        st.ports.push_back(std::move(pm));
      }

      doc.subtree_models.push_back(std::move(st));
    }
  }

  // BehaviorTrees
  for (const auto & k : ordered_trees) {
    const Program * owner = nullptr;
    const TreeDef * tree = resolve_tree_def(direct_imports, k.program, k.name, owner);
    if (!tree || !owner) {
      continue;
    }
    const AnalysisResult & a = analysis_for(owner);

    btcpp::BehaviorTreeModel tm;
    tm.id = tree_xml_ids.at(k);

    CodegenContext ctx(*owner, a, get_direct_imports(direct_imports, owner));
    ctx.subtree_id_resolver = [&](std::string_view callee_name) -> std::string {
      const NodeInfo * info = a.all_nodes.get_node(callee_name);
      if (!info || info->source != NodeSource::Tree || info->category != NodeCategory::SubTree) {
        return std::string(callee_name);
      }
      const Program * from_prog = owner;
      const Program * callee_owner = nullptr;
      const TreeDef * callee_def =
        resolve_tree_def(direct_imports, from_prog, callee_name, callee_owner);
      if (!callee_def || !callee_owner) {
        return std::string(callee_name);
      }
      TreeKey ck{callee_owner, callee_def->name};
      auto it = tree_xml_ids.find(ck);
      if (it != tree_xml_ids.end()) {
        return it->second;
      }
      return std::string(callee_name);
    };

    // Convert Tree body statement block into a single BT.CPP root.
    std::vector<btcpp::Node> roots;
    roots.reserve(tree->body.size());

    auto push_script = [&](std::string code) {
      btcpp::Node script;
      script.tag = "Script";
      script.attributes.push_back(btcpp::Attribute{"code", " " + std::move(code) + " "});
      roots.push_back(std::move(script));
    };

    for (const auto & stmt : tree->body) {
      std::visit(
        [&](const auto & s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
            roots.push_back(convert_node_stmt(*s, *owner, a.nodes, ctx));
          } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
            if (s.op == AssignOp::Assign && s.indices.empty() && is_null_literal_expr(s.value)) {
              btcpp::Node unset;
              unset.tag = "UnsetBlackboard";
              unset.attributes.push_back(
                btcpp::Attribute{"key", ctx.var_ref(s.target, ExprMode::Script)});
              roots.push_back(
                apply_preconditions_and_guard(std::move(unset), s.preconditions, ctx));
            } else {
              roots.push_back(convert_script_stmt(
                serialize_assignment_stmt(s, ctx), s.preconditions, s.docs, ctx));
            }
          } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
            const std::string key = ctx.declare_var(s.name);
            if (s.initial_value) {
              push_script(
                key + ":=" + serialize_expression(*s.initial_value, ctx, ExprMode::Script));
            }
          } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
            // xml-mapping.md §4.2: const declarations are not emitted; they are inlined.
            ctx.define_const(s, SymbolKind::LocalConst);
          }
        },
        stmt);
    }

    if (roots.size() == 1) {
      tm.root = std::move(roots.front());
    } else if (roots.size() > 1) {
      btcpp::Node seq;
      seq.tag = "Sequence";
      seq.children = std::move(roots);
      tm.root = std::move(seq);
    }

    doc.behavior_trees.push_back(std::move(tm));
  }

  return doc;
}

// ============================================================================
// BtCppXmlSerializer (tinyxml2)
// ============================================================================

static tinyxml2::XMLElement * append_node_impl(
  tinyxml2::XMLDocument & doc, tinyxml2::XMLElement * parent, const btcpp::Node & node)
{
  auto * elem = doc.NewElement(node.tag.c_str());

  for (const auto & a : node.attributes) {
    elem->SetAttribute(a.key.c_str(), a.value.c_str());
  }

  if (node.text) {
    elem->SetText(node.text->c_str());
  }

  for (const auto & ch : node.children) {
    append_node_impl(doc, elem, ch);
  }

  parent->InsertEndChild(elem);
  return elem;
}

std::string BtCppXmlSerializer::serialize(const btcpp::Document & doc_model)
{
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild(doc.NewDeclaration(R"(xml version="1.0" encoding="UTF-8")"));

  auto * root = doc.NewElement("root");
  root->SetAttribute("BTCPP_format", "4");
  root->SetAttribute("main_tree_to_execute", doc_model.main_tree_to_execute.c_str());
  doc.InsertEndChild(root);

  for (const auto & tree : doc_model.behavior_trees) {
    auto * bt = doc.NewElement("BehaviorTree");
    bt->SetAttribute("ID", tree.id.c_str());
    root->InsertEndChild(bt);

    if (tree.root) {
      append_node_impl(doc, bt, *tree.root);
    }
  }

  // TreeNodesModel (manifest) should appear under <root>.
  if (!doc_model.node_models.empty() || !doc_model.subtree_models.empty()) {
    auto * tnm = doc.NewElement("TreeNodesModel");
    root->InsertEndChild(tnm);

    for (const auto & nm : doc_model.node_models) {
      const char * tag = nullptr;
      switch (nm.kind) {
        case btcpp::NodeModelKind::Action:
          tag = "Action";
          break;
        case btcpp::NodeModelKind::Condition:
          tag = "Condition";
          break;
        case btcpp::NodeModelKind::Control:
          tag = "Control";
          break;
        case btcpp::NodeModelKind::Decorator:
          tag = "Decorator";
          break;
      }
      auto * ne = doc.NewElement(tag);
      ne->SetAttribute("ID", nm.id.c_str());
      tnm->InsertEndChild(ne);

      for (const auto & p : nm.ports) {
        const char * port_tag = nullptr;
        switch (p.kind) {
          case btcpp::PortKind::Input:
            port_tag = "input_port";
            break;
          case btcpp::PortKind::Output:
            port_tag = "output_port";
            break;
          case btcpp::PortKind::InOut:
            port_tag = "inout_port";
            break;
        }
        auto * pe = doc.NewElement(port_tag);
        pe->SetAttribute("name", p.name.c_str());
        if (p.type) {
          pe->SetAttribute("type", p.type->c_str());
        }
        ne->InsertEndChild(pe);
      }
    }

    for (const auto & st : doc_model.subtree_models) {
      auto * sub = doc.NewElement("SubTree");
      sub->SetAttribute("ID", st.id.c_str());
      tnm->InsertEndChild(sub);

      for (const auto & p : st.ports) {
        const char * port_tag = nullptr;
        switch (p.kind) {
          case btcpp::PortKind::Input:
            port_tag = "input_port";
            break;
          case btcpp::PortKind::Output:
            port_tag = "output_port";
            break;
          case btcpp::PortKind::InOut:
            port_tag = "inout_port";
            break;
        }
        auto * pe = doc.NewElement(port_tag);
        pe->SetAttribute("name", p.name.c_str());
        if (p.type) {
          pe->SetAttribute("type", p.type->c_str());
        }
        sub->InsertEndChild(pe);
      }
    }
  }

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return {printer.CStr()};
}

// ============================================================================
// XmlGenerator facade
// ============================================================================

std::string XmlGenerator::generate(const Program & program, const AnalysisResult & analysis)
{
  const AstToBtCppModelConverter converter;
  const auto model = converter.convert(program, analysis);
  return BtCppXmlSerializer::serialize(model);
}

std::string XmlGenerator::generate(
  const Program & main_program, const AnalysisResult & main_analysis,
  const ImportGraph & direct_imports)
{
  const AstToBtCppModelConverter converter;
  const auto model = converter.convert_single_output(main_program, main_analysis, direct_imports);
  return BtCppXmlSerializer::serialize(model);
}

}  // namespace bt_dsl
