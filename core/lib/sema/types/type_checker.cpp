// bt_dsl/sema/type_checker.cpp - Bidirectional type checker implementation
//
#include "bt_dsl/sema/types/type_checker.hpp"

#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/const_value.hpp"
#include "bt_dsl/sema/types/type_utils.hpp"

namespace bt_dsl
{

namespace
{

[[nodiscard]] constexpr PortDirection port_direction_or_default(
  const std::optional<PortDirection> & dir) noexcept
{
  return dir.value_or(PortDirection::In);
}

[[nodiscard]] bool contains_unknown(const Type * t) noexcept
{
  if (!t) return true;
  if (t->kind == TypeKind::Unknown) return true;

  if (t->is_nullable()) {
    return contains_unknown(t->base_type);
  }

  if (t->is_array()) {
    return contains_unknown(t->element_type);
  }

  return false;
}

[[nodiscard]] bool fits_integer_kind(int64_t v, TypeKind k) noexcept
{
  switch (k) {
    case TypeKind::Int8:
      return v >= static_cast<int64_t>(std::numeric_limits<int8_t>::min()) &&
             v <= static_cast<int64_t>(std::numeric_limits<int8_t>::max());
    case TypeKind::Int16:
      return v >= static_cast<int64_t>(std::numeric_limits<int16_t>::min()) &&
             v <= static_cast<int64_t>(std::numeric_limits<int16_t>::max());
    case TypeKind::Int32:
      return v >= static_cast<int64_t>(std::numeric_limits<int32_t>::min()) &&
             v <= static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    case TypeKind::Int64:
      return true;
    case TypeKind::UInt8:
      return v >= 0 && v <= static_cast<int64_t>(std::numeric_limits<uint8_t>::max());
    case TypeKind::UInt16:
      return v >= 0 && v <= static_cast<int64_t>(std::numeric_limits<uint16_t>::max());
    case TypeKind::UInt32:
      return v >= 0 && v <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max());
    case TypeKind::UInt64:
      // IntLiteralExpr stores int64_t, so any non-negative fits within uint64.
      return v >= 0;
    default:
      return false;
  }
}

[[nodiscard]] const Type * unify_inference_types(
  TypeContext & types, const Type * a, const Type * b)
{
  if (!a) return b;
  if (!b) return a;
  if (a == b) return a;
  if (a->is_error() || b->is_error()) return types.error_type();

  // Unknown acts as an inference placeholder.
  if (a->kind == TypeKind::Unknown) return b;
  if (b->kind == TypeKind::Unknown) return a;

  // Nullable unification. Note: we do NOT invent nullable-ness from a non-nullable variable;
  // callers should only reach here with nullable types when the declaration is nullable.
  if (a->is_nullable() || b->is_nullable()) {
    const bool a_nullable = a->is_nullable();
    const bool b_nullable = b->is_nullable();

    const Type * a_base = a_nullable ? a->base_type : a;
    const Type * b_base = b_nullable ? b->base_type : b;

    if (!a_base || !b_base) return types.error_type();

    const Type * base = unify_inference_types(types, a_base, b_base);
    if (!base || base->is_error()) return types.error_type();
    return types.get_nullable_type(base);
  }

  // Placeholder literals vs concrete (same category only).
  // Per spec §3.2.3.1: mixed int/float is always an error, even with literals.
  if (a->kind == TypeKind::IntegerLiteral && b->is_integer()) return b;
  if (b->kind == TypeKind::IntegerLiteral && a->is_integer()) return a;
  if (a->kind == TypeKind::FloatLiteral && b->is_float()) return b;
  if (b->kind == TypeKind::FloatLiteral && a->is_float()) return a;

  // Numeric widening within category. Mixed int/float is a conflict for inference.
  if (a->is_numeric() && b->is_numeric()) {
    const bool a_int = a->is_integer() || a->kind == TypeKind::IntegerLiteral;
    const bool b_int = b->is_integer() || b->kind == TypeKind::IntegerLiteral;
    const bool a_flt = a->is_float() || a->kind == TypeKind::FloatLiteral;
    const bool b_flt = b->is_float() || b->kind == TypeKind::FloatLiteral;

    // Per spec §3.2.3.1: "int と float が混在する場合、BT-DSL は厳密性を重視し、
    // 暗黙の共通型推論を行いません（明示的なキャストが必要です）"
    if ((a_int && b_flt) || (a_flt && b_int)) {
      return types.error_type();
    }

    const Type * common = common_numeric_type(types, a, b);
    return common ? common : types.error_type();
  }

  // Bool.
  if (a->kind == TypeKind::Bool && b->kind == TypeKind::Bool) return a;

  // Arrays / vec (dynamic array).
  if (a->is_array() && b->is_array()) {
    if (a->kind != b->kind) {
      return types.error_type();
    }

    if (!a->element_type || !b->element_type) {
      return types.error_type();
    }
    const Type * elem = unify_inference_types(types, a->element_type, b->element_type);
    if (!elem || elem->is_error()) return types.error_type();

    switch (a->kind) {
      case TypeKind::StaticArray:
        if (a->size != b->size) return types.error_type();
        return types.get_static_array_type(elem, a->size);
      case TypeKind::BoundedArray:
        // For inference, keep the declared bound (same kind) and unify element.
        if (a->size != b->size) return types.error_type();
        return types.get_bounded_array_type(elem, a->size);
      case TypeKind::DynamicArray:
        return types.get_dynamic_array_type(elem);
      default:
        return types.error_type();
    }
  }

  return types.error_type();
}

[[nodiscard]] const Symbol * base_lvalue_symbol(const Expr * expr) noexcept
{
  // For lvalue expressions, return the base variable symbol:
  // - VarRef -> that symbol
  // - IndexExpr -> recurse into base
  // Otherwise null.
  if (!expr) return nullptr;
  if (const auto * vr = dyn_cast<VarRefExpr>(expr)) {
    return vr->resolvedSymbol;
  }
  if (const auto * ix = dyn_cast<IndexExpr>(expr)) {
    return base_lvalue_symbol(ix->base);
  }
  return nullptr;
}

[[nodiscard]] const ParamDecl * writable_param_from_symbol(const Symbol * sym) noexcept
{
  if (!sym) return nullptr;
  if (!sym->is_parameter()) return nullptr;
  if (!sym->is_writable()) return nullptr;
  return dyn_cast<ParamDecl>(sym->astNode);
}

void collect_write_usages_in_stmt(const Stmt * stmt, std::unordered_set<const ParamDecl *> & out)
{
  if (!stmt) return;

  if (const auto * assign = dyn_cast<AssignmentStmt>(stmt)) {
    if (const ParamDecl * p = writable_param_from_symbol(assign->resolvedTarget)) {
      out.insert(p);
    }
    return;
  }

  if (const auto * node = dyn_cast<NodeStmt>(stmt)) {
    for (const auto * arg : node->args) {
      if (!arg) continue;
      if (arg->is_inline_decl()) continue;

      const PortDirection arg_dir = arg->direction.value_or(PortDirection::In);
      if (arg_dir != PortDirection::Mut && arg_dir != PortDirection::Out) {
        continue;
      }

      if (const ParamDecl * p = writable_param_from_symbol(base_lvalue_symbol(arg->valueExpr))) {
        out.insert(p);
      }
    }

    for (const auto * child : node->children) {
      collect_write_usages_in_stmt(child, out);
    }
    return;
  }
}

void warn_unused_write_params(const TreeDecl * tree, DiagnosticBag * diags)
{
  if (!tree || !diags) return;

  // Collect mut/out params (write-capable).
  std::unordered_set<const ParamDecl *> writable;
  writable.reserve(tree->params.size());
  for (const auto * param : tree->params) {
    if (!param) continue;
    const PortDirection dir = param->direction.value_or(PortDirection::In);
    if (dir == PortDirection::Mut || dir == PortDirection::Out) {
      writable.insert(param);
    }
  }

  if (writable.empty()) return;

  // Collect actual write usages.
  std::unordered_set<const ParamDecl *> used;
  used.reserve(writable.size());
  for (const auto * stmt : tree->body) {
    collect_write_usages_in_stmt(stmt, used);
  }

  for (const auto * param : tree->params) {
    if (!param) continue;
    if (writable.count(param) && !used.count(param)) {
      diags->report_warning(
        param->get_range(), std::string("Parameter '") + std::string(param->name) +
                              "' is declared as mut/out but never used for write access");
    }
  }
}

[[nodiscard]] bool is_lvalue_expr(const Expr * expr) noexcept
{
  if (!expr) return false;
  // Spec 6.4.3 (and reference impl): only identifiers and index expressions are lvalues.
  return expr->get_kind() == NodeKind::VarRef || expr->get_kind() == NodeKind::IndexExpr;
}

enum class DirDiagKind { Ok, Warning, Error };

[[nodiscard]] DirDiagKind check_dir_matrix(PortDirection arg_dir, PortDirection port_dir) noexcept
{
  // Mirror core/ reference semantics matrix used by Analyzer::validate_arguments.
  switch (arg_dir) {
    case PortDirection::In:
      return (port_dir == PortDirection::In) ? DirDiagKind::Ok : DirDiagKind::Error;
    case PortDirection::Ref:
      if (port_dir == PortDirection::Ref) return DirDiagKind::Ok;
      if (port_dir == PortDirection::In) return DirDiagKind::Warning;
      return DirDiagKind::Error;  // ref -> mut/out invalid
    case PortDirection::Mut:
      if (port_dir == PortDirection::Mut) return DirDiagKind::Ok;
      if (port_dir == PortDirection::In || port_dir == PortDirection::Ref)
        return DirDiagKind::Warning;
      return DirDiagKind::Error;  // mut -> out invalid
    case PortDirection::Out:
      return (port_dir == PortDirection::Out) ? DirDiagKind::Ok : DirDiagKind::Error;
  }
  return DirDiagKind::Error;
}

[[nodiscard]] const ExternPort * find_extern_port(const ExternDecl * decl, std::string_view name)
{
  if (!decl) return nullptr;
  for (const auto * p : decl->ports) {
    if (p && p->name == name) return p;
  }
  return nullptr;
}

[[nodiscard]] const ParamDecl * find_param(const TreeDecl * decl, std::string_view name)
{
  if (!decl) return nullptr;
  for (const auto * p : decl->params) {
    if (p && p->name == name) return p;
  }
  return nullptr;
}

[[nodiscard]] bool port_requires_lvalue(PortDirection d) noexcept
{
  return d == PortDirection::Ref || d == PortDirection::Mut || d == PortDirection::Out;
}

[[nodiscard]] bool is_assignable_for_port_check(const Type * target, const Type * source) noexcept
{
  if (!target || !source) return false;

  // Preserve error recovery behavior.
  if (target->is_error() || source->is_error()) return true;

  // The generic is_assignable() is intentionally permissive for placeholders.
  // For port validation we want the reference-like behavior:
  // - {integer} only matches integer/float
  // - {float} only matches float
  // - {null} only matches nullable
  // - ? remains permissive
  switch (source->kind) {
    case TypeKind::IntegerLiteral:
      return target->is_integer() || target->is_float();
    case TypeKind::FloatLiteral:
      return target->is_float();
    case TypeKind::NullLiteral:
      return target->is_nullable();
    case TypeKind::Unknown:
      return true;
    default:
      break;
  }

  // Null-safety is enforced in a later analysis pass (NullChecker).
  // For port validation we allow passing a nullable value (T?) to a non-nullable port (T)
  // and let the null-safety analysis reject unguarded uses.
  if (!target->is_nullable() && source->is_nullable() && source->base_type) {
    // Exact base-type match required here; widening is handled separately.
    if (is_assignable(target, source->base_type)) return true;
  }

  return is_assignable(target, source);
}

}  // namespace

