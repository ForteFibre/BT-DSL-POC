// tests/unit/reference/test_ref_xml_mapping.cpp
// Reference compliance tests for: XML Mapping Specification (xml-mapping.md)
//
// Tests that XML generation correctly implements:
// - Node translation (action, control, decorator, subtree)
// - Variable reference format ({var}, @{global})
// - Variable mangling ({name#id})
// - Script node generation
// - Precondition attribute mapping
// - @guard compound transformation

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
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

class XmlTestContext
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
    if (!checker.check(*program)) return false;

    TreeRecursionChecker recursion_checker(&diags);
    if (!recursion_checker.check(*program)) return false;

    InitializationChecker init_checker(module.values, module.nodes, &diags);
    return init_checker.check(*program);
  }

  std::string generate_xml()
  {
    if (!run_sema()) return "";
    return XmlGenerator::generate(module);
  }

  bool xml_contains(const std::string & needle)
  {
    auto xml = generate_xml();
    return xml.find(needle) != std::string::npos;
  }

  std::unique_ptr<ParsedUnit> unit;
  Program * program = nullptr;
  ModuleInfo module;
  TypeContext types;
  DiagnosticBag diags;
};

}  // namespace

// ============================================================================
// 2. Node Translation
// ============================================================================

TEST(RefXmlMapping, ActionNodeOutput)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() { DoWork(); }
  )"));
  EXPECT_TRUE(ctx.xml_contains("<DoWork"));
}

TEST(RefXmlMapping, SubtreeNodeOutput)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() { Sub(); }
    tree Sub() {}
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("SubTree") != std::string::npos || xml.find("Sub") != std::string::npos);
}

TEST(RefXmlMapping, ControlNodeWithChildren)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    #[behavior(All, Chained)] extern control Sequence();
    extern action A();
    extern action B();
    tree Main() {
      Sequence {
        A();
        B();
      }
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(
    xml.find("<Sequence>") != std::string::npos || xml.find("<Sequence") != std::string::npos);
}

// ============================================================================
// 3. Arguments and Variables
// ============================================================================

TEST(RefXmlMapping, LocalVariableReference)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      var val: int32 = 10;
      Foo(x: val);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Local var should be mangled like {val#N}
  EXPECT_TRUE(xml.find("{val") != std::string::npos);
}

TEST(RefXmlMapping, GlobalVariableReference)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    var global_val: int32 = 10;
    extern action Foo(in x: int32);
    tree Main() {
      Foo(x: global_val);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Global var should use @{...}
  EXPECT_TRUE(xml.find("@{global_val}") != std::string::npos);
}

TEST(RefXmlMapping, LiteralArgument)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      Foo(x: 42);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Literal should appear directly as "42"
  EXPECT_TRUE(xml.find("42") != std::string::npos);
}

// ============================================================================
// 4. Global Definitions
// ============================================================================

TEST(RefXmlMapping, ConstInlining)
{
  // Constants should be inlined as literals
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    const MAX = 100;
    extern action Foo(in x: int32);
    tree Main() {
      Foo(x: MAX);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Const should be inlined, not appear as variable reference
  EXPECT_TRUE(xml.find("100") != std::string::npos);
  // Should NOT have MAX as a blackboard reference
  EXPECT_FALSE(xml.find("{MAX}") != std::string::npos);
}

// ============================================================================
// 5. Preconditions
// ============================================================================

TEST(RefXmlMapping, PreconditionSkipIf)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Main() {
      @skip_if(true)
      Foo();
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("_skipIf") != std::string::npos);
}

TEST(RefXmlMapping, PreconditionFailureIf)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Main() {
      @failure_if(false)
      Foo();
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("_failureIf") != std::string::npos);
}

TEST(RefXmlMapping, PreconditionSuccessIf)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Main() {
      @success_if(true)
      Foo();
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("_successIf") != std::string::npos);
}

TEST(RefXmlMapping, PreconditionRunWhile)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo();
    tree Main() {
      @run_while(true)
      Foo();
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("_while") != std::string::npos);
}

// ============================================================================
// 6. Expressions and Assignments
// ============================================================================

TEST(RefXmlMapping, ScriptNodeForAssignment)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32 = 10;
      x = 20;
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("<Script") != std::string::npos);
}

