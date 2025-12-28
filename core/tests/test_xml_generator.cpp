// test_xml_generator.cpp - BehaviorTree.CPP XML generation tests
// Ported/adapted from legacy/test/generator.test.ts

#include <gtest/gtest.h>

#include "bt_dsl/analyzer.hpp"
#include "bt_dsl/parser.hpp"
#include "bt_dsl/xml_generator.hpp"

#include <string>

using namespace bt_dsl;

namespace {

class XmlGeneratorTest : public ::testing::Test {
protected:
  Parser parser;
  Analyzer analyzer;
  XmlGenerator generator;
  Program stdlib_program;
  bool has_stdlib = false;

  void SetUp() override {
    const std::string stdlib_src = R"(
declare Action AlwaysFailure()
declare Action AlwaysSuccess()
declare Action Sleep(in msec: int)
declare Action WasEntryUpdated(in entry: any)

declare Control Fallback()
declare Control Parallel(in failure_count: int, in success_count: int)
declare Control ReactiveFallback()
declare Control ReactiveSequence()
declare Control Sequence()
declare Control SequenceWithMemory()

declare Decorator Delay(in delay_msec: int)
declare Decorator ForceFailure()
declare Decorator ForceSuccess()
declare Decorator Inverter()
declare Decorator KeepRunningUntilFailure()
declare Decorator Repeat(in num_cycles: int)
declare Decorator RetryUntilSuccessful(in num_attempts: int)
declare Decorator RunOnce(in then_skip: bool)
declare Decorator SkipUnlessUpdated(in entry: any)
declare Decorator Timeout(in msec: int)
declare Decorator WaitValueUpdate(in entry: any)
)";

    auto parsed = parser.parse(stdlib_src);
    EXPECT_TRUE(parsed.has_value()) << "Failed to parse stdlib for tests";
    if (parsed.has_value()) {
      stdlib_program = std::move(parsed.value());
      has_stdlib = true;
    }
  }

  std::string generateXml(const std::string &source) {
    auto parse_result = parser.parse(source);
    EXPECT_TRUE(parse_result.has_value()) << "Parse failed";
    if (!parse_result.has_value()) {
      return {};
    }
    if (!has_stdlib) {
      auto analysis = analyzer.analyze(parse_result.value());
      EXPECT_FALSE(analysis.has_errors()) << "Semantic errors found";
      return generator.generate(parse_result.value(), analysis);
    }

    const std::vector<const Program *> imports = {&stdlib_program};
    auto analysis = analyzer.analyze(parse_result.value(), imports);
    EXPECT_FALSE(analysis.has_errors()) << "Semantic errors found";
    return generator.generate(parse_result.value(), analysis);
  }
};

TEST_F(XmlGeneratorTest, GeneratesBasicTreeStructure) {
  const auto xml = generateXml(R"(
        Tree Main() {
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

TEST_F(XmlGeneratorTest, GeneratesTreeNodesModelForSubtreesWithParams) {
  const auto xml = generateXml(R"(
        Tree Main() { Sequence {} }
        Tree SubTree(ref target: Vector3, amount: int) { Sequence {} }
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

TEST_F(XmlGeneratorTest, GeneratesBlackboardReferencesWithBraces) {
  const auto xml = generateXml(R"(
    declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main() {
            Action(pos: Target)
        }
    )");

  EXPECT_NE(xml.find("pos=\"{Target}\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, EscapesXmlSpecialCharactersInStringAttributes) {
  const auto xml = generateXml(R"(
    declare Action Action(in text: string)
        Tree Main() {
            Action(text: "<tag>&value</tag>")
        }
    )");

  EXPECT_NE(xml.find("&lt;tag&gt;&amp;value&lt;/tag&gt;"), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesDecoratorsAsWrapperElements) {
  const auto xml = generateXml(R"(
    declare Action Action()
        Tree Main() {
            @Repeat(3)
            @Inverter
            Action()
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

TEST_F(XmlGeneratorTest, GeneratesMetadataFromTreeDocs) {
  const auto xml = generateXml(R"(
        /// Main tree description
        Tree Main() {
            Sequence {}
        }
    )");

  EXPECT_NE(xml.find("<Metadata"), std::string::npos);
  EXPECT_NE(xml.find("key=\"description\""), std::string::npos);
  EXPECT_NE(xml.find("value=\"Main tree description\""), std::string::npos);
}

TEST_F(XmlGeneratorTest, GeneratesNodeDescriptionAttributeFromDocs) {
  const auto xml = generateXml(R"(
    declare Action MyAction()
        Tree Main() {
            /// This is an action
            MyAction()
        }
    )");

  EXPECT_NE(xml.find("_description=\"This is an action\""), std::string::npos);
}

TEST_F(XmlGeneratorTest,
       GeneratesScriptInitializationForLocalVarsWithInitialValues) {
  const auto xml = generateXml(R"(
        Tree Main() {
            var msg = "hello"
            var count = 42
            Sequence {}
        }
    )");

  EXPECT_NE(xml.find("<Script"), std::string::npos);
  // tinyxml2 may choose to escape apostrophes in attribute values.
  const bool has_raw = xml.find("msg:='hello'") != std::string::npos;
  const bool has_escaped =
      xml.find("msg:=&apos;hello&apos;") != std::string::npos;
  EXPECT_TRUE(has_raw || has_escaped);
  EXPECT_NE(xml.find("count:=42"), std::string::npos);

  // Should be wrapped by outer Sequence when initialization exists
  const auto seq = xml.find("<Sequence");
  const auto script = xml.find("<Script");
  ASSERT_NE(seq, std::string::npos);
  ASSERT_NE(script, std::string::npos);
  EXPECT_LT(seq, script);
}

TEST_F(XmlGeneratorTest,
       GeneratesScriptNodeForAssignmentStatementsInChildrenBlock) {
  const auto xml = generateXml(R"(
        var counter: int
        Tree Main() {
          Sequence {
            counter = 0
          }
        }
    )");

  EXPECT_NE(xml.find("<Script"), std::string::npos);
  EXPECT_NE(xml.find("counter = 0"), std::string::npos);
}

TEST_F(XmlGeneratorTest, WrapsBinaryExpressionsInParenthesesInScript) {
  const auto xml = generateXml(R"(
        var a: int
        var b: int
        var result: int
        Tree Main() {
          Sequence {
            result = a + b
          }
        }
    )");

  EXPECT_NE(xml.find("(a + b)"), std::string::npos);
}

} // namespace