// ============================================================================
// Constructor
// ============================================================================

TypeChecker::TypeChecker(
  TypeContext & types, const TypeTable & typeTable, const SymbolTable & values,
  DiagnosticBag * diags)
: types_(types), type_table_(typeTable), values_(values), diags_(diags)
{
}

// ============================================================================
// Entry Points
// ============================================================================

bool TypeChecker::check(Program & program)
{
  // Validate type alias definitions even if unused.
  // Spec: circular type alias definitions are prohibited.
  for (auto * alias : program.type_aliases()) {
    if (!alias || !alias->aliasedType) continue;
    (void)resolve_type(alias->aliasedType);
  }

  // Check global variable declarations
  for (auto * decl : program.global_vars()) {
    check_global_var_decl(decl);
  }

  // Check global const declarations (types should already be set by ConstEvaluator)
  for (auto * decl : program.global_consts()) {
    check_global_const_decl(decl);
  }

  // Check tree declarations
  for (auto * tree : program.trees()) {
    check_tree_decl(tree);
  }

  return !has_errors_;
}

const Type * TypeChecker::check_expr(Expr * expr)
{
  return check_expr_with_expected(expr, nullptr);
}

const Type * TypeChecker::check_expr_with_expected(Expr * expr, const Type * expected)
{
  if (!expr) return types_.error_type();

  const Type * result = nullptr;

  switch (expr->get_kind()) {
    case NodeKind::IntLiteral:
      result = infer_int_literal(cast<IntLiteralExpr>(expr), expected);
      break;
    case NodeKind::FloatLiteral:
      result = infer_float_literal(cast<FloatLiteralExpr>(expr), expected);
      break;
    case NodeKind::StringLiteral:
      result = infer_string_literal(cast<StringLiteralExpr>(expr), expected);
      break;
    case NodeKind::BoolLiteral:
      result = infer_bool_literal(cast<BoolLiteralExpr>(expr));
      break;
    case NodeKind::NullLiteral:
      result = infer_null_literal(cast<NullLiteralExpr>(expr), expected);
      break;
    case NodeKind::VarRef:
      result = infer_var_ref(cast<VarRefExpr>(expr));
      break;
    case NodeKind::BinaryExpr:
      result = infer_binary_expr(cast<BinaryExpr>(expr), expected);
      break;
    case NodeKind::UnaryExpr:
      result = infer_unary_expr(cast<UnaryExpr>(expr), expected);
      break;
    case NodeKind::CastExpr:
      result = infer_cast_expr(cast<CastExpr>(expr), expected);
      break;
    case NodeKind::IndexExpr:
      result = infer_index_expr(cast<IndexExpr>(expr));
      break;
    case NodeKind::ArrayLiteralExpr:
      result = infer_array_literal(cast<ArrayLiteralExpr>(expr), expected);
      break;
    case NodeKind::ArrayRepeatExpr:
      result = infer_array_repeat(cast<ArrayRepeatExpr>(expr), expected);
      break;
    case NodeKind::VecMacroExpr:
      result = infer_vec_macro(cast<VecMacroExpr>(expr), expected);
      break;
    case NodeKind::MissingExpr:
      result = types_.error_type();
      break;
    default:
      result = types_.error_type();
  }

  // Set the resolved type on the expression
  // Note: expr is already non-const, no cast needed
  expr->resolvedType = result;
  return result;
}

// ============================================================================
// Expression Type Inference
// ============================================================================

const Type * TypeChecker::infer_int_literal(IntLiteralExpr * node, const Type * expected)
{
  // If we have an expected numeric type, resolve to it and enforce integer bounds.
  if (expected) {
    if (expected->is_integer() && expected->kind != TypeKind::IntegerLiteral) {
      if (!fits_integer_kind(node->value, expected->kind)) {
        report_error(node->get_range(), "integer literal is out of range for the expected type");
        return types_.error_type();
      }
      return expected;
    }
    if (expected->is_float()) {
      return expected;
    }
  }

  // Return placeholder type - will be defaulted to int32 later
  return types_.integer_literal_type();
}

