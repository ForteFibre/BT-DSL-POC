// tests/sema/test_type_checker.cpp - Unit tests for type checker
//
// Tests the TypeChecker which performs bidirectional type inference.
//

#include <gtest/gtest.h>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/sema/types/type_table.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

static bool has_warning_containing(const DiagnosticBag & diags, std::string_view needle)
{
  const std::string needle_str(needle);
  const auto warns = diags.warnings();
  return std::any_of(warns.begin(), warns.end(), [&](const Diagnostic & d) {
    return d.message.find(needle_str) != std::string::npos;
  });
}

// Helper to parse, resolve names, evaluate constants, and type check
struct TestContext
{
  std::unique_ptr<ParsedUnit> unit;
  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;

  bool parse(const std::string & src)
  {
    unit = parse_source(src);
    if (!unit || !unit->diags.empty()) return false;
    program = unit->program;
    return program != nullptr;
  }

  bool resolve_names()
  {
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
    if (!builder.build(*program)) {
      return false;
    }

    NameResolver resolver(module);
    return resolver.resolve();
  }

  bool evaluate_consts()
  {
    ConstEvaluator eval(unit->ast, types, module.values, &diags);
    return eval.evaluate_program(*program);
  }

  bool type_check()
  {
    TypeChecker checker(types, module.types, module.values, &diags);
    return checker.check(*program);
  }

  bool run_all() { return resolve_names() && evaluate_consts() && type_check(); }

  // Helper to get the resolved type of a global const's value expression
  const Type * get_global_const_expr_type(size_t idx) const
  {
    if (idx >= program->globalConsts.size()) return nullptr;
    const Expr * value_expr = program->globalConsts[idx]->value;
    return value_expr ? value_expr->resolvedType : nullptr;
  }

  // Helper to get the resolved type of a global var's initial value expression
  const Type * get_global_var_expr_type(size_t idx) const
  {
    if (idx >= program->globalVars.size()) return nullptr;
    const Expr * init_expr = program->globalVars[idx]->initialValue;
    return init_expr ? init_expr->resolvedType : nullptr;
  }
};

// ============================================================================
// Integer Literal Type Tests
// ============================================================================

TEST(SemaTypeChecker, IntegerLiteralDefaultsToInt32)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 42;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  // Default for integer literal is int32
  EXPECT_TRUE(t->kind == TypeKind::IntegerLiteral || t->kind == TypeKind::Int32);
}

TEST(SemaTypeChecker, IntegerWithTypeAnnotation)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X: int64 = 42;"));
  ASSERT_TRUE(ctx.run_all());

  // Type checking should succeed
  EXPECT_FALSE(ctx.diags.has_errors());
}

// ============================================================================
// Float Literal Type Tests
// ============================================================================

TEST(SemaTypeChecker, FloatLiteralDefaultsToFloat64)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 3.14;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  // Default for float literal is float64
  EXPECT_TRUE(t->kind == TypeKind::FloatLiteral || t->kind == TypeKind::Float64);
}

// ============================================================================
// Boolean and String Literal Tests
// ============================================================================

TEST(SemaTypeChecker, BoolLiteralType)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(SemaTypeChecker, StringLiteralType)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = \"hello\";"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::String);
}

// ============================================================================
// Binary Expression Type Tests
// ============================================================================

TEST(SemaTypeChecker, BinaryAddIntegers)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 + 2;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->kind == TypeKind::IntegerLiteral);
}

TEST(SemaTypeChecker, BinaryAddFloats)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1.0 + 2.0;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_float() || t->kind == TypeKind::FloatLiteral);
}

TEST(SemaTypeChecker, BinaryComparisonReturnsBool)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 1 < 2;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(SemaTypeChecker, BinaryEqualityReturnsBool)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = 5 == 5;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(SemaTypeChecker, BinaryLogicalAnd)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = true && false;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

TEST(SemaTypeChecker, StringConcatenation)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = \"hello\" + \" world\";"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::String);
}

// ============================================================================
// Unary Expression Type Tests
// ============================================================================

TEST(SemaTypeChecker, UnaryNegation)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = -42;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_integer() || t->is_numeric() || t->kind == TypeKind::IntegerLiteral);
}

TEST(SemaTypeChecker, UnaryNot)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = !true;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

// ============================================================================
// Array Type Tests
// ============================================================================

TEST(SemaTypeChecker, ArrayLiteralType)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [1, 2, 3];"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::StaticArray);
  EXPECT_EQ(t->size, 3U);
}

TEST(SemaTypeChecker, ArrayRepeatType)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = [0; 5];"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::StaticArray);
  EXPECT_EQ(t->size, 5U);
}

