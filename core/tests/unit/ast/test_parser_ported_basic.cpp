#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/syntax/frontend.hpp"

static const bt_dsl::TreeDecl * first_tree(const bt_dsl::Program * p)
{
  if (p == nullptr) return nullptr;
  if (p->trees.empty()) return nullptr;
  return p->trees[0];
}

TEST(AstParser, PortedBasic)
{
  // A small selection ported (in spirit) from core/tests/parser/test_parser.cpp.
  const std::string src =
    "import \"nodes.bt\";\n"
    "extern action FindEnemy(in range: float, out pos: Vector3, out found: bool);\n"
    "\n"
    "tree MyTree(ref target: any, amount: int) {\n"
    "  var _: int = 0;\n"
    "  var x: _ = 1;\n"
    "  _ = 2;\n"
    "  FindEnemy();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);

  if (!unit->diags.empty()) {
    for (const auto & d : unit->diags.all()) {
      std::cerr << "diag: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(unit->diags.empty());

  const bt_dsl::Program * p = unit->program;
  ASSERT_NE(p, nullptr);

  // Import
  ASSERT_EQ(p->imports.size(), 1U);
  EXPECT_EQ(p->imports[0]->path_string(), "nodes.bt");

  // Extern decl
  ASSERT_EQ(p->externs.size(), 1U);
  const bt_dsl::ExternDecl * ex = p->externs[0];
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(ex->category, bt_dsl::ExternNodeCategory::Action);
  EXPECT_EQ(ex->name, "FindEnemy");
  ASSERT_EQ(ex->ports.size(), 3U);
  EXPECT_EQ(ex->ports[0]->name, "range");
  {
    const auto & dir_opt = ex->ports[0]->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::In);
  }
  EXPECT_EQ(ex->ports[1]->name, "pos");
  {
    const auto & dir_opt = ex->ports[1]->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::Out);
  }
  EXPECT_EQ(ex->ports[2]->name, "found");
  {
    const auto & dir_opt = ex->ports[2]->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::Out);
  }

  // Tree params
  const bt_dsl::TreeDecl * t = first_tree(p);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->name, "MyTree");
  ASSERT_EQ(t->params.size(), 2U);
  EXPECT_EQ(t->params[0]->name, "target");
  {
    const auto & dir_opt = t->params[0]->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::Ref);
  }
  ASSERT_NE(t->params[0]->type, nullptr);
  EXPECT_EQ(t->params[1]->name, "amount");
  EXPECT_FALSE(t->params[1]->direction.has_value());
  ASSERT_NE(t->params[1]->type, nullptr);

  // Statements: var _, var x: _, assignment, node call
  ASSERT_GE(t->body.size(), 4U);

  const auto * st0 = bt_dsl::dyn_cast<bt_dsl::BlackboardDeclStmt>(t->body[0]);
  ASSERT_NE(st0, nullptr);
  EXPECT_EQ(st0->name, "_");

  const auto * st1 = bt_dsl::dyn_cast<bt_dsl::BlackboardDeclStmt>(t->body[1]);
  ASSERT_NE(st1, nullptr);
  EXPECT_EQ(st1->name, "x");
  ASSERT_NE(st1->type, nullptr);
  ASSERT_NE(st1->type->base, nullptr);
  EXPECT_TRUE(bt_dsl::isa<bt_dsl::InferType>(st1->type->base));

  const auto * st2 = bt_dsl::dyn_cast<bt_dsl::AssignmentStmt>(t->body[2]);
  ASSERT_NE(st2, nullptr);
  EXPECT_EQ(st2->target, "_");

  const auto * st3 = bt_dsl::dyn_cast<bt_dsl::NodeStmt>(t->body[3]);
  ASSERT_NE(st3, nullptr);
  EXPECT_EQ(st3->nodeName, "FindEnemy");
}