const Type * TypeChecker::infer_float_literal(FloatLiteralExpr * /*node*/, const Type * expected)
{
  if (expected && expected->is_float()) {
    return expected;
  }
  // Return placeholder type - will be defaulted to float64 later
  return types_.float_literal_type();
}

const Type * TypeChecker::infer_string_literal(StringLiteralExpr * node, const Type * expected)
{
  // String literals can be contextually typed to bounded strings.
  // Reference: docs/reference/type-system.md §3.1.4 (文字列)
  // Reference: docs/reference/type-system.md §3.6 (型推論)
  if (expected && expected->kind == TypeKind::BoundedString) {
    const auto len_bytes = static_cast<uint64_t>(node->value.size());
    if (len_bytes > expected->size) {
      report_error(node->get_range(), "string literal exceeds bounded string size");
      return types_.error_type();
    }

    return expected;
  }
  return types_.string_type();
}

const Type * TypeChecker::infer_bool_literal(BoolLiteralExpr * /*node*/)
{
  return types_.bool_type();
}

const Type * TypeChecker::infer_null_literal(NullLiteralExpr * node, const Type * expected)
{
  if (expected && expected->is_nullable()) {
    return expected;
  }

  // Null without a nullable context is an error.
  // (E.g. `const X = null;` MUST FAIL per reference tests.)
  report_error(node->get_range(), "cannot infer type of null literal without a nullable context");
  return types_.error_type();
}

const Type * TypeChecker::infer_var_ref(VarRefExpr * node)
{
  if (!node->resolvedSymbol) {
    // NameResolver didn't find this - already an error
    return types_.error_type();
  }

  if (const auto it = value_type_overrides_.find(node->resolvedSymbol);
      it != value_type_overrides_.end()) {
    return it->second;
  }

  return get_symbol_type(node->resolvedSymbol);
}

const Type * TypeChecker::infer_binary_expr(BinaryExpr * node, const Type * /*expected*/)
{
  // Equality with null literal needs contextual typing: infer null from the other operand.
  // Without this, expressions like `x != null` would fail type checking even when x is nullable.
  if (node->op == BinaryOp::Eq || node->op == BinaryOp::Ne) {
    const bool lhs_is_null = node->lhs && node->lhs->get_kind() == NodeKind::NullLiteral;
    const bool rhs_is_null = node->rhs && node->rhs->get_kind() == NodeKind::NullLiteral;
    if (lhs_is_null ^ rhs_is_null) {
      Expr * non_null_expr = lhs_is_null ? node->rhs : node->lhs;
      Expr * null_expr = lhs_is_null ? node->lhs : node->rhs;

      const Type * non_null_t = check_expr(non_null_expr);
      if (!non_null_t || non_null_t->is_error()) {
        return types_.error_type();
      }

      if (!non_null_t->is_nullable() && !non_null_t->is_placeholder()) {
        report_error(null_expr->get_range(), "null can only be compared with nullable types");
        null_expr->resolvedType = types_.null_literal_type();
      } else {
        (void)check_expr_with_expected(null_expr, non_null_t);
      }
      return types_.bool_type();
    }
  }

  // Infer LHS first. RHS may need flow-sensitive narrowing (nullable) under short-circuit ops.
  const Type * lhs_type = check_expr(node->lhs);
  const Type * rhs_type = nullptr;

  struct ScopedOverride
  {
    TypeChecker & self;
    const Symbol * sym = nullptr;
    bool active = false;

    ScopedOverride(TypeChecker & s, const Symbol * symbol, const Type * ty) : self(s), sym(symbol)
    {
      if (sym && ty) {
        self.value_type_overrides_.insert_or_assign(sym, ty);
        active = true;
      }
    }

    ~ScopedOverride()
    {
      if (active) {
        (void)self.value_type_overrides_.erase(sym);
      }
    }

    ScopedOverride(const ScopedOverride &) = delete;
    ScopedOverride & operator=(const ScopedOverride &) = delete;
  };

  auto try_extract_null_check = [](Expr * e, const Symbol *& out_sym, BinaryOp & out_op) -> bool {
    if (!e || e->get_kind() != NodeKind::BinaryExpr) {
      return false;
    }
    auto * be = static_cast<BinaryExpr *>(e);
    if (be->op != BinaryOp::Eq && be->op != BinaryOp::Ne) {
      return false;
    }
    if (!be->lhs || !be->rhs) {
      return false;
    }
    const bool lhs_is_null = be->lhs->get_kind() == NodeKind::NullLiteral;
    const bool rhs_is_null = be->rhs->get_kind() == NodeKind::NullLiteral;
    if (!(lhs_is_null ^ rhs_is_null)) {
      return false;
    }

    Expr * non_null_expr = lhs_is_null ? be->rhs : be->lhs;
    if (non_null_expr->get_kind() != NodeKind::VarRef) {
      return false;
    }
    auto * vr = static_cast<VarRefExpr *>(non_null_expr);
    if (!vr->resolvedSymbol) {
      return false;
    }
    out_sym = vr->resolvedSymbol;
    out_op = be->op;
    return true;
  };

  switch (node->op) {
    // Logical operations (short-circuit) with nullable narrowing
    case BinaryOp::And:
    case BinaryOp::Or: {
      if (lhs_type->kind != TypeKind::Bool) {
        report_error(node->lhs->get_range(), "logical operation requires bool operands");
        return types_.error_type();
      }

      const Symbol * narrowed_sym = nullptr;
      BinaryOp null_check_op = BinaryOp::Eq;
      (void)try_extract_null_check(node->lhs, narrowed_sym, null_check_op);

      const Type * override_type = nullptr;
      if (narrowed_sym) {
        const Type * original = get_symbol_type(narrowed_sym);
        if (original && original->is_nullable() && original->base_type) {
          // (x != null && rhs)  => rhs sees x as non-null
          // (x == null || rhs)  => rhs sees x as non-null (since rhs runs only when lhs is false)
          const bool should_narrow = (node->op == BinaryOp::And && null_check_op == BinaryOp::Ne) ||
                                     (node->op == BinaryOp::Or && null_check_op == BinaryOp::Eq);

          if (should_narrow) {
            override_type = original->base_type;
          }
        }
      }

      const ScopedOverride scoped(*this, narrowed_sym, override_type);
      rhs_type = check_expr(node->rhs);

      if (rhs_type->kind != TypeKind::Bool) {
        report_error(node->rhs->get_range(), "logical operation requires bool operands");
        return types_.error_type();
      }
      return types_.bool_type();
    }

    default:
      break;
  }

  // For all non short-circuit ops, infer RHS normally.
  if (!rhs_type) {
    rhs_type = check_expr(node->rhs);
  }

  switch (node->op) {
    // Arithmetic operations
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div: {
      // String concatenation special case
      if (node->op == BinaryOp::Add && lhs_type->is_string() && rhs_type->is_string()) {
        return types_.string_type();
      }
      const Type * common = common_numeric_type(types_, lhs_type, rhs_type);
      if (!common) {
        report_error(
          node->get_range(), std::string("incompatible operand types for arithmetic operation: '") +
                               to_string(lhs_type) + "' and '" + to_string(rhs_type) + "'");
        return types_.error_type();
      }
      return common;
    }

    case BinaryOp::Mod: {
      // Modulo requires integer operands
      if (!lhs_type->is_integer() && lhs_type->kind != TypeKind::IntegerLiteral) {
        report_error(node->lhs->get_range(), "modulo requires integer operands");
        return types_.error_type();
      }
      if (!rhs_type->is_integer() && rhs_type->kind != TypeKind::IntegerLiteral) {
        report_error(node->rhs->get_range(), "modulo requires integer operands");
        return types_.error_type();
      }
      return common_numeric_type(types_, lhs_type, rhs_type);
    }

    // Comparison operations - result is always bool
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge: {
      if (!lhs_type->is_numeric() && !lhs_type->is_placeholder()) {
        report_error(node->lhs->get_range(), "comparison requires numeric operands");
        return types_.error_type();
      }
      if (!rhs_type->is_numeric() && !rhs_type->is_placeholder()) {
        report_error(node->rhs->get_range(), "comparison requires numeric operands");
        return types_.error_type();
      }
      return types_.bool_type();
    }

    // Equality operations
    case BinaryOp::Eq:
    case BinaryOp::Ne: {
      if (!are_comparable(lhs_type, rhs_type)) {
        report_error(node->get_range(), "operands are not comparable");
        return types_.error_type();
      }
      return types_.bool_type();
    }

    // Logical operations
    case BinaryOp::And:
    case BinaryOp::Or:
      // handled earlier
      return types_.bool_type();

    // Bitwise operations
    case BinaryOp::BitAnd:
    case BinaryOp::BitXor:
    case BinaryOp::BitOr: {
      if (!lhs_type->is_integer() && lhs_type->kind != TypeKind::IntegerLiteral) {
        report_error(node->lhs->get_range(), "bitwise operation requires integer operands");
        return types_.error_type();
      }
      if (!rhs_type->is_integer() && rhs_type->kind != TypeKind::IntegerLiteral) {
        report_error(node->rhs->get_range(), "bitwise operation requires integer operands");
        return types_.error_type();
      }
      return common_numeric_type(types_, lhs_type, rhs_type);
    }

    default:
      return types_.error_type();
  }
}

