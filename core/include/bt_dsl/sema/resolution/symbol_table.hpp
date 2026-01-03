// bt_dsl/sema/resolution/symbol_table.hpp - Scope and symbol management
//
// This header provides symbol table and scope management for semantic analysis.
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"

namespace bt_dsl
{

// ============================================================================
// Symbol Types
// ============================================================================

/**
 * Kind of symbol in the symbol table.
 */
enum class SymbolKind : uint8_t {
  GlobalVariable,  ///< var at program level
  GlobalConst,     ///< const at program level
  LocalVariable,   ///< var inside Tree
  LocalConst,      ///< const inside Tree/blocks
  BlockVariable,   ///< var declared in a children_block
  BlockConst,      ///< const declared in a children_block
  Parameter,       ///< Tree parameter
  Tree,            ///< Tree definition
  DeclaredNode,    ///< declare statement
};

/**
 * A symbol in the symbol table.
 */
struct Symbol
{
  std::string_view name;
  SymbolKind kind;
  std::optional<std::string_view> typeName;  ///< Explicit type if any
  std::optional<PortDirection> direction;    ///< For parameters (in/out/ref/mut)
  SourceRange definitionRange;               ///< Location of the symbol definition (byte offsets)

  /// Link back to AST node
  const AstNode * astNode = nullptr;

  // ===========================================================================
  // Helper Methods
  // ===========================================================================

  /// Check if this symbol is a variable (not const)
  [[nodiscard]] bool is_variable() const noexcept
  {
    return kind == SymbolKind::GlobalVariable || kind == SymbolKind::LocalVariable ||
           kind == SymbolKind::BlockVariable || kind == SymbolKind::Parameter;
  }

  /// Check if this symbol is a constant
  [[nodiscard]] bool is_const() const noexcept
  {
    return kind == SymbolKind::GlobalConst || kind == SymbolKind::LocalConst ||
           kind == SymbolKind::BlockConst;
  }

  /// Check if this symbol is writable (out or mut direction)
  [[nodiscard]] bool is_writable() const noexcept
  {
    if (!direction) {
      return false;
    }
    return *direction == PortDirection::Out || *direction == PortDirection::Mut;
  }

  /// Check if this symbol is a parameter
  [[nodiscard]] bool is_parameter() const noexcept { return kind == SymbolKind::Parameter; }

  /// Check if this symbol is global scope
  [[nodiscard]] bool is_global() const noexcept
  {
    return kind == SymbolKind::GlobalVariable || kind == SymbolKind::GlobalConst;
  }
};

// ============================================================================
// Transparent Hash/Equal for string_view keys
// ============================================================================

/// Transparent hash functor for string_view heterogeneous lookup
struct StringViewHash
{
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept
  {
    return std::hash<std::string_view>{}(sv);
  }
};

/// Transparent equality functor for string_view heterogeneous lookup
struct StringViewEqual
{
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

// ============================================================================
// Scope
// ============================================================================

/**
 * A lexical scope containing symbol definitions.
 *
 * Scopes form a hierarchy where child scopes can look up symbols in
 * parent scopes. This follows the standard lexical scoping rules.
 *
 * Note: Symbol names (keys) must be interned string_views with lifetime
 * guaranteed by AstContext (arena allocation).
 */
class Scope
{
public:
  /// Create a scope with optional parent
  explicit Scope(Scope * parent = nullptr) : parent_(parent) {}

  // ===========================================================================
  // Symbol Definition
  // ===========================================================================

  /**
   * Define a symbol in this scope.
   *
   * @param symbol The symbol to define (name must be interned)
   * @return true if defined successfully, false if name already exists
   */
  bool define(Symbol symbol)
  {
    auto [it, inserted] = symbols_.emplace(symbol.name, symbol);
    return inserted;
  }

  /**
   * Insert or overwrite a symbol in this scope.
   *
   * @param symbol The symbol to insert/update (name must be interned)
   */
  void upsert(Symbol symbol) { symbols_.insert_or_assign(symbol.name, symbol); }

  // ===========================================================================
  // Symbol Lookup
  // ===========================================================================

  /**
   * Look up a symbol by name in this scope only.
   *
   * @param name The symbol name to look up
   * @return Pointer to symbol if found, nullptr otherwise
   */
  [[nodiscard]] const Symbol * lookup_local(std::string_view name) const
  {
    auto it = symbols_.find(name);  // Heterogeneous lookup - no string copy
    return it != symbols_.end() ? &it->second : nullptr;
  }

  /**
   * Look up a symbol by name, searching parent scopes if not found locally.
   *
   * @param name The symbol name to look up
   * @return Pointer to symbol if found, nullptr otherwise
   */
  [[nodiscard]] const Symbol * lookup(std::string_view name) const
  {
    if (const Symbol * sym = lookup_local(name)) {
      return sym;
    }
    return parent_ ? parent_->lookup(name) : nullptr;
  }

