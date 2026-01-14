// bt_dsl/sema/analysis/init_checker.hpp - Initialization safety data flow analysis
//
// This pass checks that all variables are properly initialized before use,
// following the initialization safety rules in §6.1.
//
// Runs after TypeChecker in the semantic analysis pipeline.
//
#pragma once

#include <unordered_map>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/analysis/cfg.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"

namespace bt_dsl
{

// ============================================================================
// Initialization State
// ============================================================================

/**
 * Initialization state of a variable.
 *
 * Reference: docs/reference/diagnostics.md §5.1.2 (状態)
 */
enum class InitState : uint8_t {
  Uninit,  ///< Uninitialized
  Init,    ///< Initialized
};

// Forward declarations
// (BasicBlock and CFG are included via cfg.hpp at top level)

/// Type for tracking variable initialization state by name
using InitStateMap = std::unordered_map<std::string_view, InitState>;

// ============================================================================
// Initialization Checker
// ============================================================================

/**
 * Initialization safety checker for BT-DSL.
 *
 * This pass performs data flow analysis to verify that:
 * - Variables passed to `in`/`ref`/`mut` ports are initialized
 * - Variables are tracked through control flow based on DataPolicy/FlowPolicy
 *
 * ## Algorithm (§6.1.5)
 *
 * 1. Track initialization state of each variable by name
 * 2. For node calls, check arguments against port directions
 * 3. Apply DataPolicy rules when merging child results
 * 4. Apply FlowPolicy rules for sibling visibility
 * 5. Handle precondition skips (no out writes if skipped)
 *
 * ## Usage
 * ```cpp
 * InitializationChecker checker(values, nodes, &diags);
 * bool ok = checker.check(program);
 * ```
 */
class InitializationChecker
{
public:
  /**
   * Construct an InitializationChecker.
   *
   * @param values SymbolTable for variable lookups
   * @param nodes NodeRegistry for node metadata lookups
   * @param diags DiagnosticBag for error reporting (nullptr for silent mode)
   */
  InitializationChecker(
    const SymbolTable & values, const NodeRegistry & nodes, DiagnosticBag * diags = nullptr);

  // ===========================================================================
  // Entry Point
  // ===========================================================================

  /**
   * Check initialization safety for an entire program.
   *
   * @param program The program to check
   * @return true if no errors occurred
   */
  bool check(const Program & program);

  /**
   * Check initialization safety for a single tree using its CFG.
   *
   * @param tree The tree declaration
   * @param cfg The control flow graph for the tree
   */
  void check(const TreeDecl * tree, const CFG & cfg);

  // ===========================================================================
  // Error State
  // ===========================================================================

  [[nodiscard]] bool has_errors() const noexcept { return hasErrors_; }
  [[nodiscard]] size_t error_count() const noexcept { return errorCount_; }

private:
  // ===========================================================================
  // Analysis Methods
  // ===========================================================================

  /// Run forward data-flow analysis on CFG
  void analyze_data_flow(const CFG & cfg, InitStateMap & initial_state);

  /// Transfer function for a basic block
  /// Updates blockState based on statements in the block
  void transfer_block(const BasicBlock * block, InitStateMap & state, bool report_errors = true);

  /// Check argument initialization requirements
  void check_node_args(const NodeStmt * node, const InitStateMap & state);

  /// Check a statement (part of transfer function)
  void check_stmt(const Stmt * stmt, InitStateMap & state, bool report_errors = true);

  /// Check an expression for uninitialized usage
  void check_expr(const Expr * expr, const InitStateMap & state, bool report_errors = true);

  /// Transfer function for an edge
  /// Updates state based on the edge kind and source block (e.g. out params on success)
  static void transfer_edge(
    const BasicBlock::Edge & edge, const BasicBlock * source, InitStateMap & state);

  // ===========================================================================
  // Helper Methods
  // ===========================================================================

  /// Get the variable name from an expression (VarRefExpr or IndexExpr)
  static std::string_view get_var_name_from_expr(const Expr * expr);

  /// Report an error
  void report_error(SourceRange range, std::string_view message);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  const SymbolTable & values_;
  const NodeRegistry & nodes_;
  DiagnosticBag * diags_;

  bool hasErrors_ = false;
  size_t errorCount_ = 0;
};

}  // namespace bt_dsl
