// bt_dsl/analyzer.cpp - Semantic analyzer implementation
#include "bt_dsl/semantic/analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "bt_dsl/semantic/init_safety.hpp"

namespace bt_dsl
{

namespace
{
// Defined later in this file.
struct ConstEvalContext;

// Thread-local pointer to the Program currently being validated.
// validate_tree sets this before walking nodes.
thread_local const bt_dsl::Program * g_current_program_for_validation = nullptr;
thread_local const bt_dsl::TypeEnvironment * g_current_type_env_for_validation = nullptr;
thread_local const std::unordered_map<std::string, bt_dsl::Type> *
  g_current_global_types_for_validation = nullptr;
thread_local const std::unordered_set<const void *> *
  g_current_local_global_ast_nodes_for_validation = nullptr;
thread_local ConstEvalContext * g_current_const_eval_ctx_for_validation = nullptr;

// Reference spec: ambiguity across direct imports must be reported at the
// *reference site* (docs/reference/declarations-and-scopes.md 4.1.3/4.2.2).
thread_local const std::unordered_set<std::string> *
  g_current_ambiguous_imported_value_names_for_validation = nullptr;
thread_local const std::unordered_set<std::string> *
  g_current_ambiguous_imported_node_names_for_validation = nullptr;
thread_local const std::unordered_set<std::string> *
  g_current_ambiguous_imported_type_names_for_validation = nullptr;

bool type_contains_infer(const Type & t)
{
  if (std::holds_alternative<TypeInfer>(t.value)) {
    return true;
  }
  if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
    return type_contains_infer(*n->base);
  }
  if (const auto * v = std::get_if<TypeVec>(&t.value)) {
    return type_contains_infer(*v->element);
  }
  if (const auto * a = std::get_if<TypeStaticArray>(&t.value)) {
    return type_contains_infer(*a->element);
  }
  return false;
}

// Defined later in this file.
Type resolve_type_text(std::string_view text, const TypeEnvironment * env);

// Defined later in this file (used by bound normalization helpers).
bool is_public_name(std::string_view name);

// Defined later in this file (const-eval helpers used by type-bound normalization).
std::optional<int64_t> eval_const_i64(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name = std::nullopt);

std::optional<double> eval_const_f64(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name = std::nullopt);

void validate_const_expr(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols,
  const TypeEnvironment * type_env, const std::unordered_set<const void *> & local_global_ast_nodes,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name = std::nullopt);

// --------------------------------------------------------------------------
// Bounded type size normalization
// --------------------------------------------------------------------------

void build_visible_global_const_map(
  const Program & program, const std::vector<const Program *> & imported_programs,
  std::unordered_map<std::string, const ConstDeclStmt *> & out)
{
  out.clear();
  out.reserve(program.global_consts.size() + 32);

  // Local (all, including private)
  for (const auto & c : program.global_consts) {
    out.emplace(c.name, &c);
  }

  // Imported (public only, consistent with value-space visibility)
  for (const auto * imp : imported_programs) {
    if (!imp) continue;
    for (const auto & c : imp->global_consts) {
      if (!is_public_name(c.name)) {
        continue;
      }
      // Do not overwrite existing entries (local wins, and duplicates across
      // imports are handled as ambiguity at reference sites).
      out.emplace(c.name, &c);
    }
  }
}

std::optional<uint64_t> eval_type_bound_u64(
  std::string_view ident, const Scope * scope, const SymbolTable & symbols,
  const TypeEnvironment * type_env, const std::unordered_set<const void *> & local_global_ast_nodes,
  ConstEvalContext & ctx, DiagnosticBag & diagnostics, const SourceRange & diag_range)
{
  VarRef vr;
  vr.name = std::string(ident);
  vr.direction = std::nullopt;
  vr.range = diag_range;
  const Expression expr = vr;

  // Reuse existing const_expr validation for name resolution, ambiguity checks,
  // and the basic "must resolve to const" constraint.
  validate_const_expr(
    expr, scope, symbols, type_env, local_global_ast_nodes, diagnostics, std::nullopt);

  auto v = eval_const_i64(expr, scope, symbols, ctx, diagnostics, std::nullopt);
  if (!v.has_value()) {
    diagnostics.error(
      diag_range, "Type bound '" + std::string(ident) + "' must be an integer constant expression");
    return std::nullopt;
  }
  if (*v < 0) {
    diagnostics.error(diag_range, "Type bound '" + std::string(ident) + "' must be non-negative");
    return std::nullopt;
  }
  return static_cast<uint64_t>(*v);
}

void normalize_bounded_type_sizes_in_place(
  Type & t, const Scope * scope, const SymbolTable & symbols, const TypeEnvironment * type_env,
  const std::unordered_set<const void *> & local_global_ast_nodes, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, const SourceRange & diag_range)
{
  std::visit(
    [&](auto & v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, TypeNullable>) {
        normalize_bounded_type_sizes_in_place(
          *v.base, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics, diag_range);
      } else if constexpr (std::is_same_v<T, TypeVec>) {
        normalize_bounded_type_sizes_in_place(
          *v.element, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics,
          diag_range);
      } else if constexpr (std::is_same_v<T, TypeStaticArray>) {
        normalize_bounded_type_sizes_in_place(
          *v.element, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics,
          diag_range);
        if (auto * id = std::get_if<std::string>(&v.size.value)) {
          if (
            auto lit = eval_type_bound_u64(
              *id, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics,
              diag_range)) {
            v.size.value = *lit;
          }
        }
      } else if constexpr (std::is_same_v<T, TypeBoundedString>) {
        if (auto * id = std::get_if<std::string>(&v.max_bytes.value)) {
          if (
            auto lit = eval_type_bound_u64(
              *id, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics,
              diag_range)) {
            v.max_bytes.value = *lit;
          }
        }
      } else {
        (void)v;
      }
    },
    t.value);
}

std::unordered_set<const void *> collect_local_global_value_ast_nodes(const Program & program)
{
  std::unordered_set<const void *> out;
  out.reserve(program.global_vars.size() + program.global_consts.size());
  for (const auto & gv : program.global_vars) {
    out.insert(&gv);
  }
  for (const auto & gc : program.global_consts) {
    out.insert(&gc);
  }
  return out;
}

std::unordered_map<std::string, Type> build_global_type_cache(
  const SymbolTable & symbols, const TypeEnvironment * env)
{
  std::unordered_map<std::string, Type> out;
  const Scope * g = symbols.global_scope();
  if (!g) {
    return out;
  }

  out.reserve(g->symbols().size());
  for (const auto & kv : g->symbols()) {
    const Symbol & sym = kv.second;
    if (sym.kind != SymbolKind::GlobalVariable && sym.kind != SymbolKind::GlobalConst) {
      continue;
    }
    if (!sym.type_name) {
      continue;
    }
    out.emplace(sym.name, resolve_type_text(*sym.type_name, env));
  }
  return out;
}

bool is_declared_after_in_same_file(
  const Symbol & sym, const SourceRange & use_range,
  const std::unordered_set<const void *> & local_global_ast_nodes)
{
  if (
    (sym.kind != SymbolKind::GlobalVariable && sym.kind != SymbolKind::GlobalConst) ||
    !sym.ast_node || local_global_ast_nodes.count(sym.ast_node) == 0) {
    return false;
  }

  // Byte offsets are within the same source file for local globals; for imported
  // symbols they are not comparable and are excluded above.
  return sym.definition_range.start_byte > use_range.start_byte;
}

// --------------------------------------------------------------------------
// Const evaluation helpers (reference: docs/reference/declarations-and-scopes.md 4.3)
// --------------------------------------------------------------------------

bool checked_add_i64(int64_t a, int64_t b, int64_t & out)
{
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_add_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();
  if ((b > 0 && a > maxv - b) || (b < 0 && a < minv - b)) {
    return false;
  }
  out = a + b;
  return true;
#endif
}

bool checked_sub_i64(int64_t a, int64_t b, int64_t & out)
{
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_sub_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();
  if ((b > 0 && a < minv + b) || (b < 0 && a > maxv + b)) {
    return false;
  }
  out = a - b;
  return true;
#endif
}

bool checked_mul_i64(int64_t a, int64_t b, int64_t & out)
{
#if defined(__GNUC__) || defined(__clang__)
  return !__builtin_mul_overflow(a, b, &out);
#else
  const auto minv = std::numeric_limits<int64_t>::min();
  const auto maxv = std::numeric_limits<int64_t>::max();

  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }

  // Handle the one case where abs() would overflow.
  if ((a == -1 && b == minv) || (b == -1 && a == minv)) {
    return false;
  }

  if (a > 0) {
    if (b > 0) {
      if (a > maxv / b) return false;
    } else {
      if (b < minv / a) return false;
    }
  } else {  // a < 0
    if (b > 0) {
      if (a < minv / b) return false;
    } else {  // b < 0
      if (b < maxv / a) return false;
    }
  }

  out = a * b;
  return true;
#endif
}

bool checked_div_i64(int64_t a, int64_t b, int64_t & out)
{
  if (b == 0) {
    return false;
  }
  // INT64_MIN / -1 overflows.
  if (a == std::numeric_limits<int64_t>::min() && b == -1) {
    return false;
  }
  out = a / b;
  return true;
}

bool checked_mod_i64(int64_t a, int64_t b, int64_t & out)
{
  if (b == 0) {
    return false;
  }
  if (a == std::numeric_limits<int64_t>::min() && b == -1) {
    return false;
  }
  out = a % b;
  return true;
}

struct ConstArrayValue;
using ConstArrayPtr = std::shared_ptr<ConstArrayValue>;

// NOTE: This is an internal semantic-eval representation used only to enforce
// reference-spec const_expr requirements (docs/reference/declarations-and-scopes.md 4.3.4).
using ConstValue = std::variant<int64_t, double, bool, std::string, std::monostate, ConstArrayPtr>;

struct ConstArrayValue
{
  // If repeat_init is used, elements is empty.
  std::vector<ConstValue> elements;

  // repeat_init := value ; count
  std::optional<ConstValue> repeat_value;
  uint64_t repeat_count = 0;
};

bool const_values_equal(const ConstValue & a, const ConstValue & b);

struct ConstEvalContext
{
  // Only global consts (top-level) participate in forward-reference and cycle evaluation.
  std::unordered_map<std::string, const ConstDeclStmt *> global_consts;
  // Memoized integer results for globals.
  std::unordered_map<std::string, std::optional<int64_t>> memo_i64;
  // Memoized float results for globals.
  std::unordered_map<std::string, std::optional<double>> memo_f64;
  // Memoized fully-evaluated const values for globals.
  std::unordered_map<std::string, std::optional<ConstValue>> memo_value;
  // DFS stack to detect cycles during evaluation.
  std::unordered_set<std::string> in_stack;
  // Local const recursion guard (best-effort).
  std::unordered_set<const void *> local_in_stack;
};

std::optional<ConstValue> eval_const_value(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name = std::nullopt);

std::optional<ConstValue> eval_global_const_value(
  std::string_view name, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics)
{
  const std::string key(name);
  if (auto it = ctx.memo_value.find(key); it != ctx.memo_value.end()) {
    return it->second;
  }

  if (ctx.in_stack.count(key)) {
    diagnostics.error(SourceRange{}, "Cyclic constant evaluation: " + key);
    ctx.memo_value[key] = std::nullopt;
    return std::nullopt;
  }

  auto it = ctx.global_consts.find(key);
  if (it == ctx.global_consts.end() || !it->second) {
    ctx.memo_value[key] = std::nullopt;
    return std::nullopt;
  }

  ctx.in_stack.insert(key);
  const auto * decl = it->second;
  auto v = eval_const_value(decl->value, scope, symbols, ctx, diagnostics, key);
  ctx.in_stack.erase(key);

  ctx.memo_value[key] = v;
  return v;
}

std::optional<uint64_t> eval_type_bound_u64_for_const_eval(
  const TypeArraySizeExpr & size, const Scope * scope, const SymbolTable & symbols,
  ConstEvalContext & ctx, DiagnosticBag & diagnostics, const SourceRange & diag_range)
{
  if (const auto * lit = std::get_if<uint64_t>(&size.value)) {
    return *lit;
  }
  const auto * ident = std::get_if<std::string>(&size.value);
  if (!ident) {
    return std::nullopt;
  }
  VarRef vr;
  vr.name = *ident;
  vr.direction = std::nullopt;
  vr.range = diag_range;
  const Expression e = vr;
  auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, std::nullopt);
  if (!v) {
    return std::nullopt;
  }
  const auto * iv = std::get_if<int64_t>(&*v);
  if (!iv) {
    return std::nullopt;
  }
  if (*iv < 0) {
    return std::nullopt;
  }
  if (static_cast<uint64_t>(*iv) > std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(*iv);
}

bool const_values_equal(const ConstValue & a, const ConstValue & b)
{
  if (a.index() != b.index()) {
    // Allow numeric cross-compare for convenience; type errors are diagnosed elsewhere.
    if (const auto * ai = std::get_if<int64_t>(&a)) {
      if (const auto * bf = std::get_if<double>(&b)) {
        return static_cast<double>(*ai) == *bf;
      }
    }
    if (const auto * af = std::get_if<double>(&a)) {
      if (const auto * bi = std::get_if<int64_t>(&b)) {
        return *af == static_cast<double>(*bi);
      }
    }
    return false;
  }

  if (const auto * ai = std::get_if<int64_t>(&a)) return *ai == std::get<int64_t>(b);
  if (const auto * af = std::get_if<double>(&a)) return *af == std::get<double>(b);
  if (const auto * ab = std::get_if<bool>(&a)) return *ab == std::get<bool>(b);
  if (const auto * as = std::get_if<std::string>(&a)) return *as == std::get<std::string>(b);
  if (std::holds_alternative<std::monostate>(a)) return true;  // null

  const auto * aa = std::get_if<ConstArrayPtr>(&a);
  const auto * ba = std::get_if<ConstArrayPtr>(&b);
  if (!aa || !ba || !*aa || !*ba) {
    return false;
  }

  const ConstArrayValue & av = **aa;
  const ConstArrayValue & bv = **ba;

  const bool a_repeat = av.repeat_value.has_value();
  const bool b_repeat = bv.repeat_value.has_value();
  if (a_repeat != b_repeat) {
    // Slow path: compare expanded elementwise if sizes are known and reasonable.
    const auto len_a = a_repeat ? av.repeat_count : av.elements.size();
    const auto len_b = b_repeat ? bv.repeat_count : bv.elements.size();
    if (len_a != len_b) return false;
    const uint64_t len = len_a;
    if (len > 1024) {
      return false;
    }
    for (uint64_t i = 0; i < len; ++i) {
      ConstValue ea;
      ConstValue eb;
      if (a_repeat) {
        ea = *av.repeat_value;
      } else {
        ea = av.elements[static_cast<size_t>(i)];
      }
      if (b_repeat) {
        eb = *bv.repeat_value;
      } else {
        eb = bv.elements[static_cast<size_t>(i)];
      }
      if (!const_values_equal(ea, eb)) return false;
    }
    return true;
  }

  if (a_repeat) {
    if (av.repeat_count != bv.repeat_count) return false;
    return const_values_equal(*av.repeat_value, *bv.repeat_value);
  }

  if (av.elements.size() != bv.elements.size()) return false;
  for (size_t i = 0; i < av.elements.size(); ++i) {
    if (!const_values_equal(av.elements[i], bv.elements[i])) return false;
  }
  return true;
}

