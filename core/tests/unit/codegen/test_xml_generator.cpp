#include <gtest/gtest.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/sema/resolution/module_resolver.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/resolution/symbol_table_builder.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"
#include "bt_dsl/sema/types/type_table.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

static void expect_contains(const std::string & haystack, const std::string & needle)
{
  EXPECT_NE(haystack.find(needle), std::string::npos)
    << "Expected to find: " << needle << "\nIn output:\n"
    << haystack;
}

static void expect_not_contains(const std::string & haystack, const std::string & needle)
{
  EXPECT_EQ(haystack.find(needle), std::string::npos)
    << "Expected NOT to find: " << needle << "\nIn output:\n"
    << haystack;
}

struct SingleModulePipeline
{
  std::unique_ptr<ParsedUnit> unit;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;

  bool parse(const std::string & src)
  {
    unit = parse_source(src);
    if (!unit) {
      return false;
    }
    if (!unit->diags.empty()) {
      diags = unit->diags;
      return false;
    }

    module.program = unit->program;
    module.parsedUnit = std::move(unit);
    module.types.register_builtins();
    module.values.build_from_program(*module.program);

    return module.program != nullptr;
  }

  bool analyze()
  {
    for (const auto * ext_type : module.program->externTypes) {
      TypeSymbol sym;
      sym.name = ext_type->name;
      sym.decl = ext_type;
      sym.is_builtin = false;
      module.types.define(sym);
    }

    for (const auto * ext : module.program->externs) {
      NodeSymbol sym;
      sym.name = ext->name;
      sym.decl = ext;
      module.nodes.define(sym);
    }
    for (const auto * tree : module.program->trees) {
      NodeSymbol sym;
      sym.name = tree->name;
      sym.decl = tree;
      module.nodes.define(sym);
    }

    SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
    if (!builder.build(*module.program)) {
      return false;
    }

    NameResolver resolver(module, &diags);
    if (!resolver.resolve()) {
      return false;
    }

    ConstEvaluator eval(module.parsedUnit->ast, types, module.values, &diags);
    if (!eval.evaluate_program(*module.program)) {
      return false;
    }

    TypeChecker checker(types, module.types, module.values, &diags);
    return checker.check(*module.program);
  }
};

TEST(CodegenXmlGenerator, GeneratesBasicTreeStructure)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    tree Main() {
      Sequence { }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());
  EXPECT_FALSE(ctx.diags.has_errors());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<?xml version=\"1.0\"");
  expect_contains(xml, "<root");
  expect_contains(xml, "BTCPP_format=\"4\"");
  expect_contains(xml, "main_tree_to_execute=\"Main\"");
  expect_contains(xml, "<BehaviorTree");
  expect_contains(xml, "ID=\"Main\"");
  expect_contains(xml, "<Sequence");
}

TEST(CodegenXmlGenerator, GeneratesTreeNodesModelForSubtreesWithParams)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    extern type Vector3;

    tree Main(ref Target: Vector3) {
      SubTree(target: ref Target, amount: 1);
    }

    tree SubTree(ref target: Vector3, amount: int32) { Sequence { } }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<TreeNodesModel");
  expect_contains(xml, "<SubTree");
  expect_contains(xml, "ID=\"SubTree\"");

  expect_contains(xml, "<inout_port");
  expect_contains(xml, "name=\"target\"");
  expect_contains(xml, "type=\"Vector3\"");

  expect_contains(xml, "<input_port");
  expect_contains(xml, "name=\"amount\"");
  expect_contains(xml, "type=\"int32\"");
}

TEST(CodegenXmlGenerator, GeneratesGlobalBlackboardRefsWithAtBraces)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Action(in pos: int32);
    var Target: int32 = 0;
    tree Main() { Action(pos: Target); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "pos=\"@{Target}\"");
}

TEST(CodegenXmlGenerator, EscapesXmlSpecialCharsInStringAttributes)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Action(in text: string);
    tree Main() { Action(text: "<tag>&value</tag>"); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "&lt;tag&gt;&amp;value&lt;/tag&gt;");
}

