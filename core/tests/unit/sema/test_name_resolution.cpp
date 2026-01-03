// tests/sema/test_name_resolution.cpp - Unit tests for name resolution
//
// Tests the NameResolver visitor which binds identifier references to
// their corresponding declarations.
//

#include <gtest/gtest.h>

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

// ============================================================================
// Test Helper
// ============================================================================

static ModuleInfo create_test_module(ParsedUnit & unit)
{
  ModuleInfo module;
  module.program = unit.program;
  module.types.register_builtins();

  for (const auto * ext_type : unit.program->externTypes) {
    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    module.types.define(sym);
  }

  for (const auto * ext : unit.program->externs) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    module.nodes.define(sym);
  }
  for (const auto * tree : unit.program->trees) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    module.nodes.define(sym);
  }

  module.values.build_from_program(*unit.program);

  SymbolTableBuilder builder(module.values, module.types, module.nodes);
  builder.build(*unit.program);

  return module;
}

// ============================================================================
// Type Resolution Tests
// ============================================================================

TEST(SemaNameResolver, ResolveBuiltinType)
{
  const std::string src = R"(
    var x: int = 42;
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->globalVars.size(), 1U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the type is resolved
  auto * var = program->globalVars[0];
  ASSERT_NE(var->type, nullptr);
  auto * type_expr = var->type;
  ASSERT_NE(type_expr->base, nullptr);
  auto * primary_type = cast<PrimaryType>(type_expr->base);
  ASSERT_NE(primary_type, nullptr);
  ASSERT_NE(primary_type->resolvedType, nullptr);
  EXPECT_TRUE(primary_type->resolvedType->is_builtin_type());
  EXPECT_EQ(primary_type->resolvedType->name, "int32");  // "int" is alias for "int32"
}

TEST(SemaNameResolver, ResolveExternType)
{
  const std::string src = R"(
    extern type Pose;
    var pos: Pose;
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->externTypes.size(), 1U);
  ASSERT_EQ(program->globalVars.size(), 1U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the extern type is resolved
  auto * var = program->globalVars[0];
  auto * primary_type = cast<PrimaryType>(var->type->base);
  ASSERT_NE(primary_type, nullptr);
  ASSERT_NE(primary_type->resolvedType, nullptr);
  EXPECT_TRUE(primary_type->resolvedType->is_extern_type());
  EXPECT_EQ(primary_type->resolvedType->name, "Pose");
}

// ============================================================================
// Node Resolution Tests
// ============================================================================

