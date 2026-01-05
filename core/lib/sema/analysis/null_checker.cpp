// bt_dsl/sema/null_checker.cpp - Null safety checker implementation
//
#include "bt_dsl/sema/analysis/null_checker.hpp"

#include <deque>
#include <optional>

#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/sema/analysis/cfg_builder.hpp"
#include "bt_dsl/sema/types/type.hpp"

namespace bt_dsl
{

namespace
{

struct PortContract
{
  PortDirection direction = PortDirection::In;
  bool isNullable = false;
};

std::optional<PortContract> get_port_contract(const NodeStmt * node, const Argument * arg)
{
  if (node == nullptr || arg == nullptr) return std::nullopt;
  if (node->resolvedNode == nullptr || node->resolvedNode->decl == nullptr) return std::nullopt;

  const AstNode * decl = node->resolvedNode->decl;

  if (const auto * ext = dyn_cast<ExternDecl>(decl)) {
    for (const auto * port : ext->ports) {
      if (port == nullptr) continue;
      if (port->name != arg->name) continue;

      PortContract c;
      c.direction = port->direction.value_or(PortDirection::In);
      c.isNullable = (port->type != nullptr) ? port->type->nullable : false;
      return c;
    }
    return std::nullopt;
  }

  if (const auto * tree = dyn_cast<TreeDecl>(decl)) {
    for (const auto * param : tree->params) {
      if (param == nullptr) continue;
      if (param->name != arg->name) continue;

      PortContract c;
      c.direction = param->direction.value_or(PortDirection::In);
      c.isNullable = (param->type != nullptr) ? param->type->nullable : false;
      return c;
    }
    return std::nullopt;
  }

  return std::nullopt;
}

// Spec §6.2.2: Extract necessary conditions for expr to be true/false.
// We only derive facts that are sound on the given branch.
//
// Supported patterns:
// - null comparisons: x == null, x != null
// - conjunction: (a && b)
// - negation: !p (handled by flipping branchTruth and recursing)
//
// Intentionally conservative:
// - For (a && b) == false, we do not infer facts (since !a || !b).
std::string_view get_var_name_from_expr_local(const Expr * expr)
{
  if (expr == nullptr) return {};
  if (const auto * var = dyn_cast<VarRefExpr>(expr)) return var->name;
  if (const auto * index_expr = dyn_cast<IndexExpr>(expr))
    return get_var_name_from_expr_local(index_expr->base);
  return {};
}

void apply_null_facts_from_condition(const Expr * expr, bool branch_truth, NullStateSet & state)
{
  if (expr == nullptr) return;

  if (const auto * unary = dyn_cast<UnaryExpr>(expr)) {
    if (unary->op == UnaryOp::Not) {
      apply_null_facts_from_condition(unary->operand, !branch_truth, state);
    }
    return;
  }

  if (const auto * bin = dyn_cast<BinaryExpr>(expr)) {
    // Conjunction (&&)
    if (bin->op == BinaryOp::And) {
      if (branch_truth) {
        apply_null_facts_from_condition(bin->lhs, true, state);
        apply_null_facts_from_condition(bin->rhs, true, state);
      }
      return;
    }

    // Direct null comparison
    if (bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne) {
      const Expr * var_expr = nullptr;
      if (isa<NullLiteralExpr>(bin->lhs))
        var_expr = bin->rhs;
      else if (isa<NullLiteralExpr>(bin->rhs))
        var_expr = bin->lhs;

      if (var_expr == nullptr) return;

      const std::string_view var_name = get_var_name_from_expr_local(var_expr);
      if (var_name.empty()) return;

      // Decide whether this branch implies var is NotNull.
      //
      // Let P be the comparison itself.
      // - (x != null) is true  => x is NotNull
      // - (x != null) is false => x may be null (erase)
      // - (x == null) is true  => x may be null (erase)
      // - (x == null) is false => x is NotNull
      const bool is_eq = (bin->op == BinaryOp::Eq);
      const bool implies_eq_holds = branch_truth;  // P is assumed true/false
      const bool is_not_null_path = is_eq ? !implies_eq_holds : implies_eq_holds;

      if (is_not_null_path) {
        state.insert(var_name);
      } else {
        state.erase(var_name);
      }
      return;
    }
  }
}

void erase_vars_referenced_by_null_condition(const Expr * expr, NullStateSet & state)
{
  if (expr == nullptr) return;

  if (const auto * unary = dyn_cast<UnaryExpr>(expr)) {
    if (unary->op == UnaryOp::Not) {
      erase_vars_referenced_by_null_condition(unary->operand, state);
    }
    return;
  }

  if (const auto * bin = dyn_cast<BinaryExpr>(expr)) {
    if (bin->op == BinaryOp::And || bin->op == BinaryOp::Or) {
      erase_vars_referenced_by_null_condition(bin->lhs, state);
      erase_vars_referenced_by_null_condition(bin->rhs, state);
      return;
    }

    if (bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne) {
      const Expr * var_expr = nullptr;
      if (isa<NullLiteralExpr>(bin->lhs))
        var_expr = bin->rhs;
      else if (isa<NullLiteralExpr>(bin->rhs))
        var_expr = bin->lhs;

      if (var_expr == nullptr) return;

      const std::string_view var_name = get_var_name_from_expr_local(var_expr);
      if (!var_name.empty()) {
        state.erase(var_name);
      }
      return;
    }
  }
}

}  // namespace

// ============================================================================
// Constructor
// ============================================================================

NullChecker::NullChecker(
  const SymbolTable & values, const NodeRegistry & nodes, DiagnosticBag * diags)
: values_(values), nodes_(nodes), diags_(diags)
{
}

// ============================================================================
// Entry Point
// ============================================================================

bool NullChecker::check(const Program & program)
{
  CFGBuilder builder(nodes_);

  for (const auto * tree : program.trees()) {
    if (tree == nullptr) continue;

    auto cfg = builder.build(tree);
    if (cfg != nullptr) {
      check(tree, *cfg);
    }
  }

  return !has_errors_;
}

// ============================================================================
// Tree Analysis
// ============================================================================

void NullChecker::check(const TreeDecl * tree, const CFG & cfg)
{
  if (tree == nullptr) return;

  // Initialize entry state: Non-nullable parameters/globals are "NotNull"
  NullStateSet entry_state;

  // Parameters
  for (const auto * param : tree->params) {
    const bool is_nullable = (param && param->type) ? param->type->nullable : false;
    if (!is_nullable) {
      entry_state.insert(param->name);
    }
  }

  // Global variables
  const Scope * global_scope = values_.get_global_scope();
  if (global_scope != nullptr) {
    for (const auto & [name, sym] : global_scope->get_symbols()) {
      if (sym.is_variable() || sym.is_const()) {
        // For globals, only honor explicit nullable annotations when available.
        // If we can't determine nullability (e.g., inferred types), keep the
        // previous conservative behavior and treat it as NotNull.
        bool is_nullable = false;
        if (sym.astNode) {
          if (const auto * gv = dyn_cast<GlobalVarDecl>(sym.astNode)) {
            is_nullable = (gv->type != nullptr) ? gv->type->nullable : false;
          } else if (const auto * gc = dyn_cast<GlobalConstDecl>(sym.astNode)) {
            is_nullable = (gc->type != nullptr) ? gc->type->nullable : false;
          }
        }

        if (!is_nullable) {
          entry_state.insert(name);
        }
      }
    }
  }

  analyze_data_flow(cfg, entry_state);
}

// ============================================================================
// Data Flow Analysis
// ============================================================================

// Merge function: Intersection (Must be NotNull on all paths)
static bool merge_null_states(NullStateSet & target, const NullStateSet & source)
{
  bool changed = false;
  for (auto it = target.begin(); it != target.end();) {
    if (source.find(*it) == source.end()) {
      it = target.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  return changed;
}

void NullChecker::analyze_data_flow(const CFG & cfg, NullStateSet & initial_state)
{
  if (cfg.blocks.empty()) return;

  std::vector<NullStateSet> block_in_states(cfg.blocks.size());

  if (cfg.entry) {
    block_in_states[cfg.entry->id] = initial_state;
  }

  std::vector<bool> visited(cfg.blocks.size(), false);
  if (cfg.entry) visited[cfg.entry->id] = true;

  std::deque<BasicBlock *> worklist;
  if (cfg.entry) worklist.push_back(cfg.entry);

  // Phase 1: Fixed Point
  while (!worklist.empty()) {
    BasicBlock * block = worklist.front();
    worklist.pop_front();

    NullStateSet out_state = block_in_states[block->id];
    transfer_block(block, out_state, false);

    for (const auto & edge : block->successors) {
      BasicBlock * succ = edge.target;
      if (succ == nullptr) continue;

      NullStateSet succ_in_candidate = out_state;
      transfer_edge(edge, block, succ_in_candidate);

      // Isolated Policy logic (similar to InitChecker)
      if (
        succ->flowPolicy == FlowPolicy::Isolated && succ->contextEntry != nullptr &&
        edge.kind == CFGEdgeKind::ChildSuccess) {
        if (!succ->stmts.empty() && dyn_cast<NodeStmt>(succ->stmts.front())) {
          // Reset to context state
          NullStateSet context_state = block_in_states[succ->contextEntry->id];
          transfer_block(succ->contextEntry, context_state, false);
          succ_in_candidate = context_state;
        }
      }

      NullStateSet & curr_succ_in = block_in_states[succ->id];

      if (!visited[succ->id]) {
        curr_succ_in = succ_in_candidate;
        visited[succ->id] = true;
        worklist.push_back(succ);
      } else {
        if (merge_null_states(curr_succ_in, succ_in_candidate)) {
          worklist.push_back(succ);
        }
      }
    }
  }

  // Phase 2: Check
  for (const auto & unique_block : cfg.blocks) {
    BasicBlock * block = unique_block.get();
    if (visited[block->id]) {
      NullStateSet state = block_in_states[block->id];
      transfer_block(block, state, true);
    }
  }
}

// ============================================================================
// Transfer Functions
// ============================================================================

void NullChecker::transfer_block(const BasicBlock * block, NullStateSet & state, bool report_errors)
{
  for (const auto * stmt : block->stmts) {
    check_stmt(stmt, state, report_errors);
  }
}

void NullChecker::transfer_edge(
  const BasicBlock::Edge & edge, const BasicBlock * source, NullStateSet & state)
{
  // Guard-based narrowing is intentionally scoped to the guarded statement only.
  // After the statement finishes (success or failure), we discard those facts to prevent
  // "narrowing leakage" across sibling branches and subsequent statements.
  if (edge.kind == CFGEdgeKind::ChildSuccess || edge.kind == CFGEdgeKind::ChildFailure) {
    if (source != nullptr && !source->stmts.empty()) {
      const Stmt * last_stmt = source->stmts.back();
      if (const auto * node = dyn_cast<NodeStmt>(last_stmt)) {
        for (const auto * pc : node->preconditions) {
          if (pc == nullptr) continue;
          if (pc->kind != PreconditionKind::Guard) continue;
          erase_vars_referenced_by_null_condition(pc->condition, state);
        }
      }
    }
  }

  // Post-call transfer: on Success edge, apply out-port nullability contract.
  // Spec §6.2.3: Passing T? to out T is allowed; if the node succeeds, the variable becomes NotNull.
  if (edge.kind == CFGEdgeKind::ChildSuccess) {
    if (source != nullptr && !source->stmts.empty()) {
      const Stmt * last_stmt = source->stmts.back();
      if (const auto * node = dyn_cast<NodeStmt>(last_stmt)) {
        for (const auto * arg : node->args) {
          if (arg == nullptr) continue;

          const bool is_out_arg =
            (arg->direction == PortDirection::Out) || (arg->inlineDecl != nullptr);
          if (!is_out_arg) continue;

          const auto contract = get_port_contract(node, arg);
          if (!contract.has_value()) {
            // Unknown port contract: do not promote/forget (stay conservative without inventing facts).
            continue;
          }

          std::string_view name;
          if (arg->inlineDecl != nullptr) {
            name = arg->inlineDecl->name;
          } else {
            name = get_var_name_from_expr(arg->valueExpr);
          }

          if (name.empty()) continue;

          if (contract->isNullable) {
            // out T? may write null even on Success; forget NotNull knowledge.
            state.erase(name);
          } else {
            // out T guarantees non-null on Success.
            state.insert(name);
          }
        }
      }
    }
  }

  // Handle Guards: condition decomposition + narrowing (Spec §6.2.2)
  if (edge.kind == CFGEdgeKind::GuardTrue || edge.kind == CFGEdgeKind::GuardFalse) {
    const Expr * cond = edge.condition;
    if (cond == nullptr) return;

    const bool branch_truth = (edge.kind == CFGEdgeKind::GuardTrue);
    apply_null_facts_from_condition(cond, branch_truth, state);
  }
}

void NullChecker::check_stmt(const Stmt * stmt, NullStateSet & state, bool report_errors)
{
  if (stmt == nullptr) return;

  switch (stmt->get_kind()) {
    case NodeKind::NodeStmt: {
      const auto * node = cast<NodeStmt>(stmt);
      if (report_errors) check_node_args(node, state);
      break;
    }
    case NodeKind::AssignmentStmt: {
      const auto * assign = cast<AssignmentStmt>(stmt);
      // x = expr
      if (!assign->target.empty()) {
        // Reference behavior (strict): assigning a non-null value to a nullable variable
        // does NOT permanently narrow it. Assignments also invalidate any previous narrowing.
        // The only supported narrowing sources are:
        // - @guard / @run_while derived facts (scoped)
        // - successful out-port writes (post-call contract)
        bool target_is_declared_nullable = false;
        if (const auto * sym = assign->resolvedTarget) {
          if (const auto * v = dyn_cast<BlackboardDeclStmt>(sym->astNode)) {
            if (v->type != nullptr) {
              target_is_declared_nullable = v->type->nullable;
            } else {
              // Inferred: check the assigned value's type
              if (assign->value && assign->value->resolvedType) {
                target_is_declared_nullable = assign->value->resolvedType->is_nullable();
              } else {
                target_is_declared_nullable = true;  // Fallback
              }
            }
          } else if (const auto * p = dyn_cast<ParamDecl>(sym->astNode)) {
            target_is_declared_nullable = (p->type != nullptr) ? p->type->nullable : false;
          } else if (const auto * gv = dyn_cast<GlobalVarDecl>(sym->astNode)) {
            target_is_declared_nullable = (gv->type != nullptr) ? gv->type->nullable : false;
          }
        }

        // Nullable targets are never treated as NotNull based on assignment.
        // Also, assigning null to any target invalidates NotNull knowledge.
        const bool assigned_null = isa<NullLiteralExpr>(assign->value);
        if (assigned_null || target_is_declared_nullable) {
          state.erase(assign->target);
        } else {
          // Non-nullable targets remain NotNull.
          state.insert(assign->target);
        }
      }
      break;
    }
    case NodeKind::BlackboardDeclStmt: {
      const auto * decl = cast<BlackboardDeclStmt>(stmt);
      const bool is_declared_nullable = (decl->type != nullptr) ? decl->type->nullable : false;

      // See AssignmentStmt notes above: nullable declarations are not treated as NotNull
      // even if initialized with a non-null value.
      if (is_declared_nullable) {
        state.erase(decl->name);
        break;
      }

      // Non-nullable locals: treat as NotNull when initialized with a non-null literal.
      if (decl->initialValue && !isa<NullLiteralExpr>(decl->initialValue)) {
        state.insert(decl->name);
      } else {
        state.erase(decl->name);
      }
      break;
    }
    case NodeKind::ConstDeclStmt: {
      const auto * decl = cast<ConstDeclStmt>(stmt);
      const bool is_declared_nullable = (decl->type != nullptr) ? decl->type->nullable : false;
      if (!is_declared_nullable && !isa<NullLiteralExpr>(decl->value)) {
        state.insert(decl->name);
      } else {
        state.erase(decl->name);
      }
      break;
    }
    default:
      break;
  }
}

void NullChecker::check_node_args(const NodeStmt * node, const NullStateSet & state)
{
  if (node == nullptr) return;

  for (const auto * arg : node->args) {
    if (arg == nullptr) continue;
    if (arg->inlineDecl != nullptr) continue;

    const PortDirection dir = arg->direction.value_or(PortDirection::In);
    if (dir == PortDirection::Out) continue;

    const auto contract = get_port_contract(node, arg);
    const bool port_allows_null = contract.has_value() ? contract->isNullable : false;
    if (port_allows_null) continue;

    // Passing null (or possibly-null variable) into a non-nullable port is an error.
    if (arg->valueExpr != nullptr && isa<NullLiteralExpr>(arg->valueExpr)) {
      report_error(arg->get_range(), "Null is not allowed for this port");
      continue;
    }

    const std::string_view name = get_var_name_from_expr(arg->valueExpr);
    if (!name.empty()) {
      // Check if the expression's resolved type is nullable.
      // We rely on TypeChecker to have already resolved types (including inference).
      // If the type is known and non-nullable, we can skip the check.
      bool should_check = true;
      if (arg->valueExpr->resolvedType != nullptr && !arg->valueExpr->resolvedType->is_nullable()) {
        should_check = false;
      }

      if (should_check) {
        if (state.find(name) == state.end()) {
          report_error(
            arg->get_range(), std::string("Variable '") + std::string(name) + "' may be null");
        }
      }
    }
  }
}

std::string_view NullChecker::get_var_name_from_expr(const Expr * expr)
{
  if (expr == nullptr) return {};
  if (const auto * var = dyn_cast<VarRefExpr>(expr)) {
    if (var->resolvedSymbol == nullptr) return {};
    return var->name;
  }
  if (const auto * index_expr = dyn_cast<IndexExpr>(expr))
    return get_var_name_from_expr(index_expr->base);
  return {};
}

void NullChecker::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  error_count_++;
  if (diags_) diags_->report_error(range, std::string(message));
}

}  // namespace bt_dsl