  // ===========================================================================
  // Scope Properties
  // ===========================================================================

  /// Get the parent scope
  [[nodiscard]] Scope * get_parent() const noexcept { return parent_; }

  /// Get all symbols defined in this scope
  [[nodiscard]] const auto & get_symbols() const noexcept { return symbols_; }

  /// Check if this scope contains a symbol with the given name
  [[nodiscard]] bool contains(std::string_view name) const { return lookup_local(name) != nullptr; }

  /// Get the number of symbols in this scope (not including parent scopes)
  [[nodiscard]] size_t size() const noexcept { return symbols_.size(); }

  /// Check if this scope is empty
  [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }

private:
  Scope * parent_;
  std::unordered_map<std::string_view, Symbol, StringViewHash, StringViewEqual> symbols_;
};

// ============================================================================
// SymbolTable
// ============================================================================

/**
 * Symbol table managing Value-space scopes in a program.
 *
 * Reference: docs/reference/declarations-and-scopes.md 4.1.1
 * (Type / Node / Value namespaces are separate)
 *
 * Structure:
 * - One global scope for global value-space declarations (var/const)
 * - One scope per Tree definition for parameters and tree-local var/const
 */
class SymbolTable
{
public:
  SymbolTable() : global_scope_(std::make_unique<Scope>()) {}

  // ===========================================================================
  // Building the Symbol Table
  // ===========================================================================

  /**
   * Build symbol table from a parsed program.
   *
   * This populates the global scope and creates tree scopes.
   *
   * @param program The parsed program
   */
  void build_from_program(const Program & program);

  // ===========================================================================
  // Scope Access
  // ===========================================================================

  /// Get the global scope
  [[nodiscard]] Scope * get_global_scope() noexcept { return global_scope_.get(); }

  /// Get the global scope (const)
  [[nodiscard]] const Scope * get_global_scope() const noexcept { return global_scope_.get(); }

  /**
   * Get the scope for a specific tree.
   *
   * @param treeName The name of the tree
   * @return Pointer to scope if found, nullptr otherwise
   */
  [[nodiscard]] Scope * get_tree_scope(std::string_view tree_name);

  /// Get tree scope (const)
  [[nodiscard]] const Scope * get_tree_scope(std::string_view tree_name) const;

  // ===========================================================================
  // Symbol Resolution
  // ===========================================================================

  /**
   * Resolve a symbol name from a given scope context.
   *
   * Searches the given scope and its parents.
   *
   * @param name The symbol name to resolve
   * @param fromScope The scope to start searching from
   * @return Pointer to symbol if found, nullptr otherwise
   */
  [[nodiscard]] const Symbol * resolve(std::string_view name, const Scope * fromScope) const
  {
    return fromScope ? fromScope->lookup(name) : global_scope_->lookup(name);
  }

  // =========================================================================
  // Block Scopes (children_block)
  // =========================================================================

  /**
   * Create a new block scope whose lifetime is owned by this SymbolTable.
   *
   * Used by SymbolTableBuilder to pre-build scopes for children blocks so
   * later passes can just push/pop scopes via the raw pointer stored in AST.
   */
  [[nodiscard]] Scope * create_block_scope(Scope * parent)
  {
    auto scope = std::make_unique<Scope>(parent);
    Scope * ptr = scope.get();
    block_scopes_.push_back(std::move(scope));
    return ptr;
  }

  // ===========================================================================
  // Global Scope Operations
  // ===========================================================================

  /// Check if a global symbol exists
  [[nodiscard]] bool has_global(std::string_view name) const { return global_scope_->contains(name); }

  /// Get a global symbol by name
  [[nodiscard]] const Symbol * get_global(std::string_view name) const
  {
    return global_scope_->lookup_local(name);
  }

  /**
   * Define a global symbol.
   *
   * @param symbol The symbol to define
   * @return false if a symbol with the same name already exists
   */
  bool try_define_global(Symbol symbol) { return global_scope_->define(symbol); }

  /// Insert or overwrite a global symbol
  void upsert_global(Symbol symbol) { global_scope_->upsert(symbol); }

  // ===========================================================================
  // Iteration
  // ===========================================================================

  /// Get all tree names that have scopes
  [[nodiscard]] std::vector<std::string> get_tree_names() const;

private:
  /// Helper to build scope for a single tree
  void build_tree_scope(const TreeDecl & tree);

  std::unique_ptr<Scope> global_scope_;
  std::unordered_map<std::string_view, std::unique_ptr<Scope>, StringViewHash, StringViewEqual>
    tree_scopes_;

  // Owns all block scopes created during SymbolTableBuilder.
  std::vector<std::unique_ptr<Scope>> block_scopes_;
};

}  // namespace bt_dsl
