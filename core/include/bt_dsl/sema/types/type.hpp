// bt_dsl/sema/types/type.hpp - Semantic type representation
//
// Represents resolved types for semantic analysis.
// Shared between ConstEvaluator and TypeChecker phases.
//
#pragma once

#include <cstdint>
#include <deque>
#include <memory_resource>
#include <string_view>

namespace bt_dsl
{

class AstNode;

// ============================================================================
// Type Kind
// ============================================================================

/**
 * Kind of semantic type.
 */
enum class TypeKind {
  // Primitive types
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float32,
  Float64,
  Bool,
  String,
  BoundedString,  ///< string<N>

  // Composite types
  StaticArray,   ///< [T; N]
  BoundedArray,  ///< [T; <=N]
  DynamicArray,  ///< vec<T>

  // Nullable wrapper
  Nullable,  ///< T?

  // External type
  Extern,  ///< extern type

  // Inference placeholders (resolved during type checking)
  IntegerLiteral,  ///< {integer} - unresolved integer literal type
  FloatLiteral,    ///< {float} - unresolved float literal type
  NullLiteral,     ///< null literal type (base type unknown)
  Unknown,         ///< ? - unresolved type variable

  // Error
  Error,  ///< Error recovery placeholder
};

// ============================================================================
// Type
// ============================================================================

/**
 * Semantic type representation.
 *
 * Unlike AST TypeNode (syntactic representation), Type represents
 * the resolved semantic type after name resolution.
 *
 * Types are interned by TypeContext for efficient comparison.
 */
struct Type
{
  TypeKind kind;

  /// For BoundedString: max bytes
  /// For StaticArray/BoundedArray: size/max size
  uint64_t size = 0;

  /// For array types: element type
  const Type * element_type = nullptr;

  /// For Nullable: base type
  const Type * base_type = nullptr;

  /// For Extern: type name and declaration
  std::string_view name;
  const AstNode * decl = nullptr;

  // ===========================================================================
  // Type Queries
  // ===========================================================================

  /// Check if this is a signed integer type
  [[nodiscard]] bool is_signed_integer() const noexcept
  {
    return kind == TypeKind::Int8 || kind == TypeKind::Int16 || kind == TypeKind::Int32 ||
           kind == TypeKind::Int64;
  }

  /// Check if this is an unsigned integer type
  [[nodiscard]] bool is_unsigned_integer() const noexcept
  {
    return kind == TypeKind::UInt8 || kind == TypeKind::UInt16 || kind == TypeKind::UInt32 ||
           kind == TypeKind::UInt64;
  }

  /// Check if this is any integer type (including IntegerLiteral)
  [[nodiscard]] bool is_integer() const noexcept
  {
    return is_signed_integer() || is_unsigned_integer() || kind == TypeKind::IntegerLiteral;
  }

  /// Check if this is a floating point type (including FloatLiteral)
  [[nodiscard]] bool is_float() const noexcept
  {
    return kind == TypeKind::Float32 || kind == TypeKind::Float64 || kind == TypeKind::FloatLiteral;
  }

  /// Check if this is a numeric type
  [[nodiscard]] bool is_numeric() const noexcept { return is_integer() || is_float(); }

  /// Check if this is an array type
  [[nodiscard]] bool is_array() const noexcept
  {
    return kind == TypeKind::StaticArray || kind == TypeKind::BoundedArray ||
           kind == TypeKind::DynamicArray;
  }

  /// Check if this is a string type
  [[nodiscard]] bool is_string() const noexcept
  {
    return kind == TypeKind::String || kind == TypeKind::BoundedString;
  }

  /// Check if this is an error type
  [[nodiscard]] bool is_error() const noexcept { return kind == TypeKind::Error; }

  /// Check if this is nullable
  [[nodiscard]] bool is_nullable() const noexcept { return kind == TypeKind::Nullable; }

  /// Check if this is an inference placeholder type
  [[nodiscard]] bool is_placeholder() const noexcept
  {
    return kind == TypeKind::IntegerLiteral || kind == TypeKind::FloatLiteral ||
           kind == TypeKind::NullLiteral || kind == TypeKind::Unknown;
  }