TEST(SemaNameResolver, ResolveExternNode)
{
  const std::string src = R"(
    extern action Say(message: string);
    tree Main() {
      Say(message: "hello");
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->externs.size(), 1U);
  ASSERT_EQ(program->trees.size(), 1U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the node is resolved in the tree body
  auto * tree = program->trees[0];
  ASSERT_EQ(tree->body.size(), 1U);
  auto * node_stmt = cast<NodeStmt>(tree->body[0]);
  ASSERT_NE(node_stmt, nullptr);
  ASSERT_NE(node_stmt->resolvedNode, nullptr);
  EXPECT_TRUE(node_stmt->resolvedNode->is_extern_node());
  EXPECT_EQ(node_stmt->resolvedNode->name, "Say");
}

TEST(SemaNameResolver, ResolveTreeCall)
{
  const std::string src = R"(
    tree Helper() {}
    tree Main() {
      Helper();
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->trees.size(), 2U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the tree call is resolved
  auto * main_tree = program->trees[1];  // Main
  ASSERT_EQ(main_tree->body.size(), 1U);
  auto * node_stmt = cast<NodeStmt>(main_tree->body[0]);
  ASSERT_NE(node_stmt, nullptr);
  ASSERT_NE(node_stmt->resolvedNode, nullptr);
  EXPECT_TRUE(node_stmt->resolvedNode->is_tree());
  EXPECT_EQ(node_stmt->resolvedNode->name, "Helper");
}

// ============================================================================
// Value Resolution Tests
// ============================================================================

TEST(SemaNameResolver, ResolveGlobalVar)
{
  const std::string src = R"(
    var counter: int = 0;
    tree Main() {
      counter = 1;
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->globalVars.size(), 1U);
  ASSERT_EQ(program->trees.size(), 1U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the assignment target is resolved
  auto * tree = program->trees[0];
  ASSERT_EQ(tree->body.size(), 1U);
  auto * assign_stmt = cast<AssignmentStmt>(tree->body[0]);
  ASSERT_NE(assign_stmt, nullptr);
  ASSERT_NE(assign_stmt->resolvedTarget, nullptr);
  EXPECT_TRUE(assign_stmt->resolvedTarget->is_global());
  EXPECT_EQ(assign_stmt->resolvedTarget->name, "counter");
}

TEST(SemaNameResolver, ResolveParameter)
{
  const std::string src = R"(
    extern action Log(value: int);
    tree Main(in x: int) {
      Log(value: x);
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->trees.size(), 1U);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that the parameter reference is resolved
  auto * tree = program->trees[0];
  ASSERT_EQ(tree->body.size(), 1U);
  auto * node_stmt = cast<NodeStmt>(tree->body[0]);
  ASSERT_NE(node_stmt, nullptr);
  ASSERT_EQ(node_stmt->args.size(), 1U);
  auto * arg = node_stmt->args[0];
  ASSERT_NE(arg->valueExpr, nullptr);
  auto * var_ref = cast<VarRefExpr>(arg->valueExpr);
  ASSERT_NE(var_ref, nullptr);
  ASSERT_NE(var_ref->resolvedSymbol, nullptr);
  EXPECT_TRUE(var_ref->resolvedSymbol->is_parameter());
  EXPECT_EQ(var_ref->resolvedSymbol->name, "x");
}

// ============================================================================
// Error Detection Tests
// ============================================================================

TEST(SemaNameResolver, ErrorUndeclaredType)
{
  const std::string src = R"(
    var x: UnknownType = 0;
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(resolver.has_errors());
  EXPECT_EQ(resolver.error_count(), 1U);
}

TEST(SemaNameResolver, ErrorUndeclaredNode)
{
  const std::string src = R"(
    tree Main() {
      UnknownNode();
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(resolver.has_errors());
  EXPECT_EQ(resolver.error_count(), 1U);
}

TEST(SemaNameResolver, ErrorUndeclaredVariable)
{
  const std::string src = R"(
    extern action Log(value: int);
    tree Main() {
      Log(value: unknownVar);
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  EXPECT_FALSE(ok);  // Should fail
  EXPECT_TRUE(resolver.has_errors());
  EXPECT_EQ(resolver.error_count(), 1U);
}

TEST(SemaNameResolver, ErrorDuplicateTree)
{
  const std::string src = R"(
    tree Foo() {}
    tree Foo() {}
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);
  ASSERT_EQ(program->trees.size(), 2U);

  ModuleInfo module;
  module.program = program;
  module.types.register_builtins();

  bool has_duplicate = false;
  for (const auto * tree : program->trees) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    if (!module.nodes.define(sym)) {
      has_duplicate = true;
    }
  }
  EXPECT_TRUE(has_duplicate);
}

// ============================================================================
// DiagnosticBag Integration Tests
// ============================================================================

TEST(SemaNameResolver, DiagnosticBagCollectsErrors)
{
  const std::string src = R"(
    var x: UnknownType = 0;
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  DiagnosticBag diags;
  NameResolver resolver(module, &diags);
  const bool ok = resolver.resolve();
  EXPECT_FALSE(ok);

  // Check that diagnostics were collected
  EXPECT_TRUE(diags.has_errors());
  ASSERT_EQ(diags.size(), 1U);
  const auto & d = diags.all()[0];
  EXPECT_EQ(d.severity, Severity::Error);
}

// ============================================================================
// Block Scope Tests
// ============================================================================

TEST(SemaNameResolver, BlockScopeIsolation)
{
  // Variables declared in one children_block should not
  // affect another children_block
  const std::string src = R"(
    extern control Sequence();
    extern action Action();
    extern action Log(value: int);
    tree Main() {
      Sequence() {
        var blockVar: int = 1;
        Action();
      }
      Sequence() {
        var blockVar: int = 2;
        Action();
      }
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  // Should succeed - blockVar in different blocks is OK
  ASSERT_TRUE(ok);
  EXPECT_FALSE(resolver.has_errors());
}

TEST(SemaNameResolver, InlineBlackboardDecl)
{
  const std::string src = R"(
    extern control Sequence();
    extern action GetValue(out result: int);
    extern action Log(value: int);
    tree Main() {
      Sequence() {
        GetValue(result: out var x);
        Log(value: x);
      }
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  ModuleInfo module = create_test_module(*unit);

  NameResolver resolver(module);
  const bool ok = resolver.resolve();
  ASSERT_TRUE(ok);

  // Check that x is resolved in Log call
  auto * tree = program->trees[0];
  auto * seq_node = cast<NodeStmt>(tree->body[0]);
  ASSERT_NE(seq_node, nullptr);
  // Log is the second child
  auto * log_node = cast<NodeStmt>(seq_node->children[1]);
  ASSERT_NE(log_node, nullptr);
  auto * arg = log_node->args[0];
  auto * var_ref = cast<VarRefExpr>(arg->valueExpr);
  ASSERT_NE(var_ref, nullptr);
  ASSERT_NE(var_ref->resolvedSymbol, nullptr);
  EXPECT_EQ(var_ref->resolvedSymbol->name, "x");
}

TEST(SemaNameResolver, ErrorShadowingInBlock)
{
  const std::string src = R"(
    extern control Sequence();
    extern action Action();
    var globalVar: int = 0;
    tree Main() {
      Sequence() {
        var globalVar: int = 1;
        Action();
      }
    }
  )";
  auto unit = parse_source(src);
  ASSERT_NE(unit, nullptr);
  Program * program = unit->program;
  ASSERT_NE(program, nullptr);

  // Shadowing is detected by SymbolTableBuilder, not NameResolver
  ModuleInfo module;
  module.program = program;
  module.types.register_builtins();
  module.values.build_from_program(*program);

  DiagnosticBag diags;
  SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
  const bool builder_ok = builder.build(*program);

  // Should fail due to shadowing
  EXPECT_FALSE(builder_ok);
  EXPECT_TRUE(builder.has_errors());
  EXPECT_TRUE(diags.has_errors());
}