TEST(SemaTypeChecker, ArrayRepeatTypeFromConstReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const N = 5;
    const X = [0; N];
  )"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(1);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::StaticArray);
  EXPECT_EQ(t->size, 5U);
}

TEST(SemaTypeChecker, ArrayRepeatNegativeCountErrors)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const N = -1;
    var X = [0; N];
  )"));
  const bool ok = ctx.run_all();
  EXPECT_FALSE(ok);
  EXPECT_TRUE(ctx.diags.has_errors());
}

// ============================================================================
// Contextual Typing Tests
// ============================================================================

TEST(SemaTypeChecker, VarWithTypeAnnotation)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("var x: int64 = 42;"));
  ASSERT_TRUE(ctx.run_all());

  // Should pass without errors
  EXPECT_FALSE(ctx.diags.has_errors());
}

TEST(SemaTypeChecker, VarInferredType)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("var x = true;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_var_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->kind, TypeKind::Bool);
}

// ============================================================================
// Complex Expression Tests
// ============================================================================

TEST(SemaTypeChecker, NestedExpression)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse("const X = (1 + 2) * 3;"));
  ASSERT_TRUE(ctx.run_all());

  const Type * t = ctx.get_global_const_expr_type(0);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(t->is_numeric() || t->kind == TypeKind::IntegerLiteral);
}

TEST(SemaTypeChecker, ConstReference)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = 10;
    const B = A + 1;
  )"));
  ASSERT_TRUE(ctx.run_all());

  // Both should be typed
  const Type * t_a = ctx.get_global_const_expr_type(0);
  const Type * t_b = ctx.get_global_const_expr_type(1);
  EXPECT_NE(t_a, nullptr);
  EXPECT_NE(t_b, nullptr);
}

// ============================================================================
// Port / Argument Validation Tests (spec 6.4)
// ============================================================================

TEST(SemaTypeChecker, OutPortRequiresLvalue)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(out x: int32);
    tree Main() {
      Foo(x: 1 + 2);
    }
  )"));
  (void)ctx.run_all();
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaTypeChecker, DirectionMarkerRequiresLvalue)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      Foo(x: ref (1 + 2));
    }
  )"));
  (void)ctx.run_all();
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaTypeChecker, DirectionMismatchInToOutPortErrors)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(out x: int32);
    tree Main() {
      var a: int32 = 0;
      Foo(x: a);
    }
  )"));
  (void)ctx.run_all();
  EXPECT_TRUE(ctx.diags.has_errors());
}

TEST(SemaTypeChecker, InPortExpectedTypeChecksExpression)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: bool);
    tree Main() {
      Foo(x: 123);
    }
  )"));
  (void)ctx.run_all();
  EXPECT_TRUE(ctx.diags.has_errors());
}

// ============================================================================
// Unused mut/out parameter warning tests (spec 6.3.2)
// ============================================================================

TEST(SemaTypeChecker, WarnUnusedOutParamNeverWritten)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoNothing();
    tree Main(out p: int32) {
      DoNothing();
    }
  )"));
  ASSERT_TRUE(ctx.run_all());

  EXPECT_FALSE(ctx.diags.has_errors());
  EXPECT_TRUE(ctx.diags.has_warnings());
  EXPECT_TRUE(has_warning_containing(
    ctx.diags, "Parameter 'p' is declared as mut/out but never used for write access"));
}

TEST(SemaTypeChecker, WarnUnusedMutParamUsedOnlyForRead)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Log(value: int32);
    tree Main(mut p: int32) {
      Log(value: p);
    }
  )"));
  ASSERT_TRUE(ctx.run_all());

  EXPECT_FALSE(ctx.diags.has_errors());
  EXPECT_TRUE(ctx.diags.has_warnings());
  EXPECT_TRUE(has_warning_containing(
    ctx.diags, "Parameter 'p' is declared as mut/out but never used for write access"));
}

TEST(SemaTypeChecker, NoWarningWhenMutParamAssigned)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main(mut p: int32) {
      p = 1;
    }
  )"));
  ASSERT_TRUE(ctx.run_all());

  EXPECT_FALSE(ctx.diags.has_errors());
  EXPECT_FALSE(has_warning_containing(
    ctx.diags, "Parameter 'p' is declared as mut/out but never used for write access"));
}

TEST(SemaTypeChecker, NoWarningWhenMutParamUsedAsOutIndexTarget)
{
  TestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Set(out x: int32);
    tree Main(mut arr: [int32; 1]) {
      Set(x: out arr[0]);
    }
  )"));
  ASSERT_TRUE(ctx.run_all());

  EXPECT_FALSE(ctx.diags.has_errors());
  EXPECT_FALSE(has_warning_containing(
    ctx.diags, "Parameter 'arr' is declared as mut/out but never used for write access"));
}
