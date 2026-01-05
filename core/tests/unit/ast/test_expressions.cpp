#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/syntax/frontend.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

static bt_dsl::BlackboardDeclStmt * first_var_decl(gsl::span<bt_dsl::Stmt *> body)
{
  for (auto * s : body) {
    if (!s) continue;
    if (auto * v = bt_dsl::dyn_cast<bt_dsl::BlackboardDeclStmt>(s)) {
      return v;
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
// Test: ParseBinaryExpression
// ============================================================================
TEST(AstExpressions, BinaryExpression)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: int32 = a + b * c;\n"
    "  Sequence {}\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::TreeDecl * t = unit.program->trees()[0];
  ASSERT_GE(t->body.size(), 2U);

  auto * decl = first_var_decl(t->body);
  ASSERT_NE(decl, nullptr);
  ASSERT_NE(decl->initialValue, nullptr);

  // The expression should be a + (b * c) due to precedence
  auto * add = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(decl->initialValue);
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->op, bt_dsl::BinaryOp::Add);

  auto * mul = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(add->rhs);
  ASSERT_NE(mul, nullptr);
  EXPECT_EQ(mul->op, bt_dsl::BinaryOp::Mul);
}

// ============================================================================
// Test: ParseUnaryExpression
// ============================================================================
TEST(AstExpressions, UnaryExpression)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: bool;\n"
    "  Sequence {\n"
    "    result = !flag;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::NodeStmt * seq = find_node_stmt(unit.program->trees()[0]->body, "Sequence");
  ASSERT_NE(seq, nullptr);
  ASSERT_FALSE(seq->children.empty());

  auto * assign = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[0]);
  ASSERT_NE(assign, nullptr);

  auto * unary = bt_dsl::dyn_cast<bt_dsl::UnaryExpr>(assign->value);
  ASSERT_NE(unary, nullptr);
  EXPECT_EQ(unary->op, bt_dsl::UnaryOp::Not);
}

// ============================================================================
// Test: ParseComparisonExpression
// ============================================================================
TEST(AstExpressions, ComparisonExpression)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: bool;\n"
    "  Sequence {\n"
    "    result = a > b && c < d;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::NodeStmt * seq = find_node_stmt(unit.program->trees()[0]->body, "Sequence");
  ASSERT_NE(seq, nullptr);

  auto * assign = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[0]);
  ASSERT_NE(assign, nullptr);

  // Top level should be &&
  auto * and_expr = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(assign->value);
  ASSERT_NE(and_expr, nullptr);
  EXPECT_EQ(and_expr->op, bt_dsl::BinaryOp::And);

  // LHS should be a > b
  auto * gt_expr = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(and_expr->lhs);
  ASSERT_NE(gt_expr, nullptr);
  EXPECT_EQ(gt_expr->op, bt_dsl::BinaryOp::Gt);

  // RHS should be c < d
  auto * lt_expr = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(and_expr->rhs);
  ASSERT_NE(lt_expr, nullptr);
  EXPECT_EQ(lt_expr->op, bt_dsl::BinaryOp::Lt);
}

// ============================================================================
// Test: RejectChainedComparisonOperators
// ============================================================================
TEST(AstExpressions, RejectChainedComparison)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: bool;\n"
    "  Sequence {\n"
    "    result = a < b < c;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  EXPECT_FALSE(unit.diags.empty());
}

// ============================================================================
// Test: RejectChainedEqualityOperators
// ============================================================================
TEST(AstExpressions, RejectChainedEquality)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: bool;\n"
    "  Sequence {\n"
    "    result = a == b == c;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  EXPECT_FALSE(unit.diags.empty());
}

// ============================================================================
// Test: IndexExpression
// ============================================================================
TEST(AstExpressions, IndexExpression)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(x: arr[0]);\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * tree = unit.program->trees()[0];
  auto * node = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(tree->body[0]);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->args.size(), 1U);

  auto * idx = bt_dsl::dyn_cast<bt_dsl::IndexExpr>(node->args[0]->valueExpr);
  ASSERT_NE(idx, nullptr);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::VarRefExpr>(idx->base));
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::IntLiteralExpr>(idx->index));
}

// ============================================================================
// Test: CastExpression
// ============================================================================
TEST(AstExpressions, CastExpression)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(x: 1 as int32);\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * tree = unit.program->trees()[0];
  auto * node = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(tree->body[0]);
  ASSERT_NE(node, nullptr);

  auto * cast = bt_dsl::dyn_cast<bt_dsl::CastExpr>(node->args[0]->valueExpr);
  ASSERT_NE(cast, nullptr);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::IntLiteralExpr>(cast->expr));
  ASSERT_NE(cast->targetType, nullptr);
}

// ============================================================================
// Test: NegativeNumber
// ============================================================================
TEST(AstExpressions, NegativeNumber)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(x: -42);\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * tree = unit.program->trees()[0];
  auto * node = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(tree->body[0]);
  ASSERT_NE(node, nullptr);

  auto * expr = node->args[0]->valueExpr;

  // -42 may be parsed as:
  // 1. UnaryExpr(Neg, IntLiteralExpr(42)), or
  // 2. IntLiteralExpr(-42) (if grammar handles negative literals directly)
  if (auto * unary = bt_dsl::dyn_cast<bt_dsl::UnaryExpr>(expr)) {
    EXPECT_EQ(unary->op, bt_dsl::UnaryOp::Neg);
    EXPECT_TRUE(bt_dsl::isa<bt_dsl::IntLiteralExpr>(unary->operand));
  } else if (auto * int_lit = bt_dsl::dyn_cast<bt_dsl::IntLiteralExpr>(expr)) {
    EXPECT_EQ(int_lit->value, -42);
  } else {
    FAIL() << "expected UnaryExpr or IntLiteralExpr";
  }
}
