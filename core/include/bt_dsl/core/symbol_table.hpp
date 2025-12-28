// bt_dsl/symbol_table.hpp - Scope and symbol management
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bt_dsl/core/ast.hpp"

namespace bt_dsl
{

// ============================================================================
// Symbol Types
// ============================================================================

/**
 * Kind of symbol in the symbol table.
 */
enum class SymbolKind : uint8_t {
  GlobalVariable,  // var at program level
  LocalVariable,   // var inside Tree
  Parameter,       // Tree parameter
  Tree,            // Tree definition
  DeclaredNode,    // declare statement
};

/**
 * A symbol in the symbol table.
 */
struct Symbol
{
  std::string name;
  SymbolKind kind;
  std::optional<std::string> type_name;    // Explicit type if any
  std::optional<PortDirection> direction;  // For parameters (in/out/ref)
  SourceRange definition_range;

  // For linking back to AST (using void* to avoid template complexity)
  const void * ast_node = nullptr;

  // Helper methods
  [[nodiscard]] bool is_variable() const;
  [[nodiscard]] bool is_writable() const;  // out or ref direction
};

// ============================================================================
// Scope
// ============================================================================

/**
 * A lexical scope containing symbol definitions.
 */
class Scope
{
public:
  explicit Scope(Scope * parent = nullptr);

  /**
   * Define a symbol in this scope.
   * @return true if defined successfully, false if name already exists
   */
  bool define(Symbol symbol);

  /**
   * Lookup a symbol by name in this scope only.
   */
  const Symbol * lookup_local(std::string_view name) const;

  /**
   * Lookup a symbol by name, searching parent scopes if not found locally.
   */
  const Symbol * lookup(std::string_view name) const;

  /**
   * Get the parent scope.
   */
  Scope * parent() const { return parent_; }

  /**
   * Get all symbols defined in this scope.
   */
  const std::unordered_map<std::string, Symbol> & symbols() const { return symbols_; }

  /**
   * Check if this scope contains a symbol with the given name.
   */
  bool contains(std::string_view name) const;

private:
  Scope * parent_;
  std::unordered_map<std::string, Symbol> symbols_;
};

// ============================================================================
// Symbol Table
// ============================================================================

/**
 * Symbol table managing all scopes in a program.
 *
 * Structure:
 * - One global scope for global variables and declarations
 * - One scope per Tree definition for parameters and local variables
 */
class SymbolTable
{
public:
  SymbolTable();

  /**
   * Build symbol table from a parsed program.
   * This populates the global scope and creates tree scopes.
   */
  void build_from_program(const Program & program);

  /**
   * Get the global scope.
   */
  Scope * global_scope() { return global_scope_.get(); }
  const Scope * global_scope() const { return global_scope_.get(); }

  /**
   * Get the scope for a specific tree.
   * @return nullptr if tree not found
   */
  Scope * tree_scope(std::string_view tree_name);
  const Scope * tree_scope(std::string_view tree_name) const;

  /**
   * Resolve a symbol name from a given scope context.
   * Searches the given scope and its parents.
   */
  const Symbol * resolve(std::string_view name, const Scope * from_scope) const;

  /**
   * Get all tree names that have scopes.
   */
  std::vector<std::string> tree_names() const;

  /**
   * Check if a global symbol exists.
   */
  bool has_global(std::string_view name) const;

  /**
   * Get a global symbol by name.
   */
  const Symbol * get_global(std::string_view name) const;

private:
  std::unique_ptr<Scope> global_scope_;
  std::unordered_map<std::string, std::unique_ptr<Scope>> tree_scopes_;

  // Helper to build scope for a single tree
  void build_tree_scope(const TreeDef & tree);
};

}  // namespace bt_dsl
