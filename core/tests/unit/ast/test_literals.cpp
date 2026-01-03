#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/syntax/frontend.hpp"

static bt_dsl::NodeStmt * first_node_stmt(gsl::span<bt_dsl::Stmt *> body)
{
  for (auto * s : body) {
    if (!s) continue;
    if (s->get_kind() == bt_dsl::NodeKind::NodeStmt) {
      return static_cast<bt_dsl::NodeStmt *>(s);
    }
  }
  return nullptr;
}

static bt_dsl::Expr * get_arg_expr(bt_dsl::NodeStmt * node, size_t idx)
{
  if (!node || idx >= node->args.size()) return nullptr;
  return node->args[idx]->valueExpr;
}

// ============================================================================
// Test: ParseLiterals
// ============================================================================
TEST(AstLiterals, ParseLiterals)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(\n"
    "    text: \"hello\",\n"
    "    count: 42,\n"
    "    rate: 3.14,\n"
    "    active: true,\n"
    "    disabled: false\n"
    "  );\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::Program * p = unit->program;
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->trees.size(), 1U);

  bt_dsl::NodeStmt * root = first_node_stmt(p->trees[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->args.size(), 5U);

  // String literal
  auto * e0 = get_arg_expr(root, 0);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::StringLiteralExpr>(e0));
  EXPECT_EQ(bt_dsl::cast<bt_dsl::StringLiteralExpr>(e0)->value, "hello");

  // Integer literal
  auto * e1 = get_arg_expr(root, 1);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::IntLiteralExpr>(e1));
  EXPECT_EQ(bt_dsl::cast<bt_dsl::IntLiteralExpr>(e1)->value, 42);

  // Float literal
  auto * e2 = get_arg_expr(root, 2);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::FloatLiteralExpr>(e2));
  EXPECT_NEAR(bt_dsl::cast<bt_dsl::FloatLiteralExpr>(e2)->value, 3.14, 1e-9);

  // Bool literals
  auto * e3 = get_arg_expr(root, 3);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::BoolLiteralExpr>(e3));
  EXPECT_TRUE(bt_dsl::cast<bt_dsl::BoolLiteralExpr>(e3)->value);

  auto * e4 = get_arg_expr(root, 4);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::BoolLiteralExpr>(e4));
  EXPECT_FALSE(bt_dsl::cast<bt_dsl::BoolLiteralExpr>(e4)->value);
}

// ============================================================================
// Test: ParseFloatExponentLiteral
// ============================================================================
TEST(AstLiterals, FloatExponent)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(x: 1e3);\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees[0]->body);
  ASSERT_NE(root, nullptr);

  auto * e = get_arg_expr(root, 0);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::FloatLiteralExpr>(e));
  EXPECT_NEAR(bt_dsl::cast<bt_dsl::FloatLiteralExpr>(e)->value, 1000.0, 1e-9);
}

// ============================================================================
// Test: ParseStringEscapes
// ============================================================================
TEST(AstLiterals, StringEscapes)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(\n"
    "    a: \"\\n\",\n"
    "    b: \"\\t\",\n"
    "    c: \"\\r\",\n"
    "    d: \"\\0\",\n"
    "    e: \"\\b\",\n"
    "    f: \"\\f\",\n"
    "    g: \"\\\"\",\n"
    "    h: \"\\\\\",\n"
    "    i: \"\\u{41}\",\n"
    "    j: \"\\u{1F600}\"\n"
    "  );\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees[0]->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->args.size(), 10U);

  auto get_str = [&](size_t idx) -> std::string_view {
    auto * e = get_arg_expr(root, idx);
    EXPECT_TRUE(bt_dsl::isa<bt_dsl::StringLiteralExpr>(e));
    if (!bt_dsl::isa<bt_dsl::StringLiteralExpr>(e)) return {};
    return bt_dsl::cast<bt_dsl::StringLiteralExpr>(e)->value;
  };

  EXPECT_EQ(get_str(0), "\n");
  EXPECT_EQ(get_str(1), "\t");
  EXPECT_EQ(get_str(2), "\r");
  EXPECT_EQ(get_str(3), std::string_view("\0", 1));
  EXPECT_EQ(get_str(4), "\b");
  EXPECT_EQ(get_str(5), "\f");
  EXPECT_EQ(get_str(6), "\"");
  EXPECT_EQ(get_str(7), "\\");
  EXPECT_EQ(get_str(8), "A");
  EXPECT_EQ(get_str(9), "\xF0\x9F\x98\x80");  // U+1F600
}

// ============================================================================
// Test: NullLiteral
// ============================================================================
TEST(AstLiterals, NullLiteral)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(x: null);\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees[0]->body);
  ASSERT_NE(root, nullptr);

  auto * e = get_arg_expr(root, 0);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::NullLiteralExpr>(e));
}

// ============================================================================
// Test: ArrayLiteral
// ============================================================================
TEST(AstLiterals, ArrayLiteral)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(arr: [1, 2, 3]);\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees[0]->body);
  ASSERT_NE(root, nullptr);

  auto * e = get_arg_expr(root, 0);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::ArrayLiteralExpr>(e));

  auto * arr = bt_dsl::cast<bt_dsl::ArrayLiteralExpr>(e);
  ASSERT_EQ(arr->elements.size(), 3U);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::IntLiteralExpr>(arr->elements[0]));
  EXPECT_EQ(bt_dsl::cast<bt_dsl::IntLiteralExpr>(arr->elements[0])->value, 1);
}

// ============================================================================
// Test: VecMacro
// ============================================================================
TEST(AstLiterals, VecMacro)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(v: vec![1, 2]);\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::NodeStmt * root = first_node_stmt(unit->program->trees[0]->body);
  ASSERT_NE(root, nullptr);

  auto * e = get_arg_expr(root, 0);
  ASSERT_TRUE(bt_dsl::isa<bt_dsl::VecMacroExpr>(e));

  auto * vec = bt_dsl::cast<bt_dsl::VecMacroExpr>(e);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::ArrayLiteralExpr>(vec->inner));
}
