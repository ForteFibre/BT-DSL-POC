// bt_dsl/type_system.cpp - Type system implementation
#include "bt_dsl/type_system.hpp"

#include <algorithm>
#include <cctype>

#include "bt_dsl/node_registry.hpp"
#include "bt_dsl/symbol_table.hpp"

namespace bt_dsl
{

// ============================================================================
// Type Implementation
// ============================================================================

Type::Type(BuiltinType builtin) : value_(builtin) {}
Type::Type(std::string custom_name) : value_(std::move(custom_name)) {}

Type Type::int_type() { return Type(BuiltinType::Int); }
Type Type::double_type() { return Type(BuiltinType::Double); }
Type Type::bool_type() { return Type(BuiltinType::Bool); }
Type Type::string_type() { return Type(BuiltinType::String); }
Type Type::any_type() { return Type(BuiltinType::Any); }
Type Type::unknown() { return Type(BuiltinType::Unknown); }

Type Type::from_string(std::string_view name)
{
  // Normalize to lowercase for comparison
  std::string lower(name);
  std::transform(
    lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

  if (lower == "int" || lower == "integer") {
    return int_type();
  } else if (lower == "double" || lower == "float") {
    return double_type();
  } else if (lower == "bool" || lower == "boolean") {
    return bool_type();
  } else if (lower == "string") {
    return string_type();
  } else if (lower == "any") {
    return any_type();
  } else {
    // Custom type - preserve original case
    return Type(std::string(name));
  }
}

bool Type::is_builtin() const { return std::holds_alternative<BuiltinType>(value_); }

bool Type::is_custom() const { return std::holds_alternative<std::string>(value_); }

bool Type::is_numeric() const
{
  if (!is_builtin()) return false;
  auto bt = std::get<BuiltinType>(value_);
  return bt == BuiltinType::Int || bt == BuiltinType::Double;
}

bool Type::is_unknown() const
{
  return is_builtin() && std::get<BuiltinType>(value_) == BuiltinType::Unknown;
}

bool Type::is_any() const
{
  return is_builtin() && std::get<BuiltinType>(value_) == BuiltinType::Any;
}

bool Type::is_compatible_with(const Type & other) const
{
  // Any is compatible with everything
  if (is_any() || other.is_any()) return true;

  // Unknown types are treated as compatible (for partial analysis)
  if (is_unknown() || other.is_unknown()) return true;

  // Same types are compatible
  if (equals(other)) return true;

  // int is promotable to double
  if (is_builtin() && other.is_builtin()) {
    auto this_bt = std::get<BuiltinType>(value_);
    auto other_bt = std::get<BuiltinType>(other.value_);
    if (
      (this_bt == BuiltinType::Int && other_bt == BuiltinType::Double) ||
      (this_bt == BuiltinType::Double && other_bt == BuiltinType::Int)) {
      return true;
    }
  }

  return false;
}

bool Type::equals(const Type & other) const
{
  if (is_builtin() != other.is_builtin()) return false;

  if (is_builtin()) {
    return std::get<BuiltinType>(value_) == std::get<BuiltinType>(other.value_);
  } else {
    return std::get<std::string>(value_) == std::get<std::string>(other.value_);
  }
}

std::string Type::to_string() const
{
  if (is_builtin()) {
    switch (std::get<BuiltinType>(value_)) {
      case BuiltinType::Int:
        return "int";
      case BuiltinType::Double:
        return "double";
      case BuiltinType::Bool:
        return "bool";
      case BuiltinType::String:
        return "string";
      case BuiltinType::Any:
        return "any";
      case BuiltinType::Unknown:
        return "unknown";
    }
  }
  return std::get<std::string>(value_);
}

// ============================================================================
// TypeContext Implementation
// ============================================================================

void TypeContext::set_type(std::string_view name, Type type)
{
  types_.insert_or_assign(std::string(name), std::move(type));
}

const Type * TypeContext::get_type(std::string_view name) const
{
  const std::string key(name);
  auto it = types_.find(key);
  if (it != types_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool TypeContext::has_type(std::string_view name) const { return get_type(name) != nullptr; }

// ============================================================================
// TypeInferenceResult Implementation
// ============================================================================

TypeInferenceResult TypeInferenceResult::success(Type t)
{
  return TypeInferenceResult{std::move(t), std::nullopt};
}

TypeInferenceResult TypeInferenceResult::failure(Type t, std::string error_message)
{
  return TypeInferenceResult{std::move(t), std::move(error_message)};
}

// ============================================================================
// TypeResolver Implementation
// ============================================================================

TypeResolver::TypeResolver(const SymbolTable & symbols, const NodeRegistry & nodes)
: symbols_(symbols), nodes_(nodes)
{
}

TypeContext TypeResolver::resolve_tree_types(const TreeDef & tree)
{
  TypeContext ctx;

  // 1. Add explicit types from parameters
  for (const auto & param : tree.params) {
    if (param.type_name) {
      ctx.set_type(param.name, Type::from_string(*param.type_name));
    }
  }

  // 2. Add types from local variables
  for (const auto & local : tree.local_vars) {
    if (local.type_name) {
      // Explicit type annotation
      ctx.set_type(local.name, Type::from_string(*local.type_name));
    } else if (local.initial_value) {
      // Infer from initial value (which is an Expression)
      auto get_global = [](std::string_view) -> const Type * { return nullptr; };
      auto result = infer_expression_type(*local.initial_value, ctx, get_global);
      ctx.set_type(local.name, std::move(result.type));
    }
  }

  // 3. Infer remaining types from node usage
  if (tree.body) {
    infer_from_node_usage(*tree.body, ctx);
  }

  return ctx;
}

Type TypeResolver::infer_literal_type(const Literal & lit)
{
  return std::visit(
    [](const auto & val) -> Type {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, StringLiteral>) {
        return Type::string_type();
      } else if constexpr (std::is_same_v<T, IntLiteral>) {
        return Type::int_type();
      } else if constexpr (std::is_same_v<T, FloatLiteral>) {
        return Type::double_type();
      } else if constexpr (std::is_same_v<T, BoolLiteral>) {
        return Type::bool_type();
      } else {
        return Type::unknown();
      }
    },
    lit);
}

TypeInferenceResult TypeResolver::infer_expression_type(
  const Expression & expr, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type) const
{
  return std::visit(
    [&](const auto & val) -> TypeInferenceResult {
      using T = std::decay_t<decltype(val)>;

      if constexpr (std::is_same_v<T, Literal>) {
        return TypeInferenceResult::success(infer_literal_type(val));
      } else if constexpr (std::is_same_v<T, VarRef>) {
        // Look up variable type
        if (const Type * t = ctx.get_type(val.name)) {
          return TypeInferenceResult::success(*t);
        }
        if (get_global_type) {
          if (const Type * t = get_global_type(val.name)) {
            return TypeInferenceResult::success(*t);
          }
        }
        return TypeInferenceResult::failure(Type::unknown(), "Unknown variable: " + val.name);
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const BinaryExpr & binary = *val;
        auto left_result = infer_expression_type(binary.left, ctx, get_global_type);
        auto right_result = infer_expression_type(binary.right, ctx, get_global_type);

        if (left_result.has_error()) return left_result;
        if (right_result.has_error()) return right_result;

        // Determine result type based on operator
        switch (binary.op) {
          // Comparison operators return bool
          case BinaryOp::Eq:
          case BinaryOp::Ne:
          case BinaryOp::Lt:
          case BinaryOp::Le:
          case BinaryOp::Gt:
          case BinaryOp::Ge:
            return TypeInferenceResult::success(Type::bool_type());

          // Logical operators require and return bool
          case BinaryOp::And:
          case BinaryOp::Or:
            if (
              !left_result.type.equals(Type::bool_type()) ||
              !right_result.type.equals(Type::bool_type())) {
              return TypeInferenceResult::failure(
                Type::bool_type(), "Logical operators require bool operands");
            }
            return TypeInferenceResult::success(Type::bool_type());

          // Arithmetic operators
          case BinaryOp::Add:
          case BinaryOp::Sub:
          case BinaryOp::Mul:
          case BinaryOp::Div:
          case BinaryOp::Mod:
            if (!left_result.type.is_numeric() || !right_result.type.is_numeric()) {
              // Special case: string + string is allowed
              if (
                binary.op == BinaryOp::Add && left_result.type.equals(Type::string_type()) &&
                right_result.type.equals(Type::string_type())) {
                return TypeInferenceResult::success(Type::string_type());
              }
              return TypeInferenceResult::failure(
                Type::unknown(), "Operator cannot be applied to non-numeric types");
            }
            // If either is double, result is double
            if (
              left_result.type.equals(Type::double_type()) ||
              right_result.type.equals(Type::double_type())) {
              return TypeInferenceResult::success(Type::double_type());
            }
            return TypeInferenceResult::success(Type::int_type());

          default:
            return TypeInferenceResult::success(Type::unknown());
        }
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const UnaryExpr & unary = *val;
        auto operand_result = infer_expression_type(unary.operand, ctx, get_global_type);

        if (operand_result.has_error()) return operand_result;

        switch (unary.op) {
          case UnaryOp::Not:
            if (!operand_result.type.equals(Type::bool_type())) {
              return TypeInferenceResult::failure(
                Type::bool_type(), "Logical not requires bool operand");
            }
            return TypeInferenceResult::success(Type::bool_type());

          case UnaryOp::Neg:
            if (!operand_result.type.is_numeric()) {
              return TypeInferenceResult::failure(
                Type::unknown(), "Negation requires numeric operand");
            }
            return TypeInferenceResult::success(operand_result.type);

          default:
            return TypeInferenceResult::success(operand_result.type);
        }
      } else {
        return TypeInferenceResult::success(Type::unknown());
      }
    },
    expr);
}

void TypeResolver::infer_from_node_usage(const NodeStmt & node, TypeContext & ctx)
{
  // Process arguments for type inference
  for (const auto & arg : node.args) {
    process_argument_for_inference(arg, node.node_name, ctx);
  }

  // Recurse into children (ChildElement = variant<Box<NodeStmt>,
  // AssignmentStmt>)
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          infer_from_node_usage(*elem, ctx);
        }
        // Skip AssignmentStmt for type inference
      },
      child);
  }
}

