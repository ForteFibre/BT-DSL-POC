// tests/unit/reference/test_ref_syntax.cpp
// Reference compliance tests for: 2. Syntax (syntax.md)
//
// Tests that the parser correctly handles:
// - Program structure (import, extern, type, var, const, tree)
// - Type syntax
// - Expression precedence and associativity
// - Preconditions
// - Node calls

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

bool parses_ok(const std::string & src)
{
  auto unit = parse_source(src);
  return unit && unit->diags.empty();
}

bool parses_with_error(const std::string & src)
{
  auto unit = parse_source(src);
  return !unit || !unit->diags.empty();
}

}  // namespace

// ============================================================================
// 2.2 Module Structure
// ============================================================================

TEST(RefSyntax, ImportStatement)
{
  EXPECT_TRUE(parses_ok(R"(import "foo.bt";)"));
  EXPECT_TRUE(parses_ok(R"(import "./relative.bt";)"));
  EXPECT_TRUE(parses_ok(R"(import "../parent/file.bt";)"));
}

TEST(RefSyntax, ImportMissingSemicolon)
{
  // MUST FAIL: Missing semicolon
  EXPECT_TRUE(parses_with_error(R"(import "foo.bt")"));
}

TEST(RefSyntax, ExternType)
{
  EXPECT_TRUE(parses_ok("extern type Pose;"));
  EXPECT_TRUE(parses_ok("/// Doc\nextern type Point;"));
}

TEST(RefSyntax, TypeAlias)
{
  EXPECT_TRUE(parses_ok("type Distance = float64;"));
  EXPECT_TRUE(parses_ok("type OptInt = int32?;"));
  EXPECT_TRUE(parses_ok("type IntArray = [int32; 5];"));
}

TEST(RefSyntax, GlobalVar)
{
  EXPECT_TRUE(parses_ok("var x: int32;"));
  EXPECT_TRUE(parses_ok("var x: int32 = 10;"));
  EXPECT_TRUE(parses_ok("var x = 10;"));
}

TEST(RefSyntax, GlobalConst)
{
  EXPECT_TRUE(parses_ok("const X = 10;"));
  EXPECT_TRUE(parses_ok("const X: int32 = 10;"));
}

TEST(RefSyntax, GlobalConstMustHaveValue)
{
  // MUST FAIL: const must have initial value
  EXPECT_TRUE(parses_with_error("const X: int32;"));
}

// ============================================================================
// 2.3 Type Syntax
// ============================================================================

TEST(RefSyntax, NullableType)
{
  EXPECT_TRUE(parses_ok("var x: int32?;"));
  EXPECT_TRUE(parses_ok("var x: string?;"));
  EXPECT_TRUE(parses_ok("extern type Pose; var x: Pose?;"));
}

TEST(RefSyntax, StaticArrayType)
{
  EXPECT_TRUE(parses_ok("var x: [int32; 5];"));
  EXPECT_TRUE(parses_ok("const SIZE = 10; var x: [int32; SIZE];"));
}

TEST(RefSyntax, BoundedArrayType) { EXPECT_TRUE(parses_ok("var x: [int32; <=5];")); }

TEST(RefSyntax, DynamicArrayType)
{
  EXPECT_TRUE(parses_ok("var x: vec<int32>;"));
  EXPECT_TRUE(parses_ok("var x: vec<string>;"));
}

TEST(RefSyntax, BoundedStringType) { EXPECT_TRUE(parses_ok("var x: string<100>;")); }

TEST(RefSyntax, InferType)
{
  EXPECT_TRUE(parses_ok("var x: _ = 10;"));
  EXPECT_TRUE(parses_ok("var x: _? = null;"));
}

// ============================================================================
// 2.4 Expressions - Precedence and Associativity
// ============================================================================

TEST(RefSyntax, ExpressionPrecedence)
{
  // Verify various expressions parse correctly
  EXPECT_TRUE(parses_ok("const X = 1 + 2 * 3;"));       // * > +
  EXPECT_TRUE(parses_ok("const X = 1 < 2 && 3 > 0;"));  // < > > &&
  EXPECT_TRUE(parses_ok("const X = !true || false;"));  // ! > ||
  EXPECT_TRUE(parses_ok("const X = -1 + 2;"));          // unary - > +
}

TEST(RefSyntax, CastExpressionLeftAssociative)
{
  // Reference: `a as T1 as T2` is `(a as T1) as T2`
  EXPECT_TRUE(parses_ok("const X = 1 as int64 as int32;"));
}

TEST(RefSyntax, ComparisonChainForbidden)
{
  // MUST FAIL: Comparison chaining is forbidden per reference 2.4.2
  // `a < b < c` should be a syntax error
  EXPECT_TRUE(parses_with_error("const X = 1 < 2 < 3;"));
  EXPECT_TRUE(parses_with_error("const X = 1 <= 2 <= 3;"));
  EXPECT_TRUE(parses_with_error("const X = 1 > 2 > 3;"));
}

TEST(RefSyntax, EqualityChainForbidden)
{
  // MUST FAIL: Equality chaining is forbidden per reference 2.4.2
  EXPECT_TRUE(parses_with_error("const X = 1 == 2 == 3;"));
  EXPECT_TRUE(parses_with_error("const X = 1 != 2 != 3;"));
}

