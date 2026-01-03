// bt_dsl/sema/types/const_value.hpp - Compile-time constant value representation
//
// Represents the result of constant expression evaluation.
//
#pragma once

#include <cstdint>
#include <gsl/span>
#include <optional>
#include <string_view>

namespace bt_dsl
{

struct Type;

// ============================================================================
// Constant Value Kind
// ============================================================================

/**
 * Kind of constant value.
 */
enum class ConstValueKind {
  Integer,  ///< 64-bit signed integer
  Float,    ///< 64-bit floating point
  Bool,     ///< Boolean
  String,   ///< String (arena-interned)
  Null,     ///< null literal
  Array,    ///< Array of constant values
  Error,    ///< Evaluation error (recovery placeholder)
};

// ============================================================================
// Constant Value
// ============================================================================

/**
 * Compile-time constant value.
 *
 * Represents the result of evaluating a const_expr. Values are stored
 * in their most general form:
 * - Integers as int64_t (actual type determined later)
 * - Floats as double
 * - Strings as interned string_view
 * - Arrays as span of ConstValue
 *
 * The `type` field may be set during evaluation for types that are
 * immediately known (e.g., bool), or left for type inference.
 */
class ConstValue
{
public:
  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /// Create an integer constant
  static ConstValue make_integer(int64_t value)
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Integer;
    v.intValue_ = value;
    return v;
  }

  /// Create a float constant
  static ConstValue make_float(double value)
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Float;
    v.floatValue_ = value;
    return v;
  }

  /// Create a boolean constant
  static ConstValue make_bool(bool value)
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Bool;
    v.boolValue_ = value;
    return v;
  }

  /// Create a string constant (value must be arena-interned)
  static ConstValue make_string(std::string_view value)
  {
    ConstValue v;
    v.kind_ = ConstValueKind::String;
    v.stringValue_ = value;
    return v;
  }

  /// Create a null constant
  static ConstValue make_null()
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Null;
    return v;
  }

  /// Create an array constant (elements must be arena-allocated)
  static ConstValue make_array(gsl::span<const ConstValue> elements)
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Array;
    v.arrayElements_ = elements;
    return v;
  }

  /// Create an error value (for error recovery)
  static ConstValue make_error()
  {
    ConstValue v;
    v.kind_ = ConstValueKind::Error;
    return v;
  }

  // ===========================================================================
  // Kind Queries
  // ===========================================================================

  [[nodiscard]] ConstValueKind kind() const noexcept { return kind_; }

  [[nodiscard]] bool is_error() const noexcept { return kind_ == ConstValueKind::Error; }

  [[nodiscard]] bool is_integer() const noexcept { return kind_ == ConstValueKind::Integer; }

  [[nodiscard]] bool is_float() const noexcept { return kind_ == ConstValueKind::Float; }

  [[nodiscard]] bool is_bool() const noexcept { return kind_ == ConstValueKind::Bool; }

  [[nodiscard]] bool is_string() const noexcept { return kind_ == ConstValueKind::String; }

  [[nodiscard]] bool is_null() const noexcept { return kind_ == ConstValueKind::Null; }

  [[nodiscard]] bool is_array() const noexcept { return kind_ == ConstValueKind::Array; }

  /// Check if this is a numeric value (integer or float)
  [[nodiscard]] bool is_numeric() const noexcept { return is_integer() || is_float(); }

  // ===========================================================================
  // Value Accessors
  // ===========================================================================

  /// Get integer value (only valid if is_integer())
  [[nodiscard]] int64_t as_integer() const noexcept { return intValue_; }

  /// Get float value (only valid if is_float())
  [[nodiscard]] double as_float() const noexcept { return floatValue_; }

  /// Get boolean value (only valid if is_bool())
  [[nodiscard]] bool as_bool() const noexcept { return boolValue_; }

  /// Get string value (only valid if is_string())
  [[nodiscard]] std::string_view as_string() const noexcept { return stringValue_; }

  /// Get array elements (only valid if is_array())
  [[nodiscard]] gsl::span<const ConstValue> as_array() const noexcept { return arrayElements_; }

  // ===========================================================================
  // Numeric Conversion
  // ===========================================================================

  /// Convert to integer if numeric (truncates floats)
  [[nodiscard]] std::optional<int64_t> to_integer() const
  {
    if (is_integer()) return intValue_;
    if (is_float()) return static_cast<int64_t>(floatValue_);
    return std::nullopt;
  }

  /// Convert to float if numeric
  [[nodiscard]] std::optional<double> to_float() const
  {
    if (is_float()) return floatValue_;
    if (is_integer()) return static_cast<double>(intValue_);
    return std::nullopt;
  }

  /// Check if this is a non-negative integer (valid for array sizes)
  [[nodiscard]] bool is_non_negative_integer() const noexcept { return is_integer() && intValue_ >= 0; }

  // ===========================================================================
  // Type Information
  // ===========================================================================

  /// Semantic type (may be set during evaluation or left for inference)
  const Type * type = nullptr;

  /// Default constructor creates an Error value
  ConstValue() = default;

private:
  ConstValueKind kind_ = ConstValueKind::Error;
  int64_t intValue_ = 0;
  double floatValue_ = 0.0;
  bool boolValue_ = false;
  std::string_view stringValue_;
  gsl::span<const ConstValue> arrayElements_;
};

}  // namespace bt_dsl
