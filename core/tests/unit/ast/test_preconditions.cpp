#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/syntax/frontend.hpp"

static bt_dsl::NodeStmt * first_node_stmt(gsl::span<bt_dsl::Stmt *> body)
{
  for (auto * s : body) {
    if (!s) continue;
    if (auto * n = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
      return n;
    }
  }
  return nullptr;
}

static bt_dsl::NodeStmt * find_node_stmt(gsl::span<bt_dsl::Stmt *> body, std::string_view name)
{
  for (auto * s : body) {
    if (!s) continue;
    if (auto * n = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
      if (n->nodeName == name) return n;
    }
  }
  return nullptr;
}

// ============================================================================
// Test: ParsePreconditions (@guard)
// ============================================================================
TEST(AstPreconditions, ParseGuard)
{
  const std::string src =
    "tree Main() {\n"
    "  @guard(target != null)\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees()[0]->body);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->nodeName, "Action");
  ASSERT_EQ(root->preconditions.size(), 1U);
  EXPECT_EQ(root->preconditions[0]->kind, bt_dsl::PreconditionKind::Guard);
  ASSERT_NE(root->preconditions[0]->condition, nullptr);

  // Condition should be != binary expr
  auto * bin_expr = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(root->preconditions[0]->condition);
  ASSERT_NE(bin_expr, nullptr);
  EXPECT_EQ(bin_expr->op, bt_dsl::BinaryOp::Ne);
}

// ============================================================================
// Test: ParsePreconditions (@success_if)
// ============================================================================
TEST(AstPreconditions, ParseSuccessIf)
{
  const std::string src =
    "tree Main() {\n"
    "  @success_if(x > 0)\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees()[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->preconditions.size(), 1U);
  EXPECT_EQ(root->preconditions[0]->kind, bt_dsl::PreconditionKind::SuccessIf);
}

// ============================================================================
// Test: ParsePreconditions (@failure_if)
// ============================================================================
TEST(AstPreconditions, ParseFailureIf)
{
  const std::string src =
    "tree Main() {\n"
    "  @failure_if(error)\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees()[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->preconditions.size(), 1U);
  EXPECT_EQ(root->preconditions[0]->kind, bt_dsl::PreconditionKind::FailureIf);
}

// ============================================================================
// Test: ParsePreconditions (@run_while)
// ============================================================================
TEST(AstPreconditions, ParseRunWhile)
{
  const std::string src =
    "tree Main() {\n"
    "  @run_while(busy)\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees()[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->preconditions.size(), 1U);
  EXPECT_EQ(root->preconditions[0]->kind, bt_dsl::PreconditionKind::RunWhile);
}

// ============================================================================
// Test: MultiplePreconditions
// ============================================================================
TEST(AstPreconditions, MultiplePreconditions)
{
  const std::string src =
    "tree Main() {\n"
    "  @guard(target != null)\n"
    "  @success_if(done)\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees()[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->preconditions.size(), 2U);
  EXPECT_EQ(root->preconditions[0]->kind, bt_dsl::PreconditionKind::Guard);
  EXPECT_EQ(root->preconditions[1]->kind, bt_dsl::PreconditionKind::SuccessIf);
}

// ============================================================================
// Test: AssignmentWithPrecondition
// ============================================================================
TEST(AstPreconditions, AssignmentWithPrecondition)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: int32;\n"
    "  Sequence {\n"
    "    @success_if(result == 0)\n"
    "    result = 1;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * seq = find_node_stmt(unit->program->trees()[0]->body, "Sequence");
  ASSERT_NE(seq, nullptr);
  ASSERT_EQ(seq->children.size(), 1U);

  auto * assign = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[0]);
  ASSERT_NE(assign, nullptr);
  ASSERT_EQ(assign->preconditions.size(), 1U);
  EXPECT_EQ(assign->preconditions[0]->kind, bt_dsl::PreconditionKind::SuccessIf);
}
