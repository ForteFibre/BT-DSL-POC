// tests/unit/reference/test_ref_expression_typing.cpp
// Reference compliance tests for: 3.4 Expression Typing (expression-typing.md)
//
// Tests that expression typing correctly implements:
// - Unary operators (-, !)
// - Binary operators (+, -, *, /, %, <, <=, >, >=, ==, !=, &&, ||, &, |, ^)
// - Cast expressions (as)
// - Array access

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

class ExprTypingTestContext
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
// 3.4.1 Unary Operators
// ============================================================================

TEST(RefExpressionTyping, UnaryNegationInteger)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = -42;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::IntegerLiteral);
}

TEST(RefExpressionTyping, UnaryNegationFloat)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = -3.14;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_float() || t->kind == TypeKind::FloatLiteral);
}

TEST(RefExpressionTyping, UnaryNegationBoolError)
{
  // MUST FAIL: Cannot negate bool
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = -true;"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefExpressionTyping, LogicalNotBool)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = !true;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(RefExpressionTyping, LogicalNotIntegerError)
{
  // MUST FAIL: Cannot apply ! to integer
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = !42;"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.2 Binary Operators - Arithmetic
// ============================================================================

TEST(RefExpressionTyping, BinaryAddIntegers)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 + 2;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::IntegerLiteral || t->kind == TypeKind::Int32);
}

TEST(RefExpressionTyping, BinaryAddFloats)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1.0 + 2.0;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_float() || t->kind == TypeKind::FloatLiteral || t->kind == TypeKind::Float64);
}

TEST(RefExpressionTyping, BinaryAddStrings)
{
  // String concatenation
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(const X = "hello" + " world";)"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_string());
}

TEST(RefExpressionTyping, ModuloIntegers)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10 % 3;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::IntegerLiteral);
}

TEST(RefExpressionTyping, ModuloFloatError)
{
  // MUST FAIL: % on floats is not allowed
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10.0 % 3.0;"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.2 Binary Operators - Comparison
// ============================================================================

TEST(RefExpressionTyping, ComparisonReturnsBool)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 < 2;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(RefExpressionTyping, ComparisonAllOperators)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = 1 < 2;
    const B = 1 <= 2;
    const C = 1 > 2;
    const D = 1 >= 2;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefExpressionTyping, EqualityReturnsBool)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 == 2;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

// ============================================================================
// 3.4.2 Binary Operators - Logical
// ============================================================================

TEST(RefExpressionTyping, LogicalAndBool)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true && false;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(RefExpressionTyping, LogicalOrBool)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true || false;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(RefExpressionTyping, LogicalAndNonBoolError)
{
  // MUST FAIL: && requires bool operands
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 && 2;"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.2 Binary Operators - Bitwise
// ============================================================================

TEST(RefExpressionTyping, BitwiseAndIntegers)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 0xFF & 0x0F;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::IntegerLiteral);
}

TEST(RefExpressionTyping, BitwiseOrIntegers)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 0xF0 | 0x0F;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefExpressionTyping, BitwiseXorIntegers)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 0xFF ^ 0x0F;"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefExpressionTyping, BitwiseOnFloatError)
{
  // MUST FAIL: Bitwise operators require integers
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1.0 & 2.0;"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.3 Cast Expression
// ============================================================================

TEST(RefExpressionTyping, CastIntToFloat)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 42 as float64;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Float64);
}

TEST(RefExpressionTyping, CastFloatToInt)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 3.14 as int32;"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Int32);
}

// ============================================================================
// 3.4.4 Array Access
// ============================================================================

TEST(RefExpressionTyping, ArrayAccessStaticArray)
{
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const ARR: [int32; 3] = [1, 2, 3];
    const X = ARR[0];
  )"));
  EXPECT_TRUE(ctx.run_sema());
  const auto * t = ctx.get_const_type(1);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::Int32);
}

TEST(RefExpressionTyping, ArrayAccessOutOfBoundsError)
{
  // MUST FAIL: Static bounds check for const index
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const ARR: [int32; 3] = [1, 2, 3];
    const X = ARR[5];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefExpressionTyping, ArrayAccessNegativeIndexError)
{
  // MUST FAIL: Negative index
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const ARR: [int32; 3] = [1, 2, 3];
    const X = ARR[-1];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefExpressionTyping, ArrayAccessNonIntegerIndexError)
{
  // MUST FAIL: Index must be integer
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const ARR: [int32; 3] = [1, 2, 3];
    const X = ARR[1.5];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.3 Cast Constraints
// Reference: Cast to extern type is not allowed
// ============================================================================

TEST(RefExpressionTyping, CastToExternTypeError)
{
  // MUST FAIL: Cannot cast to extern type
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Pose;
    const X = 42 as Pose;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 3.4.4 Array Access on Non-Array
// ============================================================================

TEST(RefExpressionTyping, ArrayAccessOnNonArrayError)
{
  // MUST FAIL: Cannot index non-array type
  ExprTypingTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X: int32 = 42;
    const Y = X[0];
  )"));
  EXPECT_FALSE(ctx.run_sema());
}
