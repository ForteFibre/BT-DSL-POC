// tests/sema/test_init_checker.cpp - Unit tests for initialization safety checker
//
// Tests the InitializationChecker which verifies that variables are properly
// initialized before use.
//

#include <gtest/gtest.h>

#include <cassert>
#include <iostream>
#include <string>

#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Creates a ModuleInfo for a single-file test case.
 */
static ModuleInfo create_test_module(ParsedUnit & unit)
{
  ModuleInfo module;
  module.program = unit.program;
  module.types.register_builtins();
  module.values.build_from_program(*unit.program);

  for (const auto * ext_type : unit.program->extern_types()) {
    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    module.types.define(sym);
  }

  for (const auto * ext : unit.program->externs()) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    module.nodes.define(sym);
  }
  for (const auto * tree : unit.program->trees()) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    module.nodes.define(sym);
  }

  SymbolTableBuilder builder(module.values, module.types, module.nodes);
  builder.build(*unit.program);

  return module;
}

// Helper to run the full pipeline up to initialization checking
static bool check_initialization(const std::string & src, DiagnosticBag & diags)
{
  auto unit = parse_source(src);
  if (unit == nullptr || !unit->diags.empty()) {
    return false;
  }

  Program * program = unit->program;
  if (program == nullptr) {
    return false;
  }

  ModuleInfo module = create_test_module(*unit);

  // Run name resolution (needed before init checking)
  NameResolver resolver(module);
  if (!resolver.resolve()) {
    return false;
  }

  // Run initialization checking
  InitializationChecker checker(module.values, module.nodes, &diags);
  return checker.check(*program);
}

// ============================================================================
// Basic Initialization Tests
// ============================================================================

TEST(SemaInitChecker, GlobalVarIsInit)
{
  // Test global variable - known to work since it doesn't involve tree-local vars
  const std::string src = R"(
    extern action Log(value: int);
    var counter: int = 0;
    tree Main() {
      Log(value: counter);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_initialization(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaInitChecker, InParameterIsInit)
{
  const std::string src = R"(
    extern action Log(value: int);
    tree Main(in param: int) {
      Log(value: param);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_initialization(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

// ============================================================================
// Error Detection Tests
// ============================================================================

TEST(SemaInitChecker, ErrorUninitOutParamToIn)
{
  const std::string src = R"(
    extern action Log(value: int);
    tree Main(out param: int) {
      Log(value: param);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_initialization(src, diags);
  EXPECT_FALSE(ok);  // Should fail - param is out (Uninit) but passed to in port
  EXPECT_TRUE(diags.has_errors());
}

// ============================================================================
// DataPolicy Tests
// ============================================================================

TEST(SemaInitChecker, DataPolicyAllChildrenBlock)
{
  // Test Sequence (All policy): children in sequence see earlier siblings' writes
  const std::string src = R"(
    extern action GetValue(out result: int);
    extern action Use(value: int);
    extern control Sequence();
    tree Main() {
      Sequence() {
        GetValue(result: out var x);
        Use(value: x);
      }
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_initialization(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

// ============================================================================
// FlowPolicy Tests
// ============================================================================

TEST(SemaInitChecker, FlowPolicyIsolatedError)
{
  // Isolated policy: siblings can't see each other's writes
  const std::string src = R"(
    extern action GetValue(out result: int);
    extern action Use(value: int);
    #[behavior(All, Isolated)]
    extern control ParallelAll();
    tree Main() {
      ParallelAll() {
        GetValue(result: out var x);
        Use(value: x);
      }
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_initialization(src, diags);
  // x should NOT be visible to Use because of Isolated policy
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}
