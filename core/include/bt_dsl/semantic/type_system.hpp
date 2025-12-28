// bt_dsl/type_system.hpp - Type representation and inference
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/core/diagnostic.hpp"

namespace bt_dsl
{

// Forward declarations
class SymbolTable;
class Scope;
class NodeRegistry;

// ============================================================================
// Type Representation
// ============================================================================

/**
 * Built-in types in BT-DSL.
 */
enum class BuiltinType : uint8_t {
  Int,
  Double,
  Bool,
  String,
  Any,      // Compatible with everything
  Unknown,  // Type not yet resolved
};

/**
 * Represents a type in the type system.
 * Can be either a builtin type or a custom type name.
 */
class Type
{
public:
  // Constructors
  explicit Type(BuiltinType builtin);
  explicit Type(std::string custom_name);

  // Factory methods
  static Type int_type();
  static Type double_type();
  static Type bool_type();
  static Type string_type();
  static Type any_type();
  static Type unknown();
  static Type from_string(std::string_view name);

  // Type queries
  [[nodiscard]] bool is_builtin() const;
  [[nodiscard]] bool is_custom() const;
  [[nodiscard]] bool is_numeric() const;  // int or double
  [[nodiscard]] bool is_unknown() const;
  [[nodiscard]] bool is_any() const;

  /**
   * Check if this type is compatible with another.
   * Compatibility rules:
   * - Any is compatible with everything
   * - Unknown is treated as compatible (for partial analysis)
   * - Same types are compatible
   * - int is promotable to double in some contexts
   */
  [[nodiscard]] bool is_compatible_with(const Type & other) const;

  /**
   * Check strict equality.
   */
  [[nodiscard]] bool equals(const Type & other) const;

  /**
   * Get string representation.
   */
  [[nodiscard]] std::string to_string() const;

  // Get the underlying value
  [[nodiscard]] const std::variant<BuiltinType, std::string> & value() const { return value_; }

private:
  std::variant<BuiltinType, std::string> value_;
};

// ============================================================================
// Type Context
// ============================================================================

/**
 * Type context for a single Tree.
 * Holds resolved types for parameters and local variables.
 */
class TypeContext
{
public:
  TypeContext() = default;

  /**
   * Set the resolved type for a variable.
   */
  void set_type(std::string_view name, Type type);

  /**
   * Get the resolved type for a variable.
   * @return nullptr if not found
   */
  const Type * get_type(std::string_view name) const;

  /**
   * Check if a variable has a resolved type.
   */
  bool has_type(std::string_view name) const;

  /**
   * Get all resolved types.
   */
  const std::unordered_map<std::string, Type> & all_types() const { return types_; }

private:
  std::unordered_map<std::string, Type> types_;
};

// ============================================================================
// Type Inference Result
// ============================================================================

/**
 * Result of type inference for an expression.
 */
struct TypeInferenceResult
{
  Type type;
  std::optional<std::string> error;  // Error message if inference failed

  [[nodiscard]] bool has_error() const { return error.has_value(); }

  static TypeInferenceResult success(Type t);
  static TypeInferenceResult failure(Type t, std::string error_message);
};

// ============================================================================
// Type Resolver
// ============================================================================

/**
 * Resolves types for variables by analyzing their usage.
 *
 * Type resolution follows these rules:
 * 1. Explicit type annotations take precedence
 * 2. Initial values provide type information for local variables
 * 3. Port usage can infer parameter types
 */
class TypeResolver
{
public:
  TypeResolver(const SymbolTable & symbols, const NodeRegistry & nodes);

  /**
   * Resolve all types in a Tree definition.
   * @return TypeContext with resolved types
   */
  TypeContext resolve_tree_types(const TreeDef & tree);

  /**
   * Infer the type of a literal.
   */
  [[nodiscard]] static Type infer_literal_type(const Literal & lit);

  /**
   * Infer the type of an expression.
   * @param expr The expression to analyze
   * @param ctx Type context for variable lookups
   * @param get_global_type Function to get global variable types
   */
  TypeInferenceResult infer_expression_type(
    const Expression & expr, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type) const;

private:
  const SymbolTable & symbols_;
  const NodeRegistry & nodes_;

  // Infer types from node port usage
  void infer_from_node_usage(const NodeStmt & node, TypeContext & ctx);

  // Process a single argument for type inference
  void process_argument_for_inference(
    const Argument & arg, std::string_view node_name, TypeContext & ctx);
};

// ============================================================================
// Type Checker
// ============================================================================

/**
 * Validates type correctness in expressions and statements.
 */
class TypeChecker
{
public:
  TypeChecker() = default;

  /**
   * Check types in a Tree definition.
   * @param tree The tree to check
   * @param ctx Resolved type context
   * @param get_global_type Function to get global variable types
   * @param diagnostics Diagnostic bag to collect errors
   */
  void check_tree(
    const TreeDef & tree, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type,
    DiagnosticBag & diagnostics);

private:
  // Check a node statement recursively
  void check_node_stmt(
    const NodeStmt & node, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type,
    DiagnosticBag & diagnostics);

  // Check binary expression type compatibility
  static TypeInferenceResult check_binary_expr(
    const BinaryExpr & expr, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type);
};

}  // namespace bt_dsl