TEST(RefSyntax, IndexExpression)
{
  EXPECT_TRUE(parses_ok("const ARR = [1, 2, 3]; const X = ARR[0];"));
}

TEST(RefSyntax, ArrayLiteral)
{
  EXPECT_TRUE(parses_ok("const X = [1, 2, 3];"));
  EXPECT_TRUE(parses_ok("const X = [1, 2, 3,];"));  // trailing comma
  EXPECT_TRUE(parses_ok("const X = [];"));
}

TEST(RefSyntax, ArrayRepeat) { EXPECT_TRUE(parses_ok("const X = [0; 5];")); }

TEST(RefSyntax, VecMacro)
{
  EXPECT_TRUE(parses_ok("var x: vec<int32> = vec![1, 2, 3];"));
  EXPECT_TRUE(parses_ok("var x: vec<int32> = vec![0; 5];"));
}

// ============================================================================
// 2.5 Statements
// ============================================================================

TEST(RefSyntax, AssignmentStatement)
{
  EXPECT_TRUE(parses_ok(R"(
    tree Main() {
      var x: int32 = 0;
      x = 10;
    }
  )"));
}

TEST(RefSyntax, CompoundAssignment)
{
  EXPECT_TRUE(parses_ok(R"(
    tree Main() {
      var x: int32 = 0;
      x += 1;
      x -= 1;
      x *= 2;
      x /= 2;
    }
  )"));
}

// ============================================================================
// 2.6 Definitions
// ============================================================================

TEST(RefSyntax, ExternAction)
{
  EXPECT_TRUE(parses_ok("extern action MoveTo(in target: int32);"));
  EXPECT_TRUE(parses_ok("extern action MoveTo(in target: int32, out result: bool);"));
  EXPECT_TRUE(parses_ok("extern action MoveTo();"));
}

TEST(RefSyntax, ExternCondition) { EXPECT_TRUE(parses_ok("extern condition IsBatteryOk();")); }

TEST(RefSyntax, ExternControl)
{
  EXPECT_TRUE(parses_ok("extern control Sequence();"));
  EXPECT_TRUE(parses_ok("#[behavior(All, Chained)] extern control Sequence();"));
}

TEST(RefSyntax, ExternDecorator)
{
  EXPECT_TRUE(parses_ok("extern decorator Inverter();"));
  EXPECT_TRUE(parses_ok("#[behavior(None)] extern decorator ForceSuccess();"));
}

TEST(RefSyntax, ExternSubtree)
{
  EXPECT_TRUE(parses_ok("extern subtree Navigate(in goal: int32);"));
}

TEST(RefSyntax, BehaviorAttribute)
{
  EXPECT_TRUE(parses_ok("#[behavior(All)] extern control Sequence();"));
  EXPECT_TRUE(parses_ok("#[behavior(Any)] extern control Fallback();"));
  EXPECT_TRUE(parses_ok("#[behavior(None)] extern decorator ForceSuccess();"));
  EXPECT_TRUE(parses_ok("#[behavior(All, Chained)] extern control Sequence();"));
  EXPECT_TRUE(parses_ok("#[behavior(None, Isolated)] extern control Parallel();"));
}

TEST(RefSyntax, TreeDefinition)
{
  EXPECT_TRUE(parses_ok("tree Main() {}"));
  EXPECT_TRUE(parses_ok("tree Main(in x: int32) {}"));
  EXPECT_TRUE(parses_ok("tree Main(in x: int32, out y: bool) {}"));
  EXPECT_TRUE(parses_ok("tree Main(x: int32 = 10) {}"));
}

// ============================================================================
// Preconditions
// ============================================================================

TEST(RefSyntax, PreconditionSuccessIf)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() { @success_if(true) Foo(); }
  )"));
}

TEST(RefSyntax, PreconditionFailureIf)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() { @failure_if(false) Foo(); }
  )"));
}

TEST(RefSyntax, PreconditionSkipIf)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() { @skip_if(false) Foo(); }
  )"));
}

TEST(RefSyntax, PreconditionRunWhile)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() { @run_while(true) Foo(); }
  )"));
}

TEST(RefSyntax, PreconditionGuard)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() { @guard(true) Foo(); }
  )"));
}

TEST(RefSyntax, MultiplePreconditions)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo();
    tree Main() {
      @guard(true)
      @skip_if(false)
      Foo();
    }
  )"));
}

// ============================================================================
// Node Calls
// ============================================================================

TEST(RefSyntax, LeafNodeCall)
{
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo(in x: int32);
    tree Main() { Foo(x: 10); }
  )"));
}

TEST(RefSyntax, CompoundNodeCall)
{
  EXPECT_TRUE(parses_ok(R"(
    extern control Sequence();
    extern action Foo();
    tree Main() {
      Sequence {
        Foo();
      }
    }
  )"));
}

TEST(RefSyntax, InlineBlackboardDecl)
{
  // out var x syntax
  EXPECT_TRUE(parses_ok(R"(
    extern action Foo(out result: int32);
    tree Main() { Foo(result: out var x); }
  )"));
}
