// bt_dsl/sema/types/type_checker.hpp - Bidirectional type inference and checking
//
// Type checking pass that annotates all expressions with resolved types.
// Runs after ConstEvaluator in the semantic analysis pipeline.
//
#pragma once

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/type.hpp"
#include "bt_dsl/sema/types/type_table.hpp"

namespace bt_dsl
{

/**
 * Bidirectional type checker for BT-DSL.
 *
 * This pass performs type inference and checking on all expressions,
 * setting the resolvedType field on each Expr node.
 *
 * ## Algorithm (§3.2.3)
 *
 * 1. **Top-down (contextual typing)**: Propagate expected types downward
 *    - Variable declarations with type annotations
 *    - Function argument positions
 *
 * 2. **Bottom-up (synthesis)**: Infer types from sub-expressions
 *    - Literal types ({integer}, {float})
 *    - Operator result types
 *
 * 3. **Defaulting**: Apply defaults for unresolved placeholder types
 *    - {integer} → int32
 *    - {float} → float64
 *
 * ## Usage
 * ```cpp
 * TypeChecker checker(types, typeTable, values, &diags);
 * bool ok = checker.check(program);
 * // After this, all Expr::resolvedType fields are set
 * ```
 */
class TypeChecker
{
public:
  /**
   * Construct a TypeChecker.
   *
   * @param types TypeContext for type lookups and creation
   * @param typeTable TypeTable for resolving type names
   * @param values SymbolTable for variable type lookups
   * @param diags DiagnosticBag for error reporting (nullptr for silent mode)
   */
  TypeChecker(
    TypeContext & types, const TypeTable & typeTable, const SymbolTable & values,
    DiagnosticBag * diags = nullptr);

  // ===========================================================================
  // Entry Points
  // ===========================================================================

  /**
   * Type check an entire program.
   *
   * This walks all declarations and statements, inferring types
   * for all expressions and checking for type errors.
   *
   * @param program The program to check
   * @return true if no errors occurred
   */
  bool check(Program & program);

  /**
   * Infer and set the type of an expression (bottom-up).
   *
   * @param expr Expression to type
   * @return Inferred type (also set on expr->resolvedType)
   */
  const Type * check_expr(Expr * expr);

  /**
   * Check expression against an expected type (top-down).
   *
   * Uses bidirectional typing: the expected type influences
   * literal type resolution.
   *
   * @param expr Expression to check
   * @param expected Expected type from context
   * @return Resolved type (may differ from expected if compatible)
   */
  const Type * check_expr_with_expected(Expr * expr, const Type * expected);

  // ===========================================================================
  // Error State
  // ===========================================================================

  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }
  [[nodiscard]] size_t error_count() const noexcept { return error_count_; }

private:
  // ===========================================================================
  // Expression Type Inference
  // ===========================================================================

  const Type * infer_int_literal(IntLiteralExpr * node, const Type * expected);
  const Type * infer_float_literal(FloatLiteralExpr * node, const Type * expected);
  const Type * infer_string_literal(StringLiteralExpr * node, const Type * expected);
  const Type * infer_bool_literal(BoolLiteralExpr * node);
  const Type * infer_null_literal(NullLiteralExpr * node, const Type * expected);
  const Type * infer_var_ref(VarRefExpr * node);
  const Type * infer_binary_expr(BinaryExpr * node, const Type * expected);
  const Type * infer_unary_expr(UnaryExpr * node, const Type * expected);
  const Type * infer_cast_expr(CastExpr * node);
  const Type * infer_index_expr(IndexExpr * node);
  const Type * infer_array_literal(ArrayLiteralExpr * node, const Type * expected);
  const Type * infer_array_repeat(ArrayRepeatExpr * node, const Type * expected);
  const Type * infer_vec_macro(VecMacroExpr * node, const Type * expected);

  // ===========================================================================
  // Statement/Declaration Processing
  // ===========================================================================

  void check_stmt(Stmt * stmt);
  void check_node_stmt(NodeStmt * node);
  void check_assignment_stmt(AssignmentStmt * node);
  void check_blackboard_decl_stmt(BlackboardDeclStmt * node);
  void check_const_decl_stmt(ConstDeclStmt * node);

  void check_tree_decl(TreeDecl * decl);
  void check_global_var_decl(GlobalVarDecl * decl);
  void check_global_const_decl(GlobalConstDecl * decl);

  // ===========================================================================
  // Helper Methods
  // ===========================================================================

  /// Resolve a TypeNode AST to semantic Type
  const Type * resolve_type(const TypeNode * node);

  /// Get type of a symbol from SymbolTable
  const Type * get_symbol_type(const Symbol * sym);

  /// Apply defaults to placeholder types
  const Type * apply_defaults(const Type * type);

  /// Report an error
  void report_error(SourceRange range, std::string_view message);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  TypeContext & types_;
  const TypeTable & type_table_;
  const SymbolTable & values_;
  DiagnosticBag * diags_;

  bool has_errors_ = false;
  size_t error_count_ = 0;
};

}  // namespace bt_dsl
