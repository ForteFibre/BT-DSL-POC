// bt_dsl/sema/cfg_builder.cpp - CFG Builder implementation
//
#include "bt_dsl/sema/analysis/cfg_builder.hpp"

#include "bt_dsl/basic/casting.hpp"

namespace bt_dsl
{

// ============================================================================
// Constructor
// ============================================================================

CFGBuilder::CFGBuilder(const NodeRegistry & nodes) : nodes_(nodes) {}

// ============================================================================
// Entry Point
// ============================================================================

std::unique_ptr<CFG> CFGBuilder::build(const TreeDecl * tree)
{
  if (tree == nullptr) {
    return nullptr;
  }

  auto cfg = std::make_unique<CFG>();
  cfg->tree = tree;

  // Create entry and exit blocks
  cfg->entry = cfg->create_block();
  cfg->exitSuccess = cfg->create_block();
  cfg->exitFailure = cfg->create_block();

  // Build the tree body
  // Tree body is treated as Sequence (DataPolicy::All, FlowPolicy::Chained)
  BasicBlock * current = cfg->entry;

  current = build_statements(
    tree->body, *cfg, current, DataPolicy::All, FlowPolicy::Chained, nullptr, cfg->exitSuccess,
    cfg->exitFailure);

  // If we reach the end of the tree body, it's a success
  if (current != nullptr && current != cfg->exitSuccess && current != cfg->exitFailure) {
    current->add_successor(cfg->exitSuccess, CFGEdgeKind::Unconditional);
  }

  return cfg;
}

// ============================================================================
// Statement Building
// ============================================================================

BasicBlock * CFGBuilder::build_statements(
  gsl::span<Stmt * const> stmts, CFG & cfg, BasicBlock * current, DataPolicy data_policy,
  FlowPolicy flow_policy, BasicBlock * context_entry, BasicBlock * success_exit,
  BasicBlock * failure_exit)
{
  // Note: flowPolicy is stored in BasicBlock metadata but doesn't affect CFG structure.
  // Data-flow analysis uses it to determine state propagation.
  if (stmts.empty() || current == nullptr) {
    return current;
  }

  // For Isolated flow policy, we need to track the initial state
  // but for CFG building, we just need the structure
  // The data-flow analysis will handle the state propagation

  for (size_t i = 0; i < stmts.size(); ++i) {
    const Stmt * stmt = stmts[i];
    const bool is_last = (i == stmts.size() - 1);

    // Put each statement into its own block, and connect the current block to it.
    // This keeps the entry block empty and yields the structure expected by tests.
    BasicBlock * stmt_block = cfg.create_block();
    stmt_block->contextEntry = context_entry;
    stmt_block->flowPolicy = flow_policy;
    stmt_block->dataPolicy = data_policy;
    current->add_successor(stmt_block, CFGEdgeKind::Unconditional);
    current = stmt_block;

    // Determine where to go after this statement based on DataPolicy
    BasicBlock * next_on_success = nullptr;
    BasicBlock * next_on_failure = nullptr;

    if (data_policy == DataPolicy::Any) {
      // Fallback semantics: continue on failure, exit on success
      next_on_success = success_exit;
      if (is_last) {
        next_on_failure = failure_exit;
      } else {
        next_on_failure = cfg.create_block();
        next_on_failure->contextEntry = context_entry;
        next_on_failure->flowPolicy = flow_policy;
        next_on_failure->dataPolicy = data_policy;
      }
    } else {
      // DataPolicy::All or None - Sequence semantics: continue on success, exit on failure
      if (is_last) {
        next_on_success = success_exit;
      } else {
        next_on_success = cfg.create_block();
        next_on_success->contextEntry = context_entry;
        next_on_success->flowPolicy = flow_policy;
        next_on_success->dataPolicy = data_policy;
      }
      next_on_failure = failure_exit;
    }

    // Build this statement
    build_statement(stmt, cfg, current, context_entry, next_on_success, next_on_failure);

    // Move to the next block based on DataPolicy
    // For All/None: continue on success path; For Any: continue on failure path
    current = (data_policy == DataPolicy::Any) ? next_on_failure : next_on_success;
  }

  return current;
}

BasicBlock * CFGBuilder::build_statement(
  const Stmt * stmt, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
  BasicBlock * success_exit, BasicBlock * failure_exit)
{
  if (stmt == nullptr || current == nullptr) {
    return current;
  }

  switch (stmt->get_kind()) {
    case NodeKind::NodeStmt:
      return build_node_stmt(
        cast<NodeStmt>(stmt), cfg, current, context_entry, success_exit, failure_exit);

    case NodeKind::AssignmentStmt:
    case NodeKind::BlackboardDeclStmt:
    case NodeKind::ConstDeclStmt:
    default:
      // Simple/unknown statements: add to current block and continue
      CFG::add_stmt(current, stmt);
      current->add_successor(success_exit, CFGEdgeKind::Unconditional);
      return success_exit;
  }
}

// ============================================================================
// Node Statement Building
// ============================================================================

BasicBlock * CFGBuilder::build_node_stmt(
  const NodeStmt * node, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
  BasicBlock * success_exit, BasicBlock * failure_exit)
{
  if (node == nullptr || current == nullptr) {
    return current;
  }

  // Handle preconditions first (may create conditional flow that skips the node)
  current = build_preconditions(
    node->preconditions, cfg, current, context_entry, success_exit, failure_exit);

  // Add the node statement itself to the current block
  CFG::add_stmt(current, node);

  // If the node has children, build the children block
  if (node->hasChildrenBlock && !node->children.empty()) {
    return build_children_block(node, cfg, current, context_entry, success_exit, failure_exit);
  }

  // No children: node is a leaf, create success/failure edges
  current->add_successor(success_exit, CFGEdgeKind::ChildSuccess);
  current->add_successor(failure_exit, CFGEdgeKind::ChildFailure);

  return success_exit;
}

BasicBlock * CFGBuilder::build_children_block(
  const NodeStmt * node, CFG & cfg, BasicBlock * current, BasicBlock * context_entry,
  BasicBlock * success_exit, BasicBlock * failure_exit)
{
  if (node == nullptr || node->children.empty()) {
    return current;
  }

  // Get DataPolicy and FlowPolicy for this node
  const DataPolicy data_policy = get_data_policy(node);
  const FlowPolicy flow_policy = get_flow_policy(node);

  // Create sub-blocks for children
  BasicBlock * children_entry = cfg.create_block();
  children_entry->dataPolicy = data_policy;
  children_entry->flowPolicy = flow_policy;
  children_entry->parentNode = node;
  // Use parent's context for the entry block itself, but pass childrenEntry as context for kids
  children_entry->contextEntry = context_entry;

  // Connect current block to children entry
  current->add_successor(children_entry, CFGEdgeKind::Unconditional);

  // Build children with appropriate DataPolicy semantics
  // Pass childrenEntry as the NEW context for the children
  BasicBlock * result = build_statements(
    node->children, cfg, children_entry, data_policy, flow_policy, children_entry, success_exit,
    failure_exit);

  return result;
}

// ============================================================================
// Precondition Building
// ============================================================================

BasicBlock * CFGBuilder::build_preconditions(
  gsl::span<Precondition * const> preconditions, CFG & cfg, BasicBlock * current,
  BasicBlock * context_entry, BasicBlock * success_exit, BasicBlock * failure_exit)
{
  if (preconditions.empty() || current == nullptr) {
    return current;
  }

  for (const auto * precond : preconditions) {
    if (precond == nullptr) {
      continue;
    }

    // Reference: execution-model.md §5.3.3
    // - @success_if(cond): if cond is true -> skip body and return Success
    // - @failure_if(cond): if cond is true -> skip body and return Failure
    // - @skip_if(cond): if cond is true -> skip body and return Skip (treated like Success for CFG)
    // - @run_while(cond): if cond becomes false -> return Skip (treated like Success for CFG)
    // - @guard(cond): if cond becomes false -> return Failure
    // Note: For static analysis we only need a conservative pre-execution split.

    BasicBlock * enter_body = cfg.create_block();
    enter_body->contextEntry = context_entry;

    BasicBlock * on_true = nullptr;
    BasicBlock * on_false = nullptr;

    switch (precond->kind) {
      case PreconditionKind::SuccessIf:
        on_true = success_exit;
        on_false = enter_body;
        break;
      case PreconditionKind::FailureIf:
        on_true = failure_exit;
        on_false = enter_body;
        break;
      case PreconditionKind::SkipIf:
        on_true = success_exit;
        on_false = enter_body;
        break;
      case PreconditionKind::RunWhile:
        // Pre-execution approximation: if false, treat as immediate Skip.
        on_true = enter_body;
        on_false = success_exit;
        break;
      case PreconditionKind::Guard:
        on_true = enter_body;
        on_false = failure_exit;
        break;
    }

    current->add_successor(on_true, CFGEdgeKind::GuardTrue, precond->condition);
    current->add_successor(on_false, CFGEdgeKind::GuardFalse, precond->condition);
    current = enter_body;
  }

  return current;
}

// ============================================================================
// Helper Methods
// ============================================================================

DataPolicy CFGBuilder::get_data_policy(const NodeStmt * node)
{
  if (node == nullptr || node->resolvedNode == nullptr) {
    return DataPolicy::All;  // Default
  }

  const AstNode * decl = node->resolvedNode->decl;
  if (decl == nullptr) {
    return DataPolicy::All;
  }

  // Check for ExternDecl with BehaviorAttr
  if (const auto * extern_decl = dyn_cast<ExternDecl>(decl)) {
    if (extern_decl->behaviorAttr != nullptr) {
      return extern_decl->behaviorAttr->dataPolicy;
    }
  }

  return DataPolicy::All;
}

FlowPolicy CFGBuilder::get_flow_policy(const NodeStmt * node)
{
  if (node == nullptr || node->resolvedNode == nullptr) {
    return FlowPolicy::Chained;  // Default
  }

  const AstNode * decl = node->resolvedNode->decl;
  if (decl == nullptr) {
    return FlowPolicy::Chained;
  }

  // Check for ExternDecl with BehaviorAttr
  if (const auto * extern_decl = dyn_cast<ExternDecl>(decl)) {
    if (extern_decl->behaviorAttr != nullptr && extern_decl->behaviorAttr->flowPolicy.has_value()) {
      return *extern_decl->behaviorAttr->flowPolicy;
    }
  }

  return FlowPolicy::Chained;
}

}  // namespace bt_dsl