const Type * TypeChecker::infer_unary_expr(UnaryExpr * node, const Type * expected)
{
  const Type * operand_type = check_expr_with_expected(node->operand, expected);

  switch (node->op) {
    case UnaryOp::Neg: {
      // Negation applies to numeric types
      if (!operand_type->is_numeric() && !operand_type->is_placeholder()) {
        report_error(node->operand->get_range(), "negation requires numeric operand");
        return types_.error_type();
      }
      return operand_type;
    }

    case UnaryOp::Not: {
      // Logical not requires bool
      if (operand_type->kind != TypeKind::Bool) {
        report_error(node->operand->get_range(), "logical not requires bool operand");
        return types_.error_type();
      }
      return types_.bool_type();
    }

    default:
      return types_.error_type();
  }
}

const Type * TypeChecker::infer_cast_expr(CastExpr * node, const Type * expected)
{
  // Type check the source expression
  check_expr(node->expr);

  // Resolve the target type
  const Type * target_type = resolve_type(node->targetType);
  if (!target_type || target_type->is_error()) {
    return types_.error_type();
  }

  // Contextual typing for wildcard casts, e.g. `X as vec<_>` in a `vec<int32>` context.
  if (expected && contains_unknown(target_type) && !expected->is_error()) {
    const Type * specialized = unify_inference_types(types_, target_type, expected);
    if (specialized && !specialized->is_error()) {
      target_type = specialized;
    }
  }

  // For now, allow all casts - validation of cast compatibility
  // would go here if needed
  return target_type;
}

const Type * TypeChecker::infer_index_expr(IndexExpr * node)
{
  const Type * base_type = check_expr(node->base);
  const Type * index_type = check_expr(node->index);

  // Index must be integer
  if (!index_type->is_integer() && index_type->kind != TypeKind::IntegerLiteral) {
    report_error(node->index->get_range(), "array index must be integer type");
  }

  // Base must be array type
  if (!base_type->is_array()) {
    if (!base_type->is_error()) {
      report_error(node->base->get_range(), "subscripted value is not an array");
    }
    return types_.error_type();
  }

  // Return element type
  return base_type->element_type;
}

const Type * TypeChecker::infer_array_literal(ArrayLiteralExpr * node, const Type * expected)
{
  if (node->elements.empty()) {
    // Empty array - need expected type to determine element type
    if (expected && expected->is_array()) {
      return expected;
    }
    report_error(node->get_range(), "cannot infer type of empty array literal");
    return types_.error_type();
  }

  // Determine expected element type from context
  const Type * expected_elem = nullptr;
  if (expected && expected->is_array()) {
    expected_elem = expected->element_type;
  }

  // Type check all elements
  const Type * elem_type = nullptr;
  for (auto * elem : node->elements) {
    const Type * t = check_expr_with_expected(elem, expected_elem);
    if (!elem_type) {
      elem_type = t;
    } else {
      // Ensure all elements are compatible
      const Type * common = common_numeric_type(types_, elem_type, t);
      if (common) {
        elem_type = common;
      } else if (!is_assignable(elem_type, t) && !is_assignable(t, elem_type)) {
        report_error(elem->get_range(), "array element type mismatch");
        return types_.error_type();
      }
    }
  }

  // Create static array type [T; N]
  return types_.get_static_array_type(apply_defaults(elem_type), node->elements.size());
}

const Type * TypeChecker::infer_array_repeat(ArrayRepeatExpr * node, const Type * expected)
{
  // Determine expected element type from context
  const Type * expected_elem = nullptr;
  if (expected && expected->is_array()) {
    expected_elem = expected->element_type;
  }

  const Type * value_type = check_expr_with_expected(node->value, expected_elem);

  // Spec: [e; N] requires N be a const_expr (non-negative integer).
  // We intentionally re-use ConstEvaluator here since expression nodes do not
  // carry evaluated const values.
  AstContext tmp_ast;
  ConstEvaluator eval(tmp_ast, types_, values_, nullptr);
  std::optional<uint64_t> n = eval.evaluate_array_size(node->count, node->count->get_range());

  if (n.has_value()) {
    return types_.get_static_array_type(apply_defaults(value_type), *n);
  }

  report_error(
    node->count->get_range(), "array repeat count must be a non-negative integer constant");

  // Error recovery: keep going with a placeholder size to avoid cascaded errors.
  return types_.get_static_array_type(apply_defaults(value_type), 0);
}

const Type * TypeChecker::infer_vec_macro(VecMacroExpr * node, const Type * expected)
{
  // Determine expected element type from context
  const Type * expected_elem = nullptr;
  if (expected && expected->kind == TypeKind::DynamicArray) {
    expected_elem = expected->element_type;
  }

  // Check the inner array expression (ignore expectedElem for inner, as it will be array)
  const Type * inner_type = check_expr_with_expected(node->inner, nullptr);
  (void)expected_elem;  // Suppress unused warning

  // Result is a dynamic array
  if (inner_type->is_array() && inner_type->element_type) {
    return types_.get_dynamic_array_type(apply_defaults(inner_type->element_type));
  }

  report_error(node->get_range(), "vec! requires an array expression");
  return types_.error_type();
}

// ============================================================================
// Statement/Declaration Processing
// ============================================================================

void TypeChecker::check_stmt(Stmt * stmt)
{
  if (!stmt) return;

  switch (stmt->get_kind()) {
    case NodeKind::NodeStmt:
      check_node_stmt(cast<NodeStmt>(stmt));
      break;
    case NodeKind::AssignmentStmt:
      check_assignment_stmt(cast<AssignmentStmt>(stmt));
      break;
    case NodeKind::BlackboardDeclStmt:
      check_blackboard_decl_stmt(cast<BlackboardDeclStmt>(stmt));
      break;
    case NodeKind::ConstDeclStmt:
      check_const_decl_stmt(cast<ConstDeclStmt>(stmt));
      break;
    default:
      break;
  }
}

