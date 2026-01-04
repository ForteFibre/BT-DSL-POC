// bt_dsl/sema/init_checker.cpp - Initialization safety checker implementation
//
#include "bt_dsl/sema/analysis/init_checker.hpp"

#include <deque>
#include <unordered_map>

#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/sema/analysis/cfg_builder.hpp"

namespace bt_dsl
{

// ============================================================================
// Constructor
// ============================================================================

InitializationChecker::InitializationChecker(
  const SymbolTable & values, const NodeRegistry & nodes, DiagnosticBag * diags)
: values_(values), nodes_(nodes), diags_(diags)
{
}

// ============================================================================
// Entry Point
// ============================================================================

bool InitializationChecker::check(const Program & program)
{
  CFGBuilder builder(nodes_);

  // Check each tree declaration
  for (const auto * tree : program.trees) {
    if (tree == nullptr) continue;

    // Build CFG for the tree
    auto cfg = builder.build(tree);
    if (cfg != nullptr) {
      check(tree, *cfg);
    }
  }

  return !hasErrors_;
}

// ============================================================================
// Tree Analysis (CFG-based)
// ============================================================================

void InitializationChecker::check(const TreeDecl * tree, const CFG & cfg)
{
  if (tree == nullptr) return;

  // Initialize entry state: parameters with in/ref/mut are Init, out are Uninit
  InitStateMap entry_state;

  // Register parameters
  for (const auto * param : tree->params) {
    const PortDirection dir = param->direction.value_or(PortDirection::In);
    if (dir == PortDirection::Out) {
      // out parameters start as Uninit
      entry_state[param->name] = InitState::Uninit;
    } else {
      // in/ref/mut parameters start as Init
      entry_state[param->name] = InitState::Init;
    }
  }

  // Register global variables (always Init)
  const Scope * global_scope = values_.get_global_scope();
  if (global_scope != nullptr) {
    for (const auto & [name, sym] : global_scope->get_symbols()) {
      if (sym.is_variable() || sym.is_const()) {
        entry_state[name] = InitState::Init;
      }
    }
  }

  // Run data flow analysis (error reporting suppressed during fixed-point)
  analyze_data_flow(cfg, entry_state);
}

// ============================================================================
// Data Flow Analysis
// ============================================================================

static bool merge_init_states(InitStateMap & target, const InitStateMap & source)
{
  bool changed = false;

  for (auto it = target.begin(); it != target.end();) {
    const auto & name = it->first;
    const InitState target_state = it->second;

    auto src_it = source.find(name);
    const InitState source_state = (src_it != source.end()) ? src_it->second : InitState::Uninit;

    // Merge rule: Init + Init = Init, otherwise Uninit
    if (target_state == InitState::Init && source_state != InitState::Init) {
      it->second = InitState::Uninit;
      changed = true;
    }

    ++it;
  }

  for (const auto & [name, state] : source) {
    if (target.find(name) == target.end()) {
      target[name] = InitState::Uninit;
      changed = true;
    }
  }

  return changed;
}

static void union_init_states(InitStateMap & target, const InitStateMap & source)
{
  for (const auto & [name, st] : source) {
    if (st == InitState::Init) {
      target[name] = InitState::Init;
    } else {
      // Ensure key exists so later merges remain stable.
      if (target.find(name) == target.end()) {
        target[name] = InitState::Uninit;
      }
    }
  }
}

void InitializationChecker::analyze_data_flow(const CFG & cfg, InitStateMap & initial_state)
{
  if (cfg.blocks.empty()) return;

  std::vector<InitStateMap> block_in_states(cfg.blocks.size());

  if (cfg.entry) {
    block_in_states[cfg.entry->id] = initial_state;
  }

  // Worklist
  std::deque<BasicBlock *> worklist;
  if (cfg.entry) {
    worklist.push_back(cfg.entry);
  }

  // To handle "Unvisited" vs "Visited" (Top vs Value), we track visited status.
  std::vector<bool> visited(cfg.blocks.size(), false);
  if (cfg.entry) {
    visited[cfg.entry->id] = true;
  }

  // Phase 1: Fixed-point iteration (Checking disabled)
  // Additional state for FlowPolicy::Isolated:
  // - Children should not see siblings' writes.
  // - After the isolated node completes successfully, the effects of *all* successful children
  //   must become visible.
  // We approximate this by tracking a per-context "committed" state (union of child-success states),
  // while still resetting the visible state to the context baseline when entering siblings.
  std::unordered_map<size_t, InitStateMap> isolated_committed;
  std::unordered_map<size_t, bool> isolated_committed_init;

  auto ensure_isolated_committed = [&](const BasicBlock * ctx_entry) {
    if (ctx_entry == nullptr) {
      return;
    }
    const size_t ctx_id = ctx_entry->id;
    if (isolated_committed_init[ctx_id]) {
      return;
    }

    // Baseline for isolated context is the In-state at the context entry.
    // This is the state each child starts with (no sibling visibility).
    InitStateMap baseline = block_in_states[ctx_id];
    transfer_block(ctx_entry, baseline, false);
    isolated_committed[ctx_id] = std::move(baseline);
    isolated_committed_init[ctx_id] = true;
  };

  while (!worklist.empty()) {
    BasicBlock * block = worklist.front();
    worklist.pop_front();

    // Calculate Out[B] by applying transfer function to In[B]
    InitStateMap out_state = block_in_states[block->id];
    transfer_block(block, out_state, false);  // No error reporting

    // Propagate to successors
    for (const auto & edge : block->successors) {
      BasicBlock * succ = edge.target;
      if (succ == nullptr) continue;

      // First, compute the normal edge transfer (this includes out-port writes on ChildSuccess).
      InitStateMap after_edge = out_state;
      transfer_edge(edge, block, after_edge);

      InitStateMap succ_in_candidate = after_edge;

      // If we're inside an isolated context, accumulate committed effects on each child success,
      // even if this edge exits the isolated context (e.g. last child -> success exit).
      if (block->flowPolicy == FlowPolicy::Isolated && block->contextEntry != nullptr) {
        ensure_isolated_committed(block->contextEntry);
        if (edge.kind == CFGEdgeKind::ChildSuccess) {
          const size_t ctx_id = block->contextEntry->id;
          union_init_states(isolated_committed[ctx_id], after_edge);
        }
      }

      // If we're exiting an isolated context, expose the committed state.
      if (
        block->flowPolicy == FlowPolicy::Isolated && block->contextEntry != nullptr &&
        (succ->flowPolicy != FlowPolicy::Isolated || succ->contextEntry != block->contextEntry)) {
        const size_t ctx_id = block->contextEntry->id;
        auto it = isolated_committed.find(ctx_id);
        if (it != isolated_committed.end()) {
          succ_in_candidate = it->second;
        }
      }

      // If we're inside an isolated context, track committed effects and reset visibility
      // when entering a sibling.
      if (succ->flowPolicy == FlowPolicy::Isolated && succ->contextEntry != nullptr) {
        const size_t ctx_id = succ->contextEntry->id;
        ensure_isolated_committed(succ->contextEntry);

        // Reset visible state when entering blocks within the isolated context
        // (prevents sibling visibility). The committed effects will be applied when exiting.
        InitStateMap context_state = block_in_states[ctx_id];
        transfer_block(succ->contextEntry, context_state, false);
        succ_in_candidate = context_state;
      }

      InitStateMap & curr_succ_in = block_in_states[succ->id];

      if (!visited[succ->id]) {
        curr_succ_in = succ_in_candidate;
        visited[succ->id] = true;
        worklist.push_back(succ);
      } else {
        if (merge_init_states(curr_succ_in, succ_in_candidate)) {
          worklist.push_back(succ);
        }
      }
    }
  }

  for (const auto & unique_block : cfg.blocks) {
    BasicBlock * block = unique_block.get();
    if (visited[block->id]) {
      InitStateMap state = block_in_states[block->id];
      transfer_block(block, state, true);  // Report errors
    }
  }
}

void InitializationChecker::transfer_block(
  const BasicBlock * block, InitStateMap & state, bool report_errors)
{
  for (const auto * stmt : block->stmts) {
    const auto * node = dyn_cast<NodeStmt>(stmt);
    if (node) {
      if (report_errors) check_node_args(node, state);
      check_stmt(stmt, state);  // Updates state
    } else {
      check_stmt(stmt, state);
    }
  }
}

void InitializationChecker::transfer_edge(
  const BasicBlock::Edge & edge, const BasicBlock * source, InitStateMap & state)
{
  if (source == nullptr || source->stmts.empty()) return;

  if (edge.kind == CFGEdgeKind::ChildSuccess) {
    // Reference: static-analysis-and-safety.md §6.1.3 (DataPolicy None)
    // If the containing policy is None, no child out writes are guaranteed to propagate.
    if (source->dataPolicy == DataPolicy::None) {
      return;
    }
    const Stmt * last_stmt = source->stmts.back();
    if (const auto * node = dyn_cast<NodeStmt>(last_stmt)) {
      for (const auto * arg : node->args) {
        if (arg->direction == PortDirection::Out) {
          const std::string_view name = get_var_name_from_expr(arg->valueExpr);
          if (!name.empty()) {
            state[name] = InitState::Init;
          }
          if (arg->inlineDecl != nullptr) {
            state[arg->inlineDecl->name] = InitState::Init;
          }
        }
      }
    }
  }
}

void InitializationChecker::check_node_args(const NodeStmt * node, const InitStateMap & state)
{
  for (const auto * arg : node->args) {
    // Get variable name
    const std::string_view var_name = get_var_name_from_expr(arg->valueExpr);
    if (var_name.empty()) continue;

    const PortDirection dir = arg->direction.value_or(PortDirection::In);

    if (dir == PortDirection::In || dir == PortDirection::Ref || dir == PortDirection::Mut) {
      // Must be Init
      auto it = state.find(var_name);
      if (it == state.end() || it->second == InitState::Uninit) {
        report_error(
          arg->get_range(), std::string("Variable '") + std::string(var_name) +
                              "' may be uninitialized when passed to '" +
                              std::string(to_string(dir)) + "' port");
      }
    }
  }
}

void InitializationChecker::check_stmt(const Stmt * stmt, InitStateMap & state)
{
  if (stmt == nullptr) return;

  switch (stmt->get_kind()) {
    case NodeKind::NodeStmt: {
      const auto * node = cast<NodeStmt>(stmt);
      for (const auto * arg : node->args) {
        if (arg->direction == PortDirection::Out) {
          const std::string_view var_name = get_var_name_from_expr(arg->valueExpr);
          if (!var_name.empty()) {
          }
        }

        if (arg->inlineDecl != nullptr) {
          state[arg->inlineDecl->name] = InitState::Uninit;
        }
      }
      break;
    }

    case NodeKind::AssignmentStmt: {
      const auto * assign = cast<AssignmentStmt>(stmt);
      if (!assign->target.empty()) {
        state[assign->target] = InitState::Init;
      }
      break;
    }

    case NodeKind::BlackboardDeclStmt: {
      const auto * decl = cast<BlackboardDeclStmt>(stmt);
      if (decl->initialValue) {
        state[decl->name] = InitState::Init;
      } else {
        state[decl->name] = InitState::Uninit;
      }
      break;
    }

    case NodeKind::ConstDeclStmt: {
      const auto * decl = cast<ConstDeclStmt>(stmt);
      state[decl->name] = InitState::Init;
      break;
    }

    default:
      break;
  }
}

std::string_view InitializationChecker::get_var_name_from_expr(const Expr * expr)
{
  if (expr == nullptr) return {};
  if (const auto * var_ref = dyn_cast<VarRefExpr>(expr)) {
    return var_ref->name;
  }
  if (const auto * index_expr = dyn_cast<IndexExpr>(expr)) {
    return get_var_name_from_expr(index_expr->base);
  }
  return {};
}

void InitializationChecker::report_error(SourceRange range, std::string_view message)
{
  hasErrors_ = true;
  errorCount_++;
  if (diags_) {
    diags_->error(range, message);
  }
}

}  // namespace bt_dsl
