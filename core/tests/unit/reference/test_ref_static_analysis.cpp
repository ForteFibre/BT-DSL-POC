// tests/unit/reference/test_ref_static_analysis.cpp
// Reference compliance tests for: 6. Static Analysis and Safety
//
// Tests that static analysis correctly implements:
// - Initialization safety (out write guarantee)
// - DataPolicy (All, Any, None)
// - FlowPolicy (Chained, Isolated)
// - Tree recursion prohibition
// - Port direction constraints

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

class AnalysisTestContext
{
public:
  bool parse(const std::string & src)
  {
    unit = parse_source(src);
    if (!unit || !unit->diags.empty()) return false;
    program = unit->program;
    return true;
  }

  bool run_full_analysis()
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

    TypeChecker checker(types, module.types, module.values, &diags);
    if (!checker.check(*program)) return false;

    // Tree recursion check
    TreeRecursionChecker recursion_checker(&diags);
    if (!recursion_checker.check(*program)) return false;

    // Initialization safety check
    InitializationChecker init_checker(module.values, module.nodes, &diags);
    return init_checker.check(*program);
  }

  bool has_error() const { return diags.has_errors(); }

  std::unique_ptr<ParsedUnit> unit;
  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// 6.3.1 Tree Recursion Prohibition
// ============================================================================

