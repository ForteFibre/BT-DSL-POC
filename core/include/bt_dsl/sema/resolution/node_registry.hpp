// bt_dsl/sema/resolution/node_registry.hpp - Node namespace symbol table
//
// Manages node declarations (extern nodes, tree definitions).
//
#pragma once

#include <string_view>
#include <unordered_map>

#include "bt_dsl/ast/ast.hpp"

namespace bt_dsl
{

// ============================================================================
// Node Symbol
// ============================================================================

/**
 * A symbol in the Node namespace.
 */
struct NodeSymbol
{
  std::string_view name;
  const AstNode * decl = nullptr;  ///< ExternDecl or TreeDecl

  /// Check if this is an extern node declaration
  [[nodiscard]] bool is_extern_node() const noexcept
  {
    return decl && decl->get_kind() == NodeKind::ExternDecl;
  }

  /// Check if this is a tree definition
  [[nodiscard]] bool is_tree() const noexcept
  {
    return decl && decl->get_kind() == NodeKind::TreeDecl;
  }
};

// ============================================================================
// Node Registry
// ============================================================================

/// Transparent hash functor for string_view heterogeneous lookup
struct NodeRegistryHash
{
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept
  {
    return std::hash<std::string_view>{}(sv);
  }
};

/// Transparent equality functor for string_view heterogeneous lookup
struct NodeRegistryEqual
{
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

/**
 * Node namespace symbol table.
 *
 * Manages:
 * - extern node declarations
 * - tree definitions
 *
 * Reference: docs/reference/declarations-and-scopes.md 4.1.1
 */
class NodeRegistry
{
public:
  NodeRegistry() = default;

  // ===========================================================================
  // Symbol Definition
  // ===========================================================================

  /**
   * Define a node symbol.
   *
   * @param symbol The symbol to define (name must be interned)
   * @return true if defined successfully, false if name already exists
   */
  bool define(NodeSymbol symbol)
  {
    auto [it, inserted] = symbols_.emplace(symbol.name, symbol);
    return inserted;
  }

  // ===========================================================================
  // Symbol Lookup
  // ===========================================================================

  /**
   * Look up a node by name.
   *
   * @param name The node name to look up
   * @return Pointer to symbol if found, nullptr otherwise
   */
  [[nodiscard]] const NodeSymbol * lookup(std::string_view name) const
  {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
  }

  /**
   * Check if a node with the given name exists.
   */
  [[nodiscard]] bool contains(std::string_view name) const { return lookup(name) != nullptr; }

  /**
   * Get the number of registered nodes.
   */
  [[nodiscard]] size_t size() const noexcept { return symbols_.size(); }

private:
  std::unordered_map<std::string_view, NodeSymbol, NodeRegistryHash, NodeRegistryEqual> symbols_;
};

}  // namespace bt_dsl
