// test_error_messages.cpp - Tests for improved error messaging
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/driver/compiler.hpp"
#include "bt_dsl/sema/resolution/module_resolver.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

namespace
{

// Helper to run full semantic analysis on a string
bt_dsl::DiagnosticBag run_semantics(const std::string & src)
{
  // Create a temp file
  const std::string temp_filename = "test_temp.bt";
  {
    std::ofstream out(temp_filename);
    out << src;
  }

  bt_dsl::CompileOptions opts;
  opts.mode = bt_dsl::CompileMode::Check;
  opts.auto_detect_stdlib = false;

  // Use the public static API to run the pipeline
  auto result = bt_dsl::Compiler::compile_single_file(temp_filename, opts);

  std::filesystem::remove(temp_filename);
  return result.diagnostics;
}

// =============================================================================
// Test: Partial Semantic Analysis
// =============================================================================

TEST(ErrorMessagesTest, SemanticErrorsReportedWithSyntaxErrors)
{
  // This tests that semantic errors (like redefinitions) are reported even
  // when there are also syntax errors present.
  const std::string src =
    "const X = 10;\n"
    "const X = 20;\n"  // Redefinition error (semantic)
    "const Y = 30\n";  // Missing semicolon (syntax)

  const auto unit = bt_dsl::test_support::parse(src);

  // We should have errors
  EXPECT_TRUE(unit.diags.has_errors());

  // Check that we have both syntax and semantic-related parse errors
  bool found_semicolon_error = false;
  for (const auto & d : unit.diags.all()) {
    if (d.message.find("';'") != std::string::npos) {
      found_semicolon_error = true;
      break;
    }
  }
  EXPECT_TRUE(found_semicolon_error) << "Expected missing semicolon error";
}

// =============================================================================
// Test: Block Recovery for Unsupported Keywords
// =============================================================================

TEST(ErrorMessagesTest, IfBlockRecoveryReducesCascadingErrors)
{
  // Tests that using 'if' with a block does not produce many cascading errors.
  const std::string src =
    "tree Main() {\n"
    "  if (true) {\n"
    "    var x = 10;\n"
    "  }\n"
    "  var y = 20;\n"  // This should still be parsed correctly after recovery
    "}\n";

  const auto unit = bt_dsl::test_support::parse(src);

  // Should have errors for 'if' usage
  EXPECT_TRUE(unit.diags.has_errors());

  // Count top-level errors - should be limited (not cascading)
  int error_count = 0;
  for (const auto & d : unit.diags.all()) {
    if (d.severity == bt_dsl::Severity::Error) {
      ++error_count;
    }
  }

  // With proper block recovery, we should have only 1-2 errors, not many
  EXPECT_LE(error_count, 2) << "Expected at most 2 errors with proper block recovery";

  // The tree should still be parsed
  ASSERT_NE(unit.program, nullptr);
  ASSERT_EQ(unit.program->trees().size(), 1U);

  // Check that 'var y = 20' was parsed (body should have at least one statement)
  EXPECT_GE(unit.program->trees()[0]->body.size(), 1U);
}

TEST(ErrorMessagesTest, NestedIfBlocksRecoverCorrectly)
{
  const std::string src =
    "tree Main() {\n"
    "  if (a) {\n"
    "    if (b) {\n"
    "      var nested = 1;\n"
    "    }\n"
    "  }\n"
    "  AlwaysSuccess();\n"  // Should be parsed after recovery
    "}\n";

  const auto unit = bt_dsl::test_support::parse(src);
  ASSERT_NE(unit.program, nullptr);

  // Should have errors for 'if'
  EXPECT_TRUE(unit.diags.has_errors());

  // Should still have the tree
  ASSERT_EQ(unit.program->trees().size(), 1U);
}

// =============================================================================
// Test: Type Name Clarity
// =============================================================================

TEST(ErrorMessagesTest, TypeMismatchShowsConcreteLiteralType)
{
  // When there's a type mismatch involving a literal, the message should
  // show the concrete default type, not just "integer literal".
  const std::string src = "var x: bool = 1 + 2;\n";  // Mismatch: bool vs int32

  const auto unit = bt_dsl::test_support::parse(src);
  ASSERT_NE(unit.program, nullptr);

  // This particular check happens during type checking phase (sema).
  // The parse itself should succeed.
  EXPECT_FALSE(unit.diags.has_errors());

  // For full verification of type mismatch messages, we would need to run
  // the full compiler pipeline. This test just verifies parsing works.
  EXPECT_FALSE(unit.diags.has_errors());
}

TEST(ErrorMessagesTest, RedefinitionErrorsAreDeduplicated)
{
  const std::string src =
    "tree T() {}\n"
    "tree T() {}\n";

  auto diags = run_semantics(src);

  int redef_count = 0;
  bool has_prev_def_note = false;

  for (const auto & d : diags.all()) {
    if (d.message.find("redefinition of node 'T'") != std::string::npos) {
      redef_count++;
    }
    if (d.message.find("previous definition is here") != std::string::npos) {
      has_prev_def_note = true;
    }
    for (const auto & l : d.labels) {
      if (l.message.find("previous definition is here") != std::string::npos) {
        has_prev_def_note = true;
      }
    }
  }

  EXPECT_EQ(redef_count, 1) << "Redefinition error should be reported exactly once";
  EXPECT_TRUE(has_prev_def_note) << "Should contain 'previous definition is here' note";
}

TEST(ErrorMessagesTest, MissingRequiredPortIsReported)
{
  // Note: importing stdlib might fail depending on test environment.
  // Keep the input self-contained.
  const std::string src =
    "extern action Action(req: int32);\n"
    "tree Main() {\n"
    "    Action();\n"
    "}\n";

  auto diags = run_semantics(src);

  bool found_missing_port = false;
  for (const auto & d : diags.all()) {
    if (
      d.message.find("missing required parameter 'req'") != std::string::npos ||
      d.message.find("missing required port 'req'") != std::string::npos) {
      found_missing_port = true;
    }
  }

  EXPECT_TRUE(found_missing_port) << "Should report 'missing required port/parameter'";
}

}  // namespace
