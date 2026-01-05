
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/syntax/frontend.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

TEST(AstParser, InvalidAttributeRecovery)
{
  const std::string src =
    "import \"nodes.bt\";\n"
    "#[invalid_attr]\n"
    "tree Main() {\n"
    "  AlwaysSuccess();\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);

  // Expect errors
  EXPECT_FALSE(unit.diags.empty());

  // Check specific error message about misplaced attribute
  bool found_misplaced = false;
  for (const auto & d : unit.diags.all()) {
    if (d.message.find("unexpected attribute on this declaration") != std::string::npos) {
      found_misplaced = true;
      break;
    }
  }
  EXPECT_TRUE(found_misplaced) << "Expected 'unexpected attribute' error not found.";

  // BUT check that the Tree was parsed successfully despite the attribute error
  const bt_dsl::Program * p = unit.program;
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->trees().size(), 1U);
  EXPECT_EQ(p->trees()[0]->name, "Main");
  ASSERT_GE(p->trees()[0]->body.size(), 1U);
}

TEST(AstParser, MissingBraceRecovery)
{
  // Test that a missing closing brace in a tree doesn't cause cascading errors
  // into the next top-level declaration.
  const std::string src =
    "tree T1() {\n"
    "  AlwaysSuccess();\n"
    // Missing '}' here
    "tree T2() {\n"
    "  AlwaysSuccess();\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);

  // Should have error for missing '}'
  EXPECT_TRUE(unit.diags.has_errors());

  // Check that we DON'T have "keyword cannot be used as identifier" error for 'tree'
  bool keyword_as_ident = false;
  for (const auto & d : unit.diags.all()) {
    if (d.message.find("keyword cannot be used") != std::string::npos) {
      keyword_as_ident = true;
    }
    if (d.message.find("use of undeclared node 'tree'") != std::string::npos) {
      keyword_as_ident = true;
    }
  }

  // This currently FAILS (returns true) if the bug exists.
  // We expect keyword_as_ident to be TRUE currently (FAILING behavior).
  EXPECT_FALSE(keyword_as_ident)
    << "Parser mistook 'tree' for an identifier instead of recovering.";
}

TEST(AstParser, SemicolonErrorLocation)
{
  const std::string src =
    "tree T() {\n"
    "  AlwaysSuccess()\n"   // Missing semicolon. Line 2.
    "  AlwaysFailure();\n"  // Line 3.
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  EXPECT_TRUE(unit.diags.has_errors());

  bool found_semi = false;
  std::uint32_t error_line = 0;
  for (const auto & d : unit.diags.all()) {
    const auto r = d.primary_range();
    auto loc = unit.sources.get_line_column(r.get_begin());
    if (d.message.find("expected ';'") != std::string::npos) {
      found_semi = true;
      error_line = loc.line;
      break;
    }
  }
  EXPECT_TRUE(found_semi);

  // Currently we expect it to be WRONG (line 3).
  // We want line 2.
  EXPECT_EQ(error_line, 2U)
    << "Error should be reported on the line of the missing semicolon (line 2)";
}
