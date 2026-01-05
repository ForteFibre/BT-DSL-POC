// tests/sema/test_tree_recursion.cpp - Unit tests for tree recursion detection
//
// Spec §6.3.1: direct/indirect recursive tree calls are forbidden.

#include <gtest/gtest.h>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

using namespace bt_dsl;

/**
 * Creates a ModuleInfo for a single-file test case.
 */
static ModuleInfo create_test_module(Program & program, DiagnosticBag * diags = nullptr)
{
  ModuleInfo module;
  module.program = &program;
  module.types.register_builtins();

  for (const auto * ext_type : program.extern_types()) {
    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    module.types.define(sym);
  }

  for (const auto * ext : program.externs()) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    module.nodes.define(sym);
  }
  for (const auto * tree : program.trees()) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    module.nodes.define(sym);
  }

  module.values.build_from_program(program);

  SymbolTableBuilder builder(module.values, module.types, module.nodes, diags);
  builder.build(program);

  return module;
}

static bool has_error_containing(const DiagnosticBag & diags, std::string_view needle)
{
  const std::string n(needle);
  const auto & all = diags.all();
  return std::any_of(all.begin(), all.end(), [&](const Diagnostic & d) {
    if (d.severity != Severity::Error) return false;
    return d.message.find(n) != std::string::npos;
  });
}

TEST(SemaTreeRecursion, DirectRecursionIsError)
{
  const std::string src = R"(
    tree A() {
      A();
    }
  )";

  const auto unit = test_support::parse(src);
  ASSERT_NE(unit.program, nullptr);
  ASSERT_FALSE(unit.diags.has_errors());

  DiagnosticBag diags;
  ModuleInfo module = create_test_module(*unit.program, &diags);

  NameResolver resolver(module, &diags);
  bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  TreeRecursionChecker checker(&diags);
  ok = checker.check(*unit.program);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(checker.has_errors());
  EXPECT_TRUE(has_error_containing(diags, "Recursive tree call is not allowed"));
  EXPECT_TRUE(has_error_containing(diags, "A -> A"));
}

TEST(SemaTreeRecursion, IndirectRecursionIsError)
{
  const std::string src = R"(
    tree A() {
      B();
    }
    tree B() {
      C();
    }
    tree C() {
      A();
    }
  )";

  const auto unit = test_support::parse(src);
  ASSERT_NE(unit.program, nullptr);
  ASSERT_FALSE(unit.diags.has_errors());

  DiagnosticBag diags;
  ModuleInfo module = create_test_module(*unit.program, &diags);

  NameResolver resolver(module, &diags);
  bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  TreeRecursionChecker checker(&diags);
  ok = checker.check(*unit.program);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(checker.has_errors());
  EXPECT_TRUE(has_error_containing(diags, "Recursive tree call is not allowed"));
  EXPECT_TRUE(has_error_containing(diags, "A -> B -> C -> A"));
}
