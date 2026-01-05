#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

// ============================================================================
// Test: ParseSoldierAILike
// ============================================================================
TEST(AstComplexExamples, SoldierAI)
{
  const std::string src =
    "//! Soldier AI Definition v1.0\n"
    "\n"
    "import \"StandardNodes.bt\";\n"
    "\n"
    "var TargetPos: Vector3;\n"
    "var Ammo: int32;\n"
    "var IsAlerted: bool;\n"
    "\n"
    "/// Main tree\n"
    "tree Main() {\n"
    "  Repeat {\n"
    "    Sequence {\n"
    "      SearchAndDestroy(\n"
    "        target: ref TargetPos,\n"
    "        ammo: ref Ammo,\n"
    "        alert: ref IsAlerted\n"
    "      );\n"
    "    }\n"
    "  }\n"
    "}\n"
    "\n"
    "/// Sub tree for search and destroy\n"
    "tree SearchAndDestroy(ref target: Vector3, ref ammo: int32, ref alert: bool) {\n"
    "  Sequence {\n"
    "    FindEnemy(pos: out target, found: out alert);\n"
    "    AttackAction(loc: target, val: ref ammo);\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::Program * p = unit.program;
  ASSERT_NE(p, nullptr);

  // Inner docs
  ASSERT_EQ(p->innerDocs.size(), 1U);
  EXPECT_NE(p->innerDocs[0].find("Soldier AI"), std::string_view::npos);

  // Import
  ASSERT_EQ(p->imports().size(), 1U);
  EXPECT_EQ(p->imports()[0]->path_string(), "StandardNodes.bt");

  // Global vars
  ASSERT_EQ(p->global_vars().size(), 3U);
  EXPECT_EQ(p->global_vars()[0]->name, "TargetPos");
  EXPECT_EQ(p->global_vars()[1]->name, "Ammo");
  EXPECT_EQ(p->global_vars()[2]->name, "IsAlerted");

  // Trees
  ASSERT_EQ(p->trees().size(), 2U);
  EXPECT_EQ(p->trees()[0]->name, "Main");
  EXPECT_EQ(p->trees()[0]->docs.size(), 1U);
  EXPECT_EQ(p->trees()[1]->name, "SearchAndDestroy");
  EXPECT_EQ(p->trees()[1]->docs.size(), 1U);

  // Tree params
  ASSERT_EQ(p->trees()[1]->params.size(), 3U);
  EXPECT_EQ(p->trees()[1]->params[0]->name, "target");
  {
    const auto & dir_opt = p->trees()[1]->params[0]->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::Ref);
  }
}

// ============================================================================
// Test: SourceRangesArePopulated
// ============================================================================
TEST(AstComplexExamples, SourceRangesArePopulated)
{
  const std::string src =
    "tree Main() {\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::Program * p = unit.program;
  ASSERT_NE(p, nullptr);

  // Check source ranges are populated
  EXPECT_GT(p->get_range().get_end().get_offset(), p->get_range().get_begin().get_offset());
  ASSERT_EQ(p->trees().size(), 1U);
  EXPECT_GT(
    p->trees()[0]->get_range().get_end().get_offset(),
    p->trees()[0]->get_range().get_begin().get_offset());

  // Check node stmt range
  ASSERT_FALSE(p->trees()[0]->body.empty());
  auto * node = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(p->trees()[0]->body[0]);
  ASSERT_NE(node, nullptr);
  EXPECT_GT(node->get_range().get_end().get_offset(), node->get_range().get_begin().get_offset());
}

// ============================================================================
// Test: ComplexExpressionsInArgs
// ============================================================================
TEST(AstComplexExamples, ComplexExpressionsInArgs)
{
  const std::string src =
    "tree Main() {\n"
    "  Action(\n"
    "    a: (x + y) * z,\n"
    "    b: arr[i + 1] as int32,\n"
    "    c: !flag && other\n"
    "  );\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * tree = unit.program->trees()[0];
  auto * node = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(tree->body[0]);
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->args.size(), 3U);

  // arg a: (x + y) * z
  auto * a = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(node->args[0]->valueExpr);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->op, bt_dsl::BinaryOp::Mul);

  // arg b: arr[i + 1] as int32
  auto * b = bt_dsl::dyn_cast<bt_dsl::CastExpr>(node->args[1]->valueExpr);
  ASSERT_NE(b, nullptr);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::IndexExpr>(b->expr));

  // arg c: !flag && other
  auto * c = bt_dsl::dyn_cast<bt_dsl::BinaryExpr>(node->args[2]->valueExpr);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->op, bt_dsl::BinaryOp::And);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::UnaryExpr>(c->lhs));
}

// ============================================================================
// Test: AllAssignOps
// ============================================================================
TEST(AstComplexExamples, AllAssignOps)
{
  // Note: Only =, +=, -=, *=, /= are currently supported by the parser
  const std::string src =
    "tree Main() {\n"
    "  var x: int32;\n"
    "  Sequence {\n"
    "    x = 1;\n"
    "    x += 2;\n"
    "    x -= 3;\n"
    "    x *= 4;\n"
    "    x /= 5;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * tree = unit.program->trees()[0];
  bt_dsl::NodeStmt * seq = nullptr;
  for (auto * s : tree->body) {
    if (auto * n = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
      if (n->nodeName == "Sequence") seq = n;
    }
  }
  ASSERT_NE(seq, nullptr);
  ASSERT_EQ(seq->children.size(), 5U);

  auto check = [&](size_t idx, bt_dsl::AssignOp expected, const char * name) {
    auto * a = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[idx]);
    ASSERT_NE(a, nullptr) << name;
    EXPECT_EQ(a->op, expected) << name;
  };

  check(0, bt_dsl::AssignOp::Assign, "op =");
  check(1, bt_dsl::AssignOp::AddAssign, "op +=");
  check(2, bt_dsl::AssignOp::SubAssign, "op -=");
  check(3, bt_dsl::AssignOp::MulAssign, "op *=");
  check(4, bt_dsl::AssignOp::DivAssign, "op /=");
}
