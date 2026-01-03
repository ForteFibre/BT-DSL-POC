// tests/sema/test_const_evaluator.cpp - Unit tests for constant evaluator
//
// Tests the ConstEvaluator which evaluates const_expr at compile time.
//

#include <gtest/gtest.h>

#include <cassert>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type.hpp"
#include "bt_dsl/sema/types/type_table.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

// Helper to parse, resolve names, and evaluate constants
struct TestContext
{
  ModuleInfo module;
  Program * program = nullptr;
  TypeContext types;
  DiagnosticBag diags;

  bool parse(const std::string & src)
  {
    module.parsedUnit = parse_source(src);
    if (!module.parsedUnit || !module.parsedUnit->diags.empty()) return false;
    program = module.parsedUnit->program;
    module.program = program;
    return program != nullptr;
  }

  bool resolve_names()
  {
    module.types = TypeTable{};
    module.nodes = NodeRegistry{};
    module.values = SymbolTable{};
    module.imports.clear();

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
    if (!builder.build(*program)) {
      return false;
    }

    NameResolver resolver(module, &diags);
    return resolver.resolve();
  }

  bool evaluate_consts()
  {
    ConstEvaluator eval(module.parsedUnit->ast, types, module.values, &diags);
    return eval.evaluate_program(*program);
  }

  const ConstValue * get_global_const_value(size_t idx) const
  {
    if (idx >= program->globalConsts.size()) return nullptr;
    return program->globalConsts[idx]->evaluatedValue;
  }
};

// ============================================================================
// Integer Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, IntegerLiteral)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 42;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 42);
}

TEST(SemaConstEvaluator, NegativeInteger)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = -10;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), -10);
}

// ============================================================================
// Float Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, FloatLiteral)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 3.14;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_float());
  EXPECT_NEAR(val->as_float(), 3.14, 0.01);
}

// ============================================================================
// Boolean Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, BoolLiteralTrue)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_TRUE(val->as_bool());
}

TEST(SemaConstEvaluator, BoolLiteralFalse)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = false;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_FALSE(val->as_bool());
}

// ============================================================================
// String Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, StringLiteral)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = \"hello\";"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_string());
  EXPECT_EQ(val->as_string(), "hello");
}

// ============================================================================
// Null Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, NullLiteral)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = null;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  EXPECT_TRUE(val->is_null());
}

// ============================================================================
// Cast Tests
// ============================================================================

TEST(SemaConstEvaluator, CastToExternTypeErrors)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Foo;
    const X = 1 as Foo;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, CastToDynamicArrayErrors)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X = 1 as vec<int32>;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, CastNumericToFloat64)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 as float64;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_float());
  EXPECT_DOUBLE_EQ(val->as_float(), 1.0);
  ASSERT_NE(val->type, nullptr);
  EXPECT_EQ(val->type->kind, TypeKind::Float64);
}

TEST(SemaConstEvaluator, CastNullToNullableOk)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = null as int32?;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  EXPECT_TRUE(val->is_null());
  ASSERT_NE(val->type, nullptr);
  EXPECT_EQ(val->type->kind, TypeKind::Nullable);
}

// ============================================================================
// Binary Arithmetic Tests
// ============================================================================

TEST(SemaConstEvaluator, AddIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 + 2;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 3);
}

TEST(SemaConstEvaluator, SubtractIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10 - 3;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 7);
}

TEST(SemaConstEvaluator, MultiplyIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 6 * 7;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 42);
}

TEST(SemaConstEvaluator, DivideIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10 / 3;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 3);  // Integer division
}

TEST(SemaConstEvaluator, ModuloIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10 % 3;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 1);
}

TEST(SemaConstEvaluator, ComplexExpression)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 + 2 * 3;"));  // Should be 7 (precedence)
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_integer());
  EXPECT_EQ(val->as_integer(), 7);
}

TEST(SemaConstEvaluator, FloatArithmetic)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1.5 + 2.5;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_float());
  EXPECT_NEAR(val->as_float(), 4.0, 0.01);
}

TEST(SemaConstEvaluator, MixedNumeric)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 + 2.5;"));  // int + float = float
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_float());
  EXPECT_NEAR(val->as_float(), 3.5, 0.01);
}