std::optional<ConstValue> eval_const_value(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name)
{
  auto eval_as_bool = [&](const Expression & e) -> std::optional<bool> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * b = std::get_if<bool>(&*v)) return *b;
    return std::nullopt;
  };

  auto eval_as_i64 = [&](const Expression & e) -> std::optional<int64_t> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * i = std::get_if<int64_t>(&*v)) return *i;
    return std::nullopt;
  };

  auto eval_as_f64 = [&](const Expression & e) -> std::optional<double> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * f = std::get_if<double>(&*v)) return *f;
    if (const auto * i = std::get_if<int64_t>(&*v)) return static_cast<double>(*i);
    return std::nullopt;
  };

  auto eval_as_string = [&](const Expression & e) -> std::optional<std::string> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * s = std::get_if<std::string>(&*v)) return *s;
    return std::nullopt;
  };

  return std::visit(
    [&](const auto & e) -> std::optional<ConstValue> {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        if (const auto * i = std::get_if<IntLiteral>(&e)) return ConstValue{i->value};
        if (const auto * f = std::get_if<FloatLiteral>(&e)) return ConstValue{f->value};
        if (const auto * b = std::get_if<BoolLiteral>(&e)) return ConstValue{b->value};
        if (const auto * s = std::get_if<StringLiteral>(&e)) return ConstValue{s->value};
        if (std::holds_alternative<NullLiteral>(e)) return ConstValue{std::monostate{}};
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        if (current_const_name && e.name == *current_const_name) {
          // already diagnosed elsewhere
          return std::nullopt;
        }
        if (e.direction.has_value()) {
          return std::nullopt;
        }
        const Symbol * sym = symbols.resolve(e.name, scope);
        if (!sym || !sym->is_const() || sym->kind == SymbolKind::Parameter) {
          return std::nullopt;
        }
        if (sym->kind == SymbolKind::GlobalConst) {
          return eval_global_const_value(sym->name, scope, symbols, ctx, diagnostics);
        }

        if (!sym->ast_node) {
          return std::nullopt;
        }
        if (ctx.local_in_stack.count(sym->ast_node)) {
          diagnostics.error(e.range, "Cyclic constant evaluation: " + sym->name);
          return std::nullopt;
        }
        const auto * decl = static_cast<const ConstDeclStmt *>(sym->ast_node);
        ctx.local_in_stack.insert(sym->ast_node);
        auto v = eval_const_value(decl->value, scope, symbols, ctx, diagnostics, sym->name);
        ctx.local_in_stack.erase(sym->ast_node);
        return v;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        if (e->op == UnaryOp::Not) {
          auto bv = eval_as_bool(e->operand);
          if (!bv) return std::nullopt;
          return ConstValue{!(*bv)};
        }
        if (e->op == UnaryOp::Neg) {
          if (auto iv = eval_as_i64(e->operand)) {
            if (*iv == std::numeric_limits<int64_t>::min()) {
              diagnostics.error(e->range, "Integer overflow in constant expression");
              return std::nullopt;
            }
            return ConstValue{-(*iv)};
          }
          auto fv = eval_as_f64(e->operand);
          if (!fv) return std::nullopt;
          const double out = -(*fv);
          if (!std::isfinite(out)) {
            diagnostics.error(
              e->range, "Float overflow or invalid operation in constant expression");
            return std::nullopt;
          }
          return ConstValue{out};
        }
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const BinaryOp op = e->op;

        // Logical operators (short-circuit)
        if (op == BinaryOp::And) {
          auto lv = eval_as_bool(e->left);
          if (!lv) return std::nullopt;
          if (!*lv) return ConstValue{false};
          auto rv = eval_as_bool(e->right);
          if (!rv) return std::nullopt;
          return ConstValue{*rv};
        }
        if (op == BinaryOp::Or) {
          auto lv = eval_as_bool(e->left);
          if (!lv) return std::nullopt;
          if (*lv) return ConstValue{true};
          auto rv = eval_as_bool(e->right);
          if (!rv) return std::nullopt;
          return ConstValue{*rv};
        }

        // Equality
        if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
          auto lv = eval_const_value(e->left, scope, symbols, ctx, diagnostics, current_const_name);
          auto rv =
            eval_const_value(e->right, scope, symbols, ctx, diagnostics, current_const_name);
          if (!lv || !rv) return std::nullopt;
          const bool eq = const_values_equal(*lv, *rv);
          return ConstValue{op == BinaryOp::Eq ? eq : !eq};
        }

        // Comparisons (numeric)
        if (op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge) {
          if (auto li = eval_as_i64(e->left); li.has_value()) {
            if (auto ri = eval_as_i64(e->right); ri.has_value()) {
              const int64_t l = *li;
              const int64_t r = *ri;
              switch (op) {
                case BinaryOp::Lt:
                  return ConstValue{l < r};
                case BinaryOp::Le:
                  return ConstValue{l <= r};
                case BinaryOp::Gt:
                  return ConstValue{l > r};
                case BinaryOp::Ge:
                  return ConstValue{l >= r};
                default:
                  break;
              }
            }
          }

          auto lf = eval_as_f64(e->left);
          auto rf = eval_as_f64(e->right);
          if (!lf || !rf) return std::nullopt;
          const double l = *lf;
          const double r = *rf;
          switch (op) {
            case BinaryOp::Lt:
              return ConstValue{l < r};
            case BinaryOp::Le:
              return ConstValue{l <= r};
            case BinaryOp::Gt:
              return ConstValue{l > r};
            case BinaryOp::Ge:
              return ConstValue{l >= r};
            default:
              break;
          }
          return std::nullopt;
        }

        // String concatenation
        if (op == BinaryOp::Add) {
          auto ls = eval_as_string(e->left);
          auto rs = eval_as_string(e->right);
          if (ls && rs) {
            return ConstValue{*ls + *rs};
          }
        }

        // Integer ops
        if (
          op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
          op == BinaryOp::Div || op == BinaryOp::Mod || op == BinaryOp::BitAnd ||
          op == BinaryOp::BitOr) {
          if (auto li = eval_as_i64(e->left); li.has_value()) {
            if (auto ri = eval_as_i64(e->right); ri.has_value()) {
              const int64_t l = *li;
              const int64_t r = *ri;
              int64_t out = 0;
              switch (op) {
                case BinaryOp::Add:
                  if (!checked_add_i64(l, r, out)) {
                    diagnostics.error(e->range, "Integer overflow in constant expression");
                    return std::nullopt;
                  }
                  return ConstValue{out};
                case BinaryOp::Sub:
                  if (!checked_sub_i64(l, r, out)) {
                    diagnostics.error(e->range, "Integer overflow in constant expression");
                    return std::nullopt;
                  }
                  return ConstValue{out};
                case BinaryOp::Mul:
                  if (!checked_mul_i64(l, r, out)) {
                    diagnostics.error(e->range, "Integer overflow in constant expression");
                    return std::nullopt;
                  }
                  return ConstValue{out};
                case BinaryOp::Div:
                  if (!checked_div_i64(l, r, out)) {
                    diagnostics.error(
                      e->range, "Division by zero or overflow in constant expression");
                    return std::nullopt;
                  }
                  return ConstValue{out};
                case BinaryOp::Mod:
                  if (!checked_mod_i64(l, r, out)) {
                    diagnostics.error(
                      e->range, "Modulo by zero or overflow in constant expression");
                    return std::nullopt;
                  }
                  return ConstValue{out};
                case BinaryOp::BitAnd:
                  return ConstValue{l & r};
                case BinaryOp::BitOr:
                  return ConstValue{l | r};
                default:
                  break;
              }
            }
          }
        }

        // Float ops
        if (
          op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
          op == BinaryOp::Div) {
          auto lf = eval_as_f64(e->left);
          auto rf = eval_as_f64(e->right);
          if (!lf || !rf) return std::nullopt;
          const double l = *lf;
          const double r = *rf;
          double out = 0.0;
          switch (op) {
            case BinaryOp::Add:
              out = l + r;
              break;
            case BinaryOp::Sub:
              out = l - r;
              break;
            case BinaryOp::Mul:
              out = l * r;
              break;
            case BinaryOp::Div:
              if (r == 0.0) {
                diagnostics.error(
                  e->range, "Float overflow or invalid operation in constant expression");
                return std::nullopt;
              }
              out = l / r;
              break;
            default:
              break;
          }
          if (!std::isfinite(out)) {
            diagnostics.error(
              e->range, "Float overflow or invalid operation in constant expression");
            return std::nullopt;
          }
          return ConstValue{out};
        }

        return std::nullopt;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        auto src = eval_const_value(e->expr, scope, symbols, ctx, diagnostics, current_const_name);
        if (!src) return std::nullopt;

        TypeParseResult parsed = Type::parse(e->type_name);
        if (parsed.has_error()) {
          return std::nullopt;
        }
        Type tgt = std::move(parsed.type);
        if (g_current_type_env_for_validation) {
          tgt = g_current_type_env_for_validation->resolve(tgt);
        }

        // Evaluate casts against the base type for nullable wrappers.
        if (const auto * n = std::get_if<TypeNullable>(&tgt.value)) {
          tgt = *n->base;
        }

        // Extern/vec are forbidden in const_expr; already diagnosed elsewhere.
        if (
          std::holds_alternative<TypeExtern>(tgt.value) ||
          std::holds_alternative<TypeVec>(tgt.value)) {
          return std::nullopt;
        }

        if (std::holds_alternative<TypeBool>(tgt.value)) {
          if (const auto * b = std::get_if<bool>(&*src)) {
            return ConstValue{*b};
          }
          return std::nullopt;
        }

        if (std::holds_alternative<TypeString>(tgt.value)) {
          if (const auto * s = std::get_if<std::string>(&*src)) {
            return ConstValue{*s};
          }
          return std::nullopt;
        }

        if (const auto * bs = std::get_if<TypeBoundedString>(&tgt.value)) {
          const auto * s = std::get_if<std::string>(&*src);
          if (!s) return std::nullopt;

          // If the max bound is a const identifier, attempt to evaluate it.
          std::optional<uint64_t> bound = eval_type_bound_u64_for_const_eval(
            bs->max_bytes, scope, symbols, ctx, diagnostics, e->range);
          if (bound.has_value()) {
            if (s->size() > *bound) {
              diagnostics.error(
                e->range, "Bounded string literal exceeds max_bytes in constant expression");
              return std::nullopt;
            }
          }
          return ConstValue{*s};
        }

        if (const auto * ti = std::get_if<TypeInt>(&tgt.value)) {
          auto range_ok = [&](int64_t val) -> bool {
            bool ok = true;
            if (ti->is_signed) {
              int64_t lo = 0;
              int64_t hi = 0;
              if (ti->bits == 64) {
                lo = std::numeric_limits<int64_t>::min();
                hi = std::numeric_limits<int64_t>::max();
              } else if (ti->bits == 0 || ti->bits >= 64) {
                ok = false;
              } else {
                lo = -(int64_t(1) << (ti->bits - 1));
                hi = (int64_t(1) << (ti->bits - 1)) - 1;
              }
              if (ok) {
                ok = (lo <= val) && (val <= hi);
              }
            } else {
              if (val < 0) {
                ok = false;
              } else if (ti->bits < 64) {
                const uint64_t maxu = (ti->bits == 0) ? 0ULL : ((uint64_t(1) << ti->bits) - 1ULL);
                ok = static_cast<uint64_t>(val) <= maxu;
              }
            }
            return ok;
          };

          int64_t val = 0;
          if (const auto * iv = std::get_if<int64_t>(&*src)) {
            val = *iv;
          } else if (const auto * fv = std::get_if<double>(&*src)) {
            if (!std::isfinite(*fv)) {
              diagnostics.error(e->range, "Cast out of range in constant expression");
              return std::nullopt;
            }
            const double trunc = std::trunc(*fv);
            if (trunc != *fv) {
              // Cast semantics are implementation-defined; we require exact integer representability in const eval.
              return std::nullopt;
            }
            if (
              trunc < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
              trunc > static_cast<double>(std::numeric_limits<int64_t>::max())) {
              diagnostics.error(e->range, "Cast out of range in constant expression");
              return std::nullopt;
            }
            val = static_cast<int64_t>(trunc);
          } else {
            return std::nullopt;
          }

          if (!range_ok(val)) {
            diagnostics.error(e->range, "Cast out of range in constant expression");
            return std::nullopt;
          }
          return ConstValue{val};
        }

        if (std::holds_alternative<TypeFloat>(tgt.value)) {
          double val = 0.0;
          if (const auto * fv = std::get_if<double>(&*src)) {
            val = *fv;
          } else if (const auto * iv = std::get_if<int64_t>(&*src)) {
            val = static_cast<double>(*iv);
          } else {
            return std::nullopt;
          }
          if (!std::isfinite(val)) {
            diagnostics.error(
              e->range, "Float overflow or invalid operation in constant expression");
            return std::nullopt;
          }
          return ConstValue{val};
        }

        return std::nullopt;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        auto base_v =
          eval_const_value(e->base, scope, symbols, ctx, diagnostics, current_const_name);
        auto idx_v = eval_as_i64(e->index);
        if (!base_v || !idx_v) return std::nullopt;
        if (*idx_v < 0) {
          diagnostics.error(e->range, "Array index out of bounds in constant expression");
          return std::nullopt;
        }
        const auto * ap = std::get_if<ConstArrayPtr>(&*base_v);
        if (!ap || !*ap) return std::nullopt;
        const ConstArrayValue & arr = **ap;

        const auto i = static_cast<uint64_t>(*idx_v);
        const uint64_t len = arr.repeat_value.has_value()
                               ? arr.repeat_count
                               : static_cast<uint64_t>(arr.elements.size());
        if (i >= len) {
          diagnostics.error(e->range, "Array index out of bounds in constant expression");
          return std::nullopt;
        }
        if (arr.repeat_value.has_value()) {
          return arr.repeat_value;
        }
        return arr.elements[static_cast<size_t>(i)];
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        auto out = std::make_shared<ConstArrayValue>();
        if (e->repeat_value.has_value()) {
          auto rv = eval_const_value(
            *e->repeat_value, scope, symbols, ctx, diagnostics, current_const_name);
          if (!rv) return std::nullopt;
          auto rc = eval_as_i64(*e->repeat_count);
          if (!rc) return std::nullopt;
          if (*rc < 0) {
            diagnostics.error(e->range, "Array repeat count must be non-negative");
            return std::nullopt;
          }
          out->repeat_value = *rv;
          out->repeat_count = static_cast<uint64_t>(*rc);
          return ConstValue{out};
        }

        out->elements.reserve(e->elements.size());
        for (const auto & el : e->elements) {
          auto v = eval_const_value(el, scope, symbols, ctx, diagnostics, current_const_name);
          if (!v) return std::nullopt;
          out->elements.push_back(*v);
        }
        return ConstValue{out};
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        // Forbidden in const_expr.
        return std::nullopt;
      }

      return std::nullopt;
    },
    expr);
}

std::optional<int64_t> eval_global_const_i64(
  std::string_view name, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics)
{
  const std::string key(name);
  if (auto it = ctx.memo_i64.find(key); it != ctx.memo_i64.end()) {
    return it->second;
  }

  if (ctx.in_stack.count(key)) {
    // Cycle should already be diagnosed by the explicit cycle checker, but keep
    // this as a safety net.
    diagnostics.error(SourceRange{}, "Cyclic constant evaluation: " + key);
    ctx.memo_i64[key] = std::nullopt;
    return std::nullopt;
  }

  auto it = ctx.global_consts.find(key);
  if (it == ctx.global_consts.end() || !it->second) {
    ctx.memo_i64[key] = std::nullopt;
    return std::nullopt;
  }

  ctx.in_stack.insert(key);
  const auto * decl = it->second;
  auto v = eval_const_i64(decl->value, scope, symbols, ctx, diagnostics, key);
  ctx.in_stack.erase(key);

  ctx.memo_i64[key] = v;
  return v;
}

std::optional<double> eval_global_const_f64(
  std::string_view name, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics)
{
  const std::string key(name);
  if (auto it = ctx.memo_f64.find(key); it != ctx.memo_f64.end()) {
    return it->second;
  }

  if (ctx.in_stack.count(key)) {
    diagnostics.error(SourceRange{}, "Cyclic constant evaluation: " + key);
    ctx.memo_f64[key] = std::nullopt;
    return std::nullopt;
  }

  auto it = ctx.global_consts.find(key);
  if (it == ctx.global_consts.end() || !it->second) {
    ctx.memo_f64[key] = std::nullopt;
    return std::nullopt;
  }

  ctx.in_stack.insert(key);
  const auto * decl = it->second;
  auto v = eval_const_f64(decl->value, scope, symbols, ctx, diagnostics, key);
  ctx.in_stack.erase(key);

  ctx.memo_f64[key] = v;
  return v;
}

std::optional<int64_t> eval_const_i64(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name)
{
  return std::visit(
    [&](const auto & e) -> std::optional<int64_t> {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        if (const auto * i = std::get_if<IntLiteral>(&e)) {
          return i->value;
        }
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        if (current_const_name && e.name == *current_const_name) {
          // already diagnosed elsewhere
          return std::nullopt;
        }
        const Symbol * sym = symbols.resolve(e.name, scope);
        if (!sym || !sym->is_const()) {
          return std::nullopt;
        }
        if (sym->kind == SymbolKind::GlobalConst) {
          return eval_global_const_i64(sym->name, scope, symbols, ctx, diagnostics);
        }

        // local const forward refs are not allowed by scope rules; if it exists,
        // it is already defined and can be evaluated using its AST.
        if (!sym->ast_node) {
          return std::nullopt;
        }
        if (ctx.local_in_stack.count(sym->ast_node)) {
          diagnostics.error(e.range, "Cyclic constant evaluation: " + sym->name);
          return std::nullopt;
        }
        const auto * decl = static_cast<const ConstDeclStmt *>(sym->ast_node);
        ctx.local_in_stack.insert(sym->ast_node);
        auto v = eval_const_i64(decl->value, scope, symbols, ctx, diagnostics, sym->name);
        ctx.local_in_stack.erase(sym->ast_node);
        return v;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        auto v = eval_const_i64(e->operand, scope, symbols, ctx, diagnostics, current_const_name);
        if (!v) return std::nullopt;
        if (e->op == UnaryOp::Neg) {
          if (*v == std::numeric_limits<int64_t>::min()) {
            diagnostics.error(e->range, "Integer overflow in constant expression");
            return std::nullopt;
          }
          return -(*v);
        }
        // '!' is boolean
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        auto lv = eval_const_i64(e->left, scope, symbols, ctx, diagnostics, current_const_name);
        auto rv = eval_const_i64(e->right, scope, symbols, ctx, diagnostics, current_const_name);
        if (!lv || !rv) return std::nullopt;

        int64_t out = 0;
        switch (e->op) {
          case BinaryOp::Add:
            if (!checked_add_i64(*lv, *rv, out)) {
              diagnostics.error(e->range, "Integer overflow in constant expression");
              return std::nullopt;
            }
            return out;
          case BinaryOp::Sub:
            if (!checked_sub_i64(*lv, *rv, out)) {
              diagnostics.error(e->range, "Integer overflow in constant expression");
              return std::nullopt;
            }
            return out;
          case BinaryOp::Mul:
            if (!checked_mul_i64(*lv, *rv, out)) {
              diagnostics.error(e->range, "Integer overflow in constant expression");
              return std::nullopt;
            }
            return out;
          case BinaryOp::Div:
            if (!checked_div_i64(*lv, *rv, out)) {
              diagnostics.error(e->range, "Division by zero or overflow in constant expression");
              return std::nullopt;
            }
            return out;
          case BinaryOp::Mod:
            if (!checked_mod_i64(*lv, *rv, out)) {
              diagnostics.error(e->range, "Modulo by zero or overflow in constant expression");
              return std::nullopt;
            }
            return out;
          case BinaryOp::BitAnd:
            return (*lv) & (*rv);
          case BinaryOp::BitOr:
            return (*lv) | (*rv);
          default:
            // comparisons/logical operators are non-integer
            return std::nullopt;
        }
      }
      return std::nullopt;
    },
    expr);
}

