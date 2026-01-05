// tests/sema/test_cfg.cpp - Unit tests for CFG builder
//
#include <gtest/gtest.h>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "bt_dsl/sema/analysis/cfg.hpp"
#include "bt_dsl/sema/analysis/cfg_builder.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/type_table.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

// ============================================================================
// Helper Functions
// ============================================================================

struct BuiltCfg
{
  std::unique_ptr<ParsedUnit> unit;
  std::unique_ptr<CFG> cfg;
};

// Helper to build CFG from source code (keeps AST alive)
static BuiltCfg build_cfg(const std::string & src)
{
  BuiltCfg out;
  out.unit = parse_source(src);
  if (out.unit == nullptr || !out.unit->diags.empty()) {
    std::cerr << "Parse failed\n";
    return out;
  }

  Program * program = out.unit->program;
  if (program == nullptr || program->trees().empty()) {
    std::cerr << "No tree found\n";
    return out;
  }

  // Basic semantic analysis required for node resolution
  ModuleInfo module;
  module.program = program;
  module.parsedUnit = std::move(out.unit);
  module.types.register_builtins();
  module.values.build_from_program(*module.program);

  for (const auto * ext : module.program->externs()) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    module.nodes.define(sym);
  }
  for (const auto * tree : module.program->trees()) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    module.nodes.define(sym);
  }

  SymbolTableBuilder builder(module.values, module.types, module.nodes);
  (void)builder.build(*module.program);

  NameResolver resolver(module);
  (void)resolver.resolve();

  // Build CFG for the first tree
  CFGBuilder cfg_builder(module.nodes);
  out.cfg = cfg_builder.build(module.program->trees()[0]);
  out.unit = std::move(module.parsedUnit);
  return out;
}

static BasicBlock * follow_single_unconditional(BasicBlock * b)
{
  if (b == nullptr) return nullptr;
  if (b->successors.size() != 1U) return b;
  if (b->successors[0].kind != CFGEdgeKind::Unconditional) return b;
  return b->successors[0].target;
}

// Helper to count edges of a specific kind from a block
static size_t count_edges(const BasicBlock * block, CFGEdgeKind kind)
{
  size_t count = 0;
  for (const auto & edge : block->successors) {
    if (edge.kind == kind) {
      count++;
    }
  }
  return count;
}

// ============================================================================
// Tests
// ============================================================================

TEST(SemaCfgBuilder, LinearCfg)
{
  const std::string src = R"(
    extern action ActionA();
    extern action ActionB();
    tree Main() {
      ActionA();
      ActionB();
    }
  )";

  auto built = build_cfg(src);
  ASSERT_NE(built.cfg, nullptr);
  const auto & cfg = *built.cfg;

  // Structure should be:
  // Entry -> ActionA(id=0) -> ActionB(id=1) -> ExitSuccess/Failure

  // Note: Implementation detail - Create leaf nodes generates ChildSuccess/ChildFailure edges
  // So Entry connects to ActionA block
  // ActionA block has ChildSuccess -> ActionB block, ChildFailure -> ExitFailure
  // ActionB block has ChildSuccess -> ExitSuccess, ChildFailure -> ExitFailure

  // Entry block
  ASSERT_NE(cfg.entry, nullptr);
  ASSERT_EQ(cfg.entry->successors.size(), 1U);  // Unconditional to first stmt block

  // ActionA block
  BasicBlock * block_a = cfg.entry->successors[0].target;
  ASSERT_NE(block_a, nullptr);
  EXPECT_EQ(block_a->stmts.size(), 1U);  // ActionA node
  EXPECT_EQ(count_edges(block_a, CFGEdgeKind::ChildSuccess), 1U);
  EXPECT_EQ(count_edges(block_a, CFGEdgeKind::ChildFailure), 1U);

  // ActionB block (target of ActionA success)
  BasicBlock * block_b = nullptr;
  for (const auto & edge : block_a->successors) {
    if (edge.kind == CFGEdgeKind::ChildSuccess) block_b = edge.target;
  }
  // There is an intermediate unconditional block between siblings.
  block_b = follow_single_unconditional(block_b);
  ASSERT_NE(block_b, nullptr);
  EXPECT_EQ(block_b->stmts.size(), 1U);  // ActionB node

  // ActionB success -> ExitSuccess
  bool leads_to_success = false;
  for (const auto & edge : block_b->successors) {
    if (edge.kind == CFGEdgeKind::ChildSuccess && edge.target == cfg.exitSuccess) {
      leads_to_success = true;
    }
  }
  EXPECT_TRUE(leads_to_success);
}