void TypeChecker::check_node_stmt(NodeStmt * node)
{
  // Check preconditions
  for (auto * pre : node->preconditions) {
    const Type * cond_type = check_expr_with_expected(pre->condition, types_.bool_type());
    if (cond_type->kind != TypeKind::Bool && !cond_type->is_error()) {
      report_error(pre->condition->get_range(), "precondition must be boolean");
    }
  }

  // Check arguments
  for (auto * arg : node->args) {
    if (!arg) continue;

    // Look up the port/param signature from the resolved node definition.
    const Type * expected_type = nullptr;
    bool has_signature = false;
    PortDirection port_dir = PortDirection::In;

    if (node->resolvedNode && node->resolvedNode->decl) {
      const AstNode * decl = node->resolvedNode->decl;
      if (const auto * ext = dyn_cast<ExternDecl>(decl)) {
        if (const ExternPort * p = find_extern_port(ext, arg->name)) {
          has_signature = true;
          port_dir = p->direction.value_or(PortDirection::In);
          expected_type = resolve_type(p->type);
        }
      } else if (const auto * tree = dyn_cast<TreeDecl>(decl)) {
        if (const ParamDecl * p = find_param(tree, arg->name)) {
          has_signature = true;
          port_dir = p->direction.value_or(PortDirection::In);
          expected_type = resolve_type(p->type);
        }
      }
    }

    // If we have a signature but the port name is unknown, report it.
    if (node->resolvedNode && node->resolvedNode->decl && !has_signature) {
      report_error(
        arg->get_range(), std::string("Unknown port '") + std::string(arg->name) + "' for node '" +
                            std::string(node->nodeName) + "'");
      // Still type-check the expression for better error recovery.
      if (arg->valueExpr) check_expr(arg->valueExpr);
      continue;
    }

    const PortDirection arg_dir = arg->direction.value_or(PortDirection::In);

    // Inline out-var decls: they don't have a valueExpr.
    if (arg->is_inline_decl()) {
      // Keep behavior close to reference: inline decl is only for out ports.
      if (has_signature && port_dir != PortDirection::Out) {
        report_error(
          arg->inlineDecl->get_range(),
          std::string("Inline declaration is only allowed for 'out' ports (port '") +
            std::string(arg->name) + "' is not out)");
      }
      // Direction marker is implicitly out for inline decl.
      if (arg_dir != PortDirection::Out) {
        report_error(arg->inlineDecl->get_range(), "Inline declaration requires 'out' direction");
      }
      continue;
    }

    if (!arg->valueExpr) continue;

    // Spec 6.4.3: explicit ref/mut/out argument markers require an lvalue.
    if (arg_dir != PortDirection::In && !is_lvalue_expr(arg->valueExpr)) {
      report_error(
        arg->get_range(), std::string("Direction '") + std::string(to_string(arg_dir)) +
                            "' requires an lvalue argument");
    }

    // Spec 6.4.3: ref/mut/out ports require an lvalue argument.
    if (has_signature && port_requires_lvalue(port_dir) && !is_lvalue_expr(arg->valueExpr)) {
      report_error(
        arg->get_range(),
        std::string("Port '") + std::string(arg->name) + "' requires an lvalue argument");
      // Don't attempt type/direction checks further; expression isn't a legal argument here.
      continue;
    }

    // Spec 6.4.4: additional restrictions when the argument refers to a tree parameter.
    // - in/ref params cannot be written (mut/out)
    // - out params cannot be read as ref
    if (const Symbol * base_sym = base_lvalue_symbol(arg->valueExpr)) {
      if (base_sym->is_parameter()) {
        const PortDirection param_dir = base_sym->direction.value_or(PortDirection::In);
        if (
          (arg_dir == PortDirection::Mut || arg_dir == PortDirection::Out) &&
          (param_dir == PortDirection::In || param_dir == PortDirection::Ref)) {
          report_error(
            arg->get_range(), std::string("Tree parameter '") + std::string(base_sym->name) +
                                "' cannot be passed with '" + std::string(to_string(arg_dir)) +
                                "' direction");
        }
        if (arg_dir == PortDirection::Ref && param_dir == PortDirection::Out) {
          report_error(
            arg->get_range(), std::string("Tree parameter '") + std::string(base_sym->name) +
                                "' declared as 'out' cannot be passed with 'ref' direction");
        }
      }
    }

    // Spec 6.4.2: direction consistency (reference matrix).
    bool dir_mismatch_error = false;
    if (has_signature) {
      const DirDiagKind m = check_dir_matrix(arg_dir, port_dir);
      if (m == DirDiagKind::Error) {
        report_error(
          arg->get_range(), std::string("Direction mismatch: port '") + std::string(arg->name) +
                              "' is '" + std::string(to_string(port_dir)) + "' but argument is '" +
                              std::string(to_string(arg_dir)) + "'.");
        dir_mismatch_error = true;
      } else if (m == DirDiagKind::Warning) {
        if (diags_) {
          diags_->report_warning(
            arg->get_range(), std::string("Direction is more permissive than required: port '") +
                                std::string(arg->name) + "' is '" +
                                std::string(to_string(port_dir)) + "' but argument is '" +
                                std::string(to_string(arg_dir)) + "'.");
        }
      }
    }

    if (dir_mismatch_error) {
      // Direction issues are typically the primary root-cause; avoid piling on type mismatch
      // noise for the same argument.
      check_expr(arg->valueExpr);
      continue;
    }

    // Spec 6.4.1: use the port signature type as expected type where meaningful.
    // - In ports: expression is checked against port type (top-down typing for literals)
    // - Out ports: lvalue target must be able to accept port type
    // - Ref/Mut: require exact type match
    if (has_signature && expected_type && !expected_type->is_error()) {
      if (port_dir == PortDirection::In) {
        const Type * expr_type = check_expr_with_expected(arg->valueExpr, expected_type);
        if (!is_assignable_for_port_check(expected_type, expr_type)) {
          report_error(
            arg->get_range(), std::string("Type mismatch: cannot assign '") + to_string(expr_type) +
                                "' to port '" + std::string(arg->name) + "' of type '" +
                                to_string(expected_type) + "'");
        }
      } else {
        // For out/ref/mut, the signature can constrain the lvalue variable type.
        if (const Symbol * base_sym = base_lvalue_symbol(arg->valueExpr)) {
          if (is_inferable_var_symbol(base_sym)) {
            const Type * cur = get_symbol_type(base_sym);
            if (!cur || cur->is_error() || contains_unknown(cur)) {
              constrain_var_type(base_sym, expected_type, arg->get_range());
            }
          }
        }

        const Type * lv_type = check_expr(arg->valueExpr);
        if (!lv_type->is_error()) {
          bool ok = true;
          switch (port_dir) {
            case PortDirection::Out:
              ok = is_assignable_for_port_check(lv_type, expected_type);
              break;
            case PortDirection::Ref:
            case PortDirection::Mut:
              ok = (lv_type == expected_type);

              // Allow T? lvalue to bind to ref/mut T (null-safety enforced later).
              if (
                !ok && !expected_type->is_nullable() && lv_type->is_nullable() &&
                lv_type->base_type == expected_type) {
                ok = true;
              }
              break;
            case PortDirection::In:
              ok = true;
              break;
          }
          if (!ok) {
            report_error(
              arg->get_range(), std::string("Type mismatch: argument of type '") +
                                  to_string(lv_type) + "' is incompatible with port '" +
                                  std::string(arg->name) + "' of type '" +
                                  to_string(expected_type) + "'");
          }
        }
      }
    } else {
      // No signature/type: still type-check the expression.
      check_expr(arg->valueExpr);
    }
  }

  // Check for missing required ports/parameters
  if (node->resolvedNode && node->resolvedNode->decl) {
    const AstNode * decl = node->resolvedNode->decl;

    // Gather provided argument names
    std::unordered_set<std::string_view> provided_args;
    provided_args.reserve(node->args.size());
    for (const auto * arg : node->args) {
      if (arg) provided_args.insert(arg->name);
    }

    auto check_missing = [&](
                           std::string_view p_name, PortDirection dir, const Expr * default_val,
                           std::string_view kind) {
      if (provided_args.find(p_name) != provided_args.end()) {
        return;
      }

      // Spec 6.4.5 (Argument omission rules)
      // - in: optional iff it has a default
      // - out: always optional (result discarded)
      // - ref/mut: always required
      const bool required = [&]() {
        switch (dir) {
          case PortDirection::In:
            return default_val == nullptr;
          case PortDirection::Out:
            return false;
          case PortDirection::Ref:
          case PortDirection::Mut:
            return true;
        }
        return true;
      }();

      if (required) {
        report_error(
          node->get_range(),
          std::string("missing required ") + std::string(kind) + " '" + std::string(p_name) + "'");
      }
    };

    if (const auto * ext = dyn_cast<ExternDecl>(decl)) {
      for (const auto * p : ext->ports) {
        if (!p) continue;
        const PortDirection dir = port_direction_or_default(p->direction);
        check_missing(p->name, dir, p->defaultValue, "port");
      }
    } else if (const auto * tree = dyn_cast<TreeDecl>(decl)) {
      for (const auto * p : tree->params) {
        if (!p) continue;
        const PortDirection dir = port_direction_or_default(p->direction);
        check_missing(p->name, dir, p->defaultValue, "parameter");
      }
    }
  }

  // Recursively check children
  const Scope * prev_scope = current_scope_;
  if (node->resolvedBlockScope) {
    current_scope_ = node->resolvedBlockScope;
  }

  for (auto * child : node->children) {
    check_stmt(child);
  }

  current_scope_ = prev_scope;
}

