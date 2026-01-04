// tests/unit/reference/test_ref_type_compatibility.cpp
// Reference compliance tests for: 3.3 Compatibility and Conversion
//
// Tests that type compatibility correctly implements:
// - Widening conversions (implicit)
// - Narrowing conversions (require as)
// - Signed/unsigned mixing (error)
// - Array compatibility
// - ref/mut exact match requirement
// - Output widening

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
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

class CompatTestContext
{
public:
  bool parse(const std::string & src)
  {
    unit = parse_source(src);
    if (!unit || !unit->diags.empty()) return false;
    program = unit->program;
    return true;
  }

  bool run_sema()
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
    return checker.check(*program);
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
// 3.3.3 Widening Conversions (Implicit)
// Reference: int8 -> int16 -> int32 -> int64, etc.
// ============================================================================

TEST(RefTypeCompatibility, WideningIntegerOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: int8 = 1;
    const Y: int32 = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, WideningFloatOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: float32 = 1.0;
    const Y: float64 = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.3.4 Narrowing Conversions (Require as)
// ============================================================================

TEST(RefTypeCompatibility, NarrowingWithoutAsError)
{
  // MUST FAIL: Narrowing without explicit cast
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: int32 = 1;
    const Y: int8 = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeCompatibility, NarrowingWithAsOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: int32 = 1;
    const Y: int8 = X as int8;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.4.2 Mixed Signed/Unsigned Error
// Reference: int32 + uint32 is a type error
// ============================================================================

TEST(RefTypeCompatibility, MixedSignedUnsignedError)
{
  // MUST FAIL: Mixing signed and unsigned
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: int32 = 1;
    const Y: uint32 = 1;
    const Z = X + Y;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.5 Array Compatibility
// ============================================================================

TEST(RefTypeCompatibility, StaticArraySizeMismatchError)
{
  // MUST FAIL: Array sizes must match exactly
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: [int32; 3] = [1, 2, 3];
    const Y: [int32; 4] = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeCompatibility, StaticToBoundedArrayOk)
{
  // [T; N] -> [T; <=M] when N <= M
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: [int32; 3] = [1, 2, 3];
    var y: [int32; <=5] = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, StaticToBoundedArrayTooLargeError)
{
  // MUST FAIL: [T; 5] -> [T; <=3] when N > M
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: [int32; 5] = [1, 2, 3, 4, 5];
    var y: [int32; <=3] = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.6 Bounded String Compatibility
// ============================================================================

TEST(RefTypeCompatibility, BoundedStringToLargerOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: string<10> = "hello";
    const Y: string<100> = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, BoundedStringToSmallerError)
{
  // MUST FAIL: string<100> -> string<10>
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: string<100> = "hello";
    const Y: string<10> = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeCompatibility, BoundedStringToUnboundedOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: string<10> = "hello";
    const Y: string = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, UnboundedToBoundedError)
{
  // MUST FAIL: string -> string<N>
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: string = "hello";
    const Y: string<10> = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.7 ref/mut Requires Exact Match (Invariant)
// ============================================================================

TEST(RefTypeCompatibility, RefExactMatchRequired)
{
  // ref requires exact type match, no widening
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(ref x: int32);
    tree Main() {
      var x: int8 = 1;
      Foo(x: ref x);
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeCompatibility, MutExactMatchRequired)
{
  // mut requires exact type match
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(mut x: int32);
    tree Main() {
      var x: int8 = 1;
      Foo(x: mut x);
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.8 External Type Compatibility
// Reference: Extern types match only by same declaration
// ============================================================================

TEST(RefTypeCompatibility, ExternTypeSameOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    extern action GetPose(out result: Pose);
    tree Main() {
      var p: Pose;
      GetPose(result: out p);
    }
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, ExternTypeDifferentError)
{
  // MUST FAIL: Different extern types are incompatible
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    extern type Point;
    var p: Pose;
    var q: Point = p;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.3 Output Widening
// Reference: out T can be received by larger variable
// ============================================================================

TEST(RefTypeCompatibility, OutputWideningOk)
{
  // out int8 received by int32 variable
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetSmall(out result: int8);
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32;
      Sequence {
        GetSmall(result: out x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.3.5 Static Array to Dynamic Array Conversion
// Reference: Requires explicit cast
// ============================================================================

TEST(RefTypeCompatibility, StaticToDynamicArrayImplicitError)
{
  // MUST FAIL: Implicit static to dynamic array conversion
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: [int32; 3] = [1, 2, 3];
    var y: vec<int32> = X;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeCompatibility, StaticToDynamicArrayExplicitOk)
{
  // Explicit cast from static to dynamic array
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: [int32; 3] = [1, 2, 3];
    var y: vec<int32> = X as vec<_>;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.3.5 Bounded Array to Bounded Array
// Reference: [T; <=N] -> [T; <=M] when N <= M
// ============================================================================

TEST(RefTypeCompatibility, BoundedToBoundedArrayOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: [int32; <=3] = [1, 2];
    var y: [int32; <=5] = x;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeCompatibility, BoundedToBoundedArrayTooLargeError)
{
  // MUST FAIL: [T; <=5] -> [T; <=3]
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: [int32; <=5] = [1, 2];
    var y: [int32; <=3] = x;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.3.7 in Port Widening
// Reference: in port allows widening conversion
// ============================================================================

TEST(RefTypeCompatibility, InPortWideningOk)
{
  CompatTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int64);
    tree Main() {
      var x: int32 = 10;
      Foo(x: x);
    }
  )"));
  EXPECT_TRUE(ctx.run_sema());
}