TEST(RefXmlMapping, VarDeclarationWithInit)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32 = 10;
    }
  )"));
  auto xml = ctx.generate_xml();
  // var declaration should use := in Script
  EXPECT_TRUE(xml.find(":=") != std::string::npos);
}

// ============================================================================
// 9. TreeNodesModel (Manifest)
// ============================================================================

TEST(RefXmlMapping, TreeNodesModelGenerated)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action MoveTo(in target: int32);
    tree Main() {
      MoveTo(target: 10);
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("<TreeNodesModel>") != std::string::npos);
  EXPECT_TRUE(xml.find("</TreeNodesModel>") != std::string::npos);
}

TEST(RefXmlMapping, ActionInManifest)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action MyAction(in x: int32, out y: bool);
    tree Main() {
      MyAction(x: 10);
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("<Action ID=\"MyAction\"") != std::string::npos);
}

// ============================================================================
// 10. XML Structure
// ============================================================================

TEST(RefXmlMapping, RootElement)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {}
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("<root") != std::string::npos);
  EXPECT_TRUE(xml.find("BTCPP_format=\"4\"") != std::string::npos);
}

TEST(RefXmlMapping, BehaviorTreeElement)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {}
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("<BehaviorTree ID=\"Main\"") != std::string::npos);
}

// ============================================================================
// 2.4 Implicit Sequence for tree root
// ============================================================================

TEST(RefXmlMapping, ImplicitSequenceForMultipleChildren)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action A();
    extern action B();
    tree Main() {
      A();
      B();
    }
  )"));
  auto xml = ctx.generate_xml();
  // Multiple children in tree should be wrapped in Sequence
  EXPECT_TRUE(
    xml.find("<Sequence>") != std::string::npos || xml.find("<Sequence") != std::string::npos);
}

// ============================================================================
// 5.1 @guard Compound Transformation
// Reference: @guard uses _while + AlwaysSuccess + _failureIf
// ============================================================================

TEST(RefXmlMapping, GuardTransformation)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      @guard(true)
      DoWork();
    }
  )"));
  auto xml = ctx.generate_xml();
  // @guard should produce _while attribute
  EXPECT_TRUE(xml.find("_while") != std::string::npos);
}

// ============================================================================
// 6.3.2 out var x Inline Declaration
// Reference: Generates Script + declaration before node
// ============================================================================

TEST(RefXmlMapping, InlineBlackboardDeclaration)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main() {
      GetValue(result: out var x);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Should have Script for inline declaration
  EXPECT_TRUE(xml.find("<Script") != std::string::npos);
  EXPECT_TRUE(xml.find(":=") != std::string::npos);
}

// ============================================================================
// 6.3.1 Default Argument with Expression
// Reference: Generates temp variable with Script
// ============================================================================

TEST(RefXmlMapping, DefaultArgumentScript)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32 = 10);
    tree Main() {
      Foo();
    }
  )"));
  auto xml = ctx.generate_xml();
  // Default argument may generate a Script or inline the value
  EXPECT_TRUE(xml.find("10") != std::string::npos || xml.find("<Script") != std::string::npos);
}

// ============================================================================
// 6.3.3 in Port Expression Evaluation
// Reference: Complex expressions pre-evaluated to temp variable
// ============================================================================

TEST(RefXmlMapping, InPortExpressionEvaluation)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in x: int32);
    tree Main() {
      var a: int32 = 1;
      var b: int32 = 2;
      Foo(x: a + b);
    }
  )"));
  auto xml = ctx.generate_xml();
  // Expression should be pre-evaluated in Script
  EXPECT_TRUE(xml.find("<Script") != std::string::npos);
}

// ============================================================================
// 3.2 Omitted out Argument
// Reference: Generates _discard_N variable
// ============================================================================

TEST(RefXmlMapping, OmittedOutArgumentDiscard)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action GetValue(out result: int32);
    tree Main() {
      GetValue();
    }
  )"));
  auto xml = ctx.generate_xml();
  // Omitted out should either not appear or use discard variable
  // The node should still be generated
  EXPECT_TRUE(xml.find("<GetValue") != std::string::npos);
}

// ============================================================================
// 10. Multiple Tree Definitions
// Reference: Imported trees get name mangling
// ============================================================================

