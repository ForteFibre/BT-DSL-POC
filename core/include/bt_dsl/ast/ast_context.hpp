// bt_dsl/ast/ast_context.hpp - AST arena allocator and string pool
//
// This header provides the AstContext class which owns all AST nodes
// and interned strings.
//
// Uses std::pmr::monotonic_buffer_resource for efficient arena allocation.
//
#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <gsl/span>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bt_dsl
{

// Forward declaration
class AstNode;

// ============================================================================
// AstContext - PMR Arena Allocator and String Pool
// ============================================================================

/**
 * Context that owns all AST nodes and interned strings using PMR.
 *
 * All AST nodes created through this context are valid as long as the
 * context is alive. The context uses a monotonic buffer resource for
 * efficient arena-style allocation.
 *
 * Design rationale:
 * - Uses std::pmr::monotonic_buffer_resource for O(1) allocation
 * - No individual deallocation - memory freed when context is destroyed
 * - Nodes are allocated contiguously for better cache locality
 * - String interning reduces memory for repeated identifiers
 * - Destructor order is guaranteed (nodes destroyed in reverse order)
 *
 * Example:
 * @code
 *   AstContext ctx;
 *   auto* lit = ctx.create<IntLiteralExpr>(42);
 *   auto* name = ctx.intern("foo");  // returns stable string_view
 * @endcode
 */
class AstContext
{
public:
  /// Default initial buffer size (64KB)
  static constexpr size_t k_default_buffer_size = size_t{64} * size_t{1024};

  /**
   * Create an AstContext with specified initial buffer size.
   *
   * @param initialBufferSize Initial arena buffer size in bytes
   */
  explicit AstContext(size_t initialBufferSize = k_default_buffer_size)
  : arena_(initialBufferSize), allocator_(&arena_), stringPool_(&arena_)
  {
  }  // Set uses arena for internal allocations

  /**
   * Destructor - memory is freed when arena is destroyed.
   * Nodes are assumed to be trivially destructible (no dynamic allocations).
   */
  ~AstContext() = default;

  // Non-copyable and non-movable (PMR resources are not movable)
  AstContext(const AstContext &) = delete;
  AstContext & operator=(const AstContext &) = delete;
  AstContext(AstContext &&) = delete;
  AstContext & operator=(AstContext &&) = delete;

  // ===========================================================================
  // Node Creation
  // ===========================================================================

  /**
   * Create a new AST node of type T.
   *
   * The context takes ownership of the node. The node is valid until
   * the context is destroyed. Memory is allocated from the arena.
   *
   * @tparam T The concrete AST node type to create (must derive from AstNode)
   * @tparam Args Constructor argument types
   * @param args Arguments forwarded to T's constructor
   * @return Non-owning pointer to the created node
   *
   * Example:
   *   auto* lit = ctx.create<IntLiteralExpr>(42, range);
   *   auto* bin = ctx.create<BinaryExpr>(lhs, BinaryOp::Add, rhs, range);
   */
  template <typename T, typename... Args>
  T * create(Args &&... args)
  {
    static_assert(std::is_base_of_v<AstNode, T>, "T must derive from AstNode");

    // Ensure node is safe to manage by arena (no non-trivial destructor)
    static_assert(
      std::is_trivially_destructible_v<T>,
      "AST Node must be trivially destructible to be managed by Arena! "
      "Use std::string_view instead of std::string, gsl::span instead of std::vector.");

    // Allocate memory from arena (C++17 compatible)
    void * const mem = arena_.allocate(sizeof(T), alignof(T));

    // Construct in-place (no destructor tracking needed)
    T * const node = new (mem) T(std::forward<Args>(args)...);

    return node;
  }

  // ===========================================================================
  // String Interning
  // ===========================================================================

  /**
   * Intern a string and return a stable string_view.
   *
   * If the string was previously interned, returns a view to the existing
   * string. Otherwise, stores a copy and returns a view to it.
   *
   * The returned string_view is valid as long as the context is alive.
   *
   * @param s The string to intern
   * @return A stable string_view to the interned string
   *
   * Example:
   *   std::string_view name1 = ctx.intern("foo");
   *   std::string_view name2 = ctx.intern("foo");
   *   assert(name1.data() == name2.data());  // Same pointer
   */
  [[nodiscard]] std::string_view intern(std::string_view s)
  {
    // O(1) lookup - string_view keys allow direct hash comparison
    auto it = stringPool_.find(s);
    if (it != stringPool_.end()) {
      return *it;
    }

    // Copy string data to arena (only when not found)
    char * const ptr = static_cast<char *>(arena_.allocate(s.size(), 1));
    std::memcpy(ptr, s.data(), s.size());

    // Insert arena-backed string_view into the set
    const std::string_view stored_view(ptr, s.size());
    stringPool_.insert(stored_view);

    return stored_view;
  }

  /**
   * Check if a string has been interned.
   *
   * @param s The string to check
   * @return true if the string is in the pool
   */
  [[nodiscard]] bool is_interned(std::string_view s) const
  {
    return stringPool_.find(s) != stringPool_.end();
  }

  // ===========================================================================
  // Array Allocation
  // ===========================================================================

  /**
   * Allocate an uninitialized array of type T from the arena.
   *
   * @tparam T The element type
   * @param size Number of elements to allocate
   * @return A span over the allocated (uninitialized) memory
   */
  template <typename T>
  [[nodiscard]] gsl::span<T> allocate_array(size_t size)
  {
    if (size == 0) return {};
    // C++17 compatible: use arena_.allocate directly
    T * const ptr = static_cast<T *>(
      arena_.allocate(sizeof(T) * size, alignof(T)));  // NOLINT(bugprone-sizeof-expression)
    // Value-initialize elements (nullptr for pointers, 0 for arithmetic types).
    // The implementation details live in the standard library (non-user code).
    std::uninitialized_value_construct_n(ptr, size);
    return gsl::span<T>(ptr, size);
  }

  /**
   * Copy elements from a vector to an arena-allocated array.
   *
   * @tparam T The element type
   * @param vec The source vector
   * @return A span over the arena-allocated copy
   */
  template <typename T>
  [[nodiscard]] gsl::span<T> copy_to_arena(const std::vector<T> & vec)
  {
    auto span = allocate_array<T>(vec.size());
    std::uninitialized_copy(vec.begin(), vec.end(), span.begin());
    return span;
  }

  /**
   * Get the number of interned strings.
   */
  [[nodiscard]] size_t get_string_count() const noexcept { return stringPool_.size(); }

  /**
   * Get the polymorphic allocator for external use.
   *
   * This can be used to allocate auxiliary data structures that should
   * share the same arena lifetime.
   */
  [[nodiscard]] std::pmr::polymorphic_allocator<std::byte> get_allocator() const noexcept
  {
    return allocator_;
  }

  /// Arena allocator - memory is freed only when destroyed
  std::pmr::monotonic_buffer_resource arena_;

  /// Polymorphic allocator using the arena
  std::pmr::polymorphic_allocator<std::byte> allocator_;

  /// Interned strings - keys are string_views pointing to arena memory
  std::pmr::unordered_set<std::string_view> stringPool_;
};

}  // namespace bt_dsl
