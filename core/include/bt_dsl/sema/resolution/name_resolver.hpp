// bt_dsl/sema/resolution/name_resolver.hpp - Name resolution visitor
//
// Resolves identifier references to their declarations across
// Type, Node, and Value namespaces with cross-module support.
//
#pragma once

#include <memory>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/visitor.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"

namespace bt_dsl
{

/**
 * Name resolution pass for BT-DSL semantic analysis.
 *
 * This visitor walks the AST and resolves all identifier references:
 * - VarRefExpr -> Symbol (Value-space)
 * - AssignmentStmt.target -> Symbol (Value-space)
 * - NodeStmt.nodeName -> NodeSymbol (Node-space)
 * - PrimaryType.name -> TypeSymbol (Type-space)
 *
 * Features:
 * - Import visibility: only public symbols (not starting with '_') are visible
 * - Ambiguity detection: error if multiple imports define the same public name
 * - Non-transitive imports: only direct imports are searched
 * - Shadowing detection: error if declaration hides a parent scope symbol
 *
 * Reference: docs/reference/semantics.md
 */
class NameResolver : public AstVisitor<NameResolver>
{
public:
  /**
   * Construct a NameResolver.
   *
   * @param module The module to resolve names in
   * @param diags DiagnosticBag for error reporting (may be nullptr)
   */
  NameResolver(ModuleInfo & module, DiagnosticBag * diags = nullptr)
  : module_(module), diags_(diags)
  {
  }

  // ===========================================================================
  // Entry Point
  // ===========================================================================

  /**
   * Resolve all names in the module.
   *
   * @return true if no errors occurred
   */
  bool resolve();

  // ===========================================================================
  // Cross-Module Lookup
  // ===========================================================================

  /**
   * Look up a type by name.
   *
   * Search order: local types → imported public types.
   * Reports error if ambiguous (multiple imports define same name).
   *
   * @param name The type name to look up
   * @param range Source range for error reporting
   * @return Pointer to TypeSymbol if found, nullptr otherwise
   */
  const TypeSymbol * lookup_type(std::string_view name, SourceRange range);

  /**
   * Look up a node by name.
   *
   * Search order: local nodes → imported public nodes.
   * Reports error if ambiguous.
   *
   * @param name The node name to look up
   * @param range Source range for error reporting
   * @return Pointer to NodeSymbol if found, nullptr otherwise
   */
  const NodeSymbol * lookup_node(std::string_view name, SourceRange range);

  /**
   * Look up a value by name.
   *
   * Search order: current scope chain → global scope → imported public values.
   * Reports error if ambiguous.
   *
   * @param name The value name to look up
   * @param scope Current scope for lookup
   * @param range Source range for error reporting
   * @return Pointer to Symbol if found, nullptr otherwise
   */
  const Symbol * lookup_value(std::string_view name, Scope * scope, SourceRange range);

  // ===========================================================================
  // Visitor Methods
  // ===========================================================================

  // Expressions
  void visit_var_ref_expr(VarRefExpr * node);
  void visit_binary_expr(BinaryExpr * node);
  void visit_unary_expr(UnaryExpr * node);
  void visit_cast_expr(CastExpr * node);
  void visit_index_expr(IndexExpr * node);
  void visit_array_literal_expr(ArrayLiteralExpr * node);
  void visit_array_repeat_expr(ArrayRepeatExpr * node);
  void visit_vec_macro_expr(VecMacroExpr * node);

  // Type nodes
  void visit_primary_type(PrimaryType * node);
  void visit_static_array_type(StaticArrayType * node);
  void visit_dynamic_array_type(DynamicArrayType * node);
  void visit_type_expr(TypeExpr * node);

  // Statements
  void visit_node_stmt(NodeStmt * node);
  void visit_assignment_stmt(AssignmentStmt * node);
  void visit_blackboard_decl_stmt(BlackboardDeclStmt * node);
  void visit_const_decl_stmt(ConstDeclStmt * node);

  // Declarations
  void visit_tree_decl(TreeDecl * node);

  // Supporting nodes
  void visit_argument(Argument * node);
  void visit_precondition(Precondition * node);
  void visit_param_decl(ParamDecl * node);
  void visit_extern_port(ExternPort * node);

  // ===========================================================================
  // Error State
  // ===========================================================================

  /// Check if any errors occurred during resolution
  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }

  /// Get the number of errors
  [[nodiscard]] size_t error_count() const noexcept { return error_count_; }

  /// Get the current scope
  [[nodiscard]] Scope * current_scope() const noexcept { return current_scope_; }

  /// Get the collected diagnostics (may be nullptr)
  [[nodiscard]] DiagnosticBag * diagnostics() noexcept { return diags_; }

private:
  // ===========================================================================
  // Internal Helpers
  // ===========================================================================

  /// Enter a new scope
  void push_scope(Scope * scope);

  /// Exit the current scope
  void pop_scope();

  /// Create and push a new block scope (for children_block)
  Scope * push_block_scope();

  /// Pop the current block scope
  void pop_block_scope();

  /// Check for shadowing and report error if found
  bool check_shadowing(std::string_view name, SourceRange range);

  /// Report an error
  void report_error(SourceRange range, std::string_view message);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  ModuleInfo & module_;
  DiagnosticBag * diags_;

  Scope * current_scope_ = nullptr;
  std::vector<std::unique_ptr<Scope>> block_scopes_;  ///< Block scopes for children_block
  bool has_errors_ = false;
  size_t error_count_ = 0;
};

}  // namespace bt_dsl