TEST(RefXmlMapping, MultipleTreeDefinitions)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() { Sub(); }
    tree Sub() {}
  )"));
  auto xml = ctx.generate_xml();
  // Both trees should be output
  EXPECT_TRUE(xml.find("ID=\"Main\"") != std::string::npos);
  EXPECT_TRUE(xml.find("ID=\"Sub\"") != std::string::npos || xml.find("Sub") != std::string::npos);
}

// ============================================================================
// 8. Type Serialization
// Reference: Bool values as "true"/"false", strings with quotes
// ============================================================================

TEST(RefXmlMapping, BoolLiteralSerialization)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in flag: bool);
    tree Main() {
      Foo(flag: true);
    }
  )"));
  auto xml = ctx.generate_xml();
  EXPECT_TRUE(xml.find("true") != std::string::npos);
}

TEST(RefXmlMapping, StringLiteralSerialization)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action Foo(in msg: string);
    tree Main() {
      Foo(msg: "hello");
    }
  )"));
  auto xml = ctx.generate_xml();
  // String should appear in XML
  EXPECT_TRUE(xml.find("hello") != std::string::npos);
}

// ============================================================================
// 7. Nullable Types and Existence Check
// Reference: null assignment -> UnsetBlackboard, != null -> BlackboardExists
// ============================================================================

TEST(RefXmlMapping, NullAssignmentUnsetBlackboard)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32? = 10;
      x = null;
    }
  )"));
  auto xml = ctx.generate_xml();
  // x = null should generate UnsetBlackboard
  EXPECT_TRUE(xml.find("<UnsetBlackboard") != std::string::npos);
  EXPECT_TRUE(xml.find("key=\"x#") != std::string::npos);
}

TEST(RefXmlMapping, BlackboardExistsCheck)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      var x: int32? = null;
      @guard(x != null)
      DoWork();
    }
  )"));
  auto xml = ctx.generate_xml();
  // @guard(x != null) should use BlackboardExists
  EXPECT_TRUE(xml.find("<BlackboardExists") != std::string::npos);
  EXPECT_TRUE(xml.find("key=\"x#") != std::string::npos);
}

TEST(RefXmlMapping, ComplexNullCheckTransformation)
{
  // Spec 7.5: @skip_if(x != null && x > 10)
  // Should verify that the transformation logic described in 7.5 is generated.
  // This typically involves a helper variable and ForceSuccess/BlackboardExists sequence.
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      var x: int32? = 10;
      @skip_if(x != null && x > 10)
      DoWork();
    }
  )"));
  auto xml = ctx.generate_xml();

  // We expect a helper variable for the skip condition
  // And a check sequence involving BlackboardExists
  // Note: The implementation might use specific names for helpers, but we check for key components
  EXPECT_TRUE(xml.find("<ForceSuccess>") != std::string::npos);
  EXPECT_TRUE(xml.find("<BlackboardExists") != std::string::npos);
  EXPECT_TRUE(xml.find("_skipIf=") != std::string::npos);
}

TEST(RefXmlMapping, OrNullCheckPattern)
{
  // Spec 7.5: @skip_if(x == null || x < 0)
  // Should verify transformation logic for OR
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    extern action DoWork();
    tree Main() {
      var x: int32? = null;
      @skip_if(x == null || x < 0)
      DoWork();
    }
  )"));
  auto xml = ctx.generate_xml();
  // Expect helper var init to true
  EXPECT_TRUE(xml.find(":=") != std::string::npos);
  EXPECT_TRUE(xml.find("true") != std::string::npos);
  // Expect ForceSuccess block
  EXPECT_TRUE(xml.find("<ForceSuccess>") != std::string::npos);
}

// ============================================================================
// 6.1 Compound Assignment Unfolding
// Reference: x += 3 -> x = x + 3
// ============================================================================

TEST(RefXmlMapping, CompoundAssignmentUnfolding)
{
  XmlTestContext ctx;
  ASSERT_TRUE(ctx.parse(R"(
    tree Main() {
      var x: int32 = 0;
      x += 3;
    }
  )"));
  auto xml = ctx.generate_xml();
  // Should NOT find += in script
  EXPECT_FALSE(xml.find("+=") != std::string::npos);
  // Should find x = x + ...
  EXPECT_TRUE(xml.find('=') != std::string::npos && xml.find('+') != std::string::npos);
}
