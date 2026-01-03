#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/syntax/frontend.hpp"

TEST(AstProgram, TopLevel)
{
  const std::string src =
    "import \"nodes.bt\";\n"
    "/// Pose is an external type\n"
    "extern type Pose;\n"
    "/// alias\n"
    "type PoseVec = vec<Pose>;\n"
    "/// global var\n"
    "var g: int32 = 1 + 2 * 3;\n"
    "/// global const\n"
    "const C: int32 = 42;\n"
    "/// behaviorized extern\n"
    "#[behavior(Any, Isolated)] extern action MoveTo(in goal: Pose);\n"
    "\n"
    "tree Main() {\n"
    "  Action();\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);

  if (!unit->diags.empty()) {
    for (const auto & d : unit->diags.all()) {
      std::cerr << "diag: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(unit->diags.empty());

  bt_dsl::Program * p = unit->program;
  ASSERT_NE(p, nullptr);

  ASSERT_EQ(p->imports.size(), 1U);
  EXPECT_EQ(p->imports[0]->path_string(), "nodes.bt");

  ASSERT_EQ(p->externTypes.size(), 1U);
  EXPECT_EQ(p->externTypes[0]->name, "Pose");
  EXPECT_EQ(p->externTypes[0]->docs.size(), 1U);

  ASSERT_EQ(p->typeAliases.size(), 1U);
  EXPECT_EQ(p->typeAliases[0]->name, "PoseVec");
  ASSERT_NE(p->typeAliases[0]->aliasedType, nullptr);

  ASSERT_EQ(p->globalVars.size(), 1U);
  EXPECT_EQ(p->globalVars[0]->name, "g");
  ASSERT_NE(p->globalVars[0]->type, nullptr);
  ASSERT_NE(p->globalVars[0]->initialValue, nullptr);

  ASSERT_EQ(p->globalConsts.size(), 1U);
  EXPECT_EQ(p->globalConsts[0]->name, "C");
  ASSERT_NE(p->globalConsts[0]->value, nullptr);

  ASSERT_EQ(p->externs.size(), 1U);
  ASSERT_NE(p->externs[0]->behaviorAttr, nullptr);
  EXPECT_EQ(p->externs[0]->behaviorAttr->dataPolicy, bt_dsl::DataPolicy::Any);
  {
    const auto & flow_policy = p->externs[0]->behaviorAttr->flowPolicy;
    ASSERT_TRUE(flow_policy.has_value());
    if (!flow_policy.has_value()) return;
    EXPECT_EQ(*flow_policy, bt_dsl::FlowPolicy::Isolated);
  }

  ASSERT_EQ(p->trees.size(), 1U);
  EXPECT_EQ(p->trees[0]->name, "Main");
}
