// bt_dsl/sema/resolution/symbol_table_builder.hpp - Symbol table construction
//
// Traverses the AST to collect all symbols and build scope structures.
// Runs before NameResolver to ensure all symbols are registered.
//
#pragma once

#include "bt_dsl/ast/visitor.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/type_table.hpp"

namespace bt_dsl
{

/**
 * Builds symbol tables by traversing the AST.
 *
 * This class handles all symbol registration and scope construction:
 * - Global scope: global vars, global consts
 * - Tree scopes: parameters, local vars/consts
 * - Block scopes: children_block variables, inline declarations
 *
 * After this pass completes, NameResolver only needs to resolve references
 * (e.g., VarRefExpr -> Symbol) without registering any new symbols.
 *
 * ## Processing Order
 * 1. Register extern types and type aliases (TypeTable)
 * 2. Register extern nodes and trees (NodeRegistry)
 * 3. Build value-space scopes (SymbolTable)
 *    - Global scope
 *    - Tree scopes (parameters + body)
 *    - Block scopes (children_block contents)
 */
class SymbolTableBuilder : public AstVisitor<SymbolTableBuilder>
{
public:
  /**
   * Construct a SymbolTableBuilder.
   *
   * @param values SymbolTable to populate with value symbols
   * @param types TypeTable to populate with type symbols
   * @param nodes NodeRegistry to populate with node symbols
   * @param diags DiagnosticBag for error reporting (nullptr for silent mode)
   */
  SymbolTableBuilder(
    SymbolTable & values, TypeTable & types, NodeRegistry & nodes, DiagnosticBag * diags = nullptr);

  // ===========================================================================
  // Entry Point
  // ===========================================================================

  /**
   * Build symbol tables from a program.
   *
   * @param program The program to process
   * @return true if no errors occurred
   */
  bool build(Program & program);

  // ===========================================================================
  // Error State
  // ===========================================================================

  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }
  [[nodiscard]] size_t error_count() const noexcept { return error_count_; }

  // ===========================================================================
  // Visitor Methods
  // ===========================================================================

  // Statements
  void visit_node_stmt(NodeStmt * node);
  void visit_blackboard_decl_stmt(BlackboardDeclStmt * node);
  void visit_const_decl_stmt(ConstDeclStmt * node);
  void visit_assignment_stmt(AssignmentStmt * node);

  // Supporting nodes
  void visit_argument(Argument * node);

private:
  // ===========================================================================
  // Internal Helpers
  // ===========================================================================

  /// Build tree scope (parameters + body)
  void build_tree_scope(TreeDecl * tree);

  /// Check for shadowing and report warning if found
  bool check_shadowing(std::string_view name, SourceRange range);

  /// Report a redefinition error with previous definition location
  void report_redefinition(
    SourceRange range, SourceRange prev_range, std::string_view name, std::string_view kind);

  /// Report a shadowing warning/error with previous declaration location
  void report_shadowing(SourceRange range, SourceRange prev_range, std::string_view name);

  /// Report an error
  void report_error(SourceRange range, std::string_view message);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  SymbolTable & values_;
  TypeTable & types_;
  NodeRegistry & nodes_;
  DiagnosticBag * diags_;

  Scope * current_scope_ = nullptr;
  bool has_errors_ = false;
  size_t error_count_ = 0;
};

}  // namespace bt_dsl
