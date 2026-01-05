// bt_dsl/sema/types/type_utils.hpp - Shared type inference utilities
//
// Common helpers for type inference used by both ConstEvaluator and TypeChecker.
//
#pragma once

#include <string>

#include "bt_dsl/sema/types/type.hpp"

namespace bt_dsl
{

// Forward declarations
class TypeContext;
class TypeTable;
class TypeSymbol;
class TypeNode;
class TypeExpr;
class PrimaryType;
class StaticArrayType;
class DynamicArrayType;

// ============================================================================
// Type Compatibility
// ============================================================================

/**
 * Check if source type can be assigned to target type.
 *
 * This follows the assignment compatibility rules from §3.3.1:
 * - Exact type match
 * - Implicit widening conversions
 * - Non-nullable to nullable
 * - Array compatibility (static to bounded, bounded to dynamic)
 *
 * @param target The type being assigned to
 * @param source The type being assigned from
 * @return true if assignment is allowed
 */
[[nodiscard]] bool is_assignable(const Type * target, const Type * source);

/**
 * Check if source type can be implicitly widened to target type.
 *
 * Widening conversions (§3.3.2):
 * - Smaller signed integer → larger signed integer
 * - Smaller unsigned integer → larger unsigned integer
 * - int8/int16/int32 → float32/float64 (may lose precision)
 * - float32 → float64
 *
 * @param from Source type
 * @param to Target type
 * @return true if widening is allowed
 */
[[nodiscard]] bool can_widen(const Type * from, const Type * to);

/**
 * Check if two types are equal for comparison purposes.
 *
 * Used for == and != operators (§3.3.7).
 */
[[nodiscard]] bool are_comparable(const Type * lhs, const Type * rhs);

/**
 * Convert a Type to its string representation.
 *
 * @param type The type to convert
 * @return String representation of the type (e.g. "int32", "vec<int32>")
 */
[[nodiscard]] std::string to_string(const Type * type);

// ============================================================================
// Common Type Computation
// ============================================================================

/**
 * Compute the common numeric type for binary operations.
 *
 * Following §3.4.2:
 * - Both operands are widened to a common type
 * - Result is the smallest type that can represent both
 * - Mixed signed/unsigned is an error (returns nullptr)
 *
 * @param types TypeContext for creating result types
 * @param lhs Left operand type
 * @param rhs Right operand type
 * @return Common type, or nullptr if incompatible
 */
[[nodiscard]] const Type * common_numeric_type(
  TypeContext & types, const Type * lhs, const Type * rhs);

// ============================================================================
// Literal Type Resolution
// ============================================================================

/**
 * Resolve an integer literal type to a concrete type.
 *
 * If expected is a concrete integer type, resolves to that type.
 * Otherwise, defaults to int32 (§3.2.1).
 *
 * @param types TypeContext for type lookups
 * @param expected Expected type from context (may be nullptr)
 * @return Resolved concrete integer type
 */
[[nodiscard]] const Type * resolve_integer_literal(TypeContext & types, const Type * expected);

/**
 * Resolve a float literal type to a concrete type.
 *
 * If expected is a concrete float type, resolves to that type.
 * Otherwise, defaults to float64 (§3.2.1).
 *
 * @param types TypeContext for type lookups
 * @param expected Expected type from context (may be nullptr)
 * @return Resolved concrete float type
 */
[[nodiscard]] const Type * resolve_float_literal(TypeContext & types, const Type * expected);

/**
 * Apply default types to any remaining placeholder types.
 *
 * - IntegerLiteral → int32
 * - FloatLiteral → float64
 * - Other types pass through unchanged
 *
 * @param types TypeContext for type lookups
 * @param type Type to apply defaults to
 * @return Resolved type with defaults applied
 */
[[nodiscard]] const Type * apply_defaults(TypeContext & types, const Type * type);

// ============================================================================
// Type Resolution from AST
// ============================================================================

/**
 * Resolve a TypeNode AST node to a semantic Type.
 *
 * @param types TypeContext for creating types
 * @param typeTable TypeTable for looking up type names
 * @param node The TypeNode AST to resolve
 * @return Resolved Type, or error type on failure
 */
[[nodiscard]] const Type * resolve_type_node(
  TypeContext & types, const TypeTable & typeTable, const TypeNode * node);

}  // namespace bt_dsl
