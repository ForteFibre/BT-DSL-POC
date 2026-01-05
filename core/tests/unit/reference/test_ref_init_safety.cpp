// tests/unit/reference/test_ref_init_safety.cpp
// Reference compliance tests for: 6. Static Analysis and Safety (Extended)
//
// These tests extend test_ref_static_analysis.cpp with more detailed coverage for:
// - DataPolicy (All/Any positive cases)
// - Port direction matrix (Errors/Warnings)
// - Tree parameter rights (in/out/ref/mut passing)
// - Skipped node initialization guarantees

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

using namespace bt_dsl;

namespace
{

class InitSafetyTestContext
{
public:
  bool parse(const std::string & src)
  {
    auto parsed = test_support::parse(src);
    if (parsed.program == nullptr || parsed.diags.has_errors()) return false;

    program = parsed.program;
    module.file_id = parsed.file_id;
    module.ast = std::move(parsed.ast);
    module.parse_diags = std::move(parsed.diags);
    module.program = program;
    return true;
  }

  bool run_full_analysis()
  {
    if (!program) return false;

    module.program = program;
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

    SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
    if (!builder.build(*program)) return false;

    NameResolver resolver(module);
    if (!resolver.resolve()) return false;

    if (!module.ast) return false;

    ConstEvaluator const_eval(*module.ast, types, module.values, &diags);
    if (!const_eval.evaluate_program(*program)) return false;

    TypeChecker checker(types, module.types, module.values, &diags);
    if (!checker.check(*program)) return false;

    // Initialization safety check
    InitializationChecker init_checker(module.values, module.nodes, &diags);
    return init_checker.check(*program);
  }

