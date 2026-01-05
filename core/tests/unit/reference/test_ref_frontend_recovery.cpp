// tests/unit/reference/test_ref_frontend_recovery.cpp
// Reference-style tests for parser diagnostics + recovery behavior.

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

using namespace bt_dsl;

TEST(RefFrontendRecovery, MissingSemicolonDoesNotStopParsingNextDecl)
{
  auto unit = bt_dsl::test_support::parse("const A = 1\nconst B = 2;\n");
  EXPECT_FALSE(unit.diags.empty());
  Program * p = unit.program;
  ASSERT_NE(p, nullptr);

  const auto consts = p->global_consts();
  ASSERT_EQ(consts.size(), 2U);
  ASSERT_NE(consts[0], nullptr);
  ASSERT_NE(consts[1], nullptr);
  EXPECT_EQ(consts[0]->name, "A");
  EXPECT_EQ(consts[1]->name, "B");
}

TEST(RefFrontendRecovery, MissingExpressionDoesNotStopParsingNextDecl)
{
  auto unit = bt_dsl::test_support::parse("const A = ; const B = 2;\n");
  EXPECT_FALSE(unit.diags.empty());
  Program * p = unit.program;
  ASSERT_NE(p, nullptr);

  const auto consts = p->global_consts();
  ASSERT_EQ(consts.size(), 2U);
  ASSERT_NE(consts[0], nullptr);
  ASSERT_NE(consts[1], nullptr);
  EXPECT_EQ(consts[0]->name, "A");
  EXPECT_EQ(consts[1]->name, "B");
}

TEST(RefFrontendRecovery, UnexpectedTokenInTreeBodySynchronizesToNextStmt)
{
  auto unit = bt_dsl::test_support::parse("tree T() { $; var y: int32; }\n");
  EXPECT_FALSE(unit.diags.empty());
  Program * p = unit.program;
  ASSERT_NE(p, nullptr);
  const auto trees = p->trees();
  ASSERT_EQ(trees.size(), 1U);

  const TreeDecl * t = trees[0];
  ASSERT_NE(t, nullptr);

  // The unexpected token statement is dropped; the later var statement should still parse.
  bool saw_y = false;
  for (auto * st : t->body) {
    if (auto * v = dyn_cast<BlackboardDeclStmt>(st)) {
      if (v->name == "y") {
        saw_y = true;
      }
    }
  }
  EXPECT_TRUE(saw_y);
}

TEST(RefFrontendRecovery, KeywordAsIdentifierProducesDiagnostic)
{
  auto unit = bt_dsl::test_support::parse("var import: int32;\n");
  EXPECT_FALSE(unit.diags.empty());
  Program * p = unit.program;
  ASSERT_NE(p, nullptr);
}

TEST(RefFrontendRecovery, ValidInputStillParsesOk)
{
  auto unit = bt_dsl::test_support::parse("const X = 1; tree T() { var y: int32; }\n");
  EXPECT_TRUE(unit.diags.empty());
  Program * p = unit.program;
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->global_consts().size(), 1U);
  ASSERT_EQ(p->trees().size(), 1U);
}
