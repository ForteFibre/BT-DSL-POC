// test_lsp_hover_definition.cpp - Serverless LSP hover/definition tests

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "bt_dsl/lsp.hpp"

using json = nlohmann::json;

TEST(LspHoverTest, HoverShowsTypeForGlobalVarInNodeArg)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
var MyTarget: string
Tree Main() {
  MyAction(target: MyTarget)
}
)";

  ws.set_document(uri, src);

  const auto use_pos = src.find("MyTarget)");
  ASSERT_NE(use_pos, std::string::npos);

  const auto j = json::parse(ws.hover_json(uri, static_cast<uint32_t>(use_pos + 2)));

  ASSERT_TRUE(j.contains("contents"));
  ASSERT_TRUE(j["contents"].is_string());

  const std::string md = j["contents"].get<std::string>();
  EXPECT_NE(md.find("**MyTarget**"), std::string::npos);
  EXPECT_NE(md.find("Type: `string`"), std::string::npos);
}

TEST(LspDefinitionTest, DefinitionPointsToGlobalVarDeclaration)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
var MyTarget: string
Tree Main() {
  MyAction(target: MyTarget)
}
)";

  ws.set_document(uri, src);

  const auto def_pos_expected = src.find("var MyTarget") + std::string("var ").size();
  const auto use_pos = src.find("MyTarget)");
  ASSERT_NE(use_pos, std::string::npos);

  const auto j = json::parse(ws.definition_json(uri, static_cast<uint32_t>(use_pos + 1)));

  ASSERT_TRUE(j.contains("locations"));
  ASSERT_TRUE(j["locations"].is_array());
  ASSERT_FALSE(j["locations"].empty());

  const auto & loc = j["locations"][0];
  ASSERT_TRUE(loc.contains("range"));

  const uint32_t start = loc["range"]["startByte"].get<uint32_t>();
  EXPECT_EQ(start, static_cast<uint32_t>(def_pos_expected));
}

TEST(LspDefinitionTest, DefinitionJumpsToImportedDeclareEvenWhenIndented)
{
  bt_dsl::lsp::Workspace ws;

  const std::string main_uri = "file:///main.bt";
  const std::string std_uri = "file:///StandardNodes.bt";

  const std::string std_src = R"(
declare Action FindEnemy(in range: float)
)";

  const std::string main_src = R"(
import "./StandardNodes.bt"
Tree Main() {
  Sequence {
    FindEnemy(range: 1)
  }
}
)";

  ws.set_document(main_uri, main_src);
  ws.set_document(std_uri, std_src);

  const auto use_pos = main_src.find("FindEnemy");
  ASSERT_NE(use_pos, std::string::npos);

  const auto j =
    json::parse(ws.definition_json(main_uri, static_cast<uint32_t>(use_pos + 1), {std_uri}));

  ASSERT_TRUE(j.contains("locations"));
  ASSERT_TRUE(j["locations"].is_array());
  ASSERT_FALSE(j["locations"].empty());
  EXPECT_EQ(j["locations"][0]["uri"].get<std::string>(), std_uri);
}

TEST(LspDefinitionTest, DefinitionJumpsToSubTreeDefinitionInSameFile)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///soldier-ai.bt";
  const std::string src = R"(
import "./StandardNodes.bt"
var TargetPos: Vector3
Tree Main() {
  Sequence {
    SearchAndDestroy(target: ref TargetPos)
  }
}

Tree SearchAndDestroy(ref target) {
  Sequence { }
}
)";

  ws.set_document(uri, src);

  const auto call_pos = src.find("SearchAndDestroy(target");
  ASSERT_NE(call_pos, std::string::npos);

  const auto def_pos_expected = src.find("Tree SearchAndDestroy") + std::string("Tree ").size();
  ASSERT_NE(def_pos_expected, std::string::npos);

  const auto j = json::parse(ws.definition_json(uri, static_cast<uint32_t>(call_pos + 2)));
  ASSERT_TRUE(j.contains("locations"));
  ASSERT_FALSE(j["locations"].empty());

  const auto & loc = j["locations"][0];
  EXPECT_EQ(loc["uri"].get<std::string>(), uri);
  EXPECT_EQ(loc["range"]["startByte"].get<uint32_t>(), static_cast<uint32_t>(def_pos_expected));
}

TEST(LspDefinitionTest, DefinitionJumpsToImportedFileFromImportPath)
{
  bt_dsl::lsp::Workspace ws;

  const std::string main_uri = "file:///soldier-ai.bt";
  const std::string std_uri = "file:///StandardNodes.bt";

  const std::string main_src = R"(
import "./StandardNodes.bt"
Tree Main() { Sequence() }
)";
  ws.set_document(main_uri, main_src);
  ws.set_document(std_uri, "declare Action AlwaysSuccess()\n");

  const auto pos = main_src.find("./StandardNodes.bt");
  ASSERT_NE(pos, std::string::npos);

  const auto j = json::parse(ws.definition_json(main_uri, static_cast<uint32_t>(pos + 2)));
  ASSERT_TRUE(j.contains("locations"));
  ASSERT_FALSE(j["locations"].empty());
  EXPECT_EQ(j["locations"][0]["uri"].get<std::string>(), std_uri);
}