TEST(CodegenXmlGenerator, GeneratesDecoratorsAsWrapperElements)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern decorator Inverter();
    extern decorator Repeat(in num_cycles: int32);
    extern action Action();
    tree Main() {
      Inverter {
        Repeat(num_cycles: 3) {
          Action();
        }
      }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Inverter");
  expect_contains(xml, "<Repeat");
  expect_contains(xml, "num_cycles=\"3\"");
  expect_contains(xml, "<Action");

  const auto inv = xml.find("<Inverter");
  const auto rep = xml.find("<Repeat");
  const auto act = xml.find("<Action");
  ASSERT_NE(inv, std::string::npos);
  ASSERT_NE(rep, std::string::npos);
  ASSERT_NE(act, std::string::npos);
  EXPECT_LT(inv, rep);
  EXPECT_LT(rep, act);
}

TEST(CodegenXmlGenerator, DocsAreNotEmitted)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    /// Main tree description
    tree Main() { Sequence { } }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_not_contains(xml, "<Metadata");
  expect_not_contains(xml, "_description=");
}

TEST(CodegenXmlGenerator, LocalVarInitializationGeneratesScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    tree Main() {
      var msg = "hello";
      var count = 42;
      Sequence { }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Script");
  expect_contains(xml, "msg#");
  const bool has_raw = (xml.find(":= 'hello'") != std::string::npos);
  const bool has_escaped_no_space = (xml.find(":=&apos;hello&apos;") != std::string::npos);
  const bool has_escaped_with_space = (xml.find(":= &apos;hello&apos;") != std::string::npos);
  const bool has_escaped = has_escaped_no_space || has_escaped_with_space;
  EXPECT_TRUE(has_raw || has_escaped);
  expect_contains(xml, "count#");
  expect_contains(xml, ":= 42");

  const auto seq = xml.find("<Sequence");
  const auto script = xml.find("<Script");
  ASSERT_NE(seq, std::string::npos);
  ASSERT_NE(script, std::string::npos);
  EXPECT_LT(seq, script);
}

TEST(CodegenXmlGenerator, AssignmentInChildrenBlockGeneratesScriptNode)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    var counter: int32;
    tree Main() {
      Sequence {
        counter = 0;
      }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "<Script");
  expect_contains(xml, "@{counter} = 0");
}

TEST(CodegenXmlGenerator, AssignmentPreconditionsEmitAttributesOnScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    var counter: int32;
    tree Main(in ok: bool) {
      Sequence {
        @success_if(ok)
        counter = 0;
      }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "@{counter} = 0");
  expect_contains(xml, "_successIf=\"{ok}\"");
}

TEST(CodegenXmlGenerator, GuardOnAssignmentIsDesugared)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    var counter: int32;
    tree Main(in ok: bool) {
      Sequence {
        @guard(ok)
        counter = 0;
      }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Sequence");
  expect_contains(xml, "_while=\"{ok}\"");
  expect_contains(xml, "<AlwaysSuccess");
  expect_contains(xml, "_failureIf=\"!({ok})\"");
  expect_contains(xml, "<Script");
  expect_contains(xml, "@{counter} = 0");
}

TEST(CodegenXmlGenerator, WrapsBinaryExpressionsInParenthesesInScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    var a: int32 = 1;
    var b: int32 = 2;
    var result: int32;
    tree Main() {
      Sequence {
        result = a + b;
      }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "(@{a} + @{b})");
}

TEST(CodegenXmlGenerator, NullAssignmentGeneratesUnsetBlackboard)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern control Sequence();
    var maybeValue: int32?;
    tree Main() {
      Sequence { maybeValue = null; }
    }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);
  expect_contains(xml, "<UnsetBlackboard");
  expect_contains(xml, "key=\"@{maybeValue}\"");
  expect_not_contains(xml, "= null");
}

TEST(CodegenXmlGenerator, OutVarGeneratesPreScriptDeclaration)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork(out result: int32);
    tree Main() { DoWork(result: out var x); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Sequence");
  expect_contains(xml, "<Script");
  expect_contains(xml, "x#");
  expect_contains(xml, ":= 0");
  expect_contains(xml, "<DoWork");
  expect_contains(xml, "result=\"{x#");

  const auto script = xml.find("<Script");
  const auto dowork = xml.find("<DoWork");
  ASSERT_NE(script, std::string::npos);
  ASSERT_NE(dowork, std::string::npos);
  EXPECT_LT(script, dowork);
}