void TypeChecker::check_assignment_stmt(AssignmentStmt * node)
{
  // Check preconditions
  for (auto * pre : node->preconditions) {
    const Type * cond_type = check_expr_with_expected(pre->condition, types_.bool_type());
    if (cond_type->kind != TypeKind::Bool && !cond_type->is_error()) {
      report_error(pre->condition->get_range(), "precondition must be boolean");
    }
  }

  // Get target type
  const Symbol * target_sym = node->resolvedTarget;
  const Type * target_type = target_sym ? get_symbol_type(target_sym) : nullptr;

  // Check index expressions if any
  for (auto * idx : node->indices) {
    const Type * idx_type = check_expr(idx);
    if (!idx_type->is_integer() && idx_type->kind != TypeKind::IntegerLiteral) {
      report_error(idx->get_range(), "array index must be integer");
    }
    // Update target type to element type
    if (target_type && target_type->is_array()) {
      target_type = target_type->element_type;
    }
  }

  // Special case: x = y where both are inferable vars -> unify equivalence.
  if (target_sym && node->value && node->value->get_kind() == NodeKind::VarRef) {
    auto * vr = static_cast<VarRefExpr *>(node->value);
    if (
      vr->resolvedSymbol && is_inferable_var_symbol(target_sym) &&
      is_inferable_var_symbol(vr->resolvedSymbol)) {
      unify_infer_vars(target_sym, vr->resolvedSymbol, node->get_range());
    }
  }

  // Prefer not to push an Unknown expected type down into literal inference.
  const Type * expected_for_rhs =
    (target_type && !contains_unknown(target_type)) ? target_type : nullptr;

  // Check value expression with expected type
  const Type * value_type = check_expr_with_expected(node->value, expected_for_rhs);

  // Constrain/infer target variable type from RHS.
  if (target_sym && is_inferable_var_symbol(target_sym)) {
    constrain_var_type(
      target_sym, value_type, node->value ? node->value->get_range() : node->get_range());
    target_type = get_symbol_type(target_sym);
  }

  // Verify assignment compatibility
  if (target_type && value_type && !is_assignable(target_type, value_type)) {
    const SourceRange rhs_range = node->value ? node->value->get_range() : node->get_range();
    report_error(
      rhs_range, std::string("type mismatch in assignment: cannot assign '") +
                   to_string(value_type) + "' to '" + to_string(target_type) + "'");
  }
}

void TypeChecker::check_blackboard_decl_stmt(BlackboardDeclStmt * node)
{
  const Symbol * sym = (current_scope_ ? current_scope_->lookup_local(node->name) : nullptr);

  // Get declared type if any
  const Type * declared_type = nullptr;
  if (node->type) {
    declared_type = resolve_type(node->type);
  }

  // Check initializer
  if (node->initialValue) {
    const Type * init_type = check_expr_with_expected(node->initialValue, declared_type);

    const bool declared_is_infer = declared_type && contains_unknown(declared_type);

    // If the declared type contains inference placeholders (e.g. vec<_>, [_; N]),
    // infer from the initializer first, then validate.
    if (sym && is_inferable_var_symbol(sym) && declared_is_infer) {
      constrain_var_type(sym, init_type, node->initialValue->get_range());
      declared_type = get_symbol_type(sym);
    }

    if (declared_type && !declared_is_infer && !is_assignable(declared_type, init_type)) {
      report_error(
        node->initialValue->get_range(),
        std::string("initializer type mismatch: cannot initialize '") + to_string(declared_type) +
          "' with '" + to_string(init_type) + "'");
    }

    // var inference: no declared type or wildcard declared type.
    if (sym && is_inferable_var_symbol(sym)) {
      // Prefer constraining with the declared type when it exists (e.g. _?, _).
      const Type * constraint = declared_type ? declared_type : init_type;
      constrain_var_type(sym, constraint, node->initialValue->get_range());
    }
  } else {
    // Local var without initializer: must be inferred from later usage.
    if (sym && is_inferable_var_symbol(sym)) {
      const Type * initial = declared_type ? declared_type : types_.unknown_type();
      constrain_var_type(sym, initial, node->get_range());
    }
  }
}

void TypeChecker::check_const_decl_stmt(ConstDeclStmt * node)
{
  // Get declared type if any
  const Type * declared_type = nullptr;
  if (node->type) {
    declared_type = resolve_type(node->type);
  }

  // Check value expression
  const Type * value_type = check_expr_with_expected(node->value, declared_type);

  // Apply defaults to placeholder types
  if (value_type->is_placeholder()) {
    value_type = apply_defaults(value_type);
  }

  if (declared_type && !is_assignable(declared_type, value_type)) {
    report_error(
      node->value->get_range(),
      std::string("const initializer type mismatch: cannot initialize '") +
        to_string(declared_type) + "' with '" + to_string(value_type) + "'");
  }
}

void TypeChecker::check_tree_decl(TreeDecl * decl)
{
  // Reset per-tree inference state.
  inferred_symbol_types_.clear();
  infer_parent_.clear();
  unresolved_vars_.clear();

  const Scope * prev_scope = current_scope_;
  current_scope_ = values_.get_tree_scope(decl->name);

  // Check each statement in the tree body
  for (auto * stmt : decl->body) {
    check_stmt(stmt);
  }

  // Report unresolved inference variables in this tree scope.
  for (const auto & [sym, parent] : infer_parent_) {
    if (!sym) continue;
    if (parent != sym) continue;  // not a root

    const auto it = inferred_symbol_types_.find(sym);
    const Type * t = (it != inferred_symbol_types_.end()) ? it->second : nullptr;
    if (contains_unknown(t)) {
      report_error(
        sym->definitionRange,
        std::string("could not infer type of variable '") + std::string(sym->name) + "'");
    }
  }

  current_scope_ = prev_scope;

  // Spec 6.3.2: warn on mut/out tree parameters never used for writing.
  warn_unused_write_params(decl, diags_);
}

void TypeChecker::check_global_var_decl(GlobalVarDecl * decl)
{
  // Reference: docs/reference/type-system.md §3.6 (型推論)
  // Global vars must have either a type annotation or an initializer.
  if (!decl->type && !decl->initialValue) {
    report_error(decl->get_range(), "global variable must have a type annotation or initializer");
    return;
  }

  const Type * declared_type = nullptr;
  if (decl->type) {
    declared_type = resolve_type(decl->type);
  }

  if (decl->initialValue) {
    const Type * init_type = check_expr_with_expected(decl->initialValue, declared_type);

    if (declared_type && contains_unknown(declared_type)) {
      // Wildcard in annotated type: specialize from initializer when possible.
      const Type * specialized = unify_inference_types(types_, declared_type, init_type);
      if (!specialized || specialized->is_error()) {
        report_error(decl->initialValue->get_range(), "initializer type mismatch");
      }
    } else if (declared_type && !is_assignable(declared_type, init_type)) {
      report_error(
        decl->initialValue->get_range(),
        std::string("initializer type mismatch: cannot initialize '") + to_string(declared_type) +
          "' with '" + to_string(init_type) + "'");
    }
  }
}