std::optional<double> eval_const_f64(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name)
{
  return std::visit(
    [&](const auto & e) -> std::optional<double> {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        if (const auto * f = std::get_if<FloatLiteral>(&e)) {
          return f->value;
        }
        if (const auto * i = std::get_if<IntLiteral>(&e)) {
          return static_cast<double>(i->value);
        }
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        if (current_const_name && e.name == *current_const_name) {
          // already diagnosed elsewhere
          return std::nullopt;
        }
        const Symbol * sym = symbols.resolve(e.name, scope);
        if (!sym || !sym->is_const()) {
          return std::nullopt;
        }
        if (sym->kind == SymbolKind::GlobalConst) {
          return eval_global_const_f64(sym->name, scope, symbols, ctx, diagnostics);
        }

        if (!sym->ast_node) {
          return std::nullopt;
        }
        if (ctx.local_in_stack.count(sym->ast_node)) {
          diagnostics.error(e.range, "Cyclic constant evaluation: " + sym->name);
          return std::nullopt;
        }
        const auto * decl = static_cast<const ConstDeclStmt *>(sym->ast_node);
        ctx.local_in_stack.insert(sym->ast_node);
        auto v = eval_const_f64(decl->value, scope, symbols, ctx, diagnostics, sym->name);
        ctx.local_in_stack.erase(sym->ast_node);
        return v;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        auto v = eval_const_f64(e->operand, scope, symbols, ctx, diagnostics, current_const_name);
        if (!v) return std::nullopt;
        if (e->op == UnaryOp::Neg) {
          const double out = -(*v);
          if (!std::isfinite(out)) {
            diagnostics.error(
              e->range, "Float overflow or invalid operation in constant expression");
            return std::nullopt;
          }
          return out;
        }
        // '!' is boolean
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        auto lv = eval_const_f64(e->left, scope, symbols, ctx, diagnostics, current_const_name);
        auto rv = eval_const_f64(e->right, scope, symbols, ctx, diagnostics, current_const_name);
        if (!lv || !rv) return std::nullopt;

        double out = 0.0;
        switch (e->op) {
          case BinaryOp::Add:
            out = (*lv) + (*rv);
            break;
          case BinaryOp::Sub:
            out = (*lv) - (*rv);
            break;
          case BinaryOp::Mul:
            out = (*lv) * (*rv);
            break;
          case BinaryOp::Div:
            // The reference spec requires div-by-zero/overflow to be diagnosed.
            if (*rv == 0.0) {
              diagnostics.error(
                e->range, "Float overflow or invalid operation in constant expression");
              return std::nullopt;
            }
            out = (*lv) / (*rv);
            break;
          default:
            // comparisons/logical/bitwise/mod are not float constants here
            return std::nullopt;
        }

        if (!std::isfinite(out)) {
          diagnostics.error(e->range, "Float overflow or invalid operation in constant expression");
          return std::nullopt;
        }
        return out;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        // Best-effort: allow casting integer/float consts to float types.
        auto fv = eval_const_f64(e->expr, scope, symbols, ctx, diagnostics, current_const_name);
        if (!fv) {
          // If the expression is integer-only, still allow int->float.
          auto iv = eval_const_i64(e->expr, scope, symbols, ctx, diagnostics, current_const_name);
          if (!iv) return std::nullopt;
          fv = static_cast<double>(*iv);
        }

        TypeParseResult parsed = Type::parse(e->type_name);
        if (parsed.has_error()) {
          return std::nullopt;
        }
        Type tgt = std::move(parsed.type);
        if (g_current_type_env_for_validation) {
          tgt = g_current_type_env_for_validation->resolve(tgt);
        }
        if (std::holds_alternative<TypeFloat>(tgt.value)) {
          if (!std::isfinite(*fv)) {
            diagnostics.error(
              e->range, "Float overflow or invalid operation in constant expression");
            return std::nullopt;
          }
          return fv;
        }

        // Not a float cast target.
        return std::nullopt;
      }
      return std::nullopt;
    },
    expr);
}

void validate_static_array_repeat_init_fits_target(
  const Expression & initializer, Type target_type, const Scope * scope,
  const SymbolTable & symbols, const TypeEnvironment * type_env,
  const std::unordered_set<const void *> & local_global_ast_nodes, DiagnosticBag & diagnostics,
  ConstEvalContext & ctx)
{
  // Only applies to repeat-init array literals.
  const auto * arr_lit = std::get_if<Box<ArrayLiteralExpr>>(&initializer);
  if (!arr_lit) return;
  const ArrayLiteralExpr & lit = **arr_lit;
  if (!lit.repeat_count) return;

  // Normalize any bounded sizes that use const identifiers.
  if (type_env) {
    target_type = type_env->resolve(target_type);
  }
  normalize_bounded_type_sizes_in_place(
    target_type, scope, symbols, type_env, local_global_ast_nodes, ctx, diagnostics, lit.range);

  const auto * tgt_arr = std::get_if<TypeStaticArray>(&target_type.value);
  if (!tgt_arr) return;

  const auto * bound = std::get_if<uint64_t>(&tgt_arr->size.value);
  if (!bound) {
    // Symbolic bound: nothing to compare.
    return;
  }

  // Determine repeat-init length via const evaluation.
  const auto n = eval_const_i64(*lit.repeat_count, scope, symbols, ctx, diagnostics, std::nullopt);
  if (!n.has_value()) {
    return;
  }
  if (*n < 0) {
    return;
  }
  const auto len = static_cast<uint64_t>(*n);

  if (tgt_arr->size_kind == TypeArraySizeKind::Exact) {
    if (len != *bound) {
      diagnostics.error(
        lit.range, "array length mismatch (expected " + std::to_string(*bound) + ", got " +
                     std::to_string(len) + ")");
    }
  } else {
    if (len > *bound) {
      diagnostics.error(
        lit.range, "array length exceeds bound (<= " + std::to_string(*bound) + ", got " +
                     std::to_string(len) + ")");
    }
  }
}

void validate_repeat_init_counts_in_expr(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols,
  const TypeEnvironment * type_env, const std::unordered_set<const void *> & local_global_ast_nodes,
  DiagnosticBag & diagnostics, ConstEvalContext & ctx, bool in_vec_macro = false,
  bool already_validated_const = false,
  std::optional<std::string_view> current_const_name = std::nullopt)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal> || std::is_same_v<T, VarRef>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        validate_repeat_init_counts_in_expr(
          e->left, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx, in_vec_macro,
          already_validated_const, current_const_name);
        validate_repeat_init_counts_in_expr(
          e->right, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx,
          in_vec_macro, already_validated_const, current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        validate_repeat_init_counts_in_expr(
          e->operand, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx,
          in_vec_macro, already_validated_const, current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        validate_repeat_init_counts_in_expr(
          e->expr, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx, in_vec_macro,
          already_validated_const, current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        validate_repeat_init_counts_in_expr(
          e->base, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx, in_vec_macro,
          already_validated_const, current_const_name);
        validate_repeat_init_counts_in_expr(
          e->index, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx,
          in_vec_macro, already_validated_const, current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          validate_repeat_init_counts_in_expr(
            el, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx, in_vec_macro,
            already_validated_const, current_const_name);
        }
        if (e->repeat_value) {
          validate_repeat_init_counts_in_expr(
            *e->repeat_value, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx,
            in_vec_macro, already_validated_const, current_const_name);
        }
        if (e->repeat_count) {
          if (!in_vec_macro) {
            // Reference: docs/reference/syntax.md (repeat_init) + docs/reference/type-system/inference-and-resolution.md
            // Static-array repeat count must be an integer const_expr so the array length is compile-time known.
            if (!already_validated_const) {
              validate_const_expr(
                *e->repeat_count, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
                current_const_name);
            }

            auto n = eval_const_i64(
              *e->repeat_count, scope, symbols, ctx, diagnostics, current_const_name);
            if (!n.has_value()) {
              diagnostics.error(
                get_range(*e->repeat_count),
                "Array repeat count must be an integer constant expression");
            } else if (*n < 0) {
              diagnostics.error(
                get_range(*e->repeat_count), "Array repeat count must be non-negative");
            }
          }
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        for (const auto & el : e->value.elements) {
          validate_repeat_init_counts_in_expr(
            el, scope, symbols, type_env, local_global_ast_nodes, diagnostics, ctx,
            /*in_vec_macro=*/true, already_validated_const, current_const_name);
        }
        if (e->value.repeat_value) {
          validate_repeat_init_counts_in_expr(
            *e->value.repeat_value, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
            ctx,
            /*in_vec_macro=*/true, already_validated_const, current_const_name);
        }
        if (e->value.repeat_count) {
          validate_repeat_init_counts_in_expr(
            *e->value.repeat_count, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
            ctx,
            /*in_vec_macro=*/true, already_validated_const, current_const_name);
        }
        return;
      }
    },
    expr);
}

void collect_global_const_refs(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols,
  std::vector<std::string> & out)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        const Symbol * sym = symbols.resolve(e.name, scope);
        if (sym && sym->kind == SymbolKind::GlobalConst) {
          out.push_back(sym->name);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        collect_global_const_refs(e->left, scope, symbols, out);
        collect_global_const_refs(e->right, scope, symbols, out);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        collect_global_const_refs(e->operand, scope, symbols, out);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        collect_global_const_refs(e->expr, scope, symbols, out);
        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        collect_global_const_refs(e->base, scope, symbols, out);
        collect_global_const_refs(e->index, scope, symbols, out);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          collect_global_const_refs(el, scope, symbols, out);
        }
        if (e->repeat_value) {
          collect_global_const_refs(*e->repeat_value, scope, symbols, out);
        }
        if (e->repeat_count) {
          collect_global_const_refs(*e->repeat_count, scope, symbols, out);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        for (const auto & el : e->value.elements) {
          collect_global_const_refs(el, scope, symbols, out);
        }
        if (e->value.repeat_value) {
          collect_global_const_refs(*e->value.repeat_value, scope, symbols, out);
        }
        if (e->value.repeat_count) {
          collect_global_const_refs(*e->value.repeat_count, scope, symbols, out);
        }
        return;
      }
    },
    expr);
}

void check_global_const_cycles(
  const Program & program, const SymbolTable & symbols, DiagnosticBag & diagnostics)
{
  const Scope * g = symbols.global_scope();
  if (!g) return;

  std::unordered_map<std::string, const ConstDeclStmt *> decls;
  decls.reserve(program.global_consts.size());
  for (const auto & c : program.global_consts) {
    decls.emplace(c.name, &c);
  }

  std::unordered_map<std::string, std::vector<std::string>> adj;
  adj.reserve(decls.size());
  for (const auto & kv : decls) {
    const auto & name = kv.first;
    const auto * d = kv.second;
    if (!d) continue;
    std::vector<std::string> deps;
    collect_global_const_refs(d->value, g, symbols, deps);
    // only keep edges to consts defined in this module
    deps.erase(
      std::remove_if(
        deps.begin(), deps.end(),
        [&](const std::string & dep) { return decls.find(dep) == decls.end() || dep == name; }),
      deps.end());
    adj.emplace(name, std::move(deps));
  }

  enum class Color : uint8_t { White, Gray, Black };
  std::unordered_map<std::string, Color> color;
  color.reserve(decls.size());
  for (const auto & kv : decls) {
    color.emplace(kv.first, Color::White);
  }

  std::vector<std::string> stack;
  stack.reserve(64);

  std::function<void(const std::string &)> dfs;
  dfs = [&](const std::string & u) {
    color[u] = Color::Gray;
    stack.push_back(u);
    auto it = adj.find(u);
    if (it != adj.end()) {
      for (const auto & v : it->second) {
        if (color[v] == Color::Gray) {
          // cycle
          std::string msg = "Cyclic constant reference is not allowed: ";
          size_t start = 0;
          for (; start < stack.size(); ++start) {
            if (stack[start] == v) break;
          }
          for (size_t i = start; i < stack.size(); ++i) {
            if (i > start) msg += " -> ";
            msg += stack[i];
          }
          msg += " -> ";
          msg += v;

          const auto * decl = decls.find(u) != decls.end() ? decls[u] : nullptr;
          diagnostics.error(decl ? decl->range : SourceRange{}, msg);
          continue;
        }
        if (color[v] == Color::White) {
          dfs(v);
        }
      }
    }
    stack.pop_back();
    color[u] = Color::Black;
  };

  for (const auto & kv : decls) {
    if (color[kv.first] == Color::White) {
      dfs(kv.first);
    }
  }
}

