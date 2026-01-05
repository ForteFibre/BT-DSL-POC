// test_json_visitor.cpp - Unit tests for AST JSON serialization
//
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/json_visitor.hpp"
#include "bt_dsl/test_support/parse_helpers.hpp"

using nlohmann::json;

namespace bt_dsl
{

class JsonVisitorTest : public ::testing::Test
{
protected:
  static json parse_and_serialize(const std::string & source)
  {
    auto unit = test_support::parse(source);
    EXPECT_NE(unit.program, nullptr);
    return to_json(unit.program);
  }
};

TEST_F(JsonVisitorTest, EmptyProgram)
{
  auto j = parse_and_serialize("");
  EXPECT_EQ(j["type"], "Program");
  EXPECT_TRUE(j["decls"].is_array());
  EXPECT_EQ(j["decls"].size(), 0);
}

TEST_F(JsonVisitorTest, SimpleTree)
{
  auto j = parse_and_serialize(R"(
    tree main() {
      SomeNode();
    }
  )");

  EXPECT_EQ(j["type"], "Program");
  ASSERT_EQ(j["decls"].size(), 1);

  auto tree = j["decls"][0];
  EXPECT_EQ(tree["type"], "TreeDecl");
  EXPECT_EQ(tree["name"], "main");
  ASSERT_EQ(tree["body"].size(), 1);

  auto node = tree["body"][0];
  EXPECT_EQ(node["type"], "NodeStmt");
  EXPECT_EQ(node["nodeName"], "SomeNode");
}

TEST_F(JsonVisitorTest, GlobalConst)
{
  auto j = parse_and_serialize("const MAX_VALUE = 100;");

  EXPECT_EQ(j["type"], "Program");
  ASSERT_EQ(j["decls"].size(), 1);

  auto gc = j["decls"][0];
  EXPECT_EQ(gc["type"], "GlobalConstDecl");
  EXPECT_EQ(gc["name"], "MAX_VALUE");
  EXPECT_EQ(gc["value"]["type"], "IntLiteralExpr");
  EXPECT_EQ(gc["value"]["value"], 100);
}

TEST_F(JsonVisitorTest, GlobalVar)
{
  auto j = parse_and_serialize("var counter: int32;");

  ASSERT_EQ(j["decls"].size(), 1);
  auto gv = j["decls"][0];
  EXPECT_EQ(gv["type"], "GlobalVarDecl");
  EXPECT_EQ(gv["name"], "counter");
  EXPECT_EQ(gv["typeExpr"]["type"], "TypeExpr");
}

TEST_F(JsonVisitorTest, ImportDecl)
{
  auto j = parse_and_serialize(R"(import "std/nodes.bt";)");

  ASSERT_EQ(j["decls"].size(), 1);
  auto imp = j["decls"][0];
  EXPECT_EQ(imp["type"], "ImportDecl");
  EXPECT_EQ(imp["path"], "std/nodes.bt");
}

TEST_F(JsonVisitorTest, ProgramDeclOrderIsPreserved)
{
  auto j = parse_and_serialize(R"(
    import "std/nodes.bt";
    extern type Pose;
    const A = 1;
    extern action Say(message: string);
    var x: int32;
    tree Main() {}
    const B = 2;
  )");

  ASSERT_EQ(j["type"], "Program");
  ASSERT_EQ(j["decls"].size(), 7);

  EXPECT_EQ(j["decls"][0]["type"], "ImportDecl");
  EXPECT_EQ(j["decls"][0]["path"], "std/nodes.bt");

  EXPECT_EQ(j["decls"][1]["type"], "ExternTypeDecl");
  EXPECT_EQ(j["decls"][1]["name"], "Pose");

  EXPECT_EQ(j["decls"][2]["type"], "GlobalConstDecl");
  EXPECT_EQ(j["decls"][2]["name"], "A");

  EXPECT_EQ(j["decls"][3]["type"], "ExternDecl");
  EXPECT_EQ(j["decls"][3]["name"], "Say");

  EXPECT_EQ(j["decls"][4]["type"], "GlobalVarDecl");
  EXPECT_EQ(j["decls"][4]["name"], "x");

  EXPECT_EQ(j["decls"][5]["type"], "TreeDecl");
  EXPECT_EQ(j["decls"][5]["name"], "Main");

  EXPECT_EQ(j["decls"][6]["type"], "GlobalConstDecl");
  EXPECT_EQ(j["decls"][6]["name"], "B");
}

TEST_F(JsonVisitorTest, ExternDecl)
{
  auto j = parse_and_serialize(R"(
    extern action MoveForward(in speed: float64);
  )");

  ASSERT_EQ(j["decls"].size(), 1);
  auto ext = j["decls"][0];
  EXPECT_EQ(ext["type"], "ExternDecl");
  EXPECT_EQ(ext["category"], "action");
  EXPECT_EQ(ext["name"], "MoveForward");
  ASSERT_EQ(ext["ports"].size(), 1);
  EXPECT_EQ(ext["ports"][0]["name"], "speed");
  EXPECT_EQ(ext["ports"][0]["direction"], "in");
}

TEST_F(JsonVisitorTest, BinaryExpression)
{
  auto j = parse_and_serialize("const x = 1 + 2 * 3;");

  auto value = j["decls"][0]["value"];
  EXPECT_EQ(value["type"], "BinaryExpr");
  EXPECT_EQ(value["op"], "+");
  EXPECT_EQ(value["lhs"]["type"], "IntLiteralExpr");
  EXPECT_EQ(value["rhs"]["type"], "BinaryExpr");
  EXPECT_EQ(value["rhs"]["op"], "*");
}

TEST_F(JsonVisitorTest, ArrayLiteral)
{
  auto j = parse_and_serialize("const arr = [1, 2, 3];");

  auto value = j["decls"][0]["value"];
  EXPECT_EQ(value["type"], "ArrayLiteralExpr");
  ASSERT_EQ(value["elements"].size(), 3);
  EXPECT_EQ(value["elements"][0]["value"], 1);
  EXPECT_EQ(value["elements"][1]["value"], 2);
  EXPECT_EQ(value["elements"][2]["value"], 3);
}

TEST_F(JsonVisitorTest, NodeWithPrecondition)
{
  auto j = parse_and_serialize(R"(
    tree main() {
      @guard(x > 0)
      SomeNode();
    }
  )");

  auto node = j["decls"][0]["body"][0];
  EXPECT_EQ(node["type"], "NodeStmt");
  ASSERT_EQ(node["preconditions"].size(), 1);
  EXPECT_EQ(node["preconditions"][0]["kind"], "guard");
  EXPECT_EQ(node["preconditions"][0]["condition"]["type"], "BinaryExpr");
}

TEST_F(JsonVisitorTest, RangeIsPresent)
{
  auto j = parse_and_serialize("const x = 42;");

  auto decl = j["decls"][0];
  EXPECT_TRUE(decl.contains("range"));
  EXPECT_TRUE(decl["range"].contains("start"));
  EXPECT_TRUE(decl["range"].contains("end"));
  EXPECT_GE(decl["range"]["start"].get<int>(), 0);
  EXPECT_GT(decl["range"]["end"].get<int>(), decl["range"]["start"].get<int>());
}

}  // namespace bt_dsl