TEST(CodegenXmlGenerator, InPortExpressionGeneratesPreScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action MoveTo(in target: int32);
    var start: int32 = 0;
    var offset: int32 = 10;
    tree Main() { MoveTo(target: start + offset); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Sequence");
  expect_contains(xml, "<Script");
  expect_contains(xml, "_expr#");
  expect_contains(xml, "@{start}");
  expect_contains(xml, "@{offset}");
  expect_contains(xml, "<MoveTo");
  expect_contains(xml, "target=\"{_expr#");

  const auto script = xml.find("<Script");
  const auto moveto = xml.find("<MoveTo");
  ASSERT_NE(script, std::string::npos);
  ASSERT_NE(moveto, std::string::npos);
  EXPECT_LT(script, moveto);
}

TEST(CodegenXmlGenerator, OmittedDefaultArgumentGeneratesPreScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32 = 10);
    tree Main() { Foo(); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_contains(xml, "<Sequence");
  expect_contains(xml, "<Script");
  expect_contains(xml, "_default#");
  expect_contains(xml, ":= 10");
  expect_contains(xml, "<Foo");
  expect_contains(xml, "x=\"{_default#");
}

TEST(CodegenXmlGenerator, ExplicitArgumentDoesNotGenerateDefaultPreScript)
{
  SingleModulePipeline ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32 = 10);
    tree Main() { Foo(x: 42); }
  )"));
  ASSERT_TRUE(ctx.analyze());

  const std::string xml = XmlGenerator::generate(ctx.module);

  expect_not_contains(xml, "_default#");
  expect_contains(xml, "x=\"42\"");
}

// ---------------------------------------------------------------------------
// Single-output import mangling
// ---------------------------------------------------------------------------

static std::filesystem::path get_test_files_dir()
{
  const std::filesystem::path this_file = std::filesystem::absolute(__FILE__);
  return this_file.parent_path() / "module_test_files";
}

TEST(CodegenXmlGenerator, ManglesImportedTreeIdsAndSubtreeReferences)
{
  const std::filesystem::path main_path = get_test_files_dir() / "main.bt";

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);
  const bool ok = resolver.resolve(main_path);
  if (!ok) {
    for (const auto & d : diags.all()) {
      std::cerr << d.message << "\n";
    }
  }
  ASSERT_TRUE(ok);

  ModuleInfo * main_mod = graph.get_module(main_path);
  ASSERT_NE(main_mod, nullptr);

  // Run semantic pipeline on all modules loaded by ModuleResolver.
  TypeContext types;
  for (ModuleInfo * m : graph.get_all_modules()) {
    ASSERT_NE(m, nullptr);
    ASSERT_NE(m->program, nullptr);

    // ModuleResolver already performed a declaration-registration pass into these tables.
    // For this test we want a clean, consistent sema pipeline, so rebuild fresh tables.
    m->types = TypeTable{};
    m->nodes = NodeRegistry{};
    m->values = SymbolTable{};

    m->types.register_builtins();

    for (const auto * ext_type : m->program->externTypes) {
      TypeSymbol sym;
      sym.name = ext_type->name;
      sym.decl = ext_type;
      sym.is_builtin = false;
      m->types.define(sym);
    }

    for (const auto * ext : m->program->externs) {
      NodeSymbol sym;
      sym.name = ext->name;
      sym.decl = ext;
      m->nodes.define(sym);
    }
    for (const auto * tree : m->program->trees) {
      NodeSymbol sym;
      sym.name = tree->name;
      sym.decl = tree;
      m->nodes.define(sym);
    }

    SymbolTableBuilder builder(m->values, m->types, m->nodes, &diags);
    ASSERT_TRUE(builder.build(*m->program));
  }
  for (ModuleInfo * m : graph.get_all_modules()) {
    NameResolver nr(*m, &diags);
    ASSERT_TRUE(nr.resolve());
    ConstEvaluator ce(m->parsedUnit->ast, types, m->values, &diags);
    ASSERT_TRUE(ce.evaluate_program(*m->program));
    TypeChecker tc(types, m->types, m->values, &diags);
    ASSERT_TRUE(tc.check(*m->program));
  }

  const std::string xml = XmlGenerator::generate_single_output(*main_mod);

  expect_contains(xml, "<BehaviorTree ID=\"_SubTree_1_Sub\"");
  expect_contains(xml, "<SubTree ID=\"_SubTree_1_Sub\"");
}