  bool has_error() const { return diags.has_errors(); }

  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// 6.1.3 DataPolicy - All/Any (Positive Cases)
// ============================================================================

TEST(RefInitSafety, DataPolicyAnyPositive)
{
  // Fallback (Any) - if ALL children write to the SAME variable, it becomes Init.
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    extern action GetVal(out x: int32);
    extern action GetValRef(out y: int32);
    extern action Use(in x: int32);

    tree Main() {
      var x: int32;
      Fallback {
        // Branch 1 writes x
        GetVal(x: out x);
        
        // Branch 2 writes x (and y, but y is ignored for intersection)
        GetValRef(y: out x);
      }
      // x should be Init because both branches wrote to it
      Use(x: x); 
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, DataPolicyAnyComplexSubset)
{
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    extern action DoA(out a: int32, out b: int32);
    extern action DoB(out a: int32, out c: int32);
    extern action Use(in v: int32);

    tree Main() {
      var x: int32;
      var y: int32;
      var z: int32;

      Fallback {
        DoA(a: out x, b: out y);
        DoB(a: out x, c: out z);
      }
      
      // x is safe
      Use(v: x);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.1 Skipped Node Guarantees (Conditionals)
// ============================================================================

TEST(RefInitSafety, FailureIfSkippedNoGuarantee)
{
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(Any, Chained)] extern control Fallback();

    tree Main() {
      var x: int32;
      Fallback {
        // If true, returns Failure immediately, Get(x) not called -> x Uninit
        @failure_if(true)
        Get(x: out x);

        // Reached if Get fails
        Use(x: x);
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, SkipIfSkippedNoGuarantee)
{
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(All, Chained)] extern control Sequence();

    tree Main() {
      var x: int32;
      Sequence {
        @skip_if(true)
        Get(x: out x);

        Use(x: x);
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.2 Port Direction & Argument Matrix
// ============================================================================

// --- Arg: IN cases (already covered partially, checking 'in -> ref/mut')

TEST(RefInitSafety, InArgToRefPortError)
{
  // in x -> ref p: Error
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: in x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, InArgToMutPortError)
{
  // in x -> mut p: Error
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(mut p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: in x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// --- Arg: REF cases

TEST(RefInitSafety, RefArgToInPortOk)
{
  // ref x -> in p: OK (or Warning)
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: ref x);
    }
  )"));
  // Assuming warning doesn't block analysis success, unless -Werror
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, RefArgToMutPortError)
{
  // ref x -> mut p: Error (Insufficient rights, ref is ReadOnly, mut needs RW)
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(mut p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: ref x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, RefArgToOutPortError)
{
  // ref x -> out p: Error (ref is ReadOnly, out needs Write)
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(out p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: ref x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// --- Arg: MUT cases

TEST(RefInitSafety, MutArgToRefPortOk)
{
  // mut x -> ref p: OK (or Warning, but safe)
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: mut x);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, MutArgToOutPortError)
{
  // mut x -> out p: Error per table 6.4.2
  // "arg:mut vs port:out -> X Error"
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(out p: int32);
    tree Main() {
      var x: int32 = 10;
      Foo(p: mut x);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.4 Tree Parameter Rights (Passing params to nodes)
// ============================================================================

TEST(RefInitSafety, TreeInParamToRefPortError)
{
  // tree(in x) -> node(ref p): Error
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref p: int32);
    tree Main(in x: int32) {
      Foo(p: x); // Implicit 'in' arg
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, TreeOutParamToRefPortError)
{
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref p: int32);
    tree Main(out x: int32) {
      Foo(p: x); // Implicit in? Or explicit ref?
      // Even if we try explicit ref:
      // Foo(p: ref x);
      // It should fail because 'out x' is not compatible with 'ref' requirement (Read)
      // until it is initialized?
      // But table 6.4.4 says "Param out" -> "Arg ref" is Error.
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, TreeOutParamToMutPortError)
{
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Init(out x: int32);
    extern action UseMut(mut x: int32);
    #[behavior(All, Chained)] extern control Sequence();
    tree Main(out x: int32) {
      Sequence {
        Init(x: out x); // Now x is Init
        UseMut(x: mut x); // Should be OK
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, TreeOutParamToMutPortUninitError)
{
  // Same as above but verify Error if Uninit
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action UseMut(mut x: int32);
    tree Main(out x: int32) {
      UseMut(x: mut x); 
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.3 LValue Requirements
// ============================================================================

TEST(RefInitSafety, LiteralToRefError)
{
  // ref p <- 10 : Error
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref p: int32);
    tree Main() {
      Foo(p: ref 10);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, LiteralToMutError)
{
  // mut p <- 10 : Error
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(mut p: int32);
    tree Main() {
      Foo(p: mut 10);
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.4.6 Default Values
// ============================================================================

TEST(RefInitSafety, MutPortDefaultValueError)
{
  // mut p: int32 = 10 : Error
  InitSafetyTestContext ctx;
  EXPECT_FALSE(ctx.parse(R"(
    extern action Foo(mut p: int32 = 10);
  )"));
}

// ============================================================================
// Edge Cases: Nested Control Nodes
// ============================================================================

TEST(RefInitSafety, NestedSequenceInFallbackOk)
{
  // Fallback (Any) containing Sequence (All)
  // If Fallback succeeds, one of its children succeeded.
  // If that child is a Sequence, all of its children succeeded -> all outs init.
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    #[behavior(All, Chained)] extern control Sequence();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32, in b: int32);

    tree Main() {
      var a: int32;
      var b: int32;
      Fallback {
        Sequence {
          GetA(a: out a);
          GetB(b: out b);
        }
        Sequence {
          GetA(a: out a);
          GetB(b: out b);
        }
      }
      // Both branches write a and b, so they are Init after Fallback
      Use(a: a, b: b);
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, NestedFallbackInSequencePartialInit)
{
  // Sequence (All) containing Fallback (Any)
  // After Sequence succeeds, all children succeeded.
  // But Fallback only guarantees intersection of its children's outs.
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    #[behavior(All, Chained)] extern control Sequence();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32);

    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        Fallback {
          GetA(a: out a);  // Branch 1: writes a
          GetB(b: out b);  // Branch 2: writes b
        }
        // After Fallback, neither a nor b is guaranteed (no intersection)
        Use(a: a);  // Error: a might be Uninit
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, NestedSequenceAllChainedOk)
{
  // Nested Sequence with Chained flow - writes propagate through
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Chained)] extern control Sequence();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32, in b: int32);

    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        Sequence {
          GetA(a: out a);
        }
        Sequence {
          GetB(b: out b);
        }
        Use(a: a, b: b);  // Both are Init
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// Edge Cases: @run_while Loop
// ============================================================================

TEST(RefInitSafety, RunWhileLoopMayNotExecute)
{
  // @run_while(cond) - if condition is false initially, body never executes
  // So out writes inside the loop are not guaranteed
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(All, Chained)] extern control Sequence();

    tree Main() {
      var x: int32;
      Sequence {
        @run_while(false)
        Get(x: out x);  // Never executes

        Use(x: x);  // Error: x is Uninit
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, RunWhileWithPreInit)
{
  // If variable is pre-initialized, @run_while doesn't matter
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(All, Chained)] extern control Sequence();

    tree Main() {
      var x: int32 = 0;  // Pre-initialized
      Sequence {
        @run_while(false)
        Get(x: out x);

        Use(x: x);  // OK: x was initialized at declaration
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// Edge Cases: Isolated Flow Policy
// ============================================================================

TEST(RefInitSafety, IsolatedFlowPolicyAfterCompletion)
{
  // After Parallel (Isolated, All) completes successfully,
  // all children have succeeded, so all outs are initialized
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Isolated)]
    extern control ParallelAll();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32, in b: int32);
    #[behavior(All, Chained)] extern control Sequence();

    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        ParallelAll {
          GetA(a: out a);
          GetB(b: out b);
        }
        // After ParallelAll succeeds, both a and b are Init
        Use(a: a, b: b);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, IsolatedFlowPolicyNoSiblingVisibility)
{
  // Verify siblings in Isolated cannot see each other's writes
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Isolated)]
    extern control ParallelAll();
    extern action GetA(out a: int32);
    extern action Use(in a: int32);

    tree Main() {
      var a: int32;
      ParallelAll {
        GetA(a: out a);
        Use(a: a);  // Error: a is Uninit at Parallel start
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// Edge Cases: DataPolicy None Propagation
// ============================================================================

TEST(RefInitSafety, DataPolicyNoneNestedInSequence)
{
  // ForceSuccess (None) inside Sequence
  // Even if node inside ForceSuccess writes, it's not guaranteed
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(None)]
    extern decorator ForceSuccess();
    #[behavior(All, Chained)] extern control Sequence();
    extern action Get(out x: int32);
    extern action Use(in x: int32);

    tree Main() {
      var x: int32;
      Sequence {
        ForceSuccess {
          Get(x: out x);
        }
        // x is NOT guaranteed even though we're in Sequence
        Use(x: x);  // Error
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, DataPolicyNoneWithPreInit)
{
  // If variable is pre-initialized, DataPolicy None doesn't affect it
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(None)]
    extern decorator ForceSuccess();
    #[behavior(All, Chained)] extern control Sequence();
    extern action Get(out x: int32);
    extern action Use(in x: int32);

    tree Main() {
      var x: int32 = 0;  // Pre-initialized
      Sequence {
        ForceSuccess {
          Get(x: out x);
        }
        Use(x: x);  // OK: x was already Init
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

// ============================================================================
// Edge Cases: Complex Combinations
// ============================================================================

TEST(RefInitSafety, SequenceThenFallbackThenSequence)
{
  // Deep nesting: Sequence -> Fallback -> Sequence
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    #[behavior(All, Chained)] extern control Sequence();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in a: int32);

    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        Fallback {
          Sequence {
            GetA(a: out a);
            GetB(b: out b);
          }
          Sequence {
            GetA(a: out a);
            // Note: b is NOT written in this branch
          }
        }
        // After Fallback: a is Init (both branches write it)
        // After Fallback: b is NOT guaranteed (only first branch writes it)
        Use(a: a);  // OK
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, SequenceThenFallbackNonCommonError)
{
  // Same as above but try to use non-common variable
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(Any)]
    extern control Fallback();
    #[behavior(All, Chained)] extern control Sequence();
    extern action GetA(out a: int32);
    extern action GetB(out b: int32);
    extern action Use(in b: int32);

    tree Main() {
      var a: int32;
      var b: int32;
      Sequence {
        Fallback {
          Sequence {
            GetA(a: out a);
            GetB(b: out b);
          }
          Sequence {
            GetA(a: out a);
            // b is NOT written here
          }
        }
        Use(b: b);  // Error: b might be Uninit
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// Edge Cases: Guard with Initialization
// ============================================================================

TEST(RefInitSafety, GuardDoesNotAffectInitState)
{
  // @guard only affects null narrowing, not Init state
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(All, Chained)] extern control Sequence();

    tree Main() {
      var x: int32;
      Sequence {
        @guard(true)
        Get(x: out x);

        Use(x: x);  // x should be Init if guard passes and Get succeeds
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_full_analysis());
}

TEST(RefInitSafety, GuardFalseMeansNodeSkipped)
{
  // @guard(false) は Failure を返すため、子ノード本体は実行されず out も書き込まれない。
  // そのうえで「後続が実行される」構造（Fallback）では、未初期化使用が検出されるべき。
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Get(out x: int32);
    extern action Use(in x: int32);
    #[behavior(Any, Chained)] extern control Fallback();

    tree Main() {
      var x: int32;
      Fallback {
        @guard(false)
        Get(x: out x);

        Use(x: x);  // Error: Get は実行されないので x は Uninit
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

// ============================================================================
// 6.1.6 Expression Initialization Safety (New)
// ============================================================================

TEST(RefInitSafety, UninitVarInAssignmentRHSError)
{
  // var y = x; where x is uninit
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32;
      var y: int32 = x; // Error: x is uninit
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}

TEST(RefInitSafety, UninitVarInBinaryExprError)
{
  // var y = x + 1; where x is uninit
  InitSafetyTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32;
      var y: int32 = x + 1; // Error: x is uninit
    }
  )"));
  EXPECT_FALSE(ctx.run_full_analysis());
}