void TypeResolver::process_argument_for_inference(
  const Argument & arg, std::string_view node_name, TypeContext & ctx)
{
  // Skip if port name is not specified (Argument.name is the port name)
  if (!arg.name) return;

  // Get port info from registry
  const PortInfo * port = nodes_.get_port(node_name, *arg.name);
  if (!port || !port->type_name) return;

  // Check if value is a blackboard reference (ValueExpr = variant<Literal,
  // BlackboardRef>)
  std::visit(
    [&](const auto & val) {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, BlackboardRef>) {
        // If we don't have a type for this variable yet, infer from port
        if (!ctx.has_type(val.name)) {
          ctx.set_type(val.name, Type::from_string(*port->type_name));
        }
      }
      // Literals don't need type inference
    },
    arg.value);
}

// ============================================================================
// TypeChecker Implementation
// ============================================================================

void TypeChecker::check_tree(
  const TreeDef & tree, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type,
  DiagnosticBag & diagnostics)
{
  // Check local variable initial values
  for (const auto & local : tree.local_vars) {
    if (local.type_name && local.initial_value) {
      const Type declared = Type::from_string(*local.type_name);

      // Infer type from initial value expression.
      // NOTE: Avoid binding TypeResolver's reference members to temporaries.
      const SymbolTable empty_symbols;
      const NodeRegistry empty_nodes;
      const TypeResolver resolver(empty_symbols, empty_nodes);
      auto result = resolver.infer_expression_type(*local.initial_value, ctx, get_global_type);

      if (!declared.is_compatible_with(result.type)) {
        diagnostics.error(
          local.range, "Type mismatch: cannot assign " + result.type.to_string() + " to " +
                         declared.to_string());
      }
    } else if (!local.type_name && !local.initial_value) {
      diagnostics.error(
        local.range,
        "Local variable '" + local.name + "' must have either a type or initial value");
    }
  }

  // Check body node
  if (tree.body) {
    check_node_stmt(*tree.body, ctx, get_global_type, diagnostics);
  }
}

void TypeChecker::check_node_stmt(
  const NodeStmt & node, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type,
  DiagnosticBag & diagnostics)
{
  // Recurse into children (ChildElement = variant<Box<NodeStmt>,
  // AssignmentStmt>)
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & elem) {
        using T = std::decay_t<decltype(elem)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          check_node_stmt(*elem, ctx, get_global_type, diagnostics);
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          // Check assignment statement type
          // (simplified - would need full implementation for assignment
          // checking)
        }
      },
      child);
  }
}

TypeInferenceResult TypeChecker::check_binary_expr(
  const BinaryExpr & expr, const TypeContext & ctx,
  const std::function<const Type *(std::string_view)> & get_global_type)
{
  // NOTE: Avoid binding TypeResolver's reference members to temporaries.
  const SymbolTable empty_symbols;
  const NodeRegistry empty_nodes;
  const TypeResolver resolver(empty_symbols, empty_nodes);
  return resolver.infer_expression_type(Expression{Box<BinaryExpr>(expr)}, ctx, get_global_type);
}

}  // namespace bt_dsl
