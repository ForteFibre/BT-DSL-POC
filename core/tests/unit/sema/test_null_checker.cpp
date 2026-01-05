// tests/sema/test_null_checker.cpp - Unit tests for null safety checker
//

#include <gtest/gtest.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/null_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

static bool test_debug_enabled() { return std::getenv("BT_DSL_TEST_DEBUG") != nullptr; }

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Creates a ModuleInfo for a single-file test case.
 */
static ModuleInfo create_test_module(ParsedUnit & unit, DiagnosticBag * diags = nullptr)
{
  ModuleInfo module;
  module.program = unit.program;
  module.types.register_builtins();

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

  module.values.build_from_program(*unit.program);

  SymbolTableBuilder builder(module.values, module.types, module.nodes, diags);
  builder.build(*unit.program);

  return module;
}

static bool check_null_safety(const std::string & src, DiagnosticBag & diags)
{
  const bool debug = test_debug_enabled();

  auto unit = parse_source(src);
  if (unit == nullptr || !unit->diags.empty()) {
    if (unit != nullptr) {
      diags.merge(unit->diags);
      if (debug) {
        std::cout << "DEBUG: Parser failed. Diags=" << diags.size() << "\n";
        for (const auto & d : diags.all()) {
          std::cerr << "Diagnostic: " << d.message << "\n";
        }
      }
    }
    return false;
  }

  Program * program = unit->program;
  if (program == nullptr) {
    return false;
  }

  if (debug) std::cout << "DEBUG: Parser OK\n";

  ModuleInfo module = create_test_module(*unit, &diags);
  if (debug) std::cout << "DEBUG: SymbolTableBuilder OK\n";

  // Run name resolution
  NameResolver resolver(module, &diags);
  if (!resolver.resolve()) {
    if (debug) {
      std::cout << "DEBUG: NameResolver failed\n";
      std::cout << "DEBUG: Diags=" << diags.size() << "\n";
      std::cout << "DIAGNOSTICS DUMP START (NameResolver)\n" << std::flush;
      for (const auto & d : diags.all()) {
        std::string_view code;
        if (d.range.is_valid()) {
          if (d.range.get_end().get_offset() <= unit->source.size()) {
            code = unit->source.get_source_slice(d.range);
          }
        }
        std::cout << "Diagnostic: [" << d.message << "] Range: " << d.range.get_begin().get_offset()
                  << "-" << d.range.get_end().get_offset() << " Code: [" << code << "]\n"
                  << std::flush;
      }
      std::cout << "DIAGNOSTICS DUMP END\n" << std::flush;
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
        if (d.range.get_end().get_offset() <= unit->source.size()) {
          code = unit->source.get_source_slice(d.range);
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

TEST(SemaNullChecker, ParamsAreNotNull)
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

TEST(SemaNullChecker, NullAssignmentError)
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

TEST(SemaNullChecker, GuardPromotion)
{
  const std::string src = R"(
    extern action Use(value: string);
    extern control Sequence();
    extern condition Guard(cond: bool);
    
    tree Main() {
      var x: string = null;
      @guard(x != null) Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullChecker, GuardPromotionElse)
{
  // Use(x) @guard(x == null); -> x is null inside Use. Should fail.
  const std::string src = R"(
    extern action Use(value: string);
    tree Main() {
      var x: string = null;
      @guard(x == null) Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}

TEST(SemaNullChecker, GuardPromotionConjunction)
{
  const std::string src = R"(
    extern action UseBoth(a: string, b: string);

    tree Main() {
      var x: string = null;
      var y: string = null;

      @guard(x != null && y != null) UseBoth(a: x, b: y);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullChecker, GuardPromotionNegation)
{
  // Spec §6.2.2 (Negation): !(x == null) can be treated as x != null.
  const std::string src = R"(
    extern action Use(value: string);

    tree Main() {
      var x: string = null;
      @guard(!(x == null)) Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullChecker, GuardConservativeNotAnd)
{
  // !(x != null && y != null) being true only means (x == null || y == null).
  // It does *not* justify promoting x to NotNull.
  const std::string src = R"(
    extern action Use(value: string);

    tree Main() {
      var x: string = null;
      var y: string = null;

      @guard(!(x != null && y != null)) Use(value: x);
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}

TEST(SemaNullChecker, NullableOutPromotionOnSuccess)
{
  // Spec §6.2.3: T? passed to out T is allowed; on Success path it becomes NotNull.
  // In a Sequence, the next statement is reached only on Success.
  const std::string src = R"(
    extern type Pose;
    extern action FindTarget(out result: Pose);
    extern action Use(in value: Pose);
    extern control Sequence();

    tree Main() {
      var target: Pose? = null;
      Sequence() {
        FindTarget(result: out target);
        Use(value: target);
      }
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  ASSERT_TRUE(ok);
  EXPECT_FALSE(diags.has_errors());
}

TEST(SemaNullChecker, NullableOutNotPromotedOnFailurePath)
{
  // In a Fallback, the second child is reached only if the first fails.
  // Therefore, out promotion must NOT apply.
  const std::string src = R"(
    extern type Pose;
    extern action FindTarget(out result: Pose);
    extern action Use(in value: Pose);
    #[behavior(Any, Chained)]
    extern control Fallback();

    tree Main() {
      var target: Pose? = null;
      Fallback() {
        FindTarget(result: out target);
        Use(value: target);
      }
    }
  )";

  DiagnosticBag diags;
  const bool ok = check_null_safety(src, diags);
  EXPECT_FALSE(ok);
  EXPECT_TRUE(diags.has_errors());
}
