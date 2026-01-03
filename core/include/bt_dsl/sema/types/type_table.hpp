// bt_dsl/sema/types/type_table.hpp - Type namespace symbol table
//
// Manages type declarations (extern type, type alias, builtins).
//
#pragma once

#include <string_view>
#include <unordered_map>

#include "bt_dsl/ast/ast.hpp"

namespace bt_dsl
{

// ============================================================================
// Type Symbol
// ============================================================================

/**
 * A symbol in the Type namespace.
 */
struct TypeSymbol
{
  std::string_view name;
  const AstNode * decl = nullptr;  ///< ExternTypeDecl, TypeAliasDecl, or nullptr for builtins
  bool is_builtin = false;

  /// Check if this is a built-in type
  [[nodiscard]] bool is_builtin_type() const noexcept { return is_builtin; }

  /// Check if this is an extern type
  [[nodiscard]] bool is_extern_type() const noexcept
  {
    return decl && decl->get_kind() == NodeKind::ExternTypeDecl;
  }

  /// Check if this is a type alias
  [[nodiscard]] bool is_type_alias() const noexcept
  {
    return decl && decl->get_kind() == NodeKind::TypeAliasDecl;
  }
};

// ============================================================================
// Type Table
// ============================================================================

/// Transparent hash functor for string_view heterogeneous lookup
struct TypeTableHash
{
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept
  {
    return std::hash<std::string_view>{}(sv);
  }
};

/// Transparent equality functor for string_view heterogeneous lookup
struct TypeTableEqual
{
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

/**
 * Type namespace symbol table.
 *
 * Manages:
 * - Built-in types (int32, float32, bool, string, etc.)
 * - Built-in type aliases (int→int32, char→uint8, etc.)
 * - extern type declarations
 * - type alias declarations
 *
 * Reference: docs/reference/declarations-and-scopes.md 4.1.1
 * Reference: docs/reference/type-system/type-definitions.md §3.1.4.1
 */
class TypeTable
{
public:
  TypeTable() = default;

  // ===========================================================================
  // Built-in Registration
  // ===========================================================================

  /**
   * Register all built-in types and aliases.
   *
   * This should be called before processing any user code.
   */
  void register_builtins()
  {
    // Canonical signed integer types
    register_builtin("int8");
    register_builtin("int16");
    register_builtin("int32");
    register_builtin("int64");

    // Canonical unsigned integer types
    register_builtin("uint8");
    register_builtin("uint16");
    register_builtin("uint32");
    register_builtin("uint64");

    // Canonical float types
    register_builtin("float32");
    register_builtin("float64");

    // Other primitives
    register_builtin("bool");
    register_builtin("string");
    register_builtin("pose");

    // Built-in aliases (per spec §3.1.4.1)
    // These resolve to canonical types transparently
    register_alias("int", "int32");
    register_alias("float", "float32");
    register_alias("double", "float64");
    register_alias("byte", "uint8");
    register_alias("char", "uint8");
  }

  // ===========================================================================
  // Symbol Definition
  // ===========================================================================

  /**
   * Define a type symbol.
   *
   * @param symbol The symbol to define (name must be interned)
   * @return true if defined successfully, false if name already exists
   */
  bool define(TypeSymbol symbol)
  {
    auto [it, inserted] = symbols_.emplace(symbol.name, symbol);
    return inserted;
  }

  // ===========================================================================
  // Symbol Lookup
  // ===========================================================================

  /**
   * Look up a type by name, resolving aliases to their canonical types.
   *
   * @param name The type name to look up
   * @return Pointer to symbol if found, nullptr otherwise
   */
  [[nodiscard]] const TypeSymbol * lookup(std::string_view name) const
  {
    // First check if name is an alias
    auto alias_it = aliases_.find(name);
    if (alias_it != aliases_.end()) {
      name = alias_it->second;  // Resolve to canonical name
    }

    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
  }

  /**
   * Check if a type with the given name exists.
   */
  [[nodiscard]] bool contains(std::string_view name) const { return lookup(name) != nullptr; }

  /**
   * Get the number of registered types (excluding aliases).
   */
  [[nodiscard]] size_t size() const noexcept { return symbols_.size(); }

  /**
   * Get the canonical name for a type (resolves aliases).
   *
   * @param name The type name (possibly an alias)
   * @return The canonical name, or the original name if not an alias
   */
  [[nodiscard]] std::string_view canonical_name(std::string_view name) const
  {
    auto alias_it = aliases_.find(name);
    return alias_it != aliases_.end() ? alias_it->second : name;
  }

private:
  void register_builtin(std::string_view name)
  {
    TypeSymbol sym;
    sym.name = name;
    sym.decl = nullptr;
    sym.is_builtin = true;
    symbols_.emplace(name, sym);
  }

  void register_alias(std::string_view alias_name, std::string_view canonical_name)
  {
    aliases_.emplace(alias_name, canonical_name);
  }

  std::unordered_map<std::string_view, TypeSymbol, TypeTableHash, TypeTableEqual> symbols_;
  std::unordered_map<std::string_view, std::string_view, TypeTableHash, TypeTableEqual> aliases_;
};

}  // namespace bt_dsl
