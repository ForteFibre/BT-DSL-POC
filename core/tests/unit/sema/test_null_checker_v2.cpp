// tests/sema/test_null_checker.cpp - Unit tests for null safety checker
//

#include <gtest/gtest.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/null_checker.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"
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

static bool test_debug_enabled() { return std::getenv("BT_DSL_TEST_DEBUG") != nullptr; }

static bool check_null_safety(const std::string & src, DiagnosticBag & diags)
{
  const bool debug = test_debug_enabled();

  ModuleInfo module;
  module.parsedUnit = parse_source(src);
  if (module.parsedUnit == nullptr || !module.parsedUnit->diags.empty()) {
    if (module.parsedUnit != nullptr) {
      diags.merge(module.parsedUnit->diags);
      if (debug) {
        std::cout << "DEBUG: Parser failed. Diags=" << diags.size() << "\n";
        for (const auto & d : diags.all()) {
          std::cerr << "Diagnostic: " << d.message << "\n";
        }
      }
    }
    return false;
  }

  Program * program = module.parsedUnit->program;
  if (program == nullptr) {
    return false;
  }

  module.program = program;

  // Set up symbol tables
  module.types = TypeTable{};
  module.nodes = NodeRegistry{};
  module.values = SymbolTable{};
  module.imports.clear();

  module.types.register_builtins();

  for (const auto * ext_type : program->extern_types()) {
    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    module.types.define(sym);
  }

  for (const auto * ext : program->externs()) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    module.nodes.define(sym);
  }
  for (const auto * tree : program->trees()) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    module.nodes.define(sym);
  }

  module.values.build_from_program(*program);

  if (debug) std::cout << "DEBUG: Parser OK\n";

  // Build symbol tables
  SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
  if (!builder.build(*program)) {
    if (debug) std::cout << "DEBUG: SymbolTableBuilder failed\n";
    return false;
  }
  if (debug) std::cout << "DEBUG: SymbolTableBuilder OK\n";

  // Run name resolution
  NameResolver resolver(module, &diags);
  if (!resolver.resolve()) {
    if (debug) {
      std::cout << "DEBUG: NameResolver failed\n";
      std::cout << "DEBUG: Diags=" << diags.size() << "\n";
      for (const auto & d : diags.all()) {
        std::cerr << "Diagnostic: " << d.message << "\n";
      }
    }
    return false;
  }
  if (debug) std::cout << "DEBUG: NameResolver OK\n";

  // Run initialization checking (prerequisite for valid CFG/flow?)
  // Not strictly required but good to have a clean slate
  InitializationChecker init_checker(module.values, module.nodes, &diags);
  if (!init_checker.check(*program)) {
    if (debug) std::cout << "DEBUG: InitChecker failed\n";
    return false;
  }
  if (debug) std::cout << "DEBUG: InitChecker OK\n";

  // Run null checking
  NullChecker checker(module.values, module.nodes, &diags);
  const bool result = checker.check(*program);
  if (debug) std::cout << "DEBUG: NullChecker result=" << result << "\n";

  if (debug && (!result || diags.has_errors())) {
    std::cout << "DIAGNOSTICS DUMP START\n" << std::flush;
    for (const auto & d : diags.all()) {
      std::string_view code;
      if (d.range.is_valid()) {
        // Safeguard against out of bounds
        if (d.range.get_end().get_offset() <= module.parsedUnit->source.size()) {
          code = module.parsedUnit->source.get_source_slice(d.range);
        }
      }
      std::cout << "Diagnostic: [" << d.message << "] Range: " << d.range.get_begin().get_offset()
                << "-" << d.range.get_end().get_offset() << " Code: [" << code << "]\n"
                << std::flush;
    }
    std::cout << "DIAGNOSTICS DUMP END\n" << std::flush;
  }
  return result;
}

// ============================================================================
// Tests
// ============================================================================

TEST(SemaNullCheckerV2, ParamsAreNotNull)
{
  const std::string src = R"(
    extern action Use(value: string);
    tree Main(in arg: string) {
      Use(value: arg);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullCheckerV2, NullableParamIsNotAssumedNotNull)
{
  const std::string src = R"(
    extern action Use(value: string);
    tree Main(in arg: string?) {
      Use(value: arg);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}

TEST(SemaNullCheckerV2, NullAssignmentError)
{
  const std::string src = R"(
    extern action Use(value: string);
    tree Main() {
      var x: string = null;
      Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
  // Expected error: Variable 'x' may be null
}

TEST(SemaNullCheckerV2, GuardPromotion)
{
  const std::string src = R"(
    extern action Use(value: string);
    extern control Sequence();
    extern condition Guard(cond: bool);
    
    tree Main() {
      var x: string? = null;
      @guard(x != null)
      Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullCheckerV2, GuardPromotionElse)
{
  // Use(x) @guard(x == null); -> x is null inside Use. Should fail.
  const std::string src = R"(
    extern action Use(value: string);
    tree Main() {
      var x: string? = null;
      @guard(x == null)
      Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}
