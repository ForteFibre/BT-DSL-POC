// bt_dsl/sema/analysis/null_checker.hpp - Null safety data flow analysis
//
// This pass checks that nullable variables are not dereferenced without
// a check, following the null safety rules in §6.2.
//
// Runs after InitializationChecker.
//
#pragma once

#include <unordered_set>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/analysis/cfg.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"

namespace bt_dsl
{

// ============================================================================
// Null Safety Checker
// ============================================================================

/**
 * Null Safety State.
 * Tracks variables that are *known to be non-null*.
 * Variables not in the set are considered nullable (if their type is nullable).
 */
using NullStateSet = std::unordered_set<std::string_view>;

/**
 * Null safety checker for BT-DSL.
 *
 * Verifies that:
 * - Variables passed to non-nullable ports are known to be non-null.
 * - Field access / index access on nullable types is guarded.
 *
 * Implements flow-sensitive analysis (narrowing).
 */
class NullChecker
{
public:
  NullChecker(
    const SymbolTable & values, const NodeRegistry & nodes, DiagnosticBag * diags = nullptr);

  // ===========================================================================
  // Entry Point
  // ===========================================================================

  bool check(const Program & program);
  void check(const TreeDecl * tree, const CFG & cfg);

  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }

private:
  // ===========================================================================
  // Analysis
  // ===========================================================================

  void analyze_data_flow(const CFG & cfg, NullStateSet & initial_state);

  // Transfer functions
  void transfer_block(const BasicBlock * block, NullStateSet & state, bool report_errors = true);
  static void transfer_edge(
    const BasicBlock::Edge & edge, const BasicBlock * source, NullStateSet & state);

  // Checks
  void check_node_args(const NodeStmt * node, const NullStateSet & state);
  void check_stmt(const Stmt * stmt, NullStateSet & state, bool report_errors);

  // Helpers
  static std::string_view get_var_name_from_expr(const Expr * expr);

  void report_error(SourceRange range, std::string_view message);

  // ===========================================================================
  // Members
  // ===========================================================================

  const SymbolTable & values_;
  const NodeRegistry & nodes_;
  DiagnosticBag * diags_;

  bool has_errors_ = false;
  size_t error_count_ = 0;
};

}  // namespace bt_dsl
