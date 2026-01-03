// bt_dsl/sema/analysis/cfg_builder.hpp - CFG Builder for BT-DSL
//
// Builds Control Flow Graph from AST, handling BT semantics:
// - DataPolicy (All/Any/None) for children block flow
// - FlowPolicy (Chained/Isolated) for sibling visibility
// - Preconditions (@guard, @success_if, etc.)
//
#pragma once

#include <memory>

#include "bt_dsl/sema/analysis/cfg.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"

namespace bt_dsl
{

/**
 * Builds CFG from AST for a single TreeDecl.
 *
 * ## BT Control Flow Semantics
 *
 * ### DataPolicy (children_block flow)
 * - **All (Sequence)**: `ChildSuccess` → next child, `ChildFailure` → parent failure
 * - **Any (Fallback)**: `ChildFailure` → next child, `ChildSuccess` → parent success
 * - **None**: Unconditional flow, child results don't affect parent
 *
 * ### FlowPolicy (sibling visibility)
 * - **Chained**: Previous sibling's state flows to next sibling
 * - **Isolated**: Each child starts with parent's initial state
 *
 * ## Usage
 * ```cpp
 * CFGBuilder builder(nodeRegistry);
 * auto cfg = builder.build(treeDecl);
 * ```
 */
class CFGBuilder
{
public:
  /**
   * Construct a CFGBuilder.
   * @param nodes NodeRegistry for looking up node metadata (DataPolicy/FlowPolicy)
   */
  explicit CFGBuilder(const NodeRegistry & nodes);

  /**
   * Build CFG for a tree declaration.
   * @param tree The tree to build CFG for
   * @return The constructed CFG (ownership transferred to caller)
   */
  std::unique_ptr<CFG> build(const TreeDecl * tree);

private:
  // ===========================================================================
  // Build Methods
  // ===========================================================================

  /**
   * Build blocks for a statement sequence.
   * @param stmts Statements to process
   * @param cfg The CFG being built
   * @param current Current block to add statements to
   * @param dataPolicy DataPolicy for the containing children_block
   * @param flowPolicy FlowPolicy for the containing children_block
   * @param contextEntry The entry block of the current context
   * @param successExit Where to go on success
   * @param failureExit Where to go on failure
   * @return Final block after processing all statements
   */
  BasicBlock * build_statements(
    gsl::span<Stmt * const> stmts, CFG & cfg, BasicBlock * current, DataPolicy data_policy,
    FlowPolicy flow_policy, BasicBlock * context_entry, BasicBlock * success_exit,
    BasicBlock * failure_exit);

  /**
   * Build blocks for a single statement.
   * @return Block to continue from after this statement
   */
  BasicBlock * build_statement(
    const Stmt * stmt, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
    BasicBlock * success_exit, BasicBlock * failure_exit);

  /**
   * Build blocks for a NodeStmt with potential children.
   */
  BasicBlock * build_node_stmt(
    const NodeStmt * node, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
    BasicBlock * success_exit, BasicBlock * failure_exit);

  /**
   * Build children block with DataPolicy/FlowPolicy semantics.
   */
  BasicBlock * build_children_block(
    const NodeStmt * node, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
    BasicBlock * success_exit, BasicBlock * failure_exit);

  /**
   * Build precondition handling.
   * Creates conditional edges for @guard, @success_if, @failure_if, @skip_if.
   * @return Block to continue from (the "enter body" block)
   */
  static BasicBlock * build_preconditions(
    gsl::span<Precondition * const> preconditions, CFG & cfg, BasicBlock * current,
    BasicBlock * context_entry, BasicBlock * success_exit, BasicBlock * failure_exit);

  // ===========================================================================
  // Helper Methods
  // ===========================================================================

  /// Get DataPolicy for a node (from BehaviorAttr or default)
  static DataPolicy get_data_policy(const NodeStmt * node);

  /// Get FlowPolicy for a node (from BehaviorAttr or default)
  static FlowPolicy get_flow_policy(const NodeStmt * node);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  const NodeRegistry & nodes_;
};

}  // namespace bt_dsl
