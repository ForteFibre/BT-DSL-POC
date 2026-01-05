#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast_dumper.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

TEST(DumpAst, DeclarationSignatures)
{
  // Signature-only declarations test:
  // - program-level extern_stmt -> Program::externs
  // - program-level tree_def -> TreeDecl with ParamDecl (direction/default)
  // - default values are const_expr literals (current phase)
  const std::string src =
    "extern action MoveTo(in goal: Pose = null, out ok: bool);\n"
    "extern subtree Plan(ref target: Pose, mut state: int32);\n"
    "\n"
    "tree Main(in target: Pose, out ok: bool) {}\n";

  auto unit = bt_dsl::test_support::parse(src);

  // No diagnostics expected for this input.
  if (!unit.diags.empty()) {
    for (const auto & d : unit.diags.all()) {
      std::cerr << "diag: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(unit.diags.empty());

  const std::string got = bt_dsl::dump_to_string(unit.program);

  const std::string expected =
    "Program\n"
    "|-ExternDecl action name='MoveTo'\n"
    "| |-ExternPort name='goal' in\n"
    "| | |-TypeExpr\n"
    "| | | `-PrimaryType name='Pose'\n"
    "| | `-NullLiteralExpr\n"
    "| `-ExternPort name='ok' out\n"
    "|   `-TypeExpr\n"
    "|     `-PrimaryType name='bool'\n"
    "|-ExternDecl subtree name='Plan'\n"
    "| |-ExternPort name='target' ref\n"
    "| | `-TypeExpr\n"
    "| |   `-PrimaryType name='Pose'\n"
    "| `-ExternPort name='state' mut\n"
    "|   `-TypeExpr\n"
    "|     `-PrimaryType name='int32'\n"
    "`-TreeDecl name='Main'\n"
    "  |-ParamDecl name='target' in\n"
    "  | `-TypeExpr\n"
    "  |   `-PrimaryType name='Pose'\n"
    "  `-ParamDecl name='ok' out\n"
    "    `-TypeExpr\n"
    "      `-PrimaryType name='bool'\n"
    "\n";

  EXPECT_EQ(got, expected);
}
