// bt_dsl/type_system.hpp - Type representation and inference
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
// Type Representation (reference-spec compliant)
// ============================================================================

// Forward declaration (TypeParseResult is defined after Type).
struct TypeParseResult;

/**
 * Array size expression inside a type (e.g. [T; 4] or [T; SIZE]).
 */
struct TypeArraySizeExpr
{
  std::variant<uint64_t, std::string> value;  // integer literal or identifier
};

/**
 * Static array size spec: exact (N) or bounded (<=N).
 */
enum class TypeArraySizeKind : uint8_t {
  Exact,
  Max,
};

/**
 * Primitive integer type.
 */
struct TypeInt
{
  bool is_signed = true;
  uint8_t bits = 32;
};

/**
 * Primitive floating-point type.
 */
struct TypeFloat
{
  uint8_t bits = 64;
};

struct TypeBool
{
};

struct TypeString
{
};

struct TypeBoundedString
{
  TypeArraySizeExpr max_bytes;
};

/**
 * Static array type: [T; N] or [T; <=N]
 */
struct TypeStaticArray
{
  Box<struct Type> element;
  TypeArraySizeKind size_kind = TypeArraySizeKind::Exact;
  TypeArraySizeExpr size;
};

/**
 * Dynamic array type: vec<T>
 */
struct TypeVec
{
  Box<struct Type> element;
};

/**
 * Nullable type: T?
 */
struct TypeNullable
{
  Box<struct Type> base;
};

/**
 * Type inference wildcard: _ or _?
 */
struct TypeInfer
{
  // When true, the (eventual) resolved type must be nullable.
  // This is used for `_?` as well as for internal type variables derived from `null`.
  bool nullable = false;

  // Distinguish syntax wildcard (`_` / `_?`) from an internal inference variable (`?`).
  // Reference: docs/reference/type-system/inference-and-resolution.md 3.2.2 / 3.2.3.
  bool is_type_var = false;
};

/**
 * Unresolved named type (identifier) prior to resolution.
 */
struct TypeNamed
{
  std::string name;
};

/**
 * Extern (opaque) type after resolution.
 */
struct TypeExtern
{
  std::string name;
};

// --------------------------------------------------------------------------
// Internal-only types used during inference/checking.
// --------------------------------------------------------------------------

/**
 * Integer literal type ({integer}) with the literal value.
 * Default resolution is int32 unless constrained.
 */
struct TypeIntegerLiteral
{
  int64_t value = 0;
};

/**
 * Float literal type ({float}) with the literal value.
 * Default resolution is float64 unless constrained.
 */
struct TypeFloatLiteral
{
  double value = 0.0;
};

/**
 * String literal type (string) with byte length for bounded-string checks.
 */
struct TypeStringLiteral
{
  size_t byte_len = 0;
};

/**
 * null literal. Assignable to any nullable type (T?).
 */
struct TypeNullLiteral
{
};

/**
 * Top type compatible with everything (legacy/partial-analysis helper).
 */
struct TypeAny
{
};

/**
 * Unknown / error type (legacy/partial-analysis helper).
 */
struct TypeUnknown
{
};

/**
 * Represents a type in the BT-DSL type system.
 */
struct Type
{
  using Variant = std::variant<
    TypeInt, TypeFloat, TypeBool, TypeString, TypeBoundedString, TypeStaticArray, TypeVec,
    TypeNullable, TypeInfer, TypeNamed, TypeExtern,
    // internal-only
    TypeIntegerLiteral, TypeFloatLiteral, TypeStringLiteral, TypeNullLiteral,
    // helpers
    TypeAny, TypeUnknown>;

  Variant value;

  // ----------------------------------------------------------------------
  // Factory helpers
  // ----------------------------------------------------------------------
  static Type any() { return Type{TypeAny{}}; }
  static Type unknown() { return Type{TypeUnknown{}}; }

  // Backwards-compatible aliases (legacy API)
  static Type any_type() { return any(); }
  static Type unknown_type() { return unknown(); }
  static Type double_type() { return float_type(64); }

  static Type bool_type() { return Type{TypeBool{}}; }
  static Type string_type() { return Type{TypeString{}}; }
  static Type bounded_string(TypeArraySizeExpr max_bytes)
  {
    return Type{TypeBoundedString{std::move(max_bytes)}};
  }
  static Type bounded_string(uint64_t max_bytes)
  {
    TypeArraySizeExpr e;
    e.value = max_bytes;
    return bounded_string(std::move(e));
  }

  static Type int_type(bool is_signed = true, uint8_t bits = 32)
  {
    return Type{TypeInt{is_signed, bits}};
  }

  static Type float_type(uint8_t bits = 64) { return Type{TypeFloat{bits}}; }

  static Type vec(Type elem) { return Type{TypeVec{Box<Type>(std::move(elem))}}; }

  static Type static_array(Type elem, TypeArraySizeKind kind, TypeArraySizeExpr size)
  {
    return Type{TypeStaticArray{Box<Type>(std::move(elem)), kind, std::move(size)}};
  }

  static Type nullable(Type base) { return Type{TypeNullable{Box<Type>(std::move(base))}}; }

  // Syntax wildcard: `_` / `_?`
  static Type infer(bool nullable = false)
  {
    return Type{TypeInfer{nullable, /*is_type_var=*/false}};
  }

  // Internal inference variable: `?` (not a surface syntax).
  static Type type_var(bool nullable_requirement = false)
  {
    return Type{TypeInfer{nullable_requirement, /*is_type_var=*/true}};
  }

