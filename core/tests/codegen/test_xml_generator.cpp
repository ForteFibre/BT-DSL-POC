// test_xml_generator.cpp - BehaviorTree.CPP XML generation tests
// Ported/adapted from legacy/test/generator.test.ts

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/parser/parser.hpp"
#include "bt_dsl/semantic/analyzer.hpp"

using namespace bt_dsl;

namespace
{

class XmlGeneratorTest : public ::testing::Test
{
protected:
  Parser parser;
  Analyzer analyzer;
  XmlGenerator generator;
  Program stdlib_program;
  bool has_stdlib = false;

  void SetUp() override
  {
    const std::string stdlib_src = R"(
extern type Vector3;
extern type Entry;

extern action AlwaysFailure();
extern action AlwaysSuccess();
extern action Sleep(in msec: int);
extern action WasEntryUpdated(in entry: Entry);

extern control Fallback();
extern control Parallel(in failure_count: int, in success_count: int);
extern control ReactiveFallback();
extern control ReactiveSequence();
extern control Sequence();
extern control SequenceWithMemory();

extern decorator Delay(in delay_msec: int);
extern decorator ForceFailure();
extern decorator ForceSuccess();
extern decorator Inverter();
extern decorator KeepRunningUntilFailure();
extern decorator Repeat(in num_cycles: int);
extern decorator RetryUntilSuccessful(in num_attempts: int);
extern decorator RunOnce(in then_skip: bool);
extern decorator SkipUnlessUpdated(in entry: Entry);
extern decorator Timeout(in msec: int);
extern decorator WaitValueUpdate(in entry: Entry);
)";

    auto parsed = parser.parse(stdlib_src);
    EXPECT_TRUE(parsed.has_value()) << "Failed to parse stdlib for tests";
    if (parsed.has_value()) {
      stdlib_program = std::move(parsed.value());
      has_stdlib = true;
    }
  }

  std::string generate_xml(const std::string & source)
  {
    auto parse_result = parser.parse(source);
    EXPECT_TRUE(parse_result.has_value()) << "Parse failed";
    if (!parse_result.has_value()) {
      return {};
    }

    const Program * main_prog = &parse_result.value();

    if (!has_stdlib) {
      auto analysis = Analyzer::analyze(*main_prog);
      EXPECT_FALSE(analysis.has_errors()) << "Semantic errors found";
      ImportGraph g;
      g.emplace(main_prog, std::vector<const Program *>{});
      return XmlGenerator::generate(*main_prog, analysis, g);
    }

    const std::vector<const Program *> imports = {&stdlib_program};
    auto analysis = Analyzer::analyze(*main_prog, imports);
    EXPECT_FALSE(analysis.has_errors()) << "Semantic errors found";

    ImportGraph g;
    g.emplace(main_prog, imports);
    g.emplace(&stdlib_program, std::vector<const Program *>{});
    return XmlGenerator::generate(*main_prog, analysis, g);
  }
};

