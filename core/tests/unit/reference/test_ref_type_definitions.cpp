// tests/unit/reference/test_ref_type_definitions.cpp
// Reference compliance tests for: 3.1 Type Definitions (type-definitions.md)
//
// Tests that the type system correctly implements:
// - Primitive types (int, uint, float, bool, string)
// - Bounded string (string<N>)
// - Array types (static, bounded, dynamic)
// - Nullable types
// - External types
// - Type aliases

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

using namespace bt_dsl;

namespace
{

class TypeDefTestContext
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

  bool run_sema()
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
    return checker.check(*program);
  }

  bool has_error() const { return diags.has_errors(); }

  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// 3.1.1 Primitive Types - Integers
// ============================================================================

TEST(RefTypeDefinitions, SignedIntegerTypes)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A: int8 = 1;
    const B: int16 = 1;
    const C: int32 = 1;
    const D: int64 = 1;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, UnsignedIntegerTypes)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A: uint8 = 1;
    const B: uint16 = 1;
    const C: uint32 = 1;
    const D: uint64 = 1;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.1.1 Primitive Types - Floats
// ============================================================================

TEST(RefTypeDefinitions, FloatTypes)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A: float32 = 1.0;
    const B: float64 = 1.0;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.1.1 Primitive Types - Bool and String
// ============================================================================

TEST(RefTypeDefinitions, BoolType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: bool = true;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, StringType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string = "hello";)"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, BoundedStringType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string<100> = "hello";)"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.1.2 Array Types
// ============================================================================

TEST(RefTypeDefinitions, StaticArrayType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: [int32; 3] = [1, 2, 3];"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, StaticArraySizeFromConst)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const SIZE = 5;
    const X: [int32; SIZE] = [1, 2, 3, 4, 5];
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, BoundedArrayType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: [int32; <=5];"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, DynamicArrayType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: vec<int32> = vec![1, 2, 3];"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, NestedArrayType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: [[int32; 2]; 3] = [[1, 2], [3, 4], [5, 6]];"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.1.3 Nullable Types
// ============================================================================

TEST(RefTypeDefinitions, NullableType)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: int32? = null;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, NullToNonNullableError)
{
  // MUST FAIL: Cannot assign null to non-nullable type
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: int32 = null;"));
  EXPECT_FALSE(ctx.run_sema());
  EXPECT_TRUE(ctx.has_error());
}

// ============================================================================
// 3.1.4 Type Aliases
// ============================================================================

TEST(RefTypeDefinitions, TypeAliasBasic)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    type Distance = float64;
    const X: Distance = 10.0;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, TypeAliasTransparent)
{
  // Type alias is transparent - should work with original type
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    type MyInt = int32;
    const X: MyInt = 10;
    const Y: int32 = X;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.1.5 External Types
// ============================================================================

TEST(RefTypeDefinitions, ExternTypeBasic)
{
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    extern action GetPose(out result: Pose);
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, ExternTypeNoFieldAccess)
{
  // Note: Field access is not part of BT-DSL, so extern types are just opaque
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    var p: Pose;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, ExternTypeNotInterchangeable)
{
  // MUST FAIL: Different extern types are not compatible
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    extern type Point;
    extern action GetPose(out result: Pose);
    tree Main() {
      var p: Point;
      GetPose(result: out p);
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.1.4 Type Alias Constraints
// Reference: Circular type alias definitions are prohibited
// ============================================================================

TEST(RefTypeDefinitions, TypeAliasCircularError)
{
  // MUST FAIL: Circular type alias definition
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    type A = B;
    type B = A;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.1.1.4 Bounded String Byte Length
// Reference: string<N> means N bytes in UTF-8
// ============================================================================

TEST(RefTypeDefinitions, BoundedStringExceedsLimit)
{
  // MUST FAIL: String literal exceeds byte limit
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string<3> = "hello";)"));  // 5 bytes > 3
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeDefinitions, BoundedStringExactLimit)
{
  // String exactly at limit should be OK
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string<5> = "hello";)"));  // 5 bytes = 5
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, BoundedStringMultiByte)
{
  // "あ" is 3 bytes in UTF-8.
  // string<3> should fit "あ"
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string<3> = "あ";)"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeDefinitions, BoundedStringMultiByteOverflow)
{
  // "あ" is 3 bytes. string<2> should fail.
  TypeDefTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X: string<2> = "あ";)"));
  EXPECT_FALSE(ctx.run_sema());
}