TEST(SemaCfgBuilder, SequenceCfg)
{
  // Sequence is DataPolicy::All -> Success flows to next, Failure exits
  const std::string src = R"(
    extern action ActionA();
    extern action ActionB();
    extern control Sequence();
    tree Main() {
      Sequence() {
        ActionA();
        ActionB();
      }
    }
  )";

  auto built = build_cfg(src);
  ASSERT_NE(built.cfg, nullptr);
  const auto & cfg = *built.cfg;

  ASSERT_EQ(cfg.entry->successors.size(), 1U);
  BasicBlock * sequence_block = cfg.entry->successors[0].target;  // The Sequence node itself

  // Sequence node (with children block) has unconditional edge to children entry
  ASSERT_EQ(sequence_block->successors.size(), 1U);
  EXPECT_EQ(sequence_block->successors[0].kind, CFGEdgeKind::Unconditional);

  BasicBlock * children_entry = sequence_block->successors[0].target;
  ASSERT_NE(children_entry, nullptr);
  EXPECT_EQ(children_entry->dataPolicy, DataPolicy::All);

  // Children entry connects to ActionA
  ASSERT_EQ(children_entry->successors.size(), 1U);
  BasicBlock * block_a = children_entry->successors[0].target;

  // ActionA Success -> ActionB (DataPolicy::All)
  BasicBlock * block_b = nullptr;
  bool failure_to_parent_exit = false;

  for (const auto & edge : block_a->successors) {
    if (edge.kind == CFGEdgeKind::ChildSuccess) {
      block_b = follow_single_unconditional(edge.target);
    }
    if (edge.kind == CFGEdgeKind::ChildFailure && edge.target == cfg.exitFailure) {
      failure_to_parent_exit = true;
    }
  }

  EXPECT_NE(block_b, nullptr);          // Should flow to next sibling on success
  EXPECT_TRUE(failure_to_parent_exit);  // Should exit on failure
}

TEST(SemaCfgBuilder, FallbackCfg)
{
  // Fallback is DataPolicy::Any -> Failure flows to next, Success exits
  const std::string src = R"(
    extern action ActionA();
    extern action ActionB();
    #[behavior(Any)]
    extern control Fallback();
    tree Main() {
      Fallback() {
        ActionA();
        ActionB();
      }
    }
  )";

  auto built = build_cfg(src);
  ASSERT_NE(built.cfg, nullptr);
  const auto & cfg = *built.cfg;

  BasicBlock * fallback_block = cfg.entry->successors[0].target;
  BasicBlock * children_entry = fallback_block->successors[0].target;
  ASSERT_NE(children_entry, nullptr);
  EXPECT_EQ(children_entry->dataPolicy, DataPolicy::Any);

  BasicBlock * block_a = children_entry->successors[0].target;

  // ActionA Failure -> ActionB (DataPolicy::Any)
  BasicBlock * block_b = nullptr;
  bool success_to_parent_exit = false;

  for (const auto & edge : block_a->successors) {
    if (edge.kind == CFGEdgeKind::ChildFailure) {
      block_b = follow_single_unconditional(edge.target);
    }
    if (edge.kind == CFGEdgeKind::ChildSuccess && edge.target == cfg.exitSuccess) {
      success_to_parent_exit = true;
    }
  }

  EXPECT_NE(block_b, nullptr);          // Should flow to next sibling on failure
  EXPECT_TRUE(success_to_parent_exit);  // Should exit on success (parent success)
}

TEST(SemaCfgBuilder, PreconditionCfg)
{
  const std::string src = R"(
    extern action ActionA();
    tree Main() {
      @guard(true)
      ActionA();
    }
  )";

  auto built = build_cfg(src);
  ASSERT_NE(built.cfg, nullptr);
  const auto & cfg = *built.cfg;

  // Entry -> Precondition Block -> ActionA -> Exit
  //               |
  //               +-> ExitSuccess (Skip)

  BasicBlock * current = cfg.entry->successors[0].target;
  // This block handles precondition branching

  bool has_guard_true = false;
  bool has_guard_false = false;

  for (const auto & edge : current->successors) {
    if (edge.kind == CFGEdgeKind::GuardTrue) has_guard_true = true;
    if (edge.kind == CFGEdgeKind::GuardFalse) has_guard_false = true;
  }

  EXPECT_TRUE(has_guard_true);
  EXPECT_TRUE(has_guard_false);
}
