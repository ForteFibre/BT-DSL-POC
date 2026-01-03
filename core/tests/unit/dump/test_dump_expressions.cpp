#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast_dumper.hpp"
#include "bt_dsl/syntax/frontend.hpp"

TEST(DumpAst, Expressions)
{
  const std::string src =
    "tree T() {\n"
    "  Action(a: 1 + 2 * 3);\n"
    "  Action(b: !(true && false));\n"
    "  Action(c: x[0] + y[1]);\n"
    "  Action(d: 1 as int32 as float);\n"
    "  Action(e: [1, 2, 3][0]);\n"
    "  Action(f: vec![1, 2]);\n"
    "}\n";

  auto unit = bt_dsl::parse_source(src);
  ASSERT_NE(unit, nullptr);

  if (!unit->diags.empty()) {
    for (const auto & d : unit->diags.all()) {
      std::cerr << "diag: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(unit->diags.empty());

  const std::string got = bt_dsl::dump_to_string(unit->program);

  const std::string expected =
    "Program\n"
    "`-TreeDecl name='T'\n"
    "  |-NodeStmt name='Action' [props]\n"
    "  | `-Argument name='a'\n"
    "  |   `-BinaryExpr op='+'\n"
    "  |     |-IntLiteralExpr 1\n"
    "  |     `-BinaryExpr op='*'\n"
    "  |       |-IntLiteralExpr 2\n"
    "  |       `-IntLiteralExpr 3\n"
    "  |-NodeStmt name='Action' [props]\n"
    "  | `-Argument name='b'\n"
    "  |   `-UnaryExpr op='!'\n"
    "  |     `-BinaryExpr op='&&'\n"
    "  |       |-BoolLiteralExpr true\n"
    "  |       `-BoolLiteralExpr false\n"
    "  |-NodeStmt name='Action' [props]\n"
    "  | `-Argument name='c'\n"
    "  |   `-BinaryExpr op='+'\n"
    "  |     |-IndexExpr\n"
    "  |     | |-VarRefExpr name='x'\n"
    "  |     | `-IntLiteralExpr 0\n"
    "  |     `-IndexExpr\n"
    "  |       |-VarRefExpr name='y'\n"
    "  |       `-IntLiteralExpr 1\n"
    "  |-NodeStmt name='Action' [props]\n"
    "  | `-Argument name='d'\n"
    "  |   `-CastExpr\n"
    "  |     |-CastExpr\n"
    "  |     | |-IntLiteralExpr 1\n"
    "  |     | `-TypeExpr\n"
    "  |     |   `-PrimaryType name='int32'\n"
    "  |     `-TypeExpr\n"
    "  |       `-PrimaryType name='float'\n"
    "  |-NodeStmt name='Action' [props]\n"
    "  | `-Argument name='e'\n"
    "  |   `-IndexExpr\n"
    "  |     |-ArrayLiteralExpr\n"
    "  |     | |-IntLiteralExpr 1\n"
    "  |     | |-IntLiteralExpr 2\n"
    "  |     | `-IntLiteralExpr 3\n"
    "  |     `-IntLiteralExpr 0\n"
    "  `-NodeStmt name='Action' [props]\n"
    "    `-Argument name='f'\n"
    "      `-VecMacroExpr\n"
    "        `-ArrayLiteralExpr\n"
    "          |-IntLiteralExpr 1\n"
    "          `-IntLiteralExpr 2\n"
    "\n";

  EXPECT_EQ(got, expected);
}
