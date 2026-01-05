#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

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

// ============================================================================
// Test: ParseNestedChildren
// ============================================================================
TEST(AstChildrenBlocks, NestedChildren)
{
  const std::string src =
    "tree Main() {\n"
    "  Sequence {\n"
    "    Fallback {\n"
    "      Action1();\n"
    "      Action2();\n"
    "    }\n"
    "    Action3();\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::NodeStmt * seq = first_node_stmt(unit.program->trees()[0]->body);
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(seq->nodeName, "Sequence");
  ASSERT_TRUE(seq->hasChildrenBlock);
  ASSERT_EQ(seq->children.size(), 2U);

  // First child is Fallback with 2 children
  auto * fallback = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(seq->children[0]);
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ(fallback->nodeName, "Fallback");
  ASSERT_EQ(fallback->children.size(), 2U);

  auto * action1 = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(fallback->children[0]);
  ASSERT_NE(action1, nullptr);
  EXPECT_EQ(action1->nodeName, "Action1");

  auto * action2 = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(fallback->children[1]);
  ASSERT_NE(action2, nullptr);
  EXPECT_EQ(action2->nodeName, "Action2");

  // Second child is Action3
  auto * action3 = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(seq->children[1]);
  ASSERT_NE(action3, nullptr);
  EXPECT_EQ(action3->nodeName, "Action3");
}

// ============================================================================
// Test: AssignmentInChildren
// ============================================================================
TEST(AstChildrenBlocks, AssignmentInChildren)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: int32;\n"
    "  Sequence {\n"
    "    result = a + b;\n"
    "    result += 1;\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::TreeDecl * t = unit.program->trees()[0];
  bt_dsl::NodeStmt * seq = nullptr;
  for (auto * s : t->body) {
    if (auto * n = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(s)) {
      if (n->nodeName == "Sequence") {
        seq = n;
        break;
      }
    }
  }
  ASSERT_NE(seq, nullptr);
  ASSERT_EQ(seq->children.size(), 2U);

  auto * assign1 = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[0]);
  ASSERT_NE(assign1, nullptr);
  EXPECT_EQ(assign1->target, "result");
  EXPECT_EQ(assign1->op, bt_dsl::AssignOp::Assign);

  auto * assign2 = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(seq->children[1]);
  ASSERT_NE(assign2, nullptr);
  EXPECT_EQ(assign2->op, bt_dsl::AssignOp::AddAssign);
}

// ============================================================================
// Test: EmptyChildrenBlock
// ============================================================================
TEST(AstChildrenBlocks, EmptyChildrenBlock)
{
  const std::string src =
    "tree Main() {\n"
    "  Sequence {}\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::NodeStmt * seq = first_node_stmt(unit.program->trees()[0]->body);
  ASSERT_NE(seq, nullptr);
  ASSERT_TRUE(seq->hasChildrenBlock);
  EXPECT_TRUE(seq->children.empty());
}

// ============================================================================
// Test: LeafNodeWithSemicolon
// ============================================================================
TEST(AstChildrenBlocks, LeafNodeWithSemicolon)
{
  const std::string src =
    "tree Main() {\n"
    "  Sequence {\n"
    "    Action();\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::NodeStmt * seq = first_node_stmt(unit.program->trees()[0]->body);
  ASSERT_NE(seq, nullptr);
  ASSERT_EQ(seq->children.size(), 1U);

  auto * action = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(seq->children[0]);
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->nodeName, "Action");
  EXPECT_FALSE(action->hasChildrenBlock);
}

// ============================================================================
// Test: DeeplyNestedChildren
// ============================================================================
TEST(AstChildrenBlocks, DeeplyNestedChildren)
{
  const std::string src =
    "tree Main() {\n"
    "  A {\n"
    "    B {\n"
    "      C {\n"
    "        D();\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);
  ASSERT_TRUE(unit.diags.empty());

  auto * a = first_node_stmt(unit.program->trees()[0]->body);
  ASSERT_TRUE(a != nullptr && a->nodeName == "A");

  auto * b = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(a->children[0]);
  ASSERT_TRUE(b != nullptr && b->nodeName == "B");

  auto * c = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(b->children[0]);
  ASSERT_TRUE(c != nullptr && c->nodeName == "C");

  auto * d = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(c->children[0]);
  ASSERT_TRUE(d != nullptr && d->nodeName == "D");
}
