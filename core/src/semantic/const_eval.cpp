// bt_dsl/const_eval.cpp - Compile-time constant evaluation helpers
#include "bt_dsl/semantic/const_eval.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

namespace bt_dsl
{

namespace
{

bool is_public_name(std::string_view name)
{
  // Reference: docs/reference/declarations-and-scopes.md 4.1.2
  return !name.empty() && name.front() != '_';
}

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

std::optional<uint64_t> eval_type_bound_u64_for_const_eval(
  const TypeArraySizeExpr & size, const Scope * scope, const SymbolTable & symbols,
  ConstEvalContext & ctx, DiagnosticBag & diagnostics, const TypeEnvironment * type_env,
  const SourceRange & diag_range);

}  // namespace

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
    const uint64_t len = static_cast<uint64_t>(len_a);
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

std::optional<ConstValue> eval_global_const_value(
  std::string_view name, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, const TypeEnvironment * type_env)
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
  auto v = eval_const_value(decl->value, scope, symbols, ctx, diagnostics, type_env, key);
  ctx.in_stack.erase(key);

  ctx.memo_value[key] = v;
  return v;
}

namespace
{

std::optional<uint64_t> eval_type_bound_u64_for_const_eval(
  const TypeArraySizeExpr & size, const Scope * scope, const SymbolTable & symbols,
  ConstEvalContext & ctx, DiagnosticBag & diagnostics, const TypeEnvironment * type_env,
  const SourceRange & diag_range)
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
  auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, type_env, std::nullopt);
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
  return static_cast<uint64_t>(*iv);
}

}  // namespace

std::optional<ConstValue> eval_const_value(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, const TypeEnvironment * type_env,
  std::optional<std::string_view> current_const_name)
{
  auto eval_as_bool = [&](const Expression & e) -> std::optional<bool> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, type_env, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * b = std::get_if<bool>(&*v)) return *b;
    return std::nullopt;
  };

  auto eval_as_i64 = [&](const Expression & e) -> std::optional<int64_t> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, type_env, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * i = std::get_if<int64_t>(&*v)) return *i;
    return std::nullopt;
  };

  auto eval_as_f64 = [&](const Expression & e) -> std::optional<double> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, type_env, current_const_name);
    if (!v) return std::nullopt;
    if (const auto * f = std::get_if<double>(&*v)) return *f;
    if (const auto * i = std::get_if<int64_t>(&*v)) return static_cast<double>(*i);
    return std::nullopt;
  };

  auto eval_as_string = [&](const Expression & e) -> std::optional<std::string> {
    auto v = eval_const_value(e, scope, symbols, ctx, diagnostics, type_env, current_const_name);
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
          return eval_global_const_value(sym->name, scope, symbols, ctx, diagnostics, type_env);
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
        auto v =
          eval_const_value(decl->value, scope, symbols, ctx, diagnostics, type_env, sym->name);
        ctx.local_in_stack.erase(sym->ast_node);
        return v;
      } else if constexpr (std::is_same_v<T, MissingExpr>) {
        return std::nullopt;
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
          auto lv = eval_const_value(
            e->left, scope, symbols, ctx, diagnostics, type_env, current_const_name);
          auto rv = eval_const_value(
            e->right, scope, symbols, ctx, diagnostics, type_env, current_const_name);
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
        auto src =
          eval_const_value(e->expr, scope, symbols, ctx, diagnostics, type_env, current_const_name);
        if (!src) return std::nullopt;

        TypeParseResult parsed = Type::parse(e->type_name);
        if (parsed.has_error()) {
          return std::nullopt;
        }
        Type tgt = std::move(parsed.type);
        if (type_env) {
          tgt = type_env->resolve(tgt);
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

          std::optional<uint64_t> bound = eval_type_bound_u64_for_const_eval(
            bs->max_bytes, scope, symbols, ctx, diagnostics, type_env, e->range);
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
          eval_const_value(e->base, scope, symbols, ctx, diagnostics, type_env, current_const_name);
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
            *e->repeat_value, scope, symbols, ctx, diagnostics, type_env, current_const_name);
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
          auto v =
            eval_const_value(el, scope, symbols, ctx, diagnostics, type_env, current_const_name);
          if (!v) return std::nullopt;
          out->elements.push_back(*v);
        }
        return ConstValue{out};
      } else if constexpr (std::is_same_v<T, Box<VecMacroExpr>>) {
        return std::nullopt;
      }

      return std::nullopt;
    },
    expr);
}

}  // namespace bt_dsl
