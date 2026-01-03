// bt_dsl/sema/types/const_evaluator.hpp - Constant expression evaluator
//
// Evaluates const_expr at compile time per specification §4.3.
//
#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/const_value.hpp"
#include "bt_dsl/sema/types/type.hpp"

namespace bt_dsl
{

class AstContext;

/**
 * Constant expression evaluator.
 *
 * Evaluates const_expr at compile time following specification §4.3.
 *
 * ## Evaluable Expressions (§4.3.4)
 * - Literals (integer, float, string, bool, null)
 * - const references (including forward references)
 * - Parenthesized expressions
 * - Unary operations (-, !)
 * - Binary operations (+, -, *, /, %, comparisons, logical, bitwise)
 * - Cast expressions (except to extern type)
 * - Array literals ([...], [e; N])
 *
 * ## Error Conditions
 * - Division by zero
 * - Circular references
 * - Reference to runtime values (Blackboard, parameters)
 * - Use of vec![...]
 *
 * ## Usage
 * ```cpp
 * ConstEvaluator eval(astCtx, typeCtx, symbols, diags);
 * eval.evaluate_program(program);
 *
 * // Or evaluate a single expression
 * ConstValue val = eval.evaluate(expr);
 * ```
 */
class ConstEvaluator
{
public:
  /**
   * Construct a ConstEvaluator.
   *
   * @param astCtx AstContext for arena allocation of ConstValue arrays
   * @param typeCtx TypeContext for type lookups
   * @param values SymbolTable for const resolution
   * @param diags DiagnosticBag for error reporting (nullptr for silent mode)
   */
  ConstEvaluator(
    AstContext & ast_ctx, TypeContext & type_ctx, const SymbolTable & values,
    DiagnosticBag * diags = nullptr);

  // ===========================================================================
  // Entry Points
  // ===========================================================================

  /**
   * Evaluate all constants in a program.
   *
   * This method:
   * 1. Collects all const declarations
   * 2. Builds dependency graph and detects cycles
   * 3. Evaluates in topological order
   * 4. Stores results in AST nodes (evaluatedValue field)
   *
   * @param program The program to evaluate
   * @return true if no errors occurred
   */
  bool evaluate_program(Program & program);

  /**
   * Evaluate a single expression as a constant.
   *
   * @param expr Expression to evaluate
   * @return Constant value (ConstValue::Error on failure)
   */
  ConstValue evaluate(const Expr * expr);

  /**
   * Evaluate an expression as an array size (non-negative integer).
   *
   * @param expr Expression to evaluate
   * @param range Source range for error reporting
   * @return Size value, or nullopt on error
   */
  std::optional<uint64_t> evaluate_array_size(const Expr * expr, SourceRange range);

  // ===========================================================================
  // Error State
  // ===========================================================================

  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }
  [[nodiscard]] size_t error_count() const noexcept { return error_count_; }

private:
  // ===========================================================================
  // Expression Evaluation
  // ===========================================================================

  ConstValue eval_expr(const Expr * expr);
  ConstValue eval_int_literal(const IntLiteralExpr * node);
  ConstValue eval_float_literal(const FloatLiteralExpr * node);
  ConstValue eval_string_literal(const StringLiteralExpr * node);
  ConstValue eval_bool_literal(const BoolLiteralExpr * node);
  ConstValue eval_null_literal(const NullLiteralExpr * node);
  ConstValue eval_var_ref(const VarRefExpr * node);
  ConstValue eval_binary_expr(const BinaryExpr * node);
  ConstValue eval_unary_expr(const UnaryExpr * node);
  ConstValue eval_cast_expr(const CastExpr * node);
  ConstValue eval_index_expr(const IndexExpr * node);
  ConstValue eval_array_literal(const ArrayLiteralExpr * node);
  ConstValue eval_array_repeat(const ArrayRepeatExpr * node);
  ConstValue eval_vec_macro(const VecMacroExpr * node);

  // ===========================================================================
  // Binary Operation Helpers
  // ===========================================================================

  ConstValue eval_arithmetic(
    BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range);
  ConstValue eval_comparison(BinaryOp op, const ConstValue & lhs, const ConstValue & rhs);
  ConstValue eval_logical(
    BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range);
  ConstValue eval_bitwise(
    BinaryOp op, const ConstValue & lhs, const ConstValue & rhs, SourceRange range);

  // ===========================================================================
  // Dependency Analysis
  // ===========================================================================

  /// Collect const names referenced by an expression
  void collect_dependencies(const Expr * expr, std::unordered_set<std::string_view> & deps);

  /// Build evaluation order via topological sort
  /// Returns empty vector if cycle detected (errors already reported)
  std::vector<const AstNode *> build_evaluation_order(gsl::span<GlobalConstDecl *> global_consts);

  // ===========================================================================
  // Helper Methods
  // ===========================================================================

  /// Get the const value for a symbol (from cache or AST node)
  static const ConstValue * get_const_for_symbol(const Symbol * sym);

  /// Evaluate local consts within a statement (recursive into NodeStmt children)
  void evaluate_local_consts(Stmt * stmt, const Scope * current_scope);

  /// Evaluate default arguments (extern ports and tree parameters) as const_expr
  /// per spec (§4.3.1).
  void evaluate_default_args(Program & program);

  /// Report an error
  void report_error(SourceRange range, std::string_view message);

  /// Store a value in the AST arena and return a stable pointer.
  const ConstValue * store_in_arena(const ConstValue & v);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  AstContext & ast_ctx_;
  TypeContext & type_ctx_;
  const SymbolTable & values_;
  DiagnosticBag * diags_;

  /// Cache of evaluated constants (symbol -> stable arena pointer)
  std::unordered_map<const Symbol *, const ConstValue *> const_cache_;

  /// Constants currently being evaluated (for cycle detection)
  std::unordered_set<const Symbol *> evaluating_;

  bool has_errors_ = false;
  size_t error_count_ = 0;
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Get evaluated constant value from a Symbol.
 *
 * Uses Symbol's astNode to access the evaluatedValue field
 * on GlobalConstDecl or ConstDeclStmt.
 *
 * @param sym Symbol to look up
 * @return Pointer to ConstValue, or nullptr if not a const or not evaluated
 */
const ConstValue * get_const_value(const Symbol * sym);

}  // namespace bt_dsl
