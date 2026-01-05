// tests/unit/reference/test_ref_frontend_recovery.cpp
// Reference-style tests for parser diagnostics + recovery behavior.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

Program * parse_ok(std::unique_ptr<ParsedUnit> & unit, const std::string & src)
{
  unit = parse_source(src);
  if (!unit || !unit->diags.empty()) {
    return nullptr;
  }
  return unit->program;
}

Program * parse_with_errors(std::unique_ptr<ParsedUnit> & unit, const std::string & src)
{
  unit = parse_source(src);
  if (!unit) {
    return nullptr;
  }
  // We expect at least one diagnostic.
  if (unit->diags.empty()) {
    return nullptr;
  }
  return unit->program;
}

}  // namespace

TEST(RefFrontendRecovery, MissingSemicolonDoesNotStopParsingNextDecl)
{
  std::unique_ptr<ParsedUnit> unit;
  Program * p = parse_with_errors(unit, "const A = 1\nconst B = 2;\n");
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
  std::unique_ptr<ParsedUnit> unit;
  Program * p = parse_with_errors(unit, "const A = ; const B = 2;\n");
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
  std::unique_ptr<ParsedUnit> unit;
  Program * p = parse_with_errors(unit, "tree T() { $; var y: int32; }\n");
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
  std::unique_ptr<ParsedUnit> unit;
  Program * p = parse_with_errors(unit, "var import: int32;\n");
  ASSERT_NE(p, nullptr);
}

TEST(RefFrontendRecovery, ValidInputStillParsesOk)
{
  std::unique_ptr<ParsedUnit> unit;
  Program * p = parse_ok(unit, "const X = 1; tree T() { var y: int32; }\n");
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->global_consts().size(), 1U);
  ASSERT_EQ(p->trees().size(), 1U);
}