TEST(RefStaticAnalysis, DirectRecursionError)
{
  // MUST FAIL: Direct recursion
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() { Main(); }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, IndirectRecursionError)
{
  // MUST FAIL: Indirect recursion (A -> B -> A)
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree A() { B(); }
    tree B() { A(); }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, NonRecursiveTreeOk)
{
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() { Sub(); }
    tree Sub() {}
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1 Initialization Safety - Basic
// ============================================================================

TEST(RefStaticAnalysis, UseUninitializedVarError)
{
  // MUST FAIL: Using uninitialized var in 'in' port
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      var x: int32;
      Foo(x: x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, InitializedVarOk)
{
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(x: x);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, OutBeforeInOk)
{
  // out initializes var, then in can use it
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    extern action UseValue(in value: int32);
    extern control Sequence();
    tree Main() {
      var x: int32;
      Sequence {
        GetValue(result: out x);
        UseValue(value: x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.3 DataPolicy - All
// Reference: Parent success means all children succeeded, all outs initialized
// ============================================================================

TEST(RefStaticAnalysis, DataPolicyAllSequence)
{
  // Sequence (default All) - all children run, all outs initialized
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32, in b: int32);
    extern control Sequence();
    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        GetA(a: out a);
        GetB(b: out b);
      }
      Use(a: a, b: b);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.3 DataPolicy - Any
// Reference: Only common outs across all children are guaranteed
// ============================================================================

TEST(RefStaticAnalysis, DataPolicyAnyFallback)
{
  // Fallback (Any) - only common outs are initialized
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32);
    tree Main() {
      var a: int32;
      var b: int32;
      Fallback {
        GetA(a: out a);
        GetB(b: out b);
      }
      Use(a: b);
    }
  )"));
  // b is not guaranteed - only one branch writes it
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.3 DataPolicy - None
// Reference: No outs are guaranteed after parent success
// ============================================================================

TEST(RefStaticAnalysis, DataPolicyNoneNoGuarantee)
{
  // ForceSuccess (None) - no outs guaranteed
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(None)]
    extern decorator ForceSuccess();
    extern action GetValue(out result: int32);
    extern action Use(in value: int32);
    extern control Sequence();
    tree Main() {
      var x: int32;
      Sequence {
        ForceSuccess {
          GetValue(result: out x);
        }
        Use(value: x);
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.3 FlowPolicy - Isolated
// Reference: Siblings see only parent-start state, not each other's writes
// ============================================================================

TEST(RefStaticAnalysis, FlowPolicyIsolated)
{
  // Parallel (Isolated) - siblings don't see each other's writes
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(None, Isolated)]
    extern control Parallel();
    extern action GetValue(out result: int32);
    extern action Use(in value: int32);
    tree Main() {
      var x: int32;
      Parallel {
        GetValue(result: out x);
        Use(value: x);
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.2 Port Direction Compatibility
// ============================================================================

TEST(RefStaticAnalysis, PortDirectionInToOutError)
{
  // MUST FAIL: in argument to out port
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main() {
      var x: int32 = 10;
      GetValue(result: in x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, PortDirectionOutToInError)
{
  // MUST FAIL: out argument to in port
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseValue(in value: int32);
    tree Main() {
      var x: int32 = 10;
      UseValue(value: out x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, PortDirectionCorrectMatch)
{
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    extern action UseValue(in value: int32);
    extern control Sequence();
    tree Main() {
      var x: int32;
      Sequence {
        GetValue(result: out x);
        UseValue(value: x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.3 LValue Requirement
// Reference: ref/mut/out require lvalue
// ============================================================================

TEST(RefStaticAnalysis, OutRequiresLValue)
{
  // MUST FAIL: out to literal
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main() {
      GetValue(result: out 10);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.4 Tree Parameter Rights
// ============================================================================

TEST(RefStaticAnalysis, InParamCannotBeWritten)
{
  // MUST FAIL: in param passed as out
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main(in x: int32) {
      GetValue(result: out x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, OutParamCanBeWritten)
{
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main(out x: int32) {
      GetValue(result: out x);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.5 Out Argument Omission
// Reference: out arguments can be omitted (result discarded)
// ============================================================================

TEST(RefStaticAnalysis, OutArgumentOmissionOk)
{
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main() {
      GetValue();
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.2 Null Safety and Type Narrowing
// Reference: @guard(x != null) narrows T? to T
// ============================================================================

TEST(RefStaticAnalysis, NullableToOutConnection)
{
  // Nullable var connected to non-nullable out is OK
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action FindTarget(out result: int32);
    extern control Sequence();
    tree Main() {
      var target: int32? = null;
      Sequence {
        FindTarget(result: out target);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.4 Tree Parameter Rights - Additional
// ============================================================================

TEST(RefStaticAnalysis, RefParamCanBeRead)
{
  // ref param can be read (passed to in port)
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseValue(in value: int32);
    tree Main(ref x: int32) {
      UseValue(value: x);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, RefParamCannotBeWritten)
{
  // MUST FAIL: ref param cannot be written
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main(ref x: int32) {
      GetValue(result: out x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, MutParamCanBeReadAndWritten)
{
  // mut param can be read and written
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseValue(in value: int32);
    extern action GetValue(out result: int32);
    extern control Sequence();
    tree Main(mut x: int32) {
      Sequence {
        UseValue(value: x);
        GetValue(result: out x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.6 Default Value Constraints
// Reference: ref/mut/out cannot have default values
// ============================================================================

TEST(RefStaticAnalysis, RefPortDefaultValueError)
{
  // MUST FAIL at parse: ref port cannot have default
  AnalysisTestContext ctx;
  EXPECT_FALSE(ctx.parse(R"(
    extern action Foo(ref x: int32 = 10);
  )"));
}

TEST(RefStaticAnalysis, OutPortDefaultValueError)
{
  // MUST FAIL at parse: out port cannot have default
  AnalysisTestContext ctx;
  EXPECT_FALSE(ctx.parse(R"(
    extern action Foo(out x: int32 = 10);
  )"));
}

// ============================================================================
// 6.1.1 Out Write Guarantee with Preconditions
// Reference: Skipped nodes don't guarantee out writes
// ============================================================================

TEST(RefStaticAnalysis, SkippedNodeNoOutGuarantee)
{
  // When @success_if skips the node, out is not guaranteed
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    extern action UseValue(in value: int32);
    extern control Sequence();
    tree Main() {
      var x: int32;
      Sequence {
        @success_if(true)
        GetValue(result: out x);
        UseValue(value: x);
      }
    }
  )"));
  // x may not be initialized if GetValue is skipped
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.2.1 Flow-Sensitive Typing (Narrowing)
// Reference: @guard(x != null) allows x to be treated as T (not T?)
// ============================================================================

TEST(RefStaticAnalysis, NullableNarrowingInGuard)
{
  // Positive test: Passing a nullable var 'x' to an 'in int32' port
  // should SUCCEED if it is inside a @guard(x != null) block.
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        @guard(x != null)
        Use(val: x);
      }
    }
  )"));
  // If narrowing works, checking should pass
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, NarrowingWithConjunction)
{
  // @guard(x != null && y != null) -> Both x and y should be non-null
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    extern control Sequence();
    tree Main() {
      var x: int32? = null;
      var y: int32? = null;
      Sequence {
        @guard(x != null && y != null)
        Sequence {
          Use(val: x); // x should be treated as int32
          Use(val: y); // y should be treated as int32
        }
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, NarrowingWithNegation)
{
  // @guard(!(x == null)) -> Should be treated same as x != null
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Use(in val: int32);
    extern control Sequence();
    tree Main() {
      var x: int32? = null;
      Sequence {
        @guard(!(x == null))
        Use(val: x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1 Initialization Safety - ref and mut
// Reference: ref/mut arguments MUST be init at call site
// ============================================================================

TEST(RefStaticAnalysis, RefArgMustBeInit)
{
  // MUST FAIL: Passing Uninit var to ref port
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseRef(ref r: int32);
    tree Main() {
      var x: int32; // Uninit
      UseRef(r: x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefStaticAnalysis, MutArgMustBeInit)
{
  // MUST FAIL: Passing Uninit var to mut port
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseMut(mut m: int32);
    tree Main() {
      var x: int32; // Uninit
      UseMut(m: x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.3.2 Warning Check (Unused)
// ============================================================================

TEST(RefStaticAnalysis, UnusedMutParamWarning)
{
  // Should produce a WARNING (not error, but diagnostics should not be empty)
  // Note: Current testing infrastructure checks has_errors(), we need to check non-empty diags for warnings
  AnalysisTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main(mut x: int32) {
      // x is unused
    }
  )"));
  ctx.run_full_analysis();
  EXPECT_FALSE(ctx.has_error());  // Should NOT fail compilation
  // EXPECT_FALSE(ctx.diags.empty()); // Should have warnings - TBD if warning infrastructure is ready
}
