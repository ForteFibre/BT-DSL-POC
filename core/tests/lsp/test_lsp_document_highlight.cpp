// test_lsp_document_highlight.cpp - Serverless LSP document highlight tests

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "bt_dsl/lsp/lsp.hpp"

using json = nlohmann::json;

TEST(LspDocumentHighlightTest, HighlightsNodeCallOccurrencesInSameTree)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
Tree Main() {
  MyAction(target: Foo)
  MyAction(target: Bar)
}
)";

  ws.set_document(uri, src);

  const auto use_pos = src.find("MyAction(target: Bar)");
  ASSERT_NE(use_pos, std::string::npos);

  const auto j = json::parse(ws.document_highlights_json(uri, static_cast<uint32_t>(use_pos + 2)));

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  // Expect at least both call sites.
  EXPECT_GE(j["items"].size(), 2);
}

TEST(LspDocumentHighlightTest, HighlightsSymbolOccurrencesAndDefinition)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
var MyTarget: string
Tree Main() {
  MyAction(target: MyTarget)
  MyAction(target: MyTarget)
}
)";

  ws.set_document(uri, src);

  const auto use_pos = src.find("MyTarget)");
  ASSERT_NE(use_pos, std::string::npos);

  const auto j = json::parse(ws.document_highlights_json(uri, static_cast<uint32_t>(use_pos + 1)));

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  // Two uses + (best-effort) definition.
  EXPECT_GE(j["items"].size(), 2);
}
