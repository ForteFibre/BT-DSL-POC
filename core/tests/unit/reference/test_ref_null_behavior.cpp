// tests/unit/reference/test_ref_null_behavior.cpp
// Additional reference compliance tests for Null Behavior
//
// Tests:
// - Narrowing invalidation on assignment
// - Narrowing via 'out' port connection
// - Scope leakage prevention
// - XML mapping for cancellation (x == null) patterns

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/codegen/xml_generator.hpp"  // For XmlGenerator
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/null_checker.hpp"  // For NullChecker
#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"  // For TypeChecker
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

class NullTestContext
{
public:
  bool parse(const std::string & src)
  {
    unit = parse_source(src);
    if (!unit || !unit->diags.empty()) return false;
    program = unit->program;
    return true;
  }

  bool run_analysis()
  {
    if (!program) return false;

    module.program = program;
    module.types.register_builtins();

    for (const auto * ext_type : program->externTypes) {
      TypeSymbol sym;
      sym.name = ext_type->name;
      sym.decl = ext_type;
      sym.is_builtin = false;
      module.types.define(sym);
    }

    for (const auto * ext : program->externs) {
      NodeSymbol sym;
      sym.name = ext->name;
      sym.decl = ext;
      module.nodes.define(sym);
    }
    for (const auto * tree : program->trees) {
      NodeSymbol sym;
      sym.name = tree->name;
      sym.decl = tree;
      module.nodes.define(sym);
    }

    module.values.build_from_program(*program);

    SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
    if (!builder.build(*program)) return false;

    NameResolver resolver(module);
    if (!resolver.resolve()) return false;

    ConstEvaluator const_eval(unit->ast, types, module.values, &diags);
    if (!const_eval.evaluate_program(*program)) return false;

    // The key part: TypeChecker handles narrowing and type validity
    TypeChecker checker(types, module.types, module.values, &diags);
    if (!checker.check(*program)) return false;

    TreeRecursionChecker recursion_checker(&diags);
    if (!recursion_checker.check(*program)) return false;

    InitializationChecker init_checker(module.values, module.nodes, &diags);
    if (!init_checker.check(*program)) return false;

    NullChecker null_checker(module.values, module.nodes, &diags);
    return null_checker.check(*program);
  }

  std::string generate_xml()
  {
    if (!run_analysis()) return "";
    return XmlGenerator::generate(module);
  }

  bool xml_contains(const std::string & needle)
  {
    auto xml = generate_xml();
    return xml.find(needle) != std::string::npos;
  }

  std::unique_ptr<ParsedUnit> unit;
  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// Static Analysis: Narrowing & Validity
// ============================================================================

TEST(RefNullBehavior, NarrowingResetOnAssignment)
{
  // If we narrow T? -> T via guard, then assign null, it should revert to T? (or at least fail non-null check)
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        @guard(x != null)
        Sequence {
           x = null;    // Valid assignment to T?
           Use(val: x); // Should FAIL because x is now null (or type should be considered nullable again)
        }
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_analysis());
}

TEST(RefNullBehavior, NarrowingLeakPrevention)
{
  // Narrowing should not leak outside the guarded scope (User strictness request)
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        @guard(x != null)
        Use(val: x); // OK
      }
      Use(val: x); // Should FAIL: narrowing does not persist
    }
  )"));
  EXPECT_FALSE(ctx.run_analysis());
}

TEST(RefNullBehavior, OutPortImpliesNarrowing)
{
  // A variable passed to 'out' should be considered non-null if the node succeeds (DataPolicy All)
  // 6.2.3: "Success: Variable is written ... becomes non-null"
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Find(out res: int32);
    extern action Use(in val: int32);
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        Find(res: out x); // If this succeeds, x is initialized (init) and has a value (non-null)
        Use(val: x);      // Should OK without explicit guard
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_analysis());
}

// ============================================================================
// XML Mapping: Equality Assertions
// ============================================================================

TEST(RefNullBehavior, XmlMappingIsNull)
{
  // Test distinct mapping for x == null vs x != null
  // x != null -> BlackboardExists
  // x == null -> should be Inverted BlackboardExists or FailureIf(BlackboardExists)
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      var x: int32? = null;
      @guard(x == null)
      DoWork();
    }
  )"));
  // We look for evidence of inversion or "not exists" check
  EXPECT_TRUE(ctx.xml_contains("<Inverter>"));
  EXPECT_TRUE(ctx.xml_contains("<BlackboardExists"));
}

TEST(RefNullBehavior, XmlMappingComplexIsNull)
{
  // x == null || x < 0
  // Should use helper variable logic
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      var x: int32? = null;
      @guard(x == null || x < 0)
      DoWork();
    }
  )"));
  // Just ensure it generates valid XML structure with helpers
  // auto xml = ctx.generate_xml();
  EXPECT_TRUE(ctx.xml_contains("<ForceSuccess>"));
  EXPECT_TRUE(ctx.xml_contains("<BlackboardExists"));
}

// ============================================================================
// Extended Null Behavior Tests (Expanded Coverage)
// ============================================================================

TEST(RefNullBehavior, NullableUninitError)
{
  // Declaring a nullable var without initializer makes it Uninit initially.
  // Usage before assignment should fail.
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32?);
    tree Main() {
      var x: int32?; // Uninit
      Use(val: x);   // Error
    }
  )"));
  EXPECT_FALSE(ctx.run_analysis());
}

TEST(RefNullBehavior, RunWhileNarrowing)
{
  // @run_while(x != null) should narrow x to non-null inside the body.
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32); // Takes non-nullable input
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        @run_while(x != null)
        Use(val: x); // Should be OK (x is narrowed)
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_analysis());
}

TEST(RefNullBehavior, FallbackIndependentScoping)
{
  // Safety facts from one Fallback branch must NOT leak to the next.
  // Branch 1: Guard checks x != null.
  // Branch 2: Runs if Branch 1 failed (e.g. guard failed -> x is null).
  // Therefore, x is NOT safe in Branch 2.
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    #[behavior(Any, Chained)] extern control Fallback();
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Fallback {
        Sequence {
          @guard(x != null)
          Use(val: x);
        }
        // Reached if Sequence fails. x could be null.
        Use(val: x); // Should FAIL
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_analysis());
}

TEST(RefNullBehavior, AssignmentDoesNotNarrow)
{
  // Assigning a value to a nullable variable does NOT implicitly narrow it
  // (per strict specification/user request). It remains T?.
  NullTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    tree Main() {
      var x: int32? = null;
      x = 10;      // Assigning non-null literals
      Use(val: x); // Should still FAIL (x is strictly int32?)
    }
  )"));
  EXPECT_FALSE(ctx.run_analysis());
}