void TypeChecker::check_global_const_decl(GlobalConstDecl * decl)
{
  const Type * declared_type = nullptr;
  if (decl->type) {
    declared_type = resolve_type(decl->type);
  }

  const Type * value_type = check_expr_with_expected(decl->value, declared_type);

  // Apply defaults
  if (value_type->is_placeholder()) {
    value_type = apply_defaults(value_type);
  }

  if (declared_type && !is_assignable(declared_type, value_type)) {
    report_error(
      decl->value->get_range(),
      std::string("const initializer type mismatch: cannot initialize '") +
        to_string(declared_type) + "' with '" + to_string(value_type) + "'");
  }
}

// ============================================================================
// Helper Methods
// ============================================================================

const Type * TypeChecker::resolve_type(const TypeNode * node)
{
  if (!node) return types_.error_type();

  auto parse_uint_literal = [](std::string_view s) -> std::optional<uint64_t> {
    if (s.empty()) return std::nullopt;
    uint64_t out = 0;
    for (const char c : s) {
      if (c < '0' || c > '9') return std::nullopt;
      out = (out * 10) + static_cast<uint64_t>(c - '0');
    }
    return out;
  };

  auto resolve_const_usize = [&](
                               std::string_view token, SourceRange range,
                               std::string_view what) -> std::optional<uint64_t> {
    if (auto lit = parse_uint_literal(token)) {
      return lit;
    }

    // Ident form: must reference a const integer evaluatable at compile time.
    const Scope * global = values_.get_global_scope();
    const Symbol * sym = global ? global->lookup(token) : nullptr;
    if (!sym) {
      report_error(
        range, std::string("Unknown identifier '") + std::string(token) + "' used as " +
                 std::string(what));
      return std::nullopt;
    }
    if (!sym->is_const()) {
      report_error(
        range, std::string("Identifier '") + std::string(token) + "' used as " + std::string(what) +
                 " must be a const");
      return std::nullopt;
    }

    const ConstValue * cv = nullptr;
    if (sym->astNode) {
      if (const auto * gcd = dyn_cast<GlobalConstDecl>(sym->astNode)) {
        cv = gcd->evaluatedValue;
      } else if (const auto * lcd = dyn_cast<ConstDeclStmt>(sym->astNode)) {
        cv = lcd->evaluatedValue;
      }
    }
    if (!cv || cv->is_error()) {
      report_error(
        range, std::string("Identifier '") + std::string(token) + "' used as " + std::string(what) +
                 " is not a constant integer");
      return std::nullopt;
    }
    if (!cv->is_integer()) {
      report_error(
        range, std::string("Identifier '") + std::string(token) + "' used as " + std::string(what) +
                 " must be an integer constant");
      return std::nullopt;
    }
    const int64_t v = cv->as_integer();
    if (v < 0) {
      report_error(range, std::string(what) + " must be a non-negative integer");
      return std::nullopt;
    }
    return static_cast<uint64_t>(v);
  };

  std::vector<const TypeAliasDecl *> alias_stack;

  std::function<const Type *(const TypeNode *)> go = [&](const TypeNode * n) -> const Type * {
    if (!n) return types_.error_type();

    switch (n->get_kind()) {
      case NodeKind::InferType:
        // Inference placeholder '_' is represented as a concrete Unknown type.
        // This allows expressing nullable wildcard (_?) as Nullable(Unknown).
        return types_.unknown_type();

      case NodeKind::PrimaryType: {
        const auto * primary = static_cast<const PrimaryType *>(n);

        // Builtins (and builtin aliases) first.
        if (const Type * builtin = types_.lookup_builtin(primary->name)) {
          // Handle bounded string: string<N>
          if (primary->size.has_value() && builtin->kind == TypeKind::String) {
            const auto max_bytes =
              resolve_const_usize(*primary->size, primary->get_range(), "bounded string size");
            if (!max_bytes.has_value()) return types_.error_type();
            return types_.get_bounded_string_type(*max_bytes);
          }
          return builtin;
        }

        // User-defined / extern / alias types.
        if (const TypeSymbol * sym = type_table_.lookup(primary->name)) {
          if (sym->is_extern_type()) {
            return types_.get_extern_type(sym->name, sym->decl);
          }
          if (sym->is_type_alias()) {
            const auto * alias_decl = dyn_cast<TypeAliasDecl>(sym->decl);
            if (!alias_decl || !alias_decl->aliasedType) {
              return types_.error_type();
            }

            // Detect and report alias cycles once with a helpful chain: A -> B -> A.
            for (size_t i = 0; i < alias_stack.size(); ++i) {
              if (alias_stack[i] != alias_decl) continue;

              const TypeAliasDecl * anchor = alias_stack[i];
              if (reported_alias_cycles_.insert(anchor).second) {
                std::string msg = "circular type alias is not allowed: ";
                for (size_t j = i; j < alias_stack.size(); ++j) {
                  msg += std::string(alias_stack[j]->name);
                  msg += " -> ";
                }
                msg += std::string(alias_decl->name);
                report_error(primary->get_range(), msg);
              }
              return types_.error_type();
            }

            alias_stack.push_back(alias_decl);
            const Type * resolved = go(alias_decl->aliasedType);
            alias_stack.pop_back();
            return resolved ? resolved : types_.error_type();
          }
        }

        report_error(
          primary->get_range(), std::string("Unknown type '") + std::string(primary->name) + "'");
        return types_.error_type();
      }

      case NodeKind::StaticArrayType: {
        const auto * arr = static_cast<const StaticArrayType *>(n);
        const Type * elem_type = go(arr->elementType);
        if (!elem_type || elem_type->is_error()) return types_.error_type();

        const auto size = resolve_const_usize(arr->size, arr->get_range(), "array size");
        if (!size.has_value()) return types_.error_type();

        if (arr->isBounded) {
          return types_.get_bounded_array_type(elem_type, *size);
        }
        return types_.get_static_array_type(elem_type, *size);
      }

      case NodeKind::DynamicArrayType: {
        const auto * arr = static_cast<const DynamicArrayType *>(n);
        const Type * elem_type = go(arr->elementType);
        if (!elem_type || elem_type->is_error()) return types_.error_type();
        return types_.get_dynamic_array_type(elem_type);
      }

      case NodeKind::TypeExpr: {
        const auto * expr = static_cast<const TypeExpr *>(n);
        const Type * base = go(expr->base);
        if (!base) return types_.unknown_type();
        if (base->is_error()) return base;
        if (expr->nullable) return types_.get_nullable_type(base);
        return base;
      }

      default:
        return types_.error_type();
    }
  };

  return go(node);
}

const Symbol * TypeChecker::find_infer_root(const Symbol * sym)
{
  if (!sym) return nullptr;
  auto it = infer_parent_.find(sym);
  if (it == infer_parent_.end()) {
    return sym;
  }
  if (it->second == sym) {
    return sym;
  }
  it->second = find_infer_root(it->second);
  return it->second;
}

void TypeChecker::unify_infer_vars(const Symbol * a, const Symbol * b, SourceRange where)
{
  if (!a || !b) return;
  if (!is_inferable_var_symbol(a) || !is_inferable_var_symbol(b)) return;

  // Ensure both are registered for inference.
  constrain_var_type(a, types_.unknown_type(), where);
  constrain_var_type(b, types_.unknown_type(), where);

  const Symbol * ra = find_infer_root(a);
  const Symbol * rb = find_infer_root(b);
  if (!ra || !rb || ra == rb) return;

  const Type * ta = nullptr;
  const Type * tb = nullptr;
  if (auto ita = inferred_symbol_types_.find(ra); ita != inferred_symbol_types_.end()) {
    ta = ita->second;
  }
  if (auto itb = inferred_symbol_types_.find(rb); itb != inferred_symbol_types_.end()) {
    tb = itb->second;
  }

  const Type * merged = unify_inference_types(types_, ta, tb);
  if (!merged || merged->is_error()) {
    const bool suppress = (ta && ta->is_error()) || (tb && tb->is_error());
    if (!suppress) {
      report_error(
        where, std::string("conflicting type constraints: cannot unify '") + to_string(ta) +
                 "' with '" + to_string(tb) + "'");
    }
    merged = types_.error_type();
  }

  // Deterministic root choice.
  const Symbol * root = (ra < rb) ? ra : rb;
  const Symbol * other = (root == ra) ? rb : ra;
  infer_parent_.insert_or_assign(root, root);
  infer_parent_.insert_or_assign(other, root);

  inferred_symbol_types_.insert_or_assign(root, merged);
  (void)inferred_symbol_types_.erase(other);

  if (contains_unknown(merged)) {
    unresolved_vars_.insert(root);
  } else {
    (void)unresolved_vars_.erase(root);
  }
}

