// bt_dsl/sema/type_utils.cpp - Shared type inference utilities implementation
//
#include "bt_dsl/sema/types/type_utils.hpp"

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/sema/types/type_table.hpp"

namespace bt_dsl
{

// ============================================================================
// Type Compatibility
// ============================================================================

bool is_assignable(const Type * target, const Type * source)
{
  if (!target || !source) return false;

  // Exact match
  if (target == source) return true;

  // Error types are assignable anywhere (error recovery)
  if (source->is_error() || target->is_error()) return true;

  // Placeholder types are tentatively assignable (will be resolved later)
  if (source->is_placeholder()) return true;

  // Non-nullable can be assigned to nullable of same base
  if (target->is_nullable()) {
    const Type * target_base = target->base_type;
    if (source->is_nullable()) {
      return is_assignable(target_base, source->base_type);
    }
    return is_assignable(target_base, source);
  }

  // Widening conversions
  if (can_widen(source, target)) return true;

  // Bounded string compatibility (§3.3.6)
  // - string<N> -> string<M> is allowed iff N <= M
  // - string<N> -> string is allowed
  // - string -> string<N> is NOT allowed (handled by contextual typing for literals)
  if (target->kind == TypeKind::String && source->kind == TypeKind::BoundedString) {
    return true;
  }
  if (target->kind == TypeKind::BoundedString && source->kind == TypeKind::BoundedString) {
    return source->size <= target->size;
  }

  // Array compatibility
  // [T; N] → [T; <=M] where N <= M
  if (target->kind == TypeKind::BoundedArray && source->kind == TypeKind::StaticArray) {
    if (is_assignable(target->element_type, source->element_type)) {
      return source->size <= target->size;
    }
  }

  // [T; <=N] → [T; <=M] where N <= M
  if (target->kind == TypeKind::BoundedArray && source->kind == TypeKind::BoundedArray) {
    if (is_assignable(target->element_type, source->element_type)) {
      return source->size <= target->size;
    }
  }

  // NOTE: Implicit conversions between static/bounded arrays and vec<T> are forbidden.
  // Reference: docs/reference/type-system/compatibility-and-conversion.md §3.3.5

  return false;
}

bool can_widen(const Type * from, const Type * to)
{
  if (!from || !to) return false;
  if (from == to) return true;

  // Integer literal can widen to any integer or float type
  if (from->kind == TypeKind::IntegerLiteral) {
    return to->is_integer() || to->is_float();
  }

  // Float literal can widen to any float type
  if (from->kind == TypeKind::FloatLiteral) {
    return to->is_float();
  }

  // Signed integer widening
  if (from->is_signed_integer() && to->is_signed_integer()) {
    return from->get_bit_width() <= to->get_bit_width();
  }

  // Unsigned integer widening
  if (from->is_unsigned_integer() && to->is_unsigned_integer()) {
    return from->get_bit_width() <= to->get_bit_width();
  }

  // Integer to float (may lose precision for large integers)
  if (from->is_signed_integer() && to->is_float()) {
    // int8, int16, int32 → float32 is allowed
    // int64 → float64 is allowed
    if (to->kind == TypeKind::Float32) {
      return from->get_bit_width() <= 32;
    }
    if (to->kind == TypeKind::Float64) {
      return true;  // Any integer can widen to float64
    }
  }

  // Float widening: float32 → float64
  if (from->kind == TypeKind::Float32 && to->kind == TypeKind::Float64) {
    return true;
  }

  return false;
}

bool are_comparable(const Type * lhs, const Type * rhs)
{
  if (!lhs || !rhs) return false;
  if (lhs == rhs) return true;

  // Error types are comparable (error recovery)
  if (lhs->is_error() || rhs->is_error()) return true;

  // Placeholder types are comparable during inference
  if (lhs->is_placeholder() || rhs->is_placeholder()) return true;

  // Numeric types are comparable if they can be widened to a common type
  if (lhs->is_numeric() && rhs->is_numeric()) {
    // Check if there's a common type
    return (can_widen(lhs, rhs) || can_widen(rhs, lhs));
  }

  // Same kind of array
  if (lhs->is_array() && rhs->is_array()) {
    return are_comparable(lhs->element_type, rhs->element_type);
  }

  // String types
  if (lhs->is_string() && rhs->is_string()) return true;

  // Bool types
  if (lhs->kind == TypeKind::Bool && rhs->kind == TypeKind::Bool) return true;

  // Nullable comparison (null can compare with any nullable)
  if (lhs->kind == TypeKind::NullLiteral && rhs->is_nullable()) return true;
  if (rhs->kind == TypeKind::NullLiteral && lhs->is_nullable()) return true;

  return false;
}

// ============================================================================
// Common Type Computation
// ============================================================================

const Type * common_numeric_type(TypeContext & types, const Type * lhs, const Type * rhs)
{
  if (!lhs || !rhs) return types.error_type();

  // Handle placeholder types
  if (lhs->kind == TypeKind::IntegerLiteral && rhs->kind == TypeKind::IntegerLiteral) {
    return types.integer_literal_type();  // Both are literals, stay as literal
  }
  if (lhs->kind == TypeKind::FloatLiteral && rhs->kind == TypeKind::FloatLiteral) {
    return types.float_literal_type();
  }

  // Integer literal + concrete type → concrete type
  if (lhs->kind == TypeKind::IntegerLiteral && rhs->is_numeric()) {
    return rhs;
  }
  if (rhs->kind == TypeKind::IntegerLiteral && lhs->is_numeric()) {
    return lhs;
  }

  // Float literal + concrete type → float type
  if (lhs->kind == TypeKind::FloatLiteral) {
    if (rhs->is_float()) return rhs;
    if (rhs->is_integer()) return types.float64_type();  // Integer + float literal → float64
    if (rhs->kind == TypeKind::IntegerLiteral) return types.float_literal_type();
  }
  if (rhs->kind == TypeKind::FloatLiteral) {
    if (lhs->is_float()) return lhs;
    if (lhs->is_integer()) return types.float64_type();
    if (lhs->kind == TypeKind::IntegerLiteral) return types.float_literal_type();
  }

  // Both concrete
  if (!lhs->is_numeric() || !rhs->is_numeric()) {
    return nullptr;  // Not numeric types
  }

  // Mixed signed/unsigned is an error (§3.4.2)
  if (lhs->is_signed_integer() && rhs->is_unsigned_integer()) return nullptr;
  if (lhs->is_unsigned_integer() && rhs->is_signed_integer()) return nullptr;

  // Float takes precedence
  if (lhs->is_float() || rhs->is_float()) {
    // Return the larger float type
    if (lhs->kind == TypeKind::Float64 || rhs->kind == TypeKind::Float64) {
      return types.float64_type();
    }
    return types.float32_type();
  }

  // Both integers of same signedness - return larger
  const int lhs_bits = lhs->get_bit_width();
  const int rhs_bits = rhs->get_bit_width();
  const int max_bits = (lhs_bits > rhs_bits) ? lhs_bits : rhs_bits;

  if (lhs->is_signed_integer()) {
    // Signed integer
    switch (max_bits) {
      case 8:
        return types.int8_type();
      case 16:
        return types.int16_type();
      case 32:
        return types.int32_type();
      case 64:
        return types.int64_type();
      default:
        return types.int32_type();
    }
  } else {
    // Unsigned integer
    switch (max_bits) {
      case 8:
        return types.uint8_type();
      case 16:
        return types.uint16_type();
      case 32:
        return types.uint32_type();
      case 64:
        return types.uint64_type();
      default:
        return types.uint32_type();
    }
  }
}

// ============================================================================
// Literal Type Resolution
// ============================================================================

const Type * resolve_integer_literal(TypeContext & types, const Type * expected)
{
  if (!expected) return types.int32_type();  // Default

  // If expected is a concrete integer type, use it
  if (expected->is_integer()) return expected;

  // If expected is float, integer literal can be widened
  if (expected->is_float()) return expected;

  // Default to int32
  return types.int32_type();
}

const Type * resolve_float_literal(TypeContext & types, const Type * expected)
{
  if (!expected) return types.float64_type();  // Default

  // If expected is a concrete float type, use it
  if (expected->is_float()) return expected;

  // Default to float64
  return types.float64_type();
}

const Type * apply_defaults(TypeContext & types, const Type * type)
{
  if (!type) return types.error_type();

  switch (type->kind) {
    case TypeKind::IntegerLiteral:
      return types.int32_type();
    case TypeKind::FloatLiteral:
      return types.float64_type();
    case TypeKind::NullLiteral:
      // Null literal without context - error (should have been resolved)
      return types.error_type();
    default:
      return type;
  }
}

// ============================================================================
// Type Resolution from AST
// ============================================================================

const Type * resolve_type_node(
  TypeContext & types, const TypeTable & typeTable, const TypeNode * node)
{
  if (!node) return types.error_type();

  switch (node->get_kind()) {
    case NodeKind::InferType:
      // Type inference placeholder - will be resolved later
      return nullptr;  // Caller handles inference

    case NodeKind::PrimaryType: {
      const auto * primary = static_cast<const PrimaryType *>(node);
      // First try builtin lookup
      if (const Type * builtin = types.lookup_builtin(primary->name)) {
        // Handle bounded string: string<N>
        if (primary->size.has_value() && builtin->kind == TypeKind::String) {
          // Parse the size as a number
          uint64_t max_bytes = 0;
          for (const char c : *primary->size) {
            if (c >= '0' && c <= '9') {
              max_bytes = (max_bytes * 10) + static_cast<uint64_t>(c - '0');
            }
          }
          return types.get_bounded_string_type(max_bytes);
        }
        return builtin;
      }
      // Look up in TypeTable for extern types
      if (const TypeSymbol * sym = typeTable.lookup(primary->name)) {
        if (sym->is_extern_type()) {
          return types.get_extern_type(sym->name, sym->decl);
        }
        // Type alias - would need to resolve the aliased type
        // For now, treat as error
      }
      return types.error_type();  // Unknown type
    }

    case NodeKind::StaticArrayType: {
      const auto * arr = static_cast<const StaticArrayType *>(node);
      const Type * elem_type = resolve_type_node(types, typeTable, arr->elementType);
      if (!elem_type) return types.error_type();

      // Parse size
      uint64_t size = 0;
      for (const char c : arr->size) {
        if (c >= '0' && c <= '9') {
          size = (size * 10) + static_cast<uint64_t>(c - '0');
        }
      }

      if (arr->isBounded) {
        return types.get_bounded_array_type(elem_type, size);
      } else {
        return types.get_static_array_type(elem_type, size);
      }
    }

    case NodeKind::DynamicArrayType: {
      const auto * arr = static_cast<const DynamicArrayType *>(node);
      const Type * elem_type = resolve_type_node(types, typeTable, arr->elementType);
      if (!elem_type) return types.error_type();
      return types.get_dynamic_array_type(elem_type);
    }

    case NodeKind::TypeExpr: {
      const auto * expr = static_cast<const TypeExpr *>(node);
      const Type * base = resolve_type_node(types, typeTable, expr->base);
      if (!base) return nullptr;  // Inference
      if (base->is_error()) return base;
      if (expr->nullable) {
        return types.get_nullable_type(base);
      }
      return base;
    }

    default:
      return types.error_type();
  }
}

}  // namespace bt_dsl