void validate_value_space_refs_in_expr(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols,
  const std::unordered_set<const void *> & local_global_ast_nodes, DiagnosticBag & diagnostics)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        const Symbol * sym = symbols.resolve(e.name, scope);
        if (!sym) {
          diagnostics.error(e.range, "Unknown variable: " + e.name);
          return;
        }
        const bool value_space =
          sym->is_variable() || sym->is_const() || sym->kind == SymbolKind::Parameter;
        if (!value_space) {
          diagnostics.error(e.range, "'" + e.name + "' is not a value-space identifier");
          return;
        }

        // Spec: ambiguous imported names are rejected at the reference site.
        if (
          (sym->kind == SymbolKind::GlobalVariable || sym->kind == SymbolKind::GlobalConst) &&
          g_current_ambiguous_imported_value_names_for_validation &&
          g_current_ambiguous_imported_value_names_for_validation->count(e.name) > 0) {
          diagnostics.error(e.range, "Ambiguous imported value name: '" + e.name + "'");
          return;
        }

        // Spec: global const forward references are allowed (docs/reference/declarations-and-scopes.md 4.3.2).
        if (
          sym->kind != SymbolKind::GlobalConst &&
          is_declared_after_in_same_file(*sym, e.range, local_global_ast_nodes)) {
          diagnostics.error(
            e.range, "Reference to '" + e.name + "' is not allowed before its declaration");
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        validate_value_space_refs_in_expr(
          e->left, scope, symbols, local_global_ast_nodes, diagnostics);
        validate_value_space_refs_in_expr(
          e->right, scope, symbols, local_global_ast_nodes, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        validate_value_space_refs_in_expr(
          e->operand, scope, symbols, local_global_ast_nodes, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        validate_value_space_refs_in_expr(
          e->expr, scope, symbols, local_global_ast_nodes, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        validate_value_space_refs_in_expr(
          e->base, scope, symbols, local_global_ast_nodes, diagnostics);
        validate_value_space_refs_in_expr(
          e->index, scope, symbols, local_global_ast_nodes, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          validate_value_space_refs_in_expr(
            el, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        if (e->repeat_value) {
          validate_value_space_refs_in_expr(
            *e->repeat_value, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        if (e->repeat_count) {
          validate_value_space_refs_in_expr(
            *e->repeat_count, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        for (const auto & el : e->value.elements) {
          validate_value_space_refs_in_expr(
            el, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        if (e->value.repeat_value) {
          validate_value_space_refs_in_expr(
            *e->value.repeat_value, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        if (e->value.repeat_count) {
          validate_value_space_refs_in_expr(
            *e->value.repeat_count, scope, symbols, local_global_ast_nodes, diagnostics);
        }
        return;
      }
    },
    expr);
}

// Forward decl (defined later in this file).
std::optional<Type> parse_and_resolve_type_text(
  std::string_view text, const TypeEnvironment * env, const SourceRange & diag_range,
  DiagnosticBag & diagnostics, std::string_view context, const Program * owner_program,
  bool forbid_private_across_files);

void validate_cast_type_refs_in_expr(
  const Expression & expr, const Program & owner_program, const TypeEnvironment * type_env,
  DiagnosticBag & diagnostics)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal> || std::is_same_v<T, VarRef>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        validate_cast_type_refs_in_expr(e->left, owner_program, type_env, diagnostics);
        validate_cast_type_refs_in_expr(e->right, owner_program, type_env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        validate_cast_type_refs_in_expr(e->operand, owner_program, type_env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        // First, recurse into the source expression.
        validate_cast_type_refs_in_expr(e->expr, owner_program, type_env, diagnostics);

        // Then validate that the target type is syntactically valid, resolvable,
        // and not ambiguous across direct imports.
        (void)parse_and_resolve_type_text(
          e->type_name, type_env, e->range, diagnostics, "cast target type", &owner_program,
          /*forbid_private_across_files=*/false);
        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        validate_cast_type_refs_in_expr(e->base, owner_program, type_env, diagnostics);
        validate_cast_type_refs_in_expr(e->index, owner_program, type_env, diagnostics);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          validate_cast_type_refs_in_expr(el, owner_program, type_env, diagnostics);
        }
        if (e->repeat_value) {
          validate_cast_type_refs_in_expr(*e->repeat_value, owner_program, type_env, diagnostics);
        }
        if (e->repeat_count) {
          validate_cast_type_refs_in_expr(*e->repeat_count, owner_program, type_env, diagnostics);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        for (const auto & el : e->value.elements) {
          validate_cast_type_refs_in_expr(el, owner_program, type_env, diagnostics);
        }
        if (e->value.repeat_value) {
          validate_cast_type_refs_in_expr(
            *e->value.repeat_value, owner_program, type_env, diagnostics);
        }
        if (e->value.repeat_count) {
          validate_cast_type_refs_in_expr(
            *e->value.repeat_count, owner_program, type_env, diagnostics);
        }
        return;
      }
    },
    expr);
}

void validate_const_expr(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols,
  const TypeEnvironment * type_env, const std::unordered_set<const void *> & local_global_ast_nodes,
  DiagnosticBag & diagnostics, std::optional<std::string_view> current_const_name)
{
  std::visit(
    [&](const auto & e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        (void)e;
        return;
      } else if constexpr (std::is_same_v<T, VarRef>) {
        if (current_const_name && e.name == *current_const_name) {
          diagnostics.error(e.range, "Constant expression cannot reference itself: " + e.name);
          return;
        }

        if (e.direction.has_value()) {
          diagnostics.error(e.range, "Direction markers are not allowed in constant expressions");
          return;
        }

        const Symbol * sym = symbols.resolve(e.name, scope);
        if (!sym) {
          diagnostics.error(e.range, "Unknown constant: " + e.name);
          return;
        }

        // Spec: ambiguous imported names are rejected at the reference site.
        if (
          sym->kind == SymbolKind::GlobalConst &&
          g_current_ambiguous_imported_value_names_for_validation &&
          g_current_ambiguous_imported_value_names_for_validation->count(e.name) > 0) {
          diagnostics.error(e.range, "Ambiguous imported constant name: '" + e.name + "'");
          return;
        }

        // Spec: const_expr identifiers must resolve to const.
        // Note: global consts allow forward references (docs/reference/declarations-and-scopes.md 4.3.2).
        if (!sym->is_const()) {
          diagnostics.error(
            e.range, "Constant expression may only reference constants, but '" + e.name +
                       "' is not a const");
          return;
        }

        if (sym->kind == SymbolKind::Parameter) {
          diagnostics.error(
            e.range, "Constant expression cannot reference parameters (runtime value): " + e.name);
          return;
        }

        return;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        validate_const_expr(
          e->left, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);
        validate_const_expr(
          e->right, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        validate_const_expr(
          e->operand, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<CastExpr>>) {
        validate_const_expr(
          e->expr, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);

        TypeParseResult parsed = Type::parse(e->type_name);
        if (parsed.has_error()) {
          diagnostics.error(e->range, "Invalid cast type in constant expression: " + *parsed.error);
          return;
        }

        // Disallow casts to inferred types like '_'.
        if (std::holds_alternative<TypeInfer>(parsed.type.value)) {
          diagnostics.error(
            e->range,
            "Cannot cast to inferred type '_' in constant expression (use a concrete type)");
          return;
        }

        // Also force resolution for unknown named types to surface errors early.
        if (type_env) {
          if (g_current_ambiguous_imported_type_names_for_validation) {
            std::function<bool(const Type &)> contains_ambiguous;
            contains_ambiguous = [&](const Type & t) -> bool {
              if (const auto * n = std::get_if<TypeNamed>(&t.value)) {
                return g_current_ambiguous_imported_type_names_for_validation->count(n->name) > 0;
              }
              if (const auto * nn = std::get_if<TypeNullable>(&t.value)) {
                return contains_ambiguous(*nn->base);
              }
              if (const auto * vv = std::get_if<TypeVec>(&t.value)) {
                return contains_ambiguous(*vv->element);
              }
              if (const auto * aa = std::get_if<TypeStaticArray>(&t.value)) {
                return contains_ambiguous(*aa->element);
              }
              if (const auto * bs = std::get_if<TypeBoundedString>(&t.value)) {
                if (const auto * id = std::get_if<std::string>(&bs->max_bytes.value)) {
                  (void)id;
                }
              }
              return false;
            };
            if (contains_ambiguous(parsed.type)) {
              diagnostics.error(
                e->range, "Invalid cast type in constant expression: ambiguous imported type");
            }
          }
          std::optional<std::string> err;
          (void)type_env->resolve(parsed.type, &err);
          if (err) {
            diagnostics.error(e->range, "Invalid cast type in constant expression: " + *err);
          }
        }

        // Reference: docs/reference/declarations-and-scopes.md 4.3.4
        // const_expr casts must not target extern (opaque) types.
        if (type_env) {
          const Type resolved = type_env->resolve(parsed.type);
          std::function<bool(const Type &)> is_extern_or_nullable_extern;
          is_extern_or_nullable_extern = [&](const Type & t) -> bool {
            if (std::holds_alternative<TypeExtern>(t.value)) {
              return true;
            }
            if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
              return is_extern_or_nullable_extern(*n->base);
            }
            return false;
          };
          if (is_extern_or_nullable_extern(resolved)) {
            diagnostics.error(e->range, "Cannot cast to extern type in constant expression");
            return;
          }

          // Reference: docs/reference/declarations-and-scopes.md 4.3.4
          // Forbidden: dynamic array construction in const_expr (e.g. `as vec<_>`).
          std::function<bool(const Type &)> is_vec_or_nullable_vec;
          is_vec_or_nullable_vec = [&](const Type & t) -> bool {
            if (std::holds_alternative<TypeVec>(t.value)) {
              return true;
            }
            if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
              return is_vec_or_nullable_vec(*n->base);
            }
            return false;
          };
          if (is_vec_or_nullable_vec(resolved)) {
            diagnostics.error(e->range, "Cannot cast to vec<T> in constant expression");
            return;
          }
        } else {
          // Best-effort fallback without a type environment.
          std::function<bool(const Type &)> is_vec_or_nullable_vec;
          is_vec_or_nullable_vec = [&](const Type & t) -> bool {
            if (std::holds_alternative<TypeVec>(t.value)) {
              return true;
            }
            if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
              return is_vec_or_nullable_vec(*n->base);
            }
            return false;
          };
          if (is_vec_or_nullable_vec(parsed.type)) {
            diagnostics.error(e->range, "Cannot cast to vec<T> in constant expression");
            return;
          }
        }

        // Reference: docs/reference/declarations-and-scopes.md 4.3.4
        // Mandatory error condition: integer casts in const_expr must be range-checked.
        // We only enforce this when the source is an integer const_expr that we can evaluate.
        if (g_current_const_eval_ctx_for_validation) {
          Type tgt = parsed.type;
          if (type_env) {
            tgt = type_env->resolve(tgt);
          }
          if (const auto * ti = std::get_if<TypeInt>(&tgt.value)) {
            auto v = eval_const_i64(
              e->expr, scope, symbols, *g_current_const_eval_ctx_for_validation, diagnostics,
              current_const_name);
            if (v.has_value()) {
              const int64_t val = *v;
              bool ok = true;
              if (ti->is_signed) {
                int64_t lo = 0;
                int64_t hi = 0;
                if (ti->bits == 64) {
                  lo = std::numeric_limits<int64_t>::min();
                  hi = std::numeric_limits<int64_t>::max();
                } else if (ti->bits == 0 || ti->bits >= 64) {
                  ok = false;
                } else {
                  lo = -(int64_t(1) << (ti->bits - 1));
                  hi = (int64_t(1) << (ti->bits - 1)) - 1;
                }
                if (ok) {
                  ok = (lo <= val) && (val <= hi);
                }
              } else {
                if (val < 0) {
                  ok = false;
                } else if (ti->bits < 64) {
                  const uint64_t maxu = (ti->bits == 0) ? 0ULL : ((uint64_t(1) << ti->bits) - 1ULL);
                  ok = static_cast<uint64_t>(val) <= maxu;
                }
              }
              if (!ok) {
                diagnostics.error(e->range, "Cast out of range in constant expression");
              }
            }
          }
        }

        return;
      } else if constexpr (std::is_same_v<T, Box<IndexExpr>>) {
        validate_const_expr(
          e->base, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);
        validate_const_expr(
          e->index, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
          current_const_name);
        return;
      } else if constexpr (std::is_same_v<T, Box<ArrayLiteralExpr>>) {
        for (const auto & el : e->elements) {
          validate_const_expr(
            el, scope, symbols, type_env, local_global_ast_nodes, diagnostics, current_const_name);
        }
        if (e->repeat_value) {
          validate_const_expr(
            *e->repeat_value, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
            current_const_name);
        }
        if (e->repeat_count) {
          validate_const_expr(
            *e->repeat_count, scope, symbols, type_env, local_global_ast_nodes, diagnostics,
            current_const_name);
        }
        return;
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        // Reference: docs/reference/declarations-and-scopes.md 4.3.4 (禁止: 動的配列の構築)
        diagnostics.error(e->range, "vec![...] is not allowed in constant expressions");
        return;
      }
    },
    expr);
}

std::optional<std::string> extract_lvalue_base_name_for_semantic(const Expression & expr)
{
  if (const auto * vr = std::get_if<VarRef>(&expr)) {
    return vr->name;
  }
  if (const auto * idx = std::get_if<Box<IndexExpr>>(&expr)) {
    return extract_lvalue_base_name_for_semantic((*idx)->base);
  }
  return std::nullopt;
}

struct TreeEdge
{
  std::string callee;
  SourceRange range;
};

void collect_tree_calls_from_stmt(const Statement & s, std::vector<TreeEdge> & out)
{
  if (const auto * n = std::get_if<Box<NodeStmt>>(&s)) {
    const auto * node = n->get();
    if (!node) {
      return;
    }
    out.push_back(TreeEdge{node->node_name, node->range});
    for (const auto & child : node->children) {
      collect_tree_calls_from_stmt(child, out);
    }
    return;
  }

  // Other statement kinds (assign/var/const) cannot call trees.
}

std::vector<TreeEdge> collect_tree_calls_from_block(const std::vector<Statement> & stmts)
{
  std::vector<TreeEdge> out;
  out.reserve(32);
  for (const auto & s : stmts) {
    collect_tree_calls_from_stmt(s, out);
  }
  return out;
}

struct TreeDefInfo
{
  const TreeDef * def = nullptr;
  const Program * owner = nullptr;
};

bool is_public_name(std::string_view name) { return !name.empty() && name.front() != '_'; }

bool program_defines_node(const Program & program, std::string_view node_name)
{
  const bool in_trees = std::any_of(
    program.trees.begin(), program.trees.end(),
    [&](const TreeDef & t) { return t.name == node_name; });
  if (in_trees) {
    return true;
  }
  return std::any_of(
    program.declarations.begin(), program.declarations.end(),
    [&](const DeclareStmt & d) { return d.name == node_name; });
}

void add_types_from_program(
  const Program & program, TypeEnvironment & env, DiagnosticBag & diagnostics,
  bool report_duplicates, bool public_only)
{
  // extern types
  for (const auto & ex : program.extern_types) {
    if (public_only && !is_public_name(ex.name)) {
      continue;
    }
    if (env.is_extern_type(ex.name) || env.has_alias(ex.name)) {
      if (report_duplicates) {
        diagnostics.error(ex.range, "Duplicate type name: '" + ex.name + "'");
      }
      continue;
    }
    env.add_extern_type(ex.name);
  }

  // type aliases
  for (const auto & al : program.type_aliases) {
    if (public_only && !is_public_name(al.name)) {
      continue;
    }
    if (env.is_extern_type(al.name) || env.has_alias(al.name)) {
      if (report_duplicates) {
        diagnostics.error(al.range, "Duplicate type name: '" + al.name + "'");
      }
      continue;
    }

    TypeParseResult parsed = Type::parse(al.value);
    if (parsed.has_error()) {
      diagnostics.error(al.range, "Invalid type alias '" + al.name + "': " + *parsed.error);
      continue;
    }
    env.add_type_alias(al.name, std::move(parsed.type));
  }
}

std::optional<Type> parse_and_resolve_type_text(
  std::string_view text, const TypeEnvironment * env, const SourceRange & diag_range,
  DiagnosticBag & diagnostics, std::string_view context, const Program * owner_program,
  bool forbid_private_across_files)
{
  TypeParseResult parsed = Type::parse(text);
  if (parsed.has_error()) {
    diagnostics.error(
      diag_range, std::string(context) + ": invalid type '" + std::string(text) +
                    "': " + parsed.error.value_or("unknown error"));
    return std::nullopt;
  }

  // Enforce visibility for private type identifiers across imports.
  // NOTE: We intentionally do NOT treat unknown public type identifiers as
  // errors here, because the implementation historically allows opaque/custom
  // types without requiring an explicit `extern type` declaration.
  std::function<std::optional<std::string>(const Type &)> find_private_named;
  find_private_named = [&](const Type & t) -> std::optional<std::string> {
    return std::visit(
      [&](const auto & v) -> std::optional<std::string> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, TypeNamed>) {
          if (!v.name.empty() && v.name.front() == '_') {
            return v.name;
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<T, TypeNullable>) {
          return find_private_named(*v.base);
        } else if constexpr (std::is_same_v<T, TypeVec> || std::is_same_v<T, TypeStaticArray>) {
          return find_private_named(*v.element);
        } else {
          return std::nullopt;
        }
      },
      t.value);
  };

  if (auto priv = find_private_named(parsed.type)) {
    const std::string & priv_name = *priv;

    if (forbid_private_across_files) {
      diagnostics.error(
        diag_range,
        std::string(context) + ": private type '" + priv_name + "' is not visible across imports");
      return std::nullopt;
    }

    if (owner_program) {
      const bool declared_here =
        std::any_of(
          owner_program->extern_types.begin(), owner_program->extern_types.end(),
          [&](const ExternTypeStmt & s) { return s.name == priv_name; }) ||
        std::any_of(
          owner_program->type_aliases.begin(), owner_program->type_aliases.end(),
          [&](const TypeAliasStmt & s) { return s.name == priv_name; });

      if (!declared_here) {
        diagnostics.error(
          diag_range, std::string(context) + ": private type '" + priv_name +
                        "' must be declared in the same file");
        return std::nullopt;
      }
    }
  }

  // Spec: ambiguity across direct imports must be rejected at the reference site.
  if (g_current_ambiguous_imported_type_names_for_validation) {
    std::function<std::optional<std::string>(const Type &)> find_ambiguous;
    find_ambiguous = [&](const Type & t) -> std::optional<std::string> {
      if (const auto * n = std::get_if<TypeNamed>(&t.value)) {
        if (g_current_ambiguous_imported_type_names_for_validation->count(n->name) > 0) {
          return n->name;
        }
      }
      if (const auto * nn = std::get_if<TypeNullable>(&t.value)) {
        return find_ambiguous(*nn->base);
      }
      if (const auto * vv = std::get_if<TypeVec>(&t.value)) {
        return find_ambiguous(*vv->element);
      }
      if (const auto * aa = std::get_if<TypeStaticArray>(&t.value)) {
        return find_ambiguous(*aa->element);
      }
      if (const auto * bs = std::get_if<TypeBoundedString>(&t.value)) {
        (void)bs;
      }
      return std::nullopt;
    };

    if (auto amb = find_ambiguous(parsed.type)) {
      diagnostics.error(
        diag_range, std::string(context) + ": ambiguous imported type '" + *amb + "'");
      return std::nullopt;
    }
  }

  Type t = std::move(parsed.type);
  if (env) {
    // Reference: docs/reference/type-system/type-definitions.md 3.1.5 / 3.1.6
    // Surface types must resolve via `extern type` / `type` aliases; unresolved type
    // names are errors (no implicit opaque/custom types).
    std::optional<std::string> err;
    t = env->resolve(t, &err);
    if (err.has_value()) {
      diagnostics.error(
        diag_range, std::string(context) + ": invalid type '" + std::string(text) + "': " + *err);
      return std::nullopt;
    }
  }

  return t;
}

Type resolve_type_text(std::string_view text, const TypeEnvironment * env)
{
  Type t = Type::from_string(text);
  if (env) {
    t = env->resolve(t);
  }
  return t;
}

}  // namespace

// ============================================================================
// AnalysisResult Implementation
// ============================================================================

const TypeContext * AnalysisResult::get_tree_context(std::string_view tree_name) const
{
  const std::string key(tree_name);
  auto it = tree_type_contexts.find(key);
  if (it != tree_type_contexts.end()) {
    return &it->second;
  }
  return nullptr;
}

TypeContext * AnalysisResult::get_tree_context_mut(std::string_view tree_name)
{
  const std::string key(tree_name);
  auto it = tree_type_contexts.find(key);
  if (it != tree_type_contexts.end()) {
    return &it->second;
  }
  return nullptr;
}

// ============================================================================
// Analyzer Implementation
// ============================================================================

Analyzer::Analyzer() = default;

AnalysisResult Analyzer::analyze(const Program & program) { return analyze(program, {}); }

AnalysisResult Analyzer::analyze(
  const Program & program, const std::vector<const Program *> & imported_programs)
{
  AnalysisResult result;

  // Track import-induced ambiguities (report only when referenced).
  std::unordered_set<std::string> imported_value_names;
  std::unordered_set<std::string> ambiguous_imported_value_names;
  std::unordered_set<std::string> imported_public_node_names;
  std::unordered_set<std::string> ambiguous_imported_node_names;
  std::unordered_set<std::string> imported_public_type_names;
  std::unordered_set<std::string> ambiguous_imported_type_names;

  // Build local symbol table (tree scopes are per-file and always local).
  result.symbols.build_from_program(program);

  // Build local nodes (visible + all).
  {
    NodeRegistry local_nodes;
    local_nodes.build_from_program(program);
    for (const auto * n : local_nodes.all_nodes()) {
      if (!n) {
        continue;
      }
      result.nodes.upsert_node(*n);
      result.all_nodes.upsert_node(*n);
    }
  }

  // Merge imported programs.
  // - result.nodes: imports are filtered to Public names only
  // - result.all_nodes: imports include Private names as well
  // - symbols: import Public globals/consts (Private are not visible)
  for (const auto * imported : imported_programs) {
    if (!imported) {
      continue;
    }

    // Public global vars
    for (const auto & gv : imported->global_vars) {
      if (!is_public_name(gv.name)) {
        continue;
      }
      Symbol sym;
      sym.name = gv.name;
      sym.kind = SymbolKind::GlobalVariable;
      sym.type_name = gv.type_name;
      sym.direction = std::nullopt;
      sym.definition_range = gv.range;
      sym.ast_node = &gv;

      // If the name already exists due to another import, mark ambiguous but do not
      // error until a reference site actually uses it.
      if (result.symbols.has_global(sym.name)) {
        if (imported_value_names.count(sym.name) > 0) {
          ambiguous_imported_value_names.insert(sym.name);
          continue;
        }
        // Conflict with a local global definition: treat as ambiguity at reference sites.
        ambiguous_imported_value_names.insert(sym.name);
        continue;
      }

      (void)result.symbols.try_define_global(std::move(sym));
      imported_value_names.insert(gv.name);
    }

    // Public global consts
    for (const auto & gc : imported->global_consts) {
      if (!is_public_name(gc.name)) {
        continue;
      }
      Symbol sym;
      sym.name = gc.name;
      sym.kind = SymbolKind::GlobalConst;
      sym.type_name = gc.type_name;
      sym.direction = std::nullopt;
      sym.definition_range = gc.range;
      sym.ast_node = &gc;

      if (result.symbols.has_global(sym.name)) {
        if (imported_value_names.count(sym.name) > 0) {
          ambiguous_imported_value_names.insert(sym.name);
          continue;
        }
        // Conflict with a local global definition: treat as ambiguity at reference sites.
        ambiguous_imported_value_names.insert(sym.name);
        continue;
      }

      (void)result.symbols.try_define_global(std::move(sym));
      imported_value_names.insert(gc.name);
    }

    NodeRegistry imported_nodes;
    imported_nodes.build_from_program(*imported);
    for (const auto * n : imported_nodes.all_nodes()) {
      if (!n) {
        continue;
      }

      // Unfiltered registry: used for whole-program analyses.
      if (result.all_nodes.has_node(n->id)) {
        // Duplicates across imports are handled as ambiguity at reference sites.
        // Keep the first definition here to avoid breaking whole-program passes.
      } else {
        result.all_nodes.register_node(*n);
      }

      // Visible registry: public-only.
      if (!is_public_name(n->id)) {
        continue;
      }

      if (result.nodes.has_node(n->id)) {
        if (imported_public_node_names.count(n->id) > 0) {
          ambiguous_imported_node_names.insert(n->id);
          continue;
        }
        // Conflict with a local visible definition: treat as ambiguity at reference sites.
        ambiguous_imported_node_names.insert(n->id);
        continue;
      }

      result.nodes.register_node(*n);
      imported_public_node_names.insert(n->id);
    }
  }

  // Ensure local definitions win for analysis purposes (still an error if they
  // collide with imported symbols).
  // Re-upsert local nodes to override imported entries.
  {
    NodeRegistry local_nodes;
    local_nodes.build_from_program(program);
    for (const auto * n : local_nodes.all_nodes()) {
      if (!n) {
        continue;
      }
      if (result.all_nodes.has_node(n->id)) {
        // If an imported symbol had the same name, we already emitted an error
        // during import merge; keep local as the active definition.
        result.all_nodes.upsert_node(*n);
      }
      result.nodes.upsert_node(*n);
    }
  }

  // Check for duplicates
  check_duplicates(program, result);

  // Build type environment (extern types + type aliases) from main + imports.
  // We include imported *public* types only. Private type identifiers (leading
  // '_') are not visible across imports (docs/reference/declarations-and-scopes.md 4.1.2).
  TypeEnvironment type_env;
  add_types_from_program(
    program, type_env, result.diagnostics, /*report_duplicates=*/true, /*public_only=*/false);
  for (const auto * imp : imported_programs) {
    if (!imp) continue;

    // extern types
    for (const auto & ex : imp->extern_types) {
      if (!is_public_name(ex.name)) {
        continue;
      }
      if (type_env.is_extern_type(ex.name) || type_env.has_alias(ex.name)) {
        if (imported_public_type_names.count(ex.name) > 0) {
          ambiguous_imported_type_names.insert(ex.name);
          continue;
        }
        // Conflict with a local public type: treat as ambiguity at reference sites.
        ambiguous_imported_type_names.insert(ex.name);
        continue;
      }
      type_env.add_extern_type(ex.name);
      imported_public_type_names.insert(ex.name);
    }

    // type aliases
    for (const auto & al : imp->type_aliases) {
      if (!is_public_name(al.name)) {
        continue;
      }
      if (type_env.is_extern_type(al.name) || type_env.has_alias(al.name)) {
        if (imported_public_type_names.count(al.name) > 0) {
          ambiguous_imported_type_names.insert(al.name);
          continue;
        }
        // Conflict with a local public type: treat as ambiguity at reference sites.
        ambiguous_imported_type_names.insert(al.name);
        continue;
      }
      TypeParseResult parsed = Type::parse(al.value);
      if (parsed.has_error()) {
        result.diagnostics.error(
          al.range, "Invalid type alias '" + al.name + "': " + *parsed.error);
        continue;
      }
      type_env.add_type_alias(al.name, std::move(parsed.type));
      imported_public_type_names.insert(al.name);
    }
  }

  // Check for recursive tree calls across the compilation unit (main + direct imports).
  {
    // Build map: tree name -> TreeDef + owner Program.
    std::unordered_map<std::string, TreeDefInfo> trees;
    trees.reserve(program.trees.size() + 32);

    auto add_tree = [&](const Program * owner, const TreeDef & t) {
      TreeDefInfo info;
      info.def = &t;
      info.owner = owner;
      trees.emplace(t.name, info);
    };

    for (const auto & t : program.trees) {
      add_tree(&program, t);
    }
    for (const auto * imp : imported_programs) {
      if (!imp) {
        continue;
      }
      for (const auto & t : imp->trees) {
        // Imported private trees are not visible to the main file, but they
        // can participate in recursion when reached from a public imported tree.
        add_tree(imp, t);
      }
    }

    // Adjacency list (visibility-aware): caller -> list of called trees
    std::unordered_map<std::string, std::vector<TreeEdge>> adj;
    adj.reserve(trees.size());

    for (const auto & kv : trees) {
      const auto & name = kv.first;
      const auto & info = kv.second;
      if (!info.def || !info.owner) {
        continue;
      }

      auto raw_calls = collect_tree_calls_from_block(info.def->body);
      std::vector<TreeEdge> calls;
      calls.reserve(raw_calls.size());

      for (const auto & e : raw_calls) {
        auto it = trees.find(e.callee);
        if (it == trees.end()) {
          continue;  // not a tree call
        }
        const auto * callee_owner = it->second.owner;
        const bool callee_public = is_public_name(e.callee);
        const bool same_file = (callee_owner == info.owner);

        // A private tree is callable only within the defining file.
        if (!callee_public && !same_file) {
          continue;
        }

        calls.push_back(e);
      }

      adj.emplace(name, std::move(calls));
    }

    enum class Color : uint8_t { White, Gray, Black };
    std::unordered_map<std::string, Color> color;
    color.reserve(trees.size());
    for (const auto & kv : trees) {
      color.emplace(kv.first, Color::White);
    }

    std::vector<std::string> stack;
    stack.reserve(64);

    std::function<void(const std::string &)> dfs;
    dfs = [&](const std::string & u) {
      color[u] = Color::Gray;
      stack.push_back(u);

      auto it_adj = adj.find(u);
      if (it_adj != adj.end()) {
        for (const auto & e : it_adj->second) {
          auto it_c = color.find(e.callee);
          if (it_c == color.end()) {
            continue;
          }
          if (it_c->second == Color::Gray) {
            // Found a cycle; reconstruct path.
            std::string msg = "Recursive tree call is not allowed: ";
            // Find the first occurrence of callee in the stack.
            size_t start = 0;
            for (; start < stack.size(); ++start) {
              if (stack[start] == e.callee) {
                break;
              }
            }
            for (size_t i = start; i < stack.size(); ++i) {
              if (i > start) {
                msg += " -> ";
              }
              msg += stack[i];
            }
            msg += " -> ";
            msg += e.callee;

            result.diagnostics.error(e.range, msg);
            continue;
          }
          if (it_c->second == Color::White) {
            dfs(e.callee);
          }
        }
      }

      stack.pop_back();
      color[u] = Color::Black;
    };

    // Start from trees defined in the main program (including private), since
    // those are the compilation roots for this analysis.
    for (const auto & t : program.trees) {
      auto it = color.find(t.name);
      if (it != color.end() && it->second == Color::White) {
        dfs(t.name);
      }
    }
  }

  // Resolve types + validate semantics using the combined type environment.
  g_current_type_env_for_validation = &type_env;
  g_current_ambiguous_imported_value_names_for_validation = &ambiguous_imported_value_names;
  g_current_ambiguous_imported_node_names_for_validation = &ambiguous_imported_node_names;
  g_current_ambiguous_imported_type_names_for_validation = &ambiguous_imported_type_names;
  resolve_types(program, imported_programs, result);
  validate_semantics(program, imported_programs, result);
  g_current_type_env_for_validation = nullptr;
  g_current_ambiguous_imported_value_names_for_validation = nullptr;
  g_current_ambiguous_imported_node_names_for_validation = nullptr;
  g_current_ambiguous_imported_type_names_for_validation = nullptr;

  // Initialization safety (blackboard Init/Uninit analysis)
  run_initialization_safety(program, imported_programs, result.all_nodes, result.diagnostics);

  return result;
}

// ============================================================================
// Declaration Collection
// ============================================================================

void Analyzer::collect_declarations(const Program & program, AnalysisResult & result)
{
  result.symbols.build_from_program(program);

  // Merge with imported nodes (already in result.nodes)
  NodeRegistry program_nodes;
  program_nodes.build_from_program(program);
  result.nodes.merge(program_nodes);
}

// ============================================================================
// Duplicate Checking
// ============================================================================

void Analyzer::check_duplicates(const Program & program, AnalysisResult & result)
{
  check_duplicate_trees(program, result);
  check_duplicate_globals(program, result);
  check_duplicate_declares(program, result);
  check_declare_conflicts(program, result);

  // Check per-tree duplicates
  for (const auto & tree : program.trees) {
    check_duplicate_params(tree, result);
  }
}

void Analyzer::check_duplicate_trees(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const TreeDef *> seen;

  for (const auto & tree : program.trees) {
    auto [it, inserted] = seen.emplace(tree.name, &tree);
    if (!inserted) {
      result.diagnostics.error(tree.range, "Duplicate tree name: '" + tree.name + "'");
    }
  }
}

void Analyzer::check_duplicate_globals(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const GlobalVarDecl *> seen_vars;
  std::unordered_map<std::string, const ConstDeclStmt *> seen_consts;

  for (const auto & var : program.global_vars) {
    auto [it, inserted] = seen_vars.emplace(var.name, &var);
    if (!inserted) {
      result.diagnostics.error(var.range, "Duplicate global variable name: '" + var.name + "'");
      continue;
    }
    if (seen_consts.count(var.name) != 0) {
      result.diagnostics.error(
        var.range,
        "Global name: '" + var.name + "' conflicts with a global constant of the same name");
    }
  }

  for (const auto & c : program.global_consts) {
    auto [it, inserted] = seen_consts.emplace(c.name, &c);
    if (!inserted) {
      result.diagnostics.error(c.range, "Duplicate global constant name: '" + c.name + "'");
      continue;
    }
    if (seen_vars.count(c.name) != 0) {
      result.diagnostics.error(
        c.range, "Global name: '" + c.name + "' conflicts with a global variable of the same name");
    }
  }
}

void Analyzer::check_duplicate_params(const TreeDef & tree, AnalysisResult & result)
{
  std::unordered_set<std::string> seen;

  for (const auto & param : tree.params) {
    auto [it, inserted] = seen.insert(param.name);
    if (!inserted) {
      result.diagnostics.error(param.range, "Duplicate parameter name: '" + param.name + "'");
    }
  }
}

void Analyzer::check_duplicate_declares(const Program & program, AnalysisResult & result)
{
  std::unordered_map<std::string, const DeclareStmt *> seen;

  for (const auto & decl : program.declarations) {
    auto [it, inserted] = seen.emplace(decl.name, &decl);
    if (!inserted) {
      result.diagnostics.error(decl.range, "Duplicate declaration name: '" + decl.name + "'");
    }
  }
}

void Analyzer::check_declare_conflicts(const Program & program, AnalysisResult & result)
{
  std::unordered_set<std::string> tree_names;
  for (const auto & tree : program.trees) {
    tree_names.insert(tree.name);
  }

  for (const auto & decl : program.declarations) {
    if (tree_names.count(decl.name)) {
      result.diagnostics.error(
        decl.range, "Declaration '" + decl.name + "' conflicts with a Tree definition");
    }
  }
}

// ============================================================================
// Type Resolution
// ============================================================================

void Analyzer::resolve_types(
  const Program & program, const std::vector<const Program *> & imported_programs,
  AnalysisResult & result)
{
  TypeResolver resolver(result.symbols, result.nodes, g_current_type_env_for_validation);

  const auto local_global_ast_nodes = collect_local_global_value_ast_nodes(program);

  ConstEvalContext size_eval_ctx;
  build_visible_global_const_map(program, imported_programs, size_eval_ctx.global_consts);

  for (const auto & tree : program.trees) {
    TypeContext ctx = resolver.resolve_tree_types(tree);
    // NOTE:
    // Bounded type sizes must be resolved against the *reference site* scope.
    // For tree-local declarations this scope is order-sensitive (declared-after is forbidden).
    // Therefore we do not normalize bounded sizes here; normalization is performed during
    // semantic validation when an in-order scope is available.
    result.tree_type_contexts.emplace(tree.name, std::move(ctx));
  }
}

// ============================================================================
// Semantic Validation
// ============================================================================

void Analyzer::validate_semantics(
  const Program & program, const std::vector<const Program *> & imported_programs,
  AnalysisResult & result)
{
  const auto local_global_ast_nodes = collect_local_global_value_ast_nodes(program);

  ConstEvalContext global_const_ctx;
  build_visible_global_const_map(program, imported_programs, global_const_ctx.global_consts);

  // Validate that all explicit type annotations are syntactically valid and
  // resolvable in the current type environment.
  // This also enforces import visibility for Type-space identifiers.
  // Allow type-bound identifiers (string<SIZE>, [T; <=SIZE]) to const-evaluate
  // during this pass.
  g_current_const_eval_ctx_for_validation = &global_const_ctx;

  for (const auto & gv : program.global_vars) {
    // Spec (docs/reference/type-system/inference-and-resolution.md 3.2.4): global var must have
    // either a type annotation or initial value.
    if (!gv.type_name && !gv.initial_value) {
      result.diagnostics.error(
        gv.range,
        "Global variable '" + gv.name + "' must have either a type annotation or initial value");
    }
    if (gv.type_name) {
      (void)parse_and_resolve_type_text(
        *gv.type_name, g_current_type_env_for_validation, gv.range, result.diagnostics,
        std::string("global variable '") + gv.name + "' type", &program,
        /*forbid_private_across_files=*/false);
    }
  }
  for (const auto & gc : program.global_consts) {
    if (gc.type_name) {
      (void)parse_and_resolve_type_text(
        *gc.type_name, g_current_type_env_for_validation, gc.range, result.diagnostics,
        std::string("global const '") + gc.name + "' type", &program,
        /*forbid_private_across_files=*/false);
    }
  }

  // Validate global initializers.
  // - Value-space references must not appear before declaration.
  // - global const initializers must be const_expr.
  // - default values in declare ports must be const_expr (handled in validate_declare_stmt).
  if (Scope * g = result.symbols.global_scope()) {
    for (const auto & gv : program.global_vars) {
      if (!gv.initial_value) {
        continue;
      }
      // Disallow self-reference in initializer (best-effort; prevents confusing behavior).
      validate_value_space_refs_in_expr(
        *gv.initial_value, g, result.symbols, local_global_ast_nodes, result.diagnostics);

      // Validate cast target type names used in the initializer expression.
      validate_cast_type_refs_in_expr(
        *gv.initial_value, program, g_current_type_env_for_validation, result.diagnostics);

      // Enforce repeat-init count rules for static array literals.
      validate_repeat_init_counts_in_expr(
        *gv.initial_value, g, result.symbols, g_current_type_env_for_validation,
        local_global_ast_nodes, result.diagnostics, global_const_ctx);

      // If the global has an explicit static-array type, ensure repeat-init length fits.
      if (gv.type_name && g_current_const_eval_ctx_for_validation) {
        if (
          auto tgt = parse_and_resolve_type_text(
            *gv.type_name, g_current_type_env_for_validation, gv.range, result.diagnostics,
            std::string("global variable '") + gv.name + "' type", &program,
            /*forbid_private_across_files=*/false)) {
          validate_static_array_repeat_init_fits_target(
            *gv.initial_value, *tgt, g, result.symbols, g_current_type_env_for_validation,
            local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
        }
      }
    }

    for (const auto & gc : program.global_consts) {
      validate_const_expr(
        gc.value, g, result.symbols, g_current_type_env_for_validation, local_global_ast_nodes,
        result.diagnostics, std::string_view(gc.name));

      // Ensure repeat-init counts in const_expr are integer constants (not just const).
      validate_repeat_init_counts_in_expr(
        gc.value, g, result.symbols, g_current_type_env_for_validation, local_global_ast_nodes,
        result.diagnostics, global_const_ctx,
        /*in_vec_macro=*/false, /*already_validated_const=*/true, std::string_view(gc.name));

      // If the global const has an explicit static-array type, ensure repeat-init length fits.
      if (gc.type_name && g_current_const_eval_ctx_for_validation) {
        if (
          auto tgt = parse_and_resolve_type_text(
            *gc.type_name, g_current_type_env_for_validation, gc.range, result.diagnostics,
            std::string("global const '") + gc.name + "' type", &program,
            /*forbid_private_across_files=*/false)) {
          validate_static_array_repeat_init_fits_target(
            gc.value, *tgt, g, result.symbols, g_current_type_env_for_validation,
            local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
        }
      }
    }

    // Global consts allow forward references but must not be cyclic.
    check_global_const_cycles(program, result.symbols, result.diagnostics);

    // Reference: docs/reference/declarations-and-scopes.md 4.3.4
    // const_expr must be fully evaluable at compile time.
    for (const auto & kv : global_const_ctx.global_consts) {
      if (!kv.second) {
        continue;
      }
      auto v =
        eval_global_const_value(kv.first, g, result.symbols, global_const_ctx, result.diagnostics);
      if (!v.has_value()) {
        result.diagnostics.error(
          kv.second->range, "Constant initializer must be a fully evaluable constant expression");
      }
    }
  }

  // Keep const-eval ctx alive through declare validation (port defaults may reference consts).

  // Validate declare statements
  for (const auto & decl : program.declarations) {
    validate_declare_stmt(program, decl, result);
  }

  g_current_const_eval_ctx_for_validation = nullptr;

  // Validate trees
  for (const auto & tree : program.trees) {
    validate_tree(program, imported_programs, tree, result);
  }
}

void Analyzer::validate_tree(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const TreeDef & tree, AnalysisResult & result)
{
  TypeContext * ctx = result.get_tree_context_mut(tree.name);
  if (!ctx) {
    return;
  }

  const auto local_global_ast_nodes = collect_local_global_value_ast_nodes(program);
  g_current_local_global_ast_nodes_for_validation = &local_global_ast_nodes;

  // Const evaluation context for this tree: include global consts from the main
  // file and imported public consts, so bounded types may use imported consts.
  ConstEvalContext tree_const_ctx;
  build_visible_global_const_map(program, imported_programs, tree_const_ctx.global_consts);
  g_current_const_eval_ctx_for_validation = &tree_const_ctx;

  // Validate parameter type annotations.
  for (const auto & param : tree.params) {
    if (
      auto resolved = parse_and_resolve_type_text(
        param.type_name, g_current_type_env_for_validation, param.range, result.diagnostics,
        std::string("parameter '") + param.name + "' type", &program,
        /*forbid_private_across_files=*/false)) {
      // Parameter types are evaluated in the global scope (tree-local consts are not
      // visible at the parameter list).
      if (Scope * g = result.symbols.global_scope(); g && g_current_const_eval_ctx_for_validation) {
        normalize_bounded_type_sizes_in_place(
          *resolved, g, result.symbols, g_current_type_env_for_validation, local_global_ast_nodes,
          *g_current_const_eval_ctx_for_validation, result.diagnostics, param.range);
      }

      // Keep TypeContext consistent with the resolved/normalized annotation.
      ctx->set_type(param.name, *resolved);
    }
  }

  // Parameter default restrictions (reference semantics.md 5.4)
  for (const auto & param : tree.params) {
    const PortDirection dir = param.direction.value_or(PortDirection::In);
    if (param.default_value.has_value() && dir != PortDirection::In) {
      result.diagnostics.error(
        param.range, "Default value is only allowed for 'in' parameters, but parameter '" +
                       param.name + "' is '" + std::string(to_string(dir)) + "'");
    }
  }

  // (local_global_ast_nodes and tree_const_ctx are already initialized above)

  // Cache global types (including imported globals that have explicit type annotations).
  const auto global_types =
    build_global_type_cache(result.symbols, g_current_type_env_for_validation);
  g_current_global_types_for_validation = &global_types;
  // const_expr restriction for parameter defaults (docs/reference/declarations-and-scopes.md 4.3)
  if (Scope * g = result.symbols.global_scope()) {
    for (const auto & param : tree.params) {
      if (!param.default_value) {
        continue;
      }
      validate_const_expr(
        *param.default_value, g, result.symbols, g_current_type_env_for_validation,
        local_global_ast_nodes, result.diagnostics);

      // Reference: docs/reference/declarations-and-scopes.md 4.3.4
      // const_expr must be fully evaluable.
      {
        auto v = eval_const_value(
          *param.default_value, g, result.symbols, tree_const_ctx, result.diagnostics,
          std::nullopt);
        if (!v.has_value()) {
          result.diagnostics.error(
            get_range(*param.default_value),
            "Default value must be a fully evaluable constant expression");
        }
      }

      // If the parameter type is a static array, ensure repeat-init length fits.
      if (
        auto tgt = parse_and_resolve_type_text(
          param.type_name, g_current_type_env_for_validation, param.range, result.diagnostics,
          std::string("parameter '") + param.name + "' type", &program,
          /*forbid_private_across_files=*/false)) {
        validate_static_array_repeat_init_fits_target(
          *param.default_value, *tgt, g, result.symbols, g_current_type_env_for_validation,
          local_global_ast_nodes, result.diagnostics, tree_const_ctx);
      }
    }
  }

  // Validate tree body statements in a tree-local scope.
  //
  // Spec (docs/reference/declarations-and-scopes.md 4.2.3): shadowing is forbidden for
  // value-space declarations across ancestor scopes. We therefore reject parameters and
  // tree-local var/const that would hide global value-space symbols.
  //
  // We validate in-order to enforce "declared after" visibility.
  Scope * tree_scope = result.symbols.tree_scope(tree.name);
  Scope * global_scope = tree_scope ? tree_scope->parent() : nullptr;
  Scope tree_body_scope(global_scope);

  // Seed the tree-local scope with tree parameters.
  for (const auto & param : tree.params) {
    // Duplicate parameter names are checked elsewhere, but also enforce here so the
    // scope used for in-order validation is consistent.
    if (tree_body_scope.lookup_local(param.name)) {
      result.diagnostics.error(param.range, "Duplicate parameter name: '" + param.name + "'");
      continue;
    }

    // Spec (docs/reference/declarations-and-scopes.md 4.2.3): parameters are value-space
    // declarations and must not shadow global value-space names (including imported globals).
    if (global_scope) {
      if (const Symbol * gs = global_scope->lookup_local(param.name);
          gs && (gs->is_variable() || gs->is_const())) {
        result.diagnostics.error(param.range, "Shadowing is forbidden: '" + param.name + "'");
        continue;
      }
    }

    Symbol sym;
    sym.name = param.name;
    sym.kind = SymbolKind::Parameter;
    sym.type_name = param.type_name;
    sym.direction = param.direction;
    sym.definition_range = param.range;
    sym.ast_node = &param;
    tree_body_scope.define(std::move(sym));
  }

  g_current_program_for_validation = &program;
  for (const auto & stmt : tree.body) {
    validate_statement(
      program, stmt, tree, *ctx, &tree_body_scope, StatementListKind::TreeBody, result);
  }
  g_current_program_for_validation = nullptr;

  auto get_global_type = [&](std::string_view name) -> const Type * {
    if (!g_current_global_types_for_validation) {
      return nullptr;
    }
    auto it = g_current_global_types_for_validation->find(std::string(name));
    if (it == g_current_global_types_for_validation->end()) {
      return nullptr;
    }
    return &it->second;
  };

  // Type checking needs a scope that includes tree-local declarations (const/var)
  // introduced during in-order validation, so that const_expr evaluation (e.g.
  // static array bounds checks) can resolve identifiers like `SIZE` / `IDX`.
  // The SymbolTable's tree_scope currently only contains parameters, while the
  // in-order `tree_body_scope` is populated as we validate statements.
  TypeChecker checker(
    result.symbols, result.nodes, &tree_body_scope, g_current_type_env_for_validation);
  checker.check_tree(tree, *ctx, get_global_type, result.diagnostics);

  g_current_global_types_for_validation = nullptr;
  g_current_local_global_ast_nodes_for_validation = nullptr;
  g_current_const_eval_ctx_for_validation = nullptr;

  // Check for unused write parameters
  check_write_param_usage(tree, result);
}

void Analyzer::validate_statement_block(
  const Program & program, const std::vector<Statement> & stmts, const TreeDef & tree,
  TypeContext & ctx, Scope * parent_scope, AnalysisResult & result)
{
  // children_block introduces a new lexical scope.
  Scope block_scope(parent_scope);
  for (const auto & stmt : stmts) {
    validate_statement(
      program, stmt, tree, ctx, &block_scope, StatementListKind::ChildrenBlock, result);
  }
}

void Analyzer::validate_statement(
  const Program & program, const Statement & stmt, const TreeDef & tree, TypeContext & ctx,
  Scope * scope, StatementListKind list_kind, AnalysisResult & result)
{
  std::visit(
    [&](const auto & s) {
      using T = std::decay_t<decltype(s)>;
      if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
        validate_node_stmt(program, *s, tree, ctx, scope, list_kind, result);
      } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
        validate_assignment_stmt(program, s, tree, ctx, scope, result);
      } else if constexpr (std::is_same_v<T, BlackboardDeclStmt>) {
        const auto local_globals = g_current_local_global_ast_nodes_for_validation
                                     ? *g_current_local_global_ast_nodes_for_validation
                                     : collect_local_global_value_ast_nodes(program);

        std::optional<Type> declared_type;
        if (s.type_name) {
          declared_type = parse_and_resolve_type_text(
            *s.type_name, g_current_type_env_for_validation, s.range, result.diagnostics,
            std::string("variable '") + s.name + "' type", &program,
            /*forbid_private_across_files=*/false);

          // Normalize bounded sizes against the reference-site scope.
          if (declared_type.has_value() && g_current_const_eval_ctx_for_validation) {
            normalize_bounded_type_sizes_in_place(
              *declared_type, scope, result.symbols, g_current_type_env_for_validation,
              local_globals, *g_current_const_eval_ctx_for_validation, result.diagnostics, s.range);
          }

          // Keep TypeContext aligned with the (resolved + normalized) annotation.
          if (declared_type.has_value()) {
            // IMPORTANT:
            // For `_` / `_?` / `vec<_>` / `[_; N]` etc, `TypeResolver::resolve_tree_types`
            // may already have inferred a more specific type from the initializer.
            // Do not overwrite that inferred type with a raw annotation that still
            // contains inference wildcards.
            if (!type_contains_infer(*declared_type) || !ctx.has_type(s.name)) {
              ctx.set_type(s.name, *declared_type);
            }
          }
        }
        if (scope->lookup_local(s.name)) {
          result.diagnostics.error(s.range, "Duplicate variable name in scope: '" + s.name + "'");
          return;
        }

        // Spec (docs/reference/declarations-and-scopes.md 4.2.3):
        // Shadowing parent scopes is forbidden for all value-space declarations,
        // including tree-local and block scopes.
        if (scope->parent() && scope->parent()->lookup(s.name)) {
          result.diagnostics.error(s.range, "Shadowing is forbidden: '" + s.name + "'");
          return;
        }

        Symbol sym;
        sym.name = s.name;
        sym.kind = (list_kind == StatementListKind::ChildrenBlock) ? SymbolKind::BlockVariable
                                                                   : SymbolKind::LocalVariable;
        sym.type_name = s.type_name;
        sym.direction = std::nullopt;
        sym.definition_range = s.range;
        sym.ast_node = &s;
        scope->define(std::move(sym));

        if (s.initial_value.has_value()) {
          validate_value_space_refs_in_expr(
            *s.initial_value, scope, result.symbols, local_globals, result.diagnostics);

          validate_cast_type_refs_in_expr(
            *s.initial_value, program, g_current_type_env_for_validation, result.diagnostics);

          if (g_current_const_eval_ctx_for_validation) {
            validate_repeat_init_counts_in_expr(
              *s.initial_value, scope, result.symbols, g_current_type_env_for_validation,
              local_globals, result.diagnostics, *g_current_const_eval_ctx_for_validation);

            if (declared_type.has_value()) {
              validate_static_array_repeat_init_fits_target(
                *s.initial_value, *declared_type, scope, result.symbols,
                g_current_type_env_for_validation, local_globals, result.diagnostics,
                *g_current_const_eval_ctx_for_validation);
            }
          }
        }
      } else if constexpr (std::is_same_v<T, ConstDeclStmt>) {
        const auto local_global_ast_nodes = collect_local_global_value_ast_nodes(program);

        std::optional<Type> declared_type;
        if (s.type_name) {
          declared_type = parse_and_resolve_type_text(
            *s.type_name, g_current_type_env_for_validation, s.range, result.diagnostics,
            std::string("const '") + s.name + "' type", &program,
            /*forbid_private_across_files=*/false);

          // Normalize bounded sizes against the reference-site scope.
          if (declared_type.has_value() && g_current_const_eval_ctx_for_validation) {
            normalize_bounded_type_sizes_in_place(
              *declared_type, scope, result.symbols, g_current_type_env_for_validation,
              local_global_ast_nodes, *g_current_const_eval_ctx_for_validation, result.diagnostics,
              s.range);
          }

          // Keep TypeContext aligned with the (resolved + normalized) annotation.
          if (declared_type.has_value()) {
            ctx.set_type(s.name, *declared_type);
          }
        }
        if (scope->lookup_local(s.name)) {
          result.diagnostics.error(s.range, "Duplicate constant name in scope: '" + s.name + "'");
          return;
        }

        // Spec: shadowing is forbidden (see above).
        if (scope->parent() && scope->parent()->lookup(s.name)) {
          result.diagnostics.error(s.range, "Shadowing is forbidden: '" + s.name + "'");
          return;
        }

        // const initializer must be const_expr.
        validate_const_expr(
          s.value, scope, result.symbols, g_current_type_env_for_validation, local_global_ast_nodes,
          result.diagnostics, std::string_view(s.name));

        if (g_current_const_eval_ctx_for_validation) {
          validate_repeat_init_counts_in_expr(
            s.value, scope, result.symbols, g_current_type_env_for_validation,
            local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation,
            /*in_vec_macro=*/false, /*already_validated_const=*/true, std::string_view(s.name));

          // Reference: docs/reference/declarations-and-scopes.md 4.3.4
          // const_expr must be fully evaluable.
          {
            auto v = eval_const_value(
              s.value, scope, result.symbols, *g_current_const_eval_ctx_for_validation,
              result.diagnostics, std::string_view(s.name));
            if (!v.has_value()) {
              result.diagnostics.error(
                get_range(s.value),
                "Constant initializer must be a fully evaluable constant expression");
            }
          }

          if (declared_type.has_value()) {
            validate_static_array_repeat_init_fits_target(
              s.value, *declared_type, scope, result.symbols, g_current_type_env_for_validation,
              local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
          }
        }

        Symbol sym;
        sym.name = s.name;
        sym.kind = (list_kind == StatementListKind::ChildrenBlock) ? SymbolKind::BlockConst
                                                                   : SymbolKind::LocalConst;
        sym.type_name = s.type_name;
        sym.direction = std::nullopt;
        sym.definition_range = s.range;
        sym.ast_node = &s;
        scope->define(std::move(sym));
      }
    },
    stmt);
}

void Analyzer::validate_node_stmt(
  const Program & program, const NodeStmt & node, const TreeDef & tree, TypeContext & ctx,
  Scope * scope, StatementListKind list_kind, AnalysisResult & result)
{
  (void)tree;

  // Flow-sensitive narrowing (docs/reference/static-analysis-and-safety.md 6.2)
  // Within the children block of @guard(expr) / @run_while(expr), `expr` is assumed true.
  // We narrow `T?` to `T` for any variables proven non-null by the necessary conditions
  // of the expression (currently: x != null, conjunction, and ! (x == null)).
  auto collect_non_null_vars = [](
                                 const Expression & expr, bool negate, auto & self,
                                 std::unordered_set<std::string> & out) -> void {
    if (const auto * u = std::get_if<Box<UnaryExpr>>(&expr)) {
      if ((**u).op == UnaryOp::Not) {
        self((**u).operand, !negate, self, out);
      }
      return;
    }
    if (const auto * b = std::get_if<Box<BinaryExpr>>(&expr)) {
      const auto & be = **b;
      if (!negate && be.op == BinaryOp::And) {
        self(be.left, false, self, out);
        self(be.right, false, self, out);
        return;
      }

      // Optional negation handling: !(x == null) -> x != null.
      BinaryOp op = be.op;
      if (negate) {
        if (op == BinaryOp::Eq) {
          op = BinaryOp::Ne;
        } else if (op == BinaryOp::Ne) {
          op = BinaryOp::Eq;
        }
      }

      if (op == BinaryOp::Ne) {
        const auto is_null = [](const Expression & e) -> bool {
          if (const auto * lit = std::get_if<Literal>(&e)) {
            return std::holds_alternative<NullLiteral>(*lit);
          }
          return false;
        };

        if (const auto * vr = std::get_if<VarRef>(&be.left); vr && is_null(be.right)) {
          out.insert(vr->name);
          return;
        }
        if (const auto * vr = std::get_if<VarRef>(&be.right); vr && is_null(be.left)) {
          out.insert(vr->name);
          return;
        }
      }
      return;
    }
  };

  auto narrowed_type = [](const Type & t) -> std::optional<Type> {
    if (const auto * n = std::get_if<TypeNullable>(&t.value)) {
      return *n->base;
    }
    return std::nullopt;
  };

  // Spec: ambiguity across direct imports is reported at the reference site.
  if (
    g_current_ambiguous_imported_node_names_for_validation &&
    g_current_ambiguous_imported_node_names_for_validation->count(node.node_name) > 0) {
    result.diagnostics.error(node.range, "Ambiguous imported node name: '" + node.node_name + "'");
    return;
  }

  // Validate node category constraints
  validate_node_category(node, result);

  const auto local_global_ast_nodes = g_current_local_global_ast_nodes_for_validation
                                        ? *g_current_local_global_ast_nodes_for_validation
                                        : collect_local_global_value_ast_nodes(program);

  // Validate preconditions: resolve all referenced values and enforce global declaration order.
  for (const auto & pc : node.preconditions) {
    validate_value_space_refs_in_expr(
      pc.condition, scope, result.symbols, local_global_ast_nodes, result.diagnostics);

    validate_cast_type_refs_in_expr(
      pc.condition, program, g_current_type_env_for_validation, result.diagnostics);

    // Type-check: preconditions must be bool.
    const TypeResolver resolver(result.symbols, result.nodes, g_current_type_env_for_validation);
    auto get_global_type = [&](std::string_view name) -> const Type * {
      if (!g_current_global_types_for_validation) {
        return nullptr;
      }
      auto it = g_current_global_types_for_validation->find(std::string(name));
      if (it == g_current_global_types_for_validation->end()) {
        return nullptr;
      }
      return &it->second;
    };

    auto ty = resolver.infer_expression_type(pc.condition, ctx, get_global_type);
    if (ty.has_error()) {
      result.diagnostics.error(pc.range, ty.error.value_or("Invalid precondition expression"));
      continue;
    }

    Type t = ty.type;
    if (g_current_type_env_for_validation) {
      t = g_current_type_env_for_validation->resolve(t);
    }
    if (!t.is_any() && !t.is_unknown() && !std::holds_alternative<TypeBool>(t.value)) {
      result.diagnostics.error(pc.range, "Precondition must be of type bool");
    }
  }

  // Validate arguments (includes deep expression name checks)
  validate_arguments(program, node, tree, ctx, scope, list_kind, result);

  // children_block introduces a nested scope.
  if (node.has_children_block) {
    // Apply narrowing only for @guard/@run_while.
    std::unordered_set<std::string> non_null;
    non_null.reserve(8);
    for (const auto & pc : node.preconditions) {
      if (pc.kind == "guard" || pc.kind == "run_while") {
        collect_non_null_vars(pc.condition, false, collect_non_null_vars, non_null);
      }
    }

    if (non_null.empty()) {
      validate_statement_block(
        *g_current_program_for_validation, node.children, tree, ctx, scope, result);
      return;
    }

    TypeContext narrowed_ctx = ctx;

    std::unordered_map<std::string, Type> narrowed_globals;
    if (g_current_global_types_for_validation) {
      narrowed_globals = *g_current_global_types_for_validation;
    }

    for (const auto & name : non_null) {
      if (const Type * t = ctx.get_type(name)) {
        if (auto nt = narrowed_type(*t)) {
          narrowed_ctx.set_type(name, std::move(*nt));
        }
      }
      auto itg = narrowed_globals.find(name);
      if (itg != narrowed_globals.end()) {
        if (auto nt = narrowed_type(itg->second)) {
          itg->second = std::move(*nt);
        }
      }
    }

    struct GlobalTypeOverrideGuard
    {
      const std::unordered_map<std::string, Type> * saved = nullptr;
      explicit GlobalTypeOverrideGuard(const std::unordered_map<std::string, Type> * next)
      {
        saved = g_current_global_types_for_validation;
        g_current_global_types_for_validation = next;
      }
      ~GlobalTypeOverrideGuard() { g_current_global_types_for_validation = saved; }
    };

    const GlobalTypeOverrideGuard guard(
      g_current_global_types_for_validation ? &narrowed_globals : nullptr);

    validate_statement_block(
      *g_current_program_for_validation, node.children, tree, narrowed_ctx, scope, result);
  }
}

void Analyzer::validate_assignment_stmt(
  const Program & program, const AssignmentStmt & stmt, const TreeDef & tree, TypeContext & ctx,
  Scope * scope, AnalysisResult & result)
{
  (void)tree;

  // Preconditions on assignment statements are part of the reference syntax
  // (execution-model.md 5.3.3 + desugaring 5.4.2). They must be validated the
  // same way as node preconditions.
  for (const auto & pc : stmt.preconditions) {
    const auto local_global_ast_nodes = g_current_local_global_ast_nodes_for_validation
                                          ? *g_current_local_global_ast_nodes_for_validation
                                          : collect_local_global_value_ast_nodes(program);

    validate_value_space_refs_in_expr(
      pc.condition, scope, result.symbols, local_global_ast_nodes, result.diagnostics);

    validate_cast_type_refs_in_expr(
      pc.condition, program, g_current_type_env_for_validation, result.diagnostics);

    if (g_current_const_eval_ctx_for_validation) {
      validate_repeat_init_counts_in_expr(
        pc.condition, scope, result.symbols, g_current_type_env_for_validation,
        local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
    }

    // Type-check: preconditions must be bool.
    const TypeResolver resolver(result.symbols, result.nodes, g_current_type_env_for_validation);
    auto get_global_type = [&](std::string_view name) -> const Type * {
      if (!g_current_global_types_for_validation) {
        return nullptr;
      }
      auto it = g_current_global_types_for_validation->find(std::string(name));
      if (it == g_current_global_types_for_validation->end()) {
        return nullptr;
      }
      return &it->second;
    };

    auto ty = resolver.infer_expression_type(pc.condition, ctx, get_global_type);
    if (ty.has_error()) {
      result.diagnostics.error(pc.range, ty.error.value_or("Invalid precondition expression"));
      continue;
    }

    Type t = ty.type;
    if (g_current_type_env_for_validation) {
      t = g_current_type_env_for_validation->resolve(t);
    }
    if (!t.is_any() && !t.is_unknown() && !std::holds_alternative<TypeBool>(t.value)) {
      result.diagnostics.error(pc.range, "Precondition must be of type bool");
    }
  }

  {
    const auto local_global_ast_nodes = g_current_local_global_ast_nodes_for_validation
                                          ? *g_current_local_global_ast_nodes_for_validation
                                          : collect_local_global_value_ast_nodes(program);
    validate_value_space_refs_in_expr(
      stmt.value, scope, result.symbols, local_global_ast_nodes, result.diagnostics);

    validate_cast_type_refs_in_expr(
      stmt.value, program, g_current_type_env_for_validation, result.diagnostics);

    if (g_current_const_eval_ctx_for_validation) {
      validate_repeat_init_counts_in_expr(
        stmt.value, scope, result.symbols, g_current_type_env_for_validation,
        local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
    }
    for (const auto & idx : stmt.indices) {
      validate_value_space_refs_in_expr(
        idx, scope, result.symbols, local_global_ast_nodes, result.diagnostics);

      validate_cast_type_refs_in_expr(
        idx, program, g_current_type_env_for_validation, result.diagnostics);
    }
  }

  const Symbol * sym = result.symbols.resolve(stmt.target, scope);

  if (!sym || !sym->is_variable()) {
    result.diagnostics.error(stmt.range, "Unknown variable: " + stmt.target);
    return;
  }

  if (sym->is_const()) {
    result.diagnostics.error(stmt.range, "Cannot assign to const: " + stmt.target);
    return;
  }

  // Assignment to an input-only parameter is not allowed.
  if (sym->kind == SymbolKind::Parameter && !sym->is_writable()) {
    result.diagnostics.error(
      stmt.range, "Parameter '" + sym->name + "' is input-only and cannot be assigned");
    return;
  }

  auto get_global_type = [&](std::string_view name) -> const Type * {
    if (g_current_global_types_for_validation) {
      auto it = g_current_global_types_for_validation->find(std::string(name));
      if (it != g_current_global_types_for_validation->end()) {
        return &it->second;
      }
    }
    return nullptr;
  };

  // Determine target type
  Type target_type = Type::unknown();
  if (sym->type_name) {
    auto resolved = parse_and_resolve_type_text(
      *sym->type_name, g_current_type_env_for_validation, stmt.range, result.diagnostics,
      std::string("assignment target '") + sym->name + "' type", &program,
      /*forbid_private_across_files=*/false);
    if (!resolved.has_value()) {
      return;
    }
    target_type = *resolved;
  } else if (const Type * t = ctx.get_type(sym->name)) {
    target_type = *t;
  }

  // If the RHS is a repeat-init array literal and we can evaluate its count,
  // ensure it fits the static-array target size (including <= bound).
  if (g_current_const_eval_ctx_for_validation) {
    const auto local_global_ast_nodes = g_current_local_global_ast_nodes_for_validation
                                          ? *g_current_local_global_ast_nodes_for_validation
                                          : collect_local_global_value_ast_nodes(program);
    validate_static_array_repeat_init_fits_target(
      stmt.value, target_type, scope, result.symbols, g_current_type_env_for_validation,
      local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
  }

  const TypeResolver resolver(result.symbols, result.nodes, g_current_type_env_for_validation);
  auto rhs_result = resolver.infer_expression_type(stmt.value, ctx, get_global_type);
  if (rhs_result.has_error()) {
    result.diagnostics.error(stmt.range, rhs_result.error.value_or("Unknown error"));
    return;
  }

  const Type & rhs_type = rhs_result.type;

  auto emit_assign_error = [&](std::string_view msg) {
    result.diagnostics.error(stmt.range, std::string(msg));
  };

  if (stmt.op == AssignOp::Assign) {
    if (!target_type.is_compatible_with(rhs_type)) {
      emit_assign_error("Cannot assign " + rhs_type.to_string() + " to " + target_type.to_string());
    }
    return;
  }

  // Compound assignments: treat as target = target <op> rhs
  // Minimal semantics: require numeric for -=, *=, /=; allow numeric or (string
  // + string) for +=
  if (stmt.op == AssignOp::AddAssign) {
    const bool numeric_ok = target_type.is_numeric() && rhs_type.is_numeric();
    const bool string_ok =
      target_type.equals(Type::string_type()) && rhs_type.equals(Type::string_type());
    if (!(numeric_ok || string_ok)) {
      emit_assign_error("Operator cannot be applied to non-numeric types");
    }
    return;
  }

  // -=, *=, /=
  if (!(target_type.is_numeric() && rhs_type.is_numeric())) {
    emit_assign_error("Operator cannot be applied to non-numeric types");
  }
}

void Analyzer::validate_node_category(const NodeStmt & node, AnalysisResult & result)
{
  const NodeInfo * info = result.nodes.get_node(node.node_name);
  if (!info) {
    result.diagnostics.error(node.range, "Unknown node: '" + node.node_name + "'");
    return;
  }

  // Check children constraints
  if (node.has_children_block && !info->can_have_children()) {
    result.diagnostics.error(
      node.range, "Node '" + node.node_name + "' cannot have a children block. " +
                    "Only 'Control' and 'Decorator' category nodes can have children.");
  }

  // Check that nodes which can own children are used with an explicit children
  // block. (Empty blocks are allowed; semantic arity checks are out of scope
  // here.)
  if (
    (info->category == NodeCategory::Control || info->category == NodeCategory::Decorator) &&
    !node.has_children_block) {
    const std::string kind = (info->category == NodeCategory::Control) ? "Control" : "Decorator";
    result.diagnostics.error(
      node.range, kind + " node '" + node.node_name + "' requires a children block");
  }
}

void Analyzer::validate_arguments(
  const Program & program, const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx,
  Scope * scope, StatementListKind list_kind, AnalysisResult & result)
{
  (void)tree;

  const NodeInfo * info = result.nodes.get_node(node.node_name);
  if (!info) {
    return;
  }

  const bool node_is_local = program_defines_node(program, node.node_name);
  const bool forbid_private_in_port_types = !node_is_local;

  // Reference spec helpers
  const auto requires_lvalue = [](PortDirection port_dir) {
    // ref/mut/out require an lvalue argument.
    return port_dir == PortDirection::Ref || port_dir == PortDirection::Mut ||
           port_dir == PortDirection::Out;
  };

  const auto is_write_access = [](PortDirection arg_dir) {
    // Only mut/out are write access in the reference semantics.
    return arg_dir == PortDirection::Mut || arg_dir == PortDirection::Out;
  };

  enum class DirDiagKind { Ok, Warning, Error };
  const auto check_dir_matrix = [](PortDirection arg_dir, PortDirection port_dir) -> DirDiagKind {
    // Reference semantics.md 3.2 matrix
    switch (arg_dir) {
      case PortDirection::In:
        return (port_dir == PortDirection::In) ? DirDiagKind::Ok : DirDiagKind::Error;
      case PortDirection::Ref:
        if (port_dir == PortDirection::Ref) return DirDiagKind::Ok;
        if (port_dir == PortDirection::In) return DirDiagKind::Warning;
        return DirDiagKind::Error;  // ref -> mut/out is invalid
      case PortDirection::Mut:
        if (port_dir == PortDirection::Mut) return DirDiagKind::Ok;
        if (port_dir == PortDirection::In || port_dir == PortDirection::Ref)
          return DirDiagKind::Warning;
        return DirDiagKind::Error;  // mut -> out is invalid
      case PortDirection::Out:
        return (port_dir == PortDirection::Out) ? DirDiagKind::Ok : DirDiagKind::Error;
    }
    return DirDiagKind::Error;
  };

  // Track which ports are provided by this call (for omission/required checks)
  std::unordered_set<std::string> provided_ports;
  provided_ports.reserve(node.args.size());

  for (const auto & arg : node.args) {
    // Reference: docs/reference/syntax.md 2.6.4
    // argument := identifier ':' argument_expr
    if (!arg.name) {
      result.diagnostics.error(
        arg.range, "Positional arguments are not allowed; expected 'port: expr' for node '" +
                     node.node_name + "'");
      continue;
    }

    const std::string & port_name = *arg.name;

    const PortInfo * port = info->get_port(port_name);
    if (!port) {
      result.diagnostics.error(
        arg.range, "Unknown port '" + port_name + "' for node '" + node.node_name + "'");
      continue;
    }

    if (provided_ports.count(port_name)) {
      result.diagnostics.error(
        arg.range,
        "Duplicate argument for port '" + port_name + "' in node '" + node.node_name + "'");
      continue;
    }

    const PortDirection arg_dir = arg.direction.value_or(PortDirection::In);
    const PortDirection port_dir = port->direction;

    provided_ports.insert(port_name);

    std::optional<std::string> var_name;
    SourceRange var_range = arg.range;

    if (const auto * inline_decl = std::get_if<InlineBlackboardDecl>(&arg.value)) {
      var_name = inline_decl->name;
      var_range = inline_decl->range;

      if (port_dir != PortDirection::Out) {
        result.diagnostics.error(
          var_range, "Inline declaration is only allowed for 'out' ports (port '" + port_name +
                       "' is not out)");
        continue;
      }

      if (arg_dir != PortDirection::Out) {
        result.diagnostics.error(var_range, "Inline declaration requires 'out' direction");
        continue;
      }

      // Define the symbol in the current statement-list scope.
      if (scope->lookup_local(*var_name)) {
        result.diagnostics.error(
          var_range, "Duplicate variable name in scope: '" + *var_name + "'");
        continue;
      }

      // Spec: shadowing is forbidden for all declarations (tree-local and block).
      if (scope->parent() && scope->parent()->lookup(*var_name)) {
        result.diagnostics.error(var_range, "Shadowing is forbidden: '" + *var_name + "'");
        continue;
      }

      Symbol sym;
      sym.name = *var_name;
      sym.kind = (list_kind == StatementListKind::ChildrenBlock) ? SymbolKind::BlockVariable
                                                                 : SymbolKind::LocalVariable;
      sym.type_name = std::nullopt;
      sym.direction = std::nullopt;
      sym.definition_range = var_range;
      sym.ast_node = inline_decl;
      scope->define(std::move(sym));

      // Type inference for inline decl is handled by TypeResolver (port type).
      continue;
    }

    const auto * expr = std::get_if<Expression>(&arg.value);
    if (!expr) {
      continue;
    }

    const auto local_global_ast_nodes = g_current_local_global_ast_nodes_for_validation
                                          ? *g_current_local_global_ast_nodes_for_validation
                                          : collect_local_global_value_ast_nodes(program);

    // Always validate referenced value-space identifiers in the full expression.
    validate_value_space_refs_in_expr(
      *expr, scope, result.symbols, local_global_ast_nodes, result.diagnostics);

    // Also validate any cast target type names used within the expression.
    validate_cast_type_refs_in_expr(
      *expr, program, g_current_type_env_for_validation, result.diagnostics);

    // Enforce repeat-init count rules for static array literals used in arguments.
    if (g_current_const_eval_ctx_for_validation) {
      validate_repeat_init_counts_in_expr(
        *expr, scope, result.symbols, g_current_type_env_for_validation, local_global_ast_nodes,
        result.diagnostics, *g_current_const_eval_ctx_for_validation);
    }

    // Any direction marker other than 'in' implies lvalue semantics.
    if (arg_dir != PortDirection::In) {
      const bool is_var = std::holds_alternative<VarRef>(*expr);
      const bool is_index = std::holds_alternative<Box<IndexExpr>>(*expr);
      if (!(is_var || is_index)) {
        result.diagnostics.error(
          arg.range,
          "Direction '" + std::string(to_string(arg_dir)) + "' requires an lvalue argument");
      }
    }

    // LValue constraint: ref/mut/out require an identifier/index.
    if (requires_lvalue(port_dir)) {
      const bool is_var = std::holds_alternative<VarRef>(*expr);
      const bool is_index = std::holds_alternative<Box<IndexExpr>>(*expr);
      if (!(is_var || is_index)) {
        result.diagnostics.error(arg.range, "Port '" + port_name + "' requires an lvalue argument");
        continue;
      }
    }

    if (const auto * vr = std::get_if<VarRef>(expr)) {
      var_name = vr->name;
      var_range = vr->range;
    } else {
      // For IndexExpr lvalues, use the base container name for const/param restrictions.
      var_name = extract_lvalue_base_name_for_semantic(*expr);
      if (var_name) {
        var_range = get_range(*expr);
      }
    }

    // Only identifier-like arguments require symbol/type/direction checks.
    if (!var_name) {
      // Still allow type-checking of pure expressions for in-ports.
      if (port->type_name && port_dir == PortDirection::In) {
        const TypeResolver resolver(
          result.symbols, result.nodes, g_current_type_env_for_validation);
        auto get_global_type = [&](std::string_view name) -> const Type * {
          if (!g_current_global_types_for_validation) {
            return nullptr;
          }
          auto it = g_current_global_types_for_validation->find(std::string(name));
          if (it == g_current_global_types_for_validation->end()) {
            return nullptr;
          }
          return &it->second;
        };

        auto expr_ty = resolver.infer_expression_type(*expr, ctx, get_global_type);
        if (expr_ty.has_error()) {
          result.diagnostics.error(arg.range, expr_ty.error.value_or("Invalid expression"));
          continue;
        }

        auto resolved_port_type = parse_and_resolve_type_text(
          *port->type_name, g_current_type_env_for_validation, arg.range, result.diagnostics,
          std::string("port '") + port_name + "' of node '" + node.node_name + "'", nullptr,
          forbid_private_in_port_types);
        if (!resolved_port_type.has_value()) {
          continue;
        }
        const Type & port_type = *resolved_port_type;
        Type src_type = expr_ty.type;
        if (g_current_type_env_for_validation) {
          src_type = g_current_type_env_for_validation->resolve(src_type);
        }

        if (!port_type.is_compatible_with(src_type)) {
          result.diagnostics.error(
            arg.range, "Type mismatch: expression is '" + src_type.to_string() + "' but port '" +
                         port_name + "' expects '" + port_type.to_string() + "'.");
        }
      }
      continue;
    }

    const Symbol * var_sym = result.symbols.resolve(*var_name, scope);
    if (!var_sym || (!var_sym->is_variable() && !var_sym->is_const())) {
      result.diagnostics.error(var_range, "Unknown variable: " + *var_name);
      continue;
    }

    // Const restrictions (reference semantics.md 3.3):
    // - ref allows const
    // - mut/out forbid const
    if (var_sym->is_const() && (port_dir == PortDirection::Mut || port_dir == PortDirection::Out)) {
      result.diagnostics.error(
        var_range, "Cannot pass const '" + var_sym->name + "' to '" +
                     std::string(to_string(port_dir)) + "' port '" + port_name + "'");
      continue;
    }

    // Type check: var type vs port type
    if (port->type_name) {
      auto resolved_port_type = parse_and_resolve_type_text(
        *port->type_name, g_current_type_env_for_validation, arg.range, result.diagnostics,
        std::string("port '") + port_name + "' of node '" + node.node_name + "'", nullptr,
        forbid_private_in_port_types);
      if (!resolved_port_type.has_value()) {
        continue;
      }
      const Type & port_type = *resolved_port_type;

      const TypeResolver resolver(result.symbols, result.nodes, g_current_type_env_for_validation);
      auto get_global_type = [&](std::string_view name) -> const Type * {
        if (!g_current_global_types_for_validation) {
          return nullptr;
        }
        auto it = g_current_global_types_for_validation->find(std::string(name));
        if (it == g_current_global_types_for_validation->end()) {
          return nullptr;
        }
        return &it->second;
      };

      Type var_type = Type::unknown();
      auto expr_ty = resolver.infer_expression_type(*expr, ctx, get_global_type);
      if (expr_ty.has_error()) {
        result.diagnostics.error(arg.range, expr_ty.error.value_or("Invalid argument expression"));
        continue;
      }
      var_type = expr_ty.type;

      if (g_current_type_env_for_validation) {
        var_type = g_current_type_env_for_validation->resolve(var_type);
      }

      if (!var_type.is_unknown() && !port_type.is_unknown()) {
        bool ok = true;
        switch (port_dir) {
          case PortDirection::In:
            // value flows: var -> node (port is target)
            ok = port_type.is_compatible_with(var_type);
            break;
          case PortDirection::Out:
            // value flows: node -> var (var is target)
            ok = var_type.is_compatible_with(port_type);
            break;
          case PortDirection::Ref:
          case PortDirection::Mut:
            // reference ports require exact type match
            ok = port_type.equals(var_type);
            break;
        }

        if (!ok) {
          std::string msg;
          if (port_dir == PortDirection::Out) {
            msg = "Type mismatch: port '" + port_name + "' writes '" + port_type.to_string() +
                  "' but '" + var_sym->name + "' is '" + var_type.to_string() + "'.";
          } else if (port_dir == PortDirection::Ref || port_dir == PortDirection::Mut) {
            msg = "Type mismatch: port '" + port_name + "' requires exact type '" +
                  port_type.to_string() + "' but '" + var_sym->name + "' is '" +
                  var_type.to_string() + "'.";
          } else {
            msg = "Type mismatch: '" + var_sym->name + "' is '" + var_type.to_string() +
                  "' but port '" + port_name + "' expects '" + port_type.to_string() + "'.";
          }
          result.diagnostics.error(var_range, msg);
        }
      }
    }

    // Direction compatibility (reference semantics.md 3.2 matrix)
    const DirDiagKind m = check_dir_matrix(arg_dir, port_dir);
    if (m == DirDiagKind::Error) {
      result.diagnostics.error(
        arg.range, "Direction mismatch: port '" + port_name + "' is '" +
                     std::string(to_string(port_dir)) + "' but argument is '" +
                     std::string(to_string(arg_dir)) + "'.");
      continue;
    }
    if (m == DirDiagKind::Warning) {
      result.diagnostics.warning(
        arg.range, "Direction is more permissive than required: port '" + port_name + "' is '" +
                     std::string(to_string(port_dir)) + "' but argument is '" +
                     std::string(to_string(arg_dir)) + "'.");
    }

    // tree parameter permission table (reference semantics.md 3.4)
    if (var_sym->kind == SymbolKind::Parameter) {
      const PortDirection param_dir = var_sym->direction.value_or(PortDirection::In);
      const bool arg_is_ref = (arg_dir == PortDirection::Ref);
      const bool arg_is_write = is_write_access(arg_dir);

      // ref is allowed for in/ref/mut params, but not for out params
      if (arg_is_ref && param_dir == PortDirection::Out) {
        result.diagnostics.error(
          arg.range, "Parameter '" + var_sym->name + "' is 'out' and cannot be used with 'ref'");
        continue;
      }

      // mut/out write access requires mut/out params
      if (arg_is_write && (param_dir != PortDirection::Mut && param_dir != PortDirection::Out)) {
        result.diagnostics.error(
          arg.range, "Parameter '" + var_sym->name + "' is input-only but used with '" +
                       std::string(to_string(arg_dir)) + "'");
        continue;
      }
    }
  }

  // Enforce omission/required rules (reference semantics.md 5.3)
  // - in: required unless it has a default
  // - ref/mut: always required
  // - out: always optional (discardable)
  for (const auto & p : info->ports) {
    if (provided_ports.count(p.name) > 0) {
      continue;
    }
    switch (p.direction) {
      case PortDirection::In:
        if (!p.default_value.has_value()) {
          result.diagnostics.error(
            node.range,
            "Missing required input port '" + p.name + "' for node '" + node.node_name + "'");
        }
        break;
      case PortDirection::Ref:
        result.diagnostics.error(
          node.range,
          "Missing required ref port '" + p.name + "' for node '" + node.node_name + "'");
        break;
      case PortDirection::Mut:
        result.diagnostics.error(
          node.range,
          "Missing required mut port '" + p.name + "' for node '" + node.node_name + "'");
        break;
      case PortDirection::Out:
        // Always optional
        break;
    }
  }
}

void Analyzer::check_direction_permission(
  const Argument & arg, const PortInfo * port, const TreeDef & tree, const TypeContext & /* ctx */,
  AnalysisResult & result)
{
  // Deprecated by validate_arguments' unified checks; keep for API
  // compatibility. (No-op)
  (void)arg;
  (void)port;
  (void)tree;
  (void)result;
}

void Analyzer::check_write_param_usage(const TreeDef & tree, AnalysisResult & result)
{
  // Find parameters declared as mut/out (write-capable).
  std::unordered_set<std::string> writable_params;
  for (const auto & param : tree.params) {
    if (
      param.direction &&
      (*param.direction == PortDirection::Out || *param.direction == PortDirection::Mut)) {
      writable_params.insert(param.name);
    }
  }

  if (writable_params.empty()) {
    return;
  }

  // Collect parameters actually used for write access (explicit out/ref, plus
  // assignments)
  std::unordered_set<std::string> used_for_write;
  collect_write_usages_in_block(tree.body, writable_params, used_for_write);

  // Warn about unused write params
  for (const auto & param : tree.params) {
    if (writable_params.count(param.name) && !used_for_write.count(param.name)) {
      result.diagnostics.warning(
        param.range,
        "Parameter '" + param.name + "' is declared as mut/out but never used for write access");
    }
  }
}

void Analyzer::collect_write_usages_in_block(
  const std::vector<Statement> & stmts, const std::unordered_set<std::string> & writable_params,
  std::unordered_set<std::string> & used_for_write)
{
  const auto is_write_dir = [](PortDirection d) {
    return d == PortDirection::Out || d == PortDirection::Mut;
  };

  for (const auto & stmt : stmts) {
    std::visit(
      [&](const auto & s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, AssignmentStmt>) {
          if (writable_params.count(s.target)) {
            used_for_write.insert(s.target);
          }
        } else if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          for (const auto & arg : s->args) {
            const auto dir = arg.direction;
            if (!dir || !is_write_dir(*dir)) {
              continue;
            }

            std::optional<std::string> name;
            if (const auto * inline_decl = std::get_if<InlineBlackboardDecl>(&arg.value)) {
              name = inline_decl->name;
            } else if (const auto * expr = std::get_if<Expression>(&arg.value)) {
              if (const auto * vr = std::get_if<VarRef>(expr)) {
                name = vr->name;
              }
            }

            if (name && writable_params.count(*name)) {
              used_for_write.insert(*name);
            }
          }

          if (s->has_children_block) {
            collect_write_usages_in_block(s->children, writable_params, used_for_write);
          }
        }
      },
      stmt);
  }
}

void Analyzer::validate_declare_stmt(
  const Program & program, const DeclareStmt & decl, AnalysisResult & result)
{
  // Check category validity
  auto category = node_category_from_string(decl.category);
  if (!category) {
    result.diagnostics.error(
      decl.range, "Invalid category: '" + decl.category + "'. " +
                    "Valid categories are: action, condition, "
                    "control, decorator, subtree");
  }

  // Check for duplicate port names
  std::unordered_set<std::string> seen_ports;
  for (const auto & port : decl.ports) {
    auto [it, inserted] = seen_ports.insert(port.name);
    if (!inserted) {
      result.diagnostics.error(port.range, "Duplicate port name: '" + port.name + "'");
    }

    // Validate port type syntax + resolution (also enforces import visibility).
    const auto port_type = parse_and_resolve_type_text(
      port.type_name, g_current_type_env_for_validation, port.range, result.diagnostics,
      std::string("declare port '") + port.name + "' type", &program,
      /*forbid_private_across_files=*/false);

    // Default values are only allowed for `in` ports (reference semantics.md 5.4)
    const PortDirection dir = port.direction.value_or(PortDirection::In);
    if (port.default_value.has_value() && dir != PortDirection::In) {
      result.diagnostics.error(
        port.range, "Default value is only allowed for 'in' ports, but port '" + port.name +
                      "' is '" + std::string(to_string(dir)) + "'");
    }

    // Default values in declarations are const_expr.
    if (port.default_value.has_value()) {
      if (const Scope * g = result.symbols.global_scope()) {
        const auto local_global_ast_nodes = collect_local_global_value_ast_nodes(program);
        validate_const_expr(
          *port.default_value, g, result.symbols, g_current_type_env_for_validation,
          local_global_ast_nodes, result.diagnostics);

        if (g_current_const_eval_ctx_for_validation) {
          // Reference: docs/reference/declarations-and-scopes.md 4.3.4
          // const_expr must be fully evaluable.
          {
            auto v = eval_const_value(
              *port.default_value, g, result.symbols, *g_current_const_eval_ctx_for_validation,
              result.diagnostics, std::nullopt);
            if (!v.has_value()) {
              result.diagnostics.error(
                get_range(*port.default_value),
                "Default value must be a fully evaluable constant expression");
            }
          }

          // Enforce repeat-init integer const_expr rules even in defaults.
          validate_repeat_init_counts_in_expr(
            *port.default_value, g, result.symbols, g_current_type_env_for_validation,
            local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation,
            /*in_vec_macro=*/false, /*already_validated_const=*/true, std::nullopt);

          if (port_type.has_value()) {
            validate_static_array_repeat_init_fits_target(
              *port.default_value, *port_type, g, result.symbols, g_current_type_env_for_validation,
              local_global_ast_nodes, result.diagnostics, *g_current_const_eval_ctx_for_validation);
          }
        }
      }
    }
  }
}

}  // namespace bt_dsl