const Type * TypeChecker::get_declared_symbol_type(const Symbol * sym)
{
  if (!sym) return nullptr;

  // Prefer types from the declaring AST node when available.
  if (sym->astNode) {
    if (const auto * p = dyn_cast<ParamDecl>(sym->astNode)) {
      return p->type ? resolve_type(p->type) : nullptr;
    }
    if (const auto * v = dyn_cast<GlobalVarDecl>(sym->astNode)) {
      return v->type ? resolve_type(v->type) : nullptr;
    }
    if (const auto * v = dyn_cast<BlackboardDeclStmt>(sym->astNode)) {
      return v->type ? resolve_type(v->type) : nullptr;
    }
    if (const auto * c = dyn_cast<GlobalConstDecl>(sym->astNode)) {
      return c->type ? resolve_type(c->type) : nullptr;
    }
    if (const auto * c = dyn_cast<ConstDeclStmt>(sym->astNode)) {
      return c->type ? resolve_type(c->type) : nullptr;
    }
  }

  // If symbol has explicit type name, look it up.
  if (sym->typeName.has_value()) {
    if (const Type * builtin = types_.lookup_builtin(*sym->typeName)) {
      return builtin;
    }
    if (const TypeSymbol * ts = type_table_.lookup(*sym->typeName)) {
      if (ts->is_extern_type()) {
        return types_.get_extern_type(ts->name, ts->decl);
      }
    }
  }

  // Const: use evaluated const value type.
  if (sym->is_const() && sym->astNode) {
    if (const auto * gcd = dyn_cast<GlobalConstDecl>(sym->astNode)) {
      return (gcd->evaluatedValue && gcd->evaluatedValue->type) ? gcd->evaluatedValue->type
                                                                : nullptr;
    }
    if (const auto * lcd = dyn_cast<ConstDeclStmt>(sym->astNode)) {
      return (lcd->evaluatedValue && lcd->evaluatedValue->type) ? lcd->evaluatedValue->type
                                                                : nullptr;
    }
  }

  return nullptr;
}

bool TypeChecker::is_inferable_var_symbol(const Symbol * sym) noexcept
{
  if (!sym) return false;
  if (!sym->is_variable()) return false;
  if (sym->is_parameter()) return false;
  return true;
}

const Type * TypeChecker::normalize_inference_type(const Type * t)
{
  if (!t) return types_.unknown_type();
  return t;
}

void TypeChecker::constrain_var_type(
  const Symbol * sym, const Type * constraint_type, SourceRange where)
{
  if (!sym) return;
  if (!is_inferable_var_symbol(sym)) return;

  // Register symbol in the inference structure on first use.
  if (infer_parent_.find(sym) == infer_parent_.end()) {
    infer_parent_.insert_or_assign(sym, sym);

    const Type * declared = get_declared_symbol_type(sym);
    const Type * init = declared ? declared : types_.unknown_type();
    inferred_symbol_types_.insert_or_assign(sym, init);
    if (contains_unknown(init)) {
      unresolved_vars_.insert(sym);
    }
  }

  const Symbol * root = find_infer_root(sym);
  if (!root) return;

  const Type * current = types_.unknown_type();
  if (auto it = inferred_symbol_types_.find(root); it != inferred_symbol_types_.end()) {
    current = it->second;
  }

  const Type * normalized = normalize_inference_type(constraint_type);
  const Type * merged = unify_inference_types(types_, current, normalized);

  if (!merged || merged->is_error()) {
    const bool suppress =
      (current && current->is_error()) || (normalized && normalized->is_error());
    if (!suppress) {
      report_error(
        where, std::string("conflicting type constraints: cannot unify '") + to_string(current) +
                 "' with '" + to_string(normalized) + "'");
    }
    merged = types_.error_type();
  }

  inferred_symbol_types_.insert_or_assign(root, merged);

  if (contains_unknown(merged)) {
    unresolved_vars_.insert(root);
  } else {
    (void)unresolved_vars_.erase(root);
  }
}

const Type * TypeChecker::get_symbol_type(const Symbol * sym)
{
  if (!sym) return types_.error_type();

  // Inferred types take precedence for inferable vars.
  if (is_inferable_var_symbol(sym)) {
    const Symbol * root = find_infer_root(sym);
    if (root) {
      if (const auto it = inferred_symbol_types_.find(root); it != inferred_symbol_types_.end()) {
        return it->second;
      }
    }
  }

  // Prefer types from the declaring AST node when available (core SymbolTable
  // currently does not populate Symbol::typeName for vars/params).
  if (sym->astNode) {
    if (const auto * p = dyn_cast<ParamDecl>(sym->astNode)) {
      if (p->type) {
        if (const Type * t = resolve_type(p->type)) {
          return t;
        }
      }
    }
    if (const auto * v = dyn_cast<GlobalVarDecl>(sym->astNode)) {
      if (v->type) {
        if (const Type * t = resolve_type(v->type)) {
          return t;
        }
      }
    }
    if (const auto * v = dyn_cast<BlackboardDeclStmt>(sym->astNode)) {
      if (v->type) {
        if (const Type * t = resolve_type(v->type)) {
          return t;
        }
      }
    }
    if (const auto * c = dyn_cast<GlobalConstDecl>(sym->astNode)) {
      if (c->type) {
        if (const Type * t = resolve_type(c->type)) {
          return t;
        }
      }
    }
    if (const auto * c = dyn_cast<ConstDeclStmt>(sym->astNode)) {
      if (c->type) {
        if (const Type * t = resolve_type(c->type)) {
          return t;
        }
      }
    }
  }

  // If symbol has explicit type name, look it up
  if (sym->typeName.has_value()) {
    if (const Type * builtin = types_.lookup_builtin(*sym->typeName)) {
      return builtin;
    }
    // Could be extern type
    if (const TypeSymbol * ts = type_table_.lookup(*sym->typeName)) {
      if (ts->is_extern_type()) {
        return types_.get_extern_type(ts->name, ts->decl);
      }
    }
  }

  // For const symbols, get type from evaluated value
  if (sym->is_const() && sym->astNode) {
    if (const auto * gcd = dyn_cast<GlobalConstDecl>(sym->astNode)) {
      // Prefer type inferred by the TypeChecker for the initializer when available.
      if (gcd->value && gcd->value->resolvedType) {
        return gcd->value->resolvedType;
      }
      if (gcd->evaluatedValue && gcd->evaluatedValue->type) {
        return gcd->evaluatedValue->type;
      }
    }
    if (const auto * lcd = dyn_cast<ConstDeclStmt>(sym->astNode)) {
      if (lcd->value && lcd->value->resolvedType) {
        return lcd->value->resolvedType;
      }
      if (lcd->evaluatedValue && lcd->evaluatedValue->type) {
        return lcd->evaluatedValue->type;
      }
    }
  }

  // Fallback: could not determine type
  return types_.error_type();
}

const Type * TypeChecker::apply_defaults(const Type * type)
{
  return bt_dsl::apply_defaults(types_, type);
}

void TypeChecker::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  ++error_count_;

  if (diags_) {
    diags_->report_error(range, std::string(message));
  }
}

}  // namespace bt_dsl
