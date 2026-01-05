#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/syntax/frontend.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

static bt_dsl::NodeStmt * find_node_stmt(gsl::span<bt_dsl::Stmt *> body, std::string_view name)
{
  for (auto * s : body) {
    if (!s) continue;
    if (s->get_kind() != bt_dsl::NodeKind::NodeStmt) continue;
    auto * n = static_cast<bt_dsl::NodeStmt *>(s);
    if (n->nodeName == name) return n;
  }
  return nullptr;
}

TEST(AstStatements, ChildrenAndInlineDecl)
{
  const std::string src =
    "tree Main() {\n"
    "  var result: int32;\n"
    "  Sequence {\n"
    "    /// assignment doc\n"
    "    @success_if(result == 0)\n"
    "    result = 1;\n"
    "    Action(tmp: out var tmp);\n"
    "    Action(out_val: out result);\n"
    "  }\n"
    "}\n";

  auto unit = bt_dsl::test_support::parse(src);

  if (!unit.diags.empty()) {
    for (const auto & d : unit.diags.all()) {
      std::cerr << "diag: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(unit.diags.empty());

  bt_dsl::Program * p = unit.program;
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->trees().size(), 1U);

  bt_dsl::TreeDecl * t = p->trees()[0];
  bt_dsl::NodeStmt * seq = find_node_stmt(t->body, "Sequence");
  ASSERT_NE(seq, nullptr);
  ASSERT_TRUE(seq->hasChildrenBlock);

  // children: assignment + 2 leaf node calls
  ASSERT_EQ(seq->children.size(), 3U);

  auto * s0 = seq->children[0];
  ASSERT_NE(s0, nullptr);
  ASSERT_EQ(s0->get_kind(), bt_dsl::NodeKind::AssignmentStmt);
  auto * assign = static_cast<bt_dsl::AssignmentStmt *>(s0);
  EXPECT_EQ(assign->docs.size(), 1U);
  ASSERT_EQ(assign->preconditions.size(), 1U);
  EXPECT_EQ(assign->preconditions[0]->kind, bt_dsl::PreconditionKind::SuccessIf);
  EXPECT_EQ(assign->op, bt_dsl::AssignOp::Assign);

  // 2nd child: Action(tmp: out var tmp)
  auto * s1 = seq->children[1];
  ASSERT_TRUE(s1 != nullptr && s1->get_kind() == bt_dsl::NodeKind::NodeStmt);
  auto * action1 = static_cast<bt_dsl::NodeStmt *>(s1);
  ASSERT_EQ(action1->args.size(), 1U);
  bt_dsl::Argument * a0 = action1->args[0];
  ASSERT_NE(a0, nullptr);
  ASSERT_TRUE(a0->is_inline_decl());
  ASSERT_NE(a0->inlineDecl, nullptr);
  EXPECT_EQ(a0->inlineDecl->name, "tmp");

  // 3rd child: Action(out_val: out result)
  auto * s2 = seq->children[2];
  ASSERT_TRUE(s2 != nullptr && s2->get_kind() == bt_dsl::NodeKind::NodeStmt);
  auto * action2 = static_cast<bt_dsl::NodeStmt *>(s2);
  ASSERT_EQ(action2->args.size(), 1U);
  bt_dsl::Argument * a1 = action2->args[0];
  ASSERT_NE(a1, nullptr);
  ASSERT_FALSE(a1->is_inline_decl());
  {
    const auto & dir_opt = a1->direction;
    ASSERT_TRUE(dir_opt.has_value());
    if (!dir_opt.has_value()) return;
    EXPECT_EQ(*dir_opt, bt_dsl::PortDirection::Out);
  }
  ASSERT_NE(a1->valueExpr, nullptr);
  EXPECT_EQ(a1->valueExpr->get_kind(), bt_dsl::NodeKind::VarRef);
}
