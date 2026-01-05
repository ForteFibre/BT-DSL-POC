#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
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

TEST(AstDocs, CrLfIsNormalized)
{
  const std::string src = std::string("//! Module doc\r\n") + std::string("/// Tree doc\r\n") +
                          std::string("tree Main() {\r\n") + std::string("  /// Node doc\r\n") +
                          std::string("  Action();\r\n") + std::string("}\r\n");

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::Program * p = unit->program;
  ASSERT_NE(p, nullptr);
  ASSERT_EQ(p->innerDocs.size(), 1U);
  EXPECT_EQ(p->innerDocs[0].find('\r'), std::string_view::npos);

  ASSERT_EQ(p->trees().size(), 1U);
  bt_dsl::TreeDecl * t = p->trees()[0];
  ASSERT_EQ(t->docs.size(), 1U);
  EXPECT_EQ(t->docs[0].find('\r'), std::string_view::npos);

  bt_dsl::NodeStmt * root = first_node_stmt(t->body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->docs.size(), 1U);
  EXPECT_EQ(root->docs[0].find('\r'), std::string_view::npos);
}