  /// Get bit width for integer types (0 for non-integer)
  [[nodiscard]] int get_bit_width() const noexcept
  {
    switch (kind) {
      case TypeKind::Int8:
      case TypeKind::UInt8:
        return 8;
      case TypeKind::Int16:
      case TypeKind::UInt16:
        return 16;
      case TypeKind::Int32:
      case TypeKind::UInt32:
        return 32;
      case TypeKind::Int64:
      case TypeKind::UInt64:
        return 64;
      default:
        return 0;
    }
  }
};

// ============================================================================
// Type Context
// ============================================================================

/**
 * Type context for interning and managing semantic types.
 *
 * Provides singleton instances for built-in types and creates
 * interned composite types on demand.
 */
class TypeContext
{
public:
  TypeContext();

  // ===========================================================================
  // Built-in Types (Singletons)
  // ===========================================================================

  [[nodiscard]] const Type * int8_type() const noexcept { return &int8_; }
  [[nodiscard]] const Type * int16_type() const noexcept { return &int16_; }
  [[nodiscard]] const Type * int32_type() const noexcept { return &int32_; }
  [[nodiscard]] const Type * int64_type() const noexcept { return &int64_; }
  [[nodiscard]] const Type * uint8_type() const noexcept { return &uint8_; }
  [[nodiscard]] const Type * uint16_type() const noexcept { return &uint16_; }
  [[nodiscard]] const Type * uint32_type() const noexcept { return &uint32_; }
  [[nodiscard]] const Type * uint64_type() const noexcept { return &uint64_; }
  [[nodiscard]] const Type * float32_type() const noexcept { return &float32_; }
  [[nodiscard]] const Type * float64_type() const noexcept { return &float64_; }
  [[nodiscard]] const Type * bool_type() const noexcept { return &bool_; }
  [[nodiscard]] const Type * string_type() const noexcept { return &string_; }
  [[nodiscard]] const Type * error_type() const noexcept { return &error_; }

  // ===========================================================================
  // Inference Placeholder Types
  // ===========================================================================

  [[nodiscard]] const Type * integer_literal_type() const noexcept { return &integer_literal_; }
  [[nodiscard]] const Type * float_literal_type() const noexcept { return &float_literal_; }
  [[nodiscard]] const Type * null_literal_type() const noexcept { return &null_literal_; }
  [[nodiscard]] const Type * unknown_type() const noexcept { return &unknown_; }

  // ===========================================================================
  // Composite Type Creation (Interned)
  // ===========================================================================

  /// Get bounded string type: string<N>
  const Type * get_bounded_string_type(uint64_t max_bytes);

  /// Get static array type: [T; N]
  const Type * get_static_array_type(const Type * element_type, uint64_t size);

  /// Get bounded array type: [T; <=N]
  const Type * get_bounded_array_type(const Type * element_type, uint64_t max_size);

  /// Get dynamic array type: vec<T>
  const Type * get_dynamic_array_type(const Type * element_type);

  /// Get nullable type: T?
  const Type * get_nullable_type(const Type * base_type);

  /// Get extern type
  const Type * get_extern_type(std::string_view name, const AstNode * decl);

  // ===========================================================================
  // Type Lookup by Name
  // ===========================================================================

  /// Look up a built-in type by name (e.g., "int32", "float64")
  /// Returns nullptr if not a built-in type
  [[nodiscard]] const Type * lookup_builtin(std::string_view name) const;

private:
  // Built-in type singletons
  Type int8_, int16_, int32_, int64_;
  Type uint8_, uint16_, uint32_, uint64_;
  Type float32_, float64_;
  Type bool_, string_;
  Type error_;
  Type integer_literal_, float_literal_, null_literal_;
  Type unknown_;

  // Arena for composite types
  std::pmr::monotonic_buffer_resource arena_{4096};
  // NOTE: pointers to interned composite types are handed out widely.
  // We must use a container with stable element addresses.
  std::pmr::deque<Type> composite_types_{&arena_};
};

}  // namespace bt_dsl