TEST_F(XmlGeneratorTest, GeneratesBasicTreeStructure)
{
  const auto xml = generate_xml(R"(
    tree Main() {
      Sequence {}
    }
    )");

  EXPECT_NE(xml.find("<?xml version=\"1.0\""), std::string::npos);
  EXPECT_NE(xml.find("<root"), std::string::npos);
  EXPECT_NE(xml.find("BTCPP_format=\"4\""), std::string::npos);
  EXPECT_NE(xml.find("main_tree_to_execute=\"Main\""), std::string::npos);
  EXPECT_NE(xml.find("<BehaviorTree"), std::string::npos);
  EXPECT_NE(xml.find("ID=\"Main\""), std::string::npos);
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesTreeNodesModelForSubtreesWithParams)
{
  const auto xml = generate_xml(R"(
  tree Main(ref Target: Vector3) {
    SubTree(target: ref Target, amount: 1);
  }
  tree SubTree(ref target: Vector3, amount: int) { Sequence {} }
    )");

  EXPECT_NE(xml.find("<TreeNodesModel"), std::string::npos);
  EXPECT_NE(xml.find("<SubTree"), std::string::npos);
  EXPECT_NE(xml.find("ID=\"SubTree\""), std::string::npos);

  // ref -> inout_port
  EXPECT_NE(xml.find("<inout_port"), std::string::npos);
  EXPECT_NE(xml.find("name=\"target\""), std::string::npos);
  EXPECT_NE(xml.find("type=\"Vector3\""), std::string::npos);

  // default/in -> input_port
  EXPECT_NE(xml.find("<input_port"), std::string::npos);
  EXPECT_NE(xml.find("name=\"amount\""), std::string::npos);
  EXPECT_NE(xml.find("type=\"int\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesBlackboardReferencesWithBraces)
{
  const auto xml = generate_xml(R"(
  extern action Action(in pos: int);
    var Target: int = 0;
    tree Main() {
      Action(pos: Target);
    }
    )");

  // xml-mapping.md: global vars are referenced as @{g}.
  EXPECT_NE(xml.find("pos=\"@{Target}\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, MangledImportedTreeIdsAndSubTreeReferences)
{
  auto dep_parsed = parser.parse(R"(
    extern control Sequence();
    tree Sub() { Sequence {} }
  )");
  ASSERT_TRUE(dep_parsed.has_value());
  Program dep = std::move(dep_parsed.value());

  auto main_parsed = parser.parse(R"(
    import "./dep.bt"
    tree Main() { Sub(); }
  )");
  ASSERT_TRUE(main_parsed.has_value());
  Program main = std::move(main_parsed.value());

  const std::vector<const Program *> main_imports = {&dep};
  auto analysis = Analyzer::analyze(main, main_imports);
  ASSERT_FALSE(analysis.has_errors());

  ImportGraph g;
  g.emplace(&main, main_imports);
  g.emplace(&dep, std::vector<const Program *>{});

  const std::string xml = XmlGenerator::generate(main, analysis, g);

  // xml-mapping.md §1.6: imported trees must be mangled to an underscore-prefixed unique ID.
  EXPECT_NE(xml.find("<BehaviorTree ID=\"_SubTree_1_Sub\""), std::string::npos);
  EXPECT_NE(xml.find("<SubTree ID=\"_SubTree_1_Sub\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, EscapesXmlSpecialCharactersInStringAttributes)
{
  const auto xml = generate_xml(R"(
  extern action Action(in text: string<256>);
    tree Main() {
      Action(text: "<tag>&value</tag>");
    }
    )");

  EXPECT_NE(xml.find("&lt;tag&gt;&amp;value&lt;/tag&gt;"), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesDecoratorsAsWrapperElements)
{
  const auto xml = generate_xml(R"(
  extern action Action();
    tree Main() {
      Inverter {
        Repeat(num_cycles: 3) {
          Action();
        }
      }
    }
    )");

  EXPECT_NE(xml.find("<Inverter"), std::string::npos);
  EXPECT_NE(xml.find("<Repeat"), std::string::npos);
  EXPECT_NE(xml.find("num_cycles=\"3\""), std::string::npos);
  EXPECT_NE(xml.find("<Action"), std::string::npos);

  // Ensure wrapper order: <Inverter> ... <Repeat ...> ... <Action ...
  const auto inv = xml.find("<Inverter");
  const auto rep = xml.find("<Repeat");
  const auto act = xml.find("<Action");
  ASSERT_NE(inv, std::string::npos);
  ASSERT_NE(rep, std::string::npos);
  ASSERT_NE(act, std::string::npos);
  EXPECT_LT(inv, rep);
  EXPECT_LT(rep, act);
}

TEST_F(XmlGeneratorTest, GeneratesMetadataFromTreeDocs)
{
  const auto xml = generate_xml(R"(
        /// Main tree description
    tree Main() {
            Sequence {}
        }
    )");

  // xml-mapping.md §11: docs are not emitted to XML.
  EXPECT_EQ(xml.find("<Metadata"), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesNodeDescriptionAttributeFromDocs)
{
  const auto xml = generate_xml(R"(
  extern action MyAction();
    tree Main() {
            /// This is an action
      MyAction();
        }
    )");

  // xml-mapping.md §11: docs are not emitted to XML.
  EXPECT_EQ(xml.find("_description=\"This is an action\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesScriptInitializationForLocalVarsWithInitialValues)
{
  const auto xml = generate_xml(R"(
    tree Main() {
      var msg = "hello";
      var count = 42;
            Sequence {}
        }
    )");

  EXPECT_NE(xml.find("<Script"), std::string::npos);
  // xml-mapping.md: local vars are mangled as name#id in Script code.
  // tinyxml2 may choose to escape apostrophes in attribute values.
  const bool has_raw =
    xml.find("msg#") != std::string::npos && xml.find(":='hello'") != std::string::npos;
  const bool has_escaped =
    xml.find("msg#") != std::string::npos && xml.find(":=&apos;hello&apos;") != std::string::npos;
  EXPECT_TRUE(has_raw || has_escaped);
  EXPECT_NE(xml.find("count#"), std::string::npos);
  EXPECT_NE(xml.find(":=42"), std::string::npos);

  // Should be wrapped by outer Sequence when initialization exists
  const auto seq = xml.find("<Sequence");
  const auto script = xml.find("<Script");
  ASSERT_NE(seq, std::string::npos);
  ASSERT_NE(script, std::string::npos);
  EXPECT_LT(seq, script);
}

TEST_F(XmlGeneratorTest, GeneratesScriptNodeForAssignmentStatementsInChildrenBlock)
{
  const auto xml = generate_xml(R"(
        var counter: int;
        tree Main() {
          Sequence {
            counter = 0;
          }
        }
    )");

  EXPECT_NE(xml.find("<Script"), std::string::npos);
  // Global variables are referenced as @{g}.
  EXPECT_NE(xml.find("@{counter} = 0"), std::string::npos);
}

TEST_F(XmlGeneratorTest, EmitsAssignmentPreconditionsOnScriptNode)
{
  const auto xml = generate_xml(R"(
        var counter: int;
        tree Main(in ok: bool) {
          Sequence {
            @success_if(ok)
            counter = 0;
          }
        }
    )");

  EXPECT_NE(xml.find("<Script"), std::string::npos);
  EXPECT_NE(xml.find("@{counter} = 0"), std::string::npos);
  // Preconditions use BT.CPP blackboard substitution syntax.
  EXPECT_NE(xml.find("_successIf=\"{ok}\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, GuardOnAssignmentIsDesugaredToReactiveSequence)
{
  const auto xml = generate_xml(R"(
        var counter: int;
        tree Main(in ok: bool) {
          Sequence {
            @guard(ok)
            counter = 0;
          }
        }
    )");

  // xml-mapping.md §5.1: @guard(cond) -> Sequence + _while + AlwaysSuccess.
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);
  EXPECT_NE(xml.find("_while=\"{ok}\""), std::string::npos);
  EXPECT_NE(xml.find("<AlwaysSuccess"), std::string::npos);
  EXPECT_NE(xml.find("_failureIf=\"!({ok})\""), std::string::npos);
  EXPECT_NE(xml.find("<Script"), std::string::npos);
  EXPECT_NE(xml.find("@{counter} = 0"), std::string::npos);
}

TEST_F(XmlGeneratorTest, WrapsBinaryExpressionsInParenthesesInScript)
{
  const auto xml = generate_xml(R"(
        var a: int = 1;
        var b: int = 2;
        var result: int;
        tree Main() {
          Sequence {
            result = a + b;
          }
        }
    )");

  EXPECT_NE(xml.find("(@{a} + @{b})"), std::string::npos);
}

TEST_F(XmlGeneratorTest, GuardIsDesugaredToReactiveSequenceWithScriptCondition)
{
  const auto xml = generate_xml(R"(
    extern action LongAction();
    tree Main(in ok: bool) {
      @guard(ok)
      LongAction();
    }
  )");

  // Wrapper should exist
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);

  // Guard condition should be encoded as a _while precondition on the guarded node
  EXPECT_NE(xml.find("_while=\"{ok}\""), std::string::npos);
  EXPECT_NE(xml.find("<AlwaysSuccess"), std::string::npos);
  EXPECT_NE(xml.find("_failureIf=\"!({ok})\""), std::string::npos);

  // Original node should still be present
  EXPECT_NE(xml.find("<LongAction"), std::string::npos);

  // Ensure wrapper order: Sequence -> LongAction -> AlwaysSuccess
  const auto seq = xml.find("<Sequence");
  const auto act = xml.find("<LongAction");
  const auto always = xml.find("<AlwaysSuccess");
  ASSERT_NE(seq, std::string::npos);
  ASSERT_NE(act, std::string::npos);
  ASSERT_NE(always, std::string::npos);
  EXPECT_LT(seq, act);
  EXPECT_LT(act, always);
}

// =============================================================================
// xml-mapping.md §7.2: null assignment -> UnsetBlackboard
// =============================================================================

TEST_F(XmlGeneratorTest, NullAssignmentGeneratesUnsetBlackboard)
{
  const auto xml = generate_xml(R"(
    var maybeValue: int?;
    tree Main() {
      Sequence {
        maybeValue = null;
      }
    }
  )");

  // Should generate UnsetBlackboard, not Script
  EXPECT_NE(xml.find("<UnsetBlackboard"), std::string::npos);
  EXPECT_NE(xml.find("key=\"@{maybeValue}\""), std::string::npos);
  // Should NOT contain Script with null assignment
  EXPECT_EQ(xml.find("= null"), std::string::npos);
}

// =============================================================================
// xml-mapping.md §6.3.2: out var x -> pre-Script declaration
// =============================================================================

TEST_F(XmlGeneratorTest, OutVarGeneratesPreScriptDeclaration)
{
  const auto xml = generate_xml(R"(
    extern action DoWork(out result: int);
    tree Main() {
      DoWork(result: out var x);
    }
  )");

  // Should wrap in Sequence with pre-Script
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);
  EXPECT_NE(xml.find("<Script"), std::string::npos);

  // Script should initialize the variable
  EXPECT_NE(xml.find("x#"), std::string::npos);
  EXPECT_NE(xml.find(":= 0"), std::string::npos);

  // DoWork should reference the declared variable
  EXPECT_NE(xml.find("<DoWork"), std::string::npos);
  EXPECT_NE(xml.find("result=\"{x#"), std::string::npos);

  // Script should come before DoWork
  const auto script = xml.find("<Script");
  const auto dowork = xml.find("<DoWork");
  ASSERT_NE(script, std::string::npos);
  ASSERT_NE(dowork, std::string::npos);
  EXPECT_LT(script, dowork);
}

// =============================================================================
// xml-mapping.md §6.3.3: in port with expression -> pre-Script
// =============================================================================

TEST_F(XmlGeneratorTest, InPortExpressionGeneratesPreScript)
{
  const auto xml = generate_xml(R"(
    extern action MoveTo(in target: int);
    var start: int = 0;
    var offset: int = 10;
    tree Main() {
      MoveTo(target: start + offset);
    }
  )");

  // Should wrap in Sequence with pre-Script
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);
  EXPECT_NE(xml.find("<Script"), std::string::npos);

  // Script should evaluate expression into temp variable
  EXPECT_NE(xml.find("_expr#"), std::string::npos);
  EXPECT_NE(xml.find("@{start}"), std::string::npos);
  EXPECT_NE(xml.find("@{offset}"), std::string::npos);

  // MoveTo should reference the temp variable
  EXPECT_NE(xml.find("<MoveTo"), std::string::npos);
  EXPECT_NE(xml.find("target=\"{_expr#"), std::string::npos);

  // Script should come before MoveTo
  const auto script = xml.find("<Script");
  const auto moveto = xml.find("<MoveTo");
  ASSERT_NE(script, std::string::npos);
  ASSERT_NE(moveto, std::string::npos);
  EXPECT_LT(script, moveto);
}

// =============================================================================
// xml-mapping.md §6.3.1: default argument omission -> pre-Script
// =============================================================================

TEST_F(XmlGeneratorTest, OmittedDefaultArgumentGeneratesPreScript)
{
  const auto xml = generate_xml(R"(
    extern action Foo(in x: int = 10);
    tree Main() {
      Foo();
    }
  )");

  // Should wrap in Sequence with pre-Script
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);
  EXPECT_NE(xml.find("<Script"), std::string::npos);

  // Script should set default value
  EXPECT_NE(xml.find("_default#"), std::string::npos);
  EXPECT_NE(xml.find(":= 10"), std::string::npos);

  // Foo should reference the temp variable
  EXPECT_NE(xml.find("<Foo"), std::string::npos);
  EXPECT_NE(xml.find("x=\"{_default#"), std::string::npos);

  // Script should come before Foo
  const auto script = xml.find("<Script");
  const auto foo = xml.find("<Foo");
  ASSERT_NE(script, std::string::npos);
  ASSERT_NE(foo, std::string::npos);
  EXPECT_LT(script, foo);
}

TEST_F(XmlGeneratorTest, ExplicitArgumentDoesNotGenerateDefaultPreScript)
{
  const auto xml = generate_xml(R"(
    extern action Foo(in x: int = 10);
    tree Main() {
      Foo(x: 42);
    }
  )");

  // Should NOT generate _default variable when argument is explicitly provided
  EXPECT_EQ(xml.find("_default#"), std::string::npos);
  // Should have the explicit value
  EXPECT_NE(xml.find("x=\"42\""), std::string::npos);
}

// =============================================================================
// Combined scenarios
// =============================================================================

TEST_F(XmlGeneratorTest, MultiplePreScriptsInSingleSequence)
{
  const auto xml = generate_xml(R"(
    extern action DoSomething(in a: int = 5, out result: int);
    var x: int = 1;
    var y: int = 2;
    tree Main() {
      DoSomething(result: out var z);
    }
  )");

  // Should have multiple Script nodes in a Sequence
  EXPECT_NE(xml.find("<Sequence"), std::string::npos);

  // Count Script occurrences
  size_t script_count = 0;
  size_t pos = 0;
  while ((pos = xml.find("<Script", pos)) != std::string::npos) {
    script_count++;
    pos++;
  }
  // Should have at least 2 Scripts (one for default arg, one for out var)
  EXPECT_GE(script_count, 2u);
}

}  // namespace
