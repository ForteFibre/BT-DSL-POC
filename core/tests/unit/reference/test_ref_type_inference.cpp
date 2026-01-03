// tests/unit/reference/test_ref_type_inference.cpp
// Reference compliance tests for: 3.2 Type Inference (inference-and-resolution.md)
//
// Tests that type inference correctly implements:
// - Literal type defaults ({integer} -> int32, {float} -> float64)
// - null literal inference
// - Array literal inference
// - Contextual typing
// - var/const type determination

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
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

class InferenceTestContext
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

  const Type * get_const_type(size_t idx) const
  {
    if (idx >= program->globalConsts.size()) return nullptr;
    auto * expr = program->globalConsts[idx]->value;
    return expr ? expr->resolvedType : nullptr;
  }

  std::unique_ptr<ParsedUnit> unit;
  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// 3.2.1 Literal Type Inference - Integer
// Reference: Integer literals default to int32
// ============================================================================

TEST(RefTypeInference, IntegerLiteralDefaultsToInt32)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 42;"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  // Default for integer literal is int32 or IntegerLiteral
  EXPECT_TRUE(type->kind == TypeKind::Int32 || type->kind == TypeKind::IntegerLiteral);
}

TEST(RefTypeInference, IntegerLiteralWithAnnotation)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: int64 = 42;"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->kind, TypeKind::Int64);
}

// ============================================================================
// 3.2.1 Literal Type Inference - Float
// Reference: Float literals default to float64
// ============================================================================

TEST(RefTypeInference, FloatLiteralDefaultsToFloat64)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 3.14;"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  EXPECT_TRUE(type->kind == TypeKind::Float64 || type->kind == TypeKind::FloatLiteral);
}

TEST(RefTypeInference, FloatLiteralWithAnnotation)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: float32 = 3.14;"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->kind, TypeKind::Float32);
}

// ============================================================================
// 3.2.1 Literal Type Inference - null
// Reference: null requires context to determine base type
// ============================================================================

TEST(RefTypeInference, NullWithTypeAnnotation)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: int32? = null;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, NullWithoutContextError)
{
  // MUST FAIL: null without context cannot determine type
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x = null;"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.2.1 Literal Type Inference - Array
// Reference: [e1, e2, e3] defaults to static array [T; 3]
// ============================================================================

TEST(RefTypeInference, ArrayLiteralInfersStaticArray)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [1, 2, 3];"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->kind, TypeKind::StaticArray);
  EXPECT_EQ(type->size, 3U);
  ASSERT_NE(type->element_type, nullptr);
  EXPECT_TRUE(
    type->element_type->is_integer() || type->element_type->kind == TypeKind::IntegerLiteral);
}

TEST(RefTypeInference, ArrayRepeatInfersStaticArray)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [0; 5];"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(type->kind, TypeKind::StaticArray);
  EXPECT_EQ(type->size, 5U);
}

TEST(RefTypeInference, VecMacroInfersDynamicArray)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: vec<int32> = vec![1, 2, 3];"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.2.2 Type Inference Wildcards
// ============================================================================

TEST(RefTypeInference, WildcardInferFromInit)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: _ = 10;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, NullableWildcard)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: _? = 1.0;"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 3.2.4 var/const Type Determination
// Reference: Priority: 1) type annotation 2) init expression
// ============================================================================

TEST(RefTypeInference, VarTypeFromAnnotation)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: int64 = 10;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, VarTypeFromInit)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x = 10;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, GlobalVarMustHaveTypeOrInit)
{
  // MUST FAIL: Global var without type or init
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("var x;"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefTypeInference, ConstMustHaveInit)
{
  // const must have initializer (syntax level)
  InferenceTestContext ctx;
  // This should fail at parse level
  EXPECT_FALSE(ctx.parse("const X: int32;"));
}

// ============================================================================
// 3.2.4 Const reference in array size
// Reference: Array size identifier must be const
// ============================================================================

TEST(RefTypeInference, ArraySizeFromConst)
{
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const SIZE = 3;
    const X: [int32; SIZE] = [1, 2, 3];
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, ArraySizeFromVarError)
{
  // MUST FAIL: Array size cannot be from var
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var size: int32 = 3;
    var x: [int32; size];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.2.1 Contextual Typing for Bounded Arrays
// Reference: Upper-bounded arrays use contextual typing
// ============================================================================

TEST(RefTypeInference, ContextualTypingBoundedArray)
{
  // Bounded array gets type from context
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: [int32; <=5] = [1, 2, 3];
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefTypeInference, BoundedArrayExceedsLimitError)
{
  // MUST FAIL: Array literal exceeds bound
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: [int32; <=3] = [1, 2, 3, 4, 5];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.2.1 Upper-bounded array cannot be inferred from literal
// Reference: [T; <=N] requires explicit type annotation
// ============================================================================

TEST(RefTypeInference, BoundedArrayNotInferredFromLiteral)
{
  // Array literal infers static array, not bounded
  InferenceTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [1, 2, 3];"));
  EXPECT_TRUE(ctx.run_sema());

  const auto * type = ctx.get_const_type(0);
  ASSERT_NE(type, nullptr);
  // Should be StaticArray, not BoundedArray
  EXPECT_EQ(type->kind, TypeKind::StaticArray);
}
