#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "bt_dsl/ast/ast_dumper.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

TEST(DumpAst, TypesAndLiterals)
{
  // Minimal program covering:
  // - Types: string<10>, [int32; 4], vec<_?>
  // - Literals: integer, string (unescape), boolean, null
  const std::string src =
    "tree T(x: string<10>, y: [int32; 4], z: vec<_?>) {\n"
    "  Action(a: 42, b: \"hi\", c: true, d: null);\n"
    "}\n";

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
    "`-TreeDecl name='T'\n"
    "  |-ParamDecl name='x'\n"
    "  | `-TypeExpr\n"
    "  |   `-PrimaryType name='string' size='10'\n"
    "  |-ParamDecl name='y'\n"
    "  | `-TypeExpr\n"
    "  |   `-StaticArrayType size='4'\n"
    "  |     `-TypeExpr\n"
    "  |       `-PrimaryType name='int32'\n"
    "  |-ParamDecl name='z'\n"
    "  | `-TypeExpr\n"
    "  |   `-DynamicArrayType\n"
    "  |     `-TypeExpr nullable\n"
    "  |       `-InferType\n"
    "  `-NodeStmt name='Action' [props]\n"
    "    |-Argument name='a'\n"
    "    | `-IntLiteralExpr 42\n"
    "    |-Argument name='b'\n"
    "    | `-StringLiteralExpr \"hi\"\n"
    "    |-Argument name='c'\n"
    "    | `-BoolLiteralExpr true\n"
    "    `-Argument name='d'\n"
    "      `-NullLiteralExpr\n"
    "\n";

  EXPECT_EQ(got, expected);
}
