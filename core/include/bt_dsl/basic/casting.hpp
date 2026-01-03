// bt_dsl/basic/casting.hpp - LLVM-style RTTI casting utilities
//
// This header provides type-safe casting utilities.
// These work with any class hierarchy that implements the `classof`
// static method pattern.
//
// Usage:
//   if (isa<BinaryExpr>(node)) { ... }
//   auto* bin = cast<BinaryExpr>(node);      // asserts on failure
//   if (auto* bin = dyn_cast<BinaryExpr>(node)) { ... }  // nullptr on failure
//
#pragma once

#include <cassert>
#include <type_traits>

namespace bt_dsl
{

// Forward declaration
class AstNode;

// ============================================================================
// Type Traits for RTTI Support
// ============================================================================

namespace detail
{

/// Check if T has a classof static method
template <typename T, typename From, typename = void>
struct HasClassof : std::false_type
{
};

template <typename T, typename From>
struct HasClassof<T, From, std::void_t<decltype(T::classof(std::declval<const From *>()))>>
: std::true_type
{
};

template <typename T, typename From>
inline constexpr bool has_classof_v = HasClassof<T, From>::value;

}  // namespace detail

// ============================================================================
// isa<T> - Type checking
// ============================================================================

/**
 * Check if a node is of type T.
 *
 * @tparam T The target type to check for
 * @param node The node to check (may be nullptr)
 * @return true if node is of type T, false otherwise (including if node is null)
 *
 * Example:
 *   if (isa<BinaryExpr>(node)) {
 *     // node is definitely a BinaryExpr
 *   }
 */
template <typename T, typename From>
[[nodiscard]] inline bool isa(const From * node) noexcept
{
  static_assert(detail::has_classof_v<T, From>, "Target type must have a classof() static method");
  return node != nullptr && T::classof(node);
}

/// isa for non-const pointers
template <typename T, typename From>
[[nodiscard]] inline bool isa(From * node) noexcept
{
  return isa<T>(static_cast<const From *>(node));
}

// ============================================================================
// cast<T> - Unchecked cast (asserts on failure)
// ============================================================================

/**
 * Cast a node to type T, asserting on failure.
 *
 * @tparam T The target type to cast to
 * @param node The node to cast (must not be nullptr, must be of type T)
 * @return Pointer to the node as type T
 *
 * @note This will assert if node is nullptr or not of type T.
 *       Use dyn_cast if you need safe casting.
 *
 * Example:
 *   auto* bin = cast<BinaryExpr>(node);  // asserts if not BinaryExpr
 */
template <typename T, typename From>
[[nodiscard]] inline T * cast(From * node) noexcept
{
  assert(node != nullptr && "cast<T>() called with nullptr");
  assert(isa<T>(node) && "Invalid cast");
  return static_cast<T *>(node);
}

template <typename T, typename From>
[[nodiscard]] inline const T * cast(const From * node) noexcept
{
  assert(node != nullptr && "cast<T>() called with nullptr");
  assert(isa<T>(node) && "Invalid cast");
  return static_cast<const T *>(node);
}

// ============================================================================
// dyn_cast<T> - Safe dynamic cast (returns nullptr on failure)
// ============================================================================

/**
 * Safely cast a node to type T, returning nullptr on failure.
 *
 * @tparam T The target type to cast to
 * @param node The node to cast (may be nullptr)
 * @return Pointer to the node as type T, or nullptr if cast fails
 *
 * Example:
 *   if (auto* bin = dyn_cast<BinaryExpr>(node)) {
 *     // Use bin safely
 *   }
 */
template <typename T, typename From>
[[nodiscard]] inline T * dyn_cast(From * node) noexcept
{
  return isa<T>(node) ? static_cast<T *>(node) : nullptr;
}

template <typename T, typename From>
[[nodiscard]] inline const T * dyn_cast(const From * node) noexcept
{
  return isa<T>(node) ? static_cast<const T *>(node) : nullptr;
}

// ============================================================================
// cast_or_null<T> - Cast allowing null input
// ============================================================================

/**
 * Cast a node to type T, allowing nullptr input.
 *
 * @tparam T The target type to cast to
 * @param node The node to cast (may be nullptr)
 * @return Pointer to the node as type T, or nullptr if input was nullptr
 *
 * @note Unlike cast<T>, this accepts nullptr. However, if node is non-null,
 *       it must be of type T (asserts otherwise).
 *
 * Example:
 *   auto* bin = cast_or_null<BinaryExpr>(maybeNull);
 */
template <typename T, typename From>
[[nodiscard]] inline T * cast_or_null(From * node) noexcept
{
  return node != nullptr ? cast<T>(node) : nullptr;
}

template <typename T, typename From>
[[nodiscard]] inline const T * cast_or_null(const From * node) noexcept
{
  return node != nullptr ? cast<T>(node) : nullptr;
}

// ============================================================================
// dyn_cast_or_null<T> - Safe dynamic cast allowing null input
// ============================================================================

/**
 * Safely cast a node to type T, explicitly handling nullptr input.
 *
 * This is semantically equivalent to dyn_cast but makes the intent clearer
 * when the input is expected to potentially be nullptr.
 *
 * @tparam T The target type to cast to
 * @param node The node to cast (may be nullptr)
 * @return Pointer to the node as type T, or nullptr if cast fails or input is null
 */
template <typename T, typename From>
[[nodiscard]] inline T * dyn_cast_or_null(From * node) noexcept
{
  return dyn_cast<T>(node);
}

template <typename T, typename From>
[[nodiscard]] inline const T * dyn_cast_or_null(const From * node) noexcept
{
  return dyn_cast<T>(node);
}

}  // namespace bt_dsl