TEST(SemaConstEvaluator, StringConcatenation)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = \"hello\" + \" world\";"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_string());
  EXPECT_EQ(val->as_string(), "hello world");
}

// ============================================================================
// Comparison Tests
// ============================================================================

TEST(SemaConstEvaluator, ComparisonLessThan)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 < 2;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_TRUE(val->as_bool());
}

TEST(SemaConstEvaluator, ComparisonEqual)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 5 == 5;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_TRUE(val->as_bool());
}

// ============================================================================
// Logical Operation Tests
// ============================================================================

TEST(SemaConstEvaluator, LogicalAnd)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true && false;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_FALSE(val->as_bool());
}

TEST(SemaConstEvaluator, LogicalOr)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true || false;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_TRUE(val->as_bool());
}

TEST(SemaConstEvaluator, LogicalNot)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = !true;"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_bool());
  EXPECT_FALSE(val->as_bool());
}

// ============================================================================
// Const Reference Tests
// ============================================================================

TEST(SemaConstEvaluator, ConstReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = 10;
    const B = A;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val_a = ctx.get_global_const_value(0);
  const ConstValue * val_b = ctx.get_global_const_value(1);
  ASSERT_NE(val_a, nullptr);
  ASSERT_NE(val_b, nullptr);
  EXPECT_EQ(val_a->as_integer(), 10);
  EXPECT_EQ(val_b->as_integer(), 10);
}

TEST(SemaConstEvaluator, ConstForwardReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const B = A + 1;
    const A = 10;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val_b = ctx.get_global_const_value(0);
  const ConstValue * val_a = ctx.get_global_const_value(1);
  ASSERT_NE(val_a, nullptr);
  ASSERT_NE(val_b, nullptr);
  EXPECT_EQ(val_a->as_integer(), 10);
  EXPECT_EQ(val_b->as_integer(), 11);
}

TEST(SemaConstEvaluator, ConstChain)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = 1;
    const B = A + 1;
    const C = B + 1;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val_a = ctx.get_global_const_value(0);
  const ConstValue * val_b = ctx.get_global_const_value(1);
  const ConstValue * val_c = ctx.get_global_const_value(2);
  ASSERT_NE(val_a, nullptr);
  ASSERT_NE(val_b, nullptr);
  ASSERT_NE(val_c, nullptr);
  EXPECT_EQ(val_a->as_integer(), 1);
  EXPECT_EQ(val_b->as_integer(), 2);
  EXPECT_EQ(val_c->as_integer(), 3);
}

// ============================================================================
// Array Literal Tests
// ============================================================================

TEST(SemaConstEvaluator, ArrayLiteral)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [1, 2, 3];"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_array());
  auto arr = val->as_array();
  ASSERT_EQ(arr.size(), 3U);
  EXPECT_EQ(arr[0].as_integer(), 1);
  EXPECT_EQ(arr[1].as_integer(), 2);
  EXPECT_EQ(arr[2].as_integer(), 3);
}

TEST(SemaConstEvaluator, ArrayRepeat)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [0; 5];"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(0);
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_array());
  auto arr = val->as_array();
  ASSERT_EQ(arr.size(), 5U);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(arr[i].as_integer(), 0);
  }
}

TEST(SemaConstEvaluator, ArrayWithConstSize)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const N = 3;
    const X = [0; N];
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  ASSERT_TRUE(ctx.evaluate_consts());

  const ConstValue * val = ctx.get_global_const_value(1);  // X
  ASSERT_NE(val, nullptr);
  ASSERT_TRUE(val->is_array());
  auto arr = val->as_array();
  EXPECT_EQ(arr.size(), 3U);
}

// ============================================================================
// Error Cases
// ============================================================================

TEST(SemaConstEvaluator, ErrorDivisionByZero)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 / 0;"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, ErrorModuloByZero)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 10 % 0;"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, ErrorCircularReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = B;
    const B = A;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, ErrorNonConstReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: int = 10;
    const Y = x;
  )"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaConstEvaluator, ErrorVecMacro)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = vec![1, 2, 3];"));
  ASSERT_TRUE(ctx.resolve_names());
  const bool ok = ctx.evaluate_consts();
  EXPECT_FALSE(ok);  // vec is not allowed in const
  EXPECT_TRUE(ctx.diags.has_errors());
}