  static Type named(std::string name) { return Type{TypeNamed{std::move(name)}}; }
  static Type extern_type(std::string name) { return Type{TypeExtern{std::move(name)}}; }

  // internal literals
  static Type integer_literal(int64_t v) { return Type{TypeIntegerLiteral{v}}; }
  static Type float_literal(double v) { return Type{TypeFloatLiteral{v}}; }
  static Type string_literal_bytes(size_t len) { return Type{TypeStringLiteral{len}}; }
  static Type null_literal() { return Type{TypeNullLiteral{}}; }

  // ----------------------------------------------------------------------
  // Parsing (syntax only)
  // ----------------------------------------------------------------------
  static TypeParseResult parse(std::string_view text);
  static Type from_string(std::string_view text);  // legacy wrapper (returns unknown() on error)

  // ----------------------------------------------------------------------
  // Queries
  // ----------------------------------------------------------------------
  [[nodiscard]] bool is_unknown() const { return std::holds_alternative<TypeUnknown>(value); }
  [[nodiscard]] bool is_any() const { return std::holds_alternative<TypeAny>(value); }

  [[nodiscard]] bool is_numeric() const;
  [[nodiscard]] bool is_integer() const;
  [[nodiscard]] bool is_float() const;
  [[nodiscard]] bool is_nullable() const;

  // Backwards-compatible predicate (legacy API)
  [[nodiscard]] bool is_custom() const
  {
    return std::holds_alternative<TypeNamed>(value) || std::holds_alternative<TypeExtern>(value);
  }

  [[nodiscard]] bool equals(const Type & other) const;
  [[nodiscard]] std::string to_string() const;

  /**
   * Check assignability/compatibility (reference type-system.md 5.x).
   *
   * This is a best-effort check: it handles builtins, arrays, bounded strings,
   * nullable, and extern types. It intentionally does not perform full
   * constraint solving; callers should still apply bidirectional checks for
   * literals/array literals where required.
   */
  [[nodiscard]] bool is_compatible_with(const Type & other) const;
};

/**
 * BT-DSL type syntax parse result.
 *
 * NOTE: Parsing only checks *syntax* of the type string. Name resolution
 * (type aliases / extern types) is handled separately by TypeEnvironment.
 */
struct TypeParseResult
{
  Type type;
  std::optional<std::string> error;

  [[nodiscard]] bool has_error() const { return error.has_value(); }
};

// ============================================================================
// Type Environment (name resolution)
// ============================================================================

/**
 * Holds type alias and extern-type declarations for name resolution.
 */
class TypeEnvironment
{
public:
  TypeEnvironment() = default;

  void add_extern_type(std::string name);
  void add_type_alias(std::string name, Type type);

  [[nodiscard]] bool is_extern_type(std::string_view name) const;
  [[nodiscard]] bool has_alias(std::string_view name) const;

  /**
   * Resolve a type by expanding aliases and converting named extern types to
   * opaque extern types.
   *
   * On resolution failure (unknown named type / alias cycle), returns
   * Type::unknown() and optionally writes an error message.
   */
  [[nodiscard]] Type resolve(const Type & t, std::optional<std::string> * error = nullptr) const;

private:
  std::unordered_map<std::string, Type> aliases_;
  std::unordered_set<std::string> extern_types_;

  [[nodiscard]] Type resolve_impl(
    const Type & t, std::unordered_set<std::string> & visiting,
    std::optional<std::string> * error) const;
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

  /**
   * Mutate all resolved types in-place.
   *
   * This is intentionally a narrow API for post-resolution normalization passes
   * (e.g. const-evaluating bounded type sizes).
   */
  void for_each_type_mut(const std::function<void(Type &)> & fn)
  {
    for (auto & kv : types_) {
      fn(kv.second);
    }
  }

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
  TypeResolver(
    const SymbolTable & symbols, const NodeRegistry & nodes, const TypeEnvironment * env = nullptr);

  // Provide a scope for const-evaluation in expression typing.
  // This is used outside resolve_tree_types() when callers need reference-required
  // const_expr behavior (e.g. static array repeat-init lengths, const index bounds checks).
  void set_scope_for_const_eval(const Scope * scope) { current_scope_ = scope; }

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
  const TypeEnvironment * env_ = nullptr;

  // Set by resolve_tree_types() to enable const-evaluation of const_expr in
  // expression typing (e.g. repeat-init array lengths).
  const Scope * current_scope_ = nullptr;

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
  explicit TypeChecker(const TypeEnvironment * env = nullptr) : env_(env) {}

  TypeChecker(
    const SymbolTable & symbols, const NodeRegistry & nodes, const Scope * scope,
    const TypeEnvironment * env = nullptr)
  : symbols_(&symbols), nodes_(&nodes), scope_(scope), env_(env)
  {
  }

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
  const SymbolTable * symbols_ = nullptr;
  const NodeRegistry * nodes_ = nullptr;
  const Scope * scope_ = nullptr;
  const TypeEnvironment * env_ = nullptr;

  // Check a node statement recursively
  void check_node_stmt(
    const NodeStmt & node, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type,
    DiagnosticBag & diagnostics);

  // Check binary expression type compatibility
  TypeInferenceResult check_binary_expr(
    const BinaryExpr & expr, const TypeContext & ctx,
    const std::function<const Type *(std::string_view)> & get_global_type);
};

}  // namespace bt_dsl
