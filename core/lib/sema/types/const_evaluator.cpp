// bt_dsl/sema/const_evaluator.cpp - Constant evaluator implementation
//
#include "bt_dsl/sema/types/const_evaluator.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>

#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/sema/types/type_table.hpp"

namespace bt_dsl
{

namespace
{

std::optional<uint64_t> parse_unsigned(std::string_view sv)
{
  uint64_t value = 0;
  const char * begin = sv.data();
  const char * end = sv.data() + sv.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

struct ConstCastTarget
{
  const Type * type = nullptr;       // nullptr when unsupported/unknown
  const Type * base_type = nullptr;  // for nullable targets
  bool is_nullable = false;
  bool is_extern = false;
  bool is_dynamic_array = false;
};

ConstCastTarget analyze_const_cast_target(TypeContext & type_ctx, const TypeNode * node)
{
  ConstCastTarget out;
  if (node == nullptr) return out;

  if (const auto * te = dyn_cast<TypeExpr>(node)) {
    out.is_nullable = te->nullable;
    const ConstCastTarget inner = analyze_const_cast_target(type_ctx, te->base);
    out.is_extern = inner.is_extern;
    out.is_dynamic_array = inner.is_dynamic_array;
    out.base_type = inner.type;
    out.type = out.is_nullable && inner.type ? type_ctx.get_nullable_type(inner.type) : inner.type;
    return out;
  }

  if (const auto * dyn_arr = dyn_cast<DynamicArrayType>(node)) {
    (void)dyn_arr;
    out.is_dynamic_array = true;
    return out;
  }

  if (const auto * prim = dyn_cast<PrimaryType>(node)) {
    // For const-cast restrictions we only need to reject extern types.
    if (prim->resolvedType && prim->resolvedType->is_extern_type()) {
      out.is_extern = true;
      return out;
    }

    if (prim->name == "string" && prim->size.has_value()) {
      if (auto n = parse_unsigned(*prim->size)) {
        out.type = type_ctx.get_bounded_string_type(*n);
      }
      return out;
    }

    // Builtins (including aliases like int/float/byte)
    out.type = type_ctx.lookup_builtin(prim->name);
    return out;
  }

  // Other type forms (arrays, aliases, etc.) are not currently supported for const casts.
  return out;
}

ConstValue convert_const_value_to_target(
  TypeContext & type_ctx, const ConstValue & val, const ConstCastTarget & target, SourceRange range,
  const std::function<void(SourceRange, std::string_view)> & report_error)
{
  if (target.is_extern) {
    report_error(range, "cannot cast to extern type in constant expression");
    return ConstValue::make_error();
  }

  if (target.is_dynamic_array) {
    report_error(range, "dynamic array type is not allowed in constant expressions");
    return ConstValue::make_error();
  }

  if (target.type == nullptr) {
    report_error(range, "unsupported cast in constant expression");
    return ConstValue::make_error();
  }

  const Type * dst = target.type;

  // Nullable targets: null is always OK; non-null values are converted to base type.
  if (dst->kind == TypeKind::Nullable) {
    if (val.is_null()) {
      auto out = ConstValue::make_null();
      out.type = dst;
      return out;
    }

    ConstCastTarget base_target;
    base_target.type = dst->base_type;
    ConstValue converted =
      convert_const_value_to_target(type_ctx, val, base_target, range, report_error);
    if (converted.is_error()) return converted;
    converted.type = dst;
    return converted;
  }

  // Numeric casts
  if (dst->is_integer()) {
    auto i = val.to_integer();
    if (!i.has_value()) {
      report_error(range, "cannot cast non-numeric value to integer type");
      return ConstValue::make_error();
    }
    auto out = ConstValue::make_integer(*i);
    out.type = dst;
    return out;
  }

  if (dst->is_float()) {
    auto f = val.to_float();
    if (!f.has_value()) {
      report_error(range, "cannot cast non-numeric value to float type");
      return ConstValue::make_error();
    }
    auto out = ConstValue::make_float(*f);
    out.type = dst;
    return out;
  }

  // Bool casts (keep strict for now)
  if (dst->kind == TypeKind::Bool) {
    if (!val.is_bool()) {
      report_error(range, "cannot cast non-boolean value to bool");
      return ConstValue::make_error();
    }
    auto out = ConstValue::make_bool(val.as_bool());
    out.type = dst;
    return out;
  }

  // String casts (including bounded string)
  if (dst->is_string()) {
    if (!val.is_string()) {
      report_error(range, "cannot cast non-string value to string");
      return ConstValue::make_error();
    }
    auto out = ConstValue::make_string(val.as_string());
    out.type = dst;
    return out;
  }

  report_error(range, "unsupported cast target type in constant expression");
  return ConstValue::make_error();
}

}  // namespace

// ============================================================================
// Constructor
// ============================================================================

ConstEvaluator::ConstEvaluator(
  AstContext & ast_ctx, TypeContext & type_ctx, const SymbolTable & values, DiagnosticBag * diags)
: ast_ctx_(ast_ctx), type_ctx_(type_ctx), values_(values), diags_(diags)
{
}

// ============================================================================
// Entry Points
// ============================================================================

bool ConstEvaluator::evaluate_program(Program & program)
{
  has_errors_ = false;
  error_count_ = 0;

  // Build evaluation order for global consts
  auto order = build_evaluation_order(program.globalConsts);

  // Evaluate global consts in order
  for (const auto * node : order) {
    if (const auto * gc = dyn_cast<GlobalConstDecl>(node)) {
      const Symbol * sym = values_.get_global(gc->name);

      evaluating_.clear();
      if (sym) {
        evaluating_.insert(sym);
      }

      const ConstValue val = evaluate(gc->value);
      if (!val.is_error()) {
        const ConstValue * stored = store_in_arena(val);
        if (sym) {
          const_cache_[sym] = stored;
        }
        // Store in AST node for later access (must be stable beyond this object)
        const_cast<GlobalConstDecl *>(gc)->evaluatedValue = stored;
      }

      if (sym) {
        evaluating_.erase(sym);
      }
    }
  }

  // Evaluate default arguments (extern ports / tree params) as const_expr.
  evaluate_default_args(program);

  // Evaluate local consts in tree bodies
  for (auto * tree : program.trees) {
    const Scope * tree_scope = values_.get_tree_scope(tree->name);
    for (auto * stmt : tree->body) {
      evaluate_local_consts(stmt, tree_scope);
    }
  }

  return !has_errors_;
}

void ConstEvaluator::evaluate_default_args(Program & program)
{
  // Extern default ports
  for (auto * ext : program.externs) {
    for (auto * port : ext->ports) {
      if (port && port->defaultValue) {
        (void)evaluate(port->defaultValue);
      }
    }
  }

  // Tree parameter defaults
  for (auto * tree : program.trees) {
    for (auto * param : tree->params) {
      if (param && param->defaultValue) {
        (void)evaluate(param->defaultValue);
      }
    }
  }
}

void ConstEvaluator::evaluate_local_consts(Stmt * stmt, const Scope * current_scope)
{
  if (auto * cs = dyn_cast<ConstDeclStmt>(stmt)) {
    const Symbol * sym = values_.resolve(cs->name, current_scope);

    evaluating_.clear();
    if (sym) {
      evaluating_.insert(sym);
    }

    const ConstValue val = evaluate(cs->value);
    if (!val.is_error()) {
      const ConstValue * stored = store_in_arena(val);
      if (sym) {
        const_cache_[sym] = stored;
      }
      const_cast<ConstDeclStmt *>(cs)->evaluatedValue = stored;
    }

    if (sym) {
      evaluating_.erase(sym);
    }
  } else if (auto * ns = dyn_cast<NodeStmt>(stmt)) {
    // Recurse into children with appropriate block scope, if available.
    const Scope * child_scope = current_scope;
    if (ns->hasChildrenBlock && ns->resolvedBlockScope != nullptr) {
      child_scope = ns->resolvedBlockScope;
    }

    for (auto * child : ns->children) {
      evaluate_local_consts(child, child_scope);
    }
  }
}

ConstValue ConstEvaluator::evaluate(const Expr * expr)
{
  if (!expr) {
    return ConstValue::make_error();
  }
  return eval_expr(expr);
}

std::optional<uint64_t> ConstEvaluator::evaluate_array_size(const Expr * expr, SourceRange range)
{
  const ConstValue val = evaluate(expr);
  if (val.is_error()) {
    return std::nullopt;
  }

  if (!val.is_non_negative_integer()) {
    report_error(range, "array size must be a non-negative integer");
    return std::nullopt;
  }

  return static_cast<uint64_t>(val.as_integer());
}

// ============================================================================
// Expression Evaluation
// ============================================================================

ConstValue ConstEvaluator::eval_expr(const Expr * expr)
{
  switch (expr->get_kind()) {
    case NodeKind::IntLiteral:
      return eval_int_literal(cast<IntLiteralExpr>(expr));
    case NodeKind::FloatLiteral:
      return eval_float_literal(cast<FloatLiteralExpr>(expr));
    case NodeKind::StringLiteral:
      return eval_string_literal(cast<StringLiteralExpr>(expr));
    case NodeKind::BoolLiteral:
      return eval_bool_literal(cast<BoolLiteralExpr>(expr));
    case NodeKind::NullLiteral:
      return eval_null_literal(cast<NullLiteralExpr>(expr));
    case NodeKind::VarRef:
      return eval_var_ref(cast<VarRefExpr>(expr));
    case NodeKind::BinaryExpr:
      return eval_binary_expr(cast<BinaryExpr>(expr));
    case NodeKind::UnaryExpr:
      return eval_unary_expr(cast<UnaryExpr>(expr));
    case NodeKind::CastExpr:
      return eval_cast_expr(cast<CastExpr>(expr));
    case NodeKind::IndexExpr:
      return eval_index_expr(cast<IndexExpr>(expr));
    case NodeKind::ArrayLiteralExpr:
      return eval_array_literal(cast<ArrayLiteralExpr>(expr));
    case NodeKind::ArrayRepeatExpr:
      return eval_array_repeat(cast<ArrayRepeatExpr>(expr));
    case NodeKind::VecMacroExpr:
      return eval_vec_macro(cast<VecMacroExpr>(expr));
    case NodeKind::MissingExpr:
      return ConstValue::make_error();
    default:
      report_error(expr->get_range(), "expression is not allowed in constant context");
      return ConstValue::make_error();
  }
}

ConstValue ConstEvaluator::eval_int_literal(const IntLiteralExpr * node)
{
  auto val = ConstValue::make_integer(node->value);
  val.type = type_ctx_.integer_literal_type();
  return val;
}

ConstValue ConstEvaluator::eval_float_literal(const FloatLiteralExpr * node)
{
  auto val = ConstValue::make_float(node->value);
  val.type = type_ctx_.float_literal_type();
  return val;
}

ConstValue ConstEvaluator::eval_string_literal(const StringLiteralExpr * node)
{
  auto val = ConstValue::make_string(node->value);
  val.type = type_ctx_.string_type();
  return val;
}

ConstValue ConstEvaluator::eval_bool_literal(const BoolLiteralExpr * node)
{
  auto val = ConstValue::make_bool(node->value);
  val.type = type_ctx_.bool_type();
  return val;
}

ConstValue ConstEvaluator::eval_null_literal(const NullLiteralExpr * /*node*/)
{
  auto val = ConstValue::make_null();
  val.type = type_ctx_.null_literal_type();
  return val;
}

ConstValue ConstEvaluator::eval_var_ref(const VarRefExpr * node)
{
  // Check for circular reference
  const Symbol * sym = node->resolvedSymbol;
  if (sym && evaluating_.count(sym)) {
    report_error(node->get_range(), "circular reference in constant expression");
    return ConstValue::make_error();
  }

  // Check cache first
  auto it = sym ? const_cache_.find(sym) : const_cache_.end();
  if (it != const_cache_.end()) {
    return *it->second;
  }

  // Get resolved symbol
  if (!sym) {
    report_error(node->get_range(), "unresolved identifier");
    return ConstValue::make_error();
  }

  // Must be a const
  if (!sym->is_const()) {
    report_error(node->get_range(), "non-constant value in constant expression");
    return ConstValue::make_error();
  }

  // Get init expression from AST node
  const Expr * init_expr = nullptr;
  GlobalConstDecl * gc_mut = nullptr;
  ConstDeclStmt * cs_mut = nullptr;
  if (const auto * gc = dyn_cast<GlobalConstDecl>(sym->astNode)) {
    // Check if already evaluated
    if (gc->evaluatedValue) {
      const_cache_[sym] = gc->evaluatedValue;
      return *gc->evaluatedValue;
    }
    gc_mut = const_cast<GlobalConstDecl *>(gc);
    init_expr = gc->value;
  } else if (const auto * cs = dyn_cast<ConstDeclStmt>(sym->astNode)) {
    if (cs->evaluatedValue) {
      const_cache_[sym] = cs->evaluatedValue;
      return *cs->evaluatedValue;
    }
    cs_mut = const_cast<ConstDeclStmt *>(cs);
    init_expr = cs->value;
  }

  if (!init_expr) {
    report_error(node->get_range(), "constant has no initializer");
    return ConstValue::make_error();
  }

  // Evaluate the initializer
  evaluating_.insert(sym);
  ConstValue val = evaluate(init_expr);
  evaluating_.erase(sym);

  if (!val.is_error()) {
    const ConstValue * stored = store_in_arena(val);
    const_cache_[sym] = stored;
    // Also publish to the declaration node so later phases (e.g., codegen)
    // can use the evaluated value.
    if (gc_mut) {
      gc_mut->evaluatedValue = stored;
    }
    if (cs_mut) {
      cs_mut->evaluatedValue = stored;
    }
  }

  return val;
}

const ConstValue * ConstEvaluator::store_in_arena(const ConstValue & v)
{
  auto slot = ast_ctx_.allocate_array<ConstValue>(1);
  slot[0] = v;
  return slot.data();
}

ConstValue ConstEvaluator::eval_binary_expr(const BinaryExpr * node)
{
  ConstValue lhs = eval_expr(node->lhs);
  if (lhs.is_error()) return lhs;

  ConstValue rhs = eval_expr(node->rhs);
  if (rhs.is_error()) return rhs;

  const BinaryOp op = node->op;
  const SourceRange range = node->get_range();

  // Arithmetic ops
  if (
    op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
    op == BinaryOp::Mod) {
    return eval_arithmetic(op, lhs, rhs, range);
  }

  // Comparison ops
  if (
    op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge ||
    op == BinaryOp::Eq || op == BinaryOp::Ne) {
    return eval_comparison(op, lhs, rhs);
  }

  // Logical ops
  if (op == BinaryOp::And || op == BinaryOp::Or) {
    return eval_logical(op, lhs, rhs, range);
  }

  // Bitwise ops
  if (op == BinaryOp::BitAnd || op == BinaryOp::BitXor || op == BinaryOp::BitOr) {
    return eval_bitwise(op, lhs, rhs, range);
  }

  report_error(range, "unsupported binary operator in constant expression");
  return ConstValue::make_error();
}

ConstValue ConstEvaluator::eval_unary_expr(const UnaryExpr * node)
{
  ConstValue operand = eval_expr(node->operand);
  if (operand.is_error()) return operand;

  switch (node->op) {
    case UnaryOp::Neg:
      if (operand.is_integer()) {
        auto val = ConstValue::make_integer(-operand.as_integer());
        val.type = operand.type;
        return val;
      }
      if (operand.is_float()) {
        auto val = ConstValue::make_float(-operand.as_float());
        val.type = operand.type;
        return val;
      }
      report_error(node->get_range(), "cannot negate non-numeric value");
      return ConstValue::make_error();

    case UnaryOp::Not:
      if (operand.is_bool()) {
        auto val = ConstValue::make_bool(!operand.as_bool());
        val.type = type_ctx_.bool_type();
        return val;
      }
      report_error(node->get_range(), "logical not requires boolean operand");
      return ConstValue::make_error();
  }

  return ConstValue::make_error();
}

ConstValue ConstEvaluator::eval_cast_expr(const CastExpr * node)
{
  ConstValue val = eval_expr(node->expr);
  if (val.is_error()) return val;

  const ConstCastTarget target = analyze_const_cast_target(type_ctx_, node->targetType);

  auto reporter = [&](SourceRange r, std::string_view msg) { report_error(r, msg); };
  return convert_const_value_to_target(type_ctx_, val, target, node->get_range(), reporter);
}

ConstValue ConstEvaluator::eval_index_expr(const IndexExpr * node)
{
  ConstValue base = eval_expr(node->base);
  if (base.is_error()) return base;

  ConstValue index = eval_expr(node->index);
  if (index.is_error()) return index;

  if (!base.is_array()) {
    report_error(node->get_range(), "cannot index non-array value");
    return ConstValue::make_error();
  }

  if (!index.is_integer()) {
    report_error(node->get_range(), "array index must be an integer");
    return ConstValue::make_error();
  }

  const int64_t idx = index.as_integer();
  auto elements = base.as_array();

  if (idx < 0 || static_cast<size_t>(idx) >= elements.size()) {
    report_error(node->get_range(), "array index out of bounds");
    return ConstValue::make_error();
  }

  return elements[static_cast<size_t>(idx)];
}

ConstValue ConstEvaluator::eval_array_literal(const ArrayLiteralExpr * node)
{
  std::vector<ConstValue> elements;
  elements.reserve(node->elements.size());

  for (const auto * elem : node->elements) {
    ConstValue val = eval_expr(elem);
    if (val.is_error()) return val;
    elements.push_back(val);
  }

  // Allocate in arena
  const gsl::span<ConstValue> arr = ast_ctx_.allocate_array<ConstValue>(elements.size());
  std::copy(elements.begin(), elements.end(), arr.begin());

  return ConstValue::make_array(arr);
}

ConstValue ConstEvaluator::eval_array_repeat(const ArrayRepeatExpr * node)
{
  ConstValue value = eval_expr(node->value);
  if (value.is_error()) return value;

  ConstValue count = eval_expr(node->count);
  if (count.is_error()) return count;

  if (!count.is_non_negative_integer()) {
    report_error(node->count->get_range(), "array repeat count must be a non-negative integer");
    return ConstValue::make_error();
  }

  auto n = static_cast<size_t>(count.as_integer());
  const gsl::span<ConstValue> arr = ast_ctx_.allocate_array<ConstValue>(n);
  std::fill(arr.begin(), arr.end(), value);

  return ConstValue::make_array(arr);
}

ConstValue ConstEvaluator::eval_vec_macro(const VecMacroExpr * node)
{
  report_error(node->get_range(), "vec![] is not allowed in constant expressions");
  return ConstValue::make_error();
}

// ============================================================================
// Binary Operation Helpers
// ============================================================================

ConstValue ConstEvaluator::eval_arithmetic(
  BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range)
{
  // String concatenation
  if (op == BinaryOp::Add && lhs.is_string() && rhs.is_string()) {
    // Concatenate strings in arena
    std::string concat;
    concat.reserve(lhs.as_string().size() + rhs.as_string().size());
    concat += lhs.as_string();
    concat += rhs.as_string();
    const std::string_view interned = ast_ctx_.intern(concat);
    auto val = ConstValue::make_string(interned);
    val.type = type_ctx_.string_type();
    return val;
  }

  // Integer arithmetic
  if (lhs.is_integer() && rhs.is_integer()) {
    const int64_t l = lhs.as_integer();
    const int64_t r = rhs.as_integer();
    int64_t result = 0;

    switch (op) {
      case BinaryOp::Add:
        result = l + r;
        break;
      case BinaryOp::Sub:
        result = l - r;
        break;
      case BinaryOp::Mul:
        result = l * r;
        break;
      case BinaryOp::Div:
        if (r == 0) {
          report_error(range, "division by zero");
          return ConstValue::make_error();
        }
        result = l / r;
        break;
      case BinaryOp::Mod:
        if (r == 0) {
          report_error(range, "division by zero");
          return ConstValue::make_error();
        }
        result = l % r;
        break;
      default:
        break;
    }

    auto val = ConstValue::make_integer(result);
    val.type = type_ctx_.integer_literal_type();
    return val;
  }

  // Float arithmetic (promote if either is float)
  if (lhs.is_numeric() && rhs.is_numeric() && (lhs.is_float() || rhs.is_float())) {
    const double l = lhs.to_float().value_or(0.0);
    const double r = rhs.to_float().value_or(0.0);
    double result = 0.0;

    switch (op) {
      case BinaryOp::Add:
        result = l + r;
        break;
      case BinaryOp::Sub:
        result = l - r;
        break;
      case BinaryOp::Mul:
        result = l * r;
        break;
      case BinaryOp::Div:
        if (r == 0.0) {
          report_error(range, "division by zero");
          return ConstValue::make_error();
        }
        result = l / r;
        break;
      case BinaryOp::Mod:
        report_error(range, "modulo operator requires integer operands");
        return ConstValue::make_error();
      default:
        break;
    }

    auto val = ConstValue::make_float(result);
    val.type = type_ctx_.float_literal_type();
    return val;
  }

  report_error(range, "invalid operands to binary arithmetic");
  return ConstValue::make_error();
}

ConstValue ConstEvaluator::eval_comparison(
  BinaryOp op, const ConstValue & lhs, const ConstValue & rhs)
{
  bool result = false;

  // Numeric comparison
  if (lhs.is_numeric() && rhs.is_numeric()) {
    // Use float comparison if either is float
    if (lhs.is_float() || rhs.is_float()) {
      const double l = lhs.to_float().value_or(0.0);
      const double r = rhs.to_float().value_or(0.0);

      switch (op) {
        case BinaryOp::Lt:
          result = l < r;
          break;
        case BinaryOp::Le:
          result = l <= r;
          break;
        case BinaryOp::Gt:
          result = l > r;
          break;
        case BinaryOp::Ge:
          result = l >= r;
          break;
        case BinaryOp::Eq:
          result = l == r;
          break;
        case BinaryOp::Ne:
          result = l != r;
          break;
        default:
          break;
      }
    } else {
      const int64_t l = lhs.as_integer();
      const int64_t r = rhs.as_integer();

      switch (op) {
        case BinaryOp::Lt:
          result = l < r;
          break;
        case BinaryOp::Le:
          result = l <= r;
          break;
        case BinaryOp::Gt:
          result = l > r;
          break;
        case BinaryOp::Ge:
          result = l >= r;
          break;
        case BinaryOp::Eq:
          result = l == r;
          break;
        case BinaryOp::Ne:
          result = l != r;
          break;
        default:
          break;
      }
    }
  }
  // Boolean equality
  else if (lhs.is_bool() && rhs.is_bool()) {
    if (op == BinaryOp::Eq) {
      result = lhs.as_bool() == rhs.as_bool();
    } else if (op == BinaryOp::Ne) {
      result = lhs.as_bool() != rhs.as_bool();
    }
  }
  // String equality
  else if (lhs.is_string() && rhs.is_string()) {
    if (op == BinaryOp::Eq) {
      result = lhs.as_string() == rhs.as_string();
    } else if (op == BinaryOp::Ne) {
      result = lhs.as_string() != rhs.as_string();
    }
  }
  // Null equality
  else if (lhs.is_null() && rhs.is_null()) {
    result = (op == BinaryOp::Eq);
  }

  auto val = ConstValue::make_bool(result);
  val.type = type_ctx_.bool_type();
  return val;
}

ConstValue ConstEvaluator::eval_logical(
  BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range)
{
  if (!lhs.is_bool() || !rhs.is_bool()) {
    report_error(range, "logical operators require boolean operands");
    return ConstValue::make_error();
  }

  bool result = false;
  switch (op) {
    case BinaryOp::And:
      result = lhs.as_bool() && rhs.as_bool();
      break;
    case BinaryOp::Or:
      result = lhs.as_bool() || rhs.as_bool();
      break;
    default:
      break;
  }

  auto val = ConstValue::make_bool(result);
  val.type = type_ctx_.bool_type();
  return val;
}

ConstValue ConstEvaluator::eval_bitwise(
  BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range)
{
  if (!lhs.is_integer() || !rhs.is_integer()) {
    report_error(range, "bitwise operators require integer operands");
    return ConstValue::make_error();
  }

  const int64_t l = lhs.as_integer();
  const int64_t r = rhs.as_integer();
  int64_t result = 0;

  switch (op) {
    case BinaryOp::BitAnd:
      result = l & r;
      break;
    case BinaryOp::BitXor:
      result = l ^ r;
      break;
    case BinaryOp::BitOr:
      result = l | r;
      break;
    default:
      break;
  }

  auto val = ConstValue::make_integer(result);
  val.type = type_ctx_.integer_literal_type();
  return val;
}

// ============================================================================
// Dependency Analysis
// ============================================================================

void ConstEvaluator::collect_dependencies(
  const Expr * expr, std::unordered_set<std::string_view> & deps)
{
  if (!expr) return;

  switch (expr->get_kind()) {
    case NodeKind::VarRef: {
      const auto * ref = cast<VarRefExpr>(expr);
      if (ref->resolvedSymbol && ref->resolvedSymbol->is_const()) {
        deps.insert(ref->name);
      }
      break;
    }
    case NodeKind::BinaryExpr: {
      const auto * bin = cast<BinaryExpr>(expr);
      collect_dependencies(bin->lhs, deps);
      collect_dependencies(bin->rhs, deps);
      break;
    }
    case NodeKind::UnaryExpr: {
      const auto * un = cast<UnaryExpr>(expr);
      collect_dependencies(un->operand, deps);
      break;
    }
    case NodeKind::CastExpr: {
      const auto * c = cast<CastExpr>(expr);
      collect_dependencies(c->expr, deps);
      break;
    }
    case NodeKind::IndexExpr: {
      const auto * idx = cast<IndexExpr>(expr);
      collect_dependencies(idx->base, deps);
      collect_dependencies(idx->index, deps);
      break;
    }
    case NodeKind::ArrayLiteralExpr: {
      const auto * arr_lit = cast<ArrayLiteralExpr>(expr);
      for (const auto * elem : arr_lit->elements) {
        collect_dependencies(elem, deps);
      }
      break;
    }
    case NodeKind::ArrayRepeatExpr: {
      const auto * arr_rep = cast<ArrayRepeatExpr>(expr);
      collect_dependencies(arr_rep->value, deps);
      collect_dependencies(arr_rep->count, deps);
      break;
    }
    default:
      break;
  }
}

std::vector<const AstNode *> ConstEvaluator::build_evaluation_order(
  gsl::span<GlobalConstDecl *> global_consts)
{
  // Build name -> node map
  std::unordered_map<std::string_view, GlobalConstDecl *> const_map;
  for (auto * gc : global_consts) {
    const_map[gc->name] = gc;
  }

  // Build dependency graph
  std::unordered_map<std::string_view, std::unordered_set<std::string_view>> deps;
  for (auto * gc : global_consts) {
    std::unordered_set<std::string_view> d;
    collect_dependencies(gc->value, d);
    // Filter to only include global consts
    std::unordered_set<std::string_view> filtered;
    for (const auto & name : d) {
      if (const_map.count(name)) {
        filtered.insert(name);
      }
    }
    deps[gc->name] = std::move(filtered);
  }

  // Use DFS with cycle detection for topological sort
  std::vector<const AstNode *> order;
  std::unordered_set<std::string_view> visited;
  std::unordered_set<std::string_view> in_stack;

  std::function<bool(std::string_view)> visit = [&](std::string_view name) {
    if (in_stack.count(name)) {
      // Cycle detected
      if (auto * gc = const_map[name]) {
        report_error(gc->get_range(), "circular dependency in constant");
      }
      return false;
    }
    if (visited.count(name)) {
      return true;
    }

    in_stack.insert(name);
    for (const auto & dep : deps[name]) {
      if (!visit(dep)) {
        return false;
      }
    }
    in_stack.erase(name);
    visited.insert(name);
    order.push_back(const_map[name]);
    return true;
  };

  for (auto * gc : global_consts) {
    if (!visit(gc->name)) {
      // Cycle detected, continue to find more
    }
  }

  return order;
}

// ============================================================================
// Helper Methods
// ============================================================================

const ConstValue * ConstEvaluator::get_const_for_symbol(const Symbol * sym)
{
  return get_const_value(sym);
}

void ConstEvaluator::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  error_count_++;

  if (diags_) {
    diags_->error(range, message);
  }
}

// ============================================================================
// Free Functions
// ============================================================================

const ConstValue * get_const_value(const Symbol * sym)
{
  if (!sym || !sym->is_const() || !sym->astNode) {
    return nullptr;
  }

  if (const auto * gc = dyn_cast<GlobalConstDecl>(sym->astNode)) {
    return gc->evaluatedValue;
  }
  if (const auto * cs = dyn_cast<ConstDeclStmt>(sym->astNode)) {
    return cs->evaluatedValue;
  }

  return nullptr;
}

}  // namespace bt_dsl
