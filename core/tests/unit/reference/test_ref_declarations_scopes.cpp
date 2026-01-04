// tests/unit/reference/test_ref_declarations_scopes.cpp
// Reference compliance tests for: 4. Declarations and Scopes
//
// Tests that name resolution correctly implements:
// - Namespace separation (Type, Node, Value)
// - Visibility rules (Public/Private)
// - Import non-transitivity
// - Scope hierarchy and name resolution priority
// - Duplicate declarations (error)
// - Shadowing prohibition

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

class ScopeTestContext
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
// 4.1.1 Namespace Separation
// Reference: Type, Node, Value spaces are independent
// ============================================================================

TEST(RefDeclScopes, NamespaceSeparation)
{
  // Same name in different namespaces is OK
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Foo;
    extern action Foo();
    var Foo: int32 = 1;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 4.2.3 Duplicate Declarations in Same Scope
// Reference: Same scope, same namespace -> error
// ============================================================================

TEST(RefDeclScopes, DuplicateTypeError)
{
  // MUST FAIL: Duplicate extern type
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern type Foo;
    extern type Foo;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, DuplicateNodeError)
{
  // MUST FAIL: Duplicate extern node
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    extern action Foo();
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, DuplicateVarError)
{
  // MUST FAIL: Duplicate global var
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: int32 = 1;
    var x: int32 = 2;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, DuplicateConstError)
{
  // MUST FAIL: Duplicate global const
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X = 1;
    const X = 2;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, DuplicateTreeError)
{
  // MUST FAIL: Duplicate tree
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Foo() {}
    tree Foo() {}
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ExternAndTreeConflict)
{
  // MUST FAIL: extern and tree in same node namespace
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Foo() {}
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 4.2.3 Shadowing Prohibition
// Reference: Shadowing parent scope identifiers is forbidden
// ============================================================================

TEST(RefDeclScopes, ShadowingInTreeError)
{
  // MUST FAIL: Local var shadows global
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: int32 = 1;
    tree Main() {
      var x: int32 = 2;
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ShadowingParamError)
{
  // MUST FAIL: Local var shadows param
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main(in x: int32) {
      var x: int32 = 2;
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ShadowingInBlockError)
{
  // MUST FAIL: Block var shadows tree-level var
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Chained)] extern control Sequence();
    tree Main() {
      var x: int32 = 1;
      Sequence {
        var x: int32 = 2;
      }
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, SeparateBlocksSameNameOk)
{
  // Different blocks (not ancestor relation) can have same name
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Chained)] extern control Sequence();
    extern action Foo(in val: int32);
    tree Main() {
      Sequence {
        var x: int32 = 1;
        Foo(val: x);
      }
      Sequence {
        var x: int32 = 2;
        Foo(val: x);
      }
    }
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 4.2.4 Forward Reference
// Reference: Top-level definitions allow forward reference
// ============================================================================

TEST(RefDeclScopes, ForwardReferenceTopLevel)
{
  // Forward reference to tree is OK
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() { Sub(); }
    tree Sub() {}
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefDeclScopes, ForwardReferenceConst)
{
  // Forward reference to const is OK
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = B + 1;
    const B = 10;
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

TEST(RefDeclScopes, LocalVarNoForwardReference)
{
  // MUST FAIL: Local var cannot be used before declaration
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in val: int32);
    tree Main() {
      Foo(val: x);
      var x: int32 = 1;
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 4.3 Constant Evaluation
// Reference: const must be compile-time evaluable
// ============================================================================

TEST(RefDeclScopes, ConstFromVarError)
{
  // MUST FAIL: const cannot reference var
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: int32 = 10;
    const Y = x;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ConstCircularError)
{
  // MUST FAIL: Circular const reference
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const A = B;
    const B = A;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 4.2 Name Resolution Priority
// Reference: Block > Tree local > Global
// ============================================================================

TEST(RefDeclScopes, NameResolutionPriorityParam)
{
  // Param should be found before global
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in val: int32);
    const x: int32 = 100;
    tree Main(in x: int32) {
      Foo(val: x);
    }
  )"));
  // Per spec 4.2.3: "Shadowing parent scope identifiers is prohibited"
  // So defining a param 'x' when 'x' exists in global scope should be an ERROR.
  // Although the original test expected success, strict compliance requires failure.
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ShadowingGlobalByTreeOkIfDifferentNamespace)
{
  // Same name in different namespace (e.g. Node vs Value) is OK
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Main() {
      // Foo is a Node, x is a Value -> OK
      var Foo: int32 = 1;
    }
  )"));
  EXPECT_TRUE(ctx.run_sema());
}

// ============================================================================
// 4.3.3 Const Evaluation Errors
// Reference: Division by zero and overflow are compile errors
// ============================================================================

TEST(RefDeclScopes, ConstDivisionByZeroError)
{
  // MUST FAIL: Division by zero in const evaluation
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X = 10 / 0;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, ConstModuloByZeroError)
{
  // MUST FAIL: Modulo by zero in const evaluation
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const X = 10 % 0;
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

// ============================================================================
// 4.3.1 Const Expression Constraints
// Reference: const_expr cannot reference runtime values
// ============================================================================

TEST(RefDeclScopes, ConstFromParamError)
{
  // MUST FAIL: const cannot reference tree parameter
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main(in x: int32) {
      const Y = x;
    }
  )"));
  EXPECT_FALSE(ctx.run_sema());
}

TEST(RefDeclScopes, DefaultArgFromVarError)
{
  // MUST FAIL: Default argument cannot reference var
  ScopeTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var x: int32 = 10;
    extern action Foo(in val: int32 = x);
  )"));
  EXPECT_FALSE(ctx.run_sema());
}
