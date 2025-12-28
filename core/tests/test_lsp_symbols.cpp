// test_lsp_symbols.cpp - Serverless LSP document symbols tests

#include "bt_dsl/lsp.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static bool has_symbol(const json &symbols, const std::string &name,
                       const std::string &kind) {
  for (const auto &s : symbols) {
    if (s.contains("name") && s.contains("kind") && s["name"] == name &&
        s["kind"] == kind) {
      return true;
    }
  }
  return false;
}

TEST(LspDocumentSymbolsTest, ListsTreesDeclaresAndGlobals) {
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
var GlobalX: int
Tree Main() {
  Sequence {}
}
)";

  ws.set_document(uri, src);

  const auto j = json::parse(ws.document_symbols_json(uri));

  ASSERT_TRUE(j.contains("symbols"));
  ASSERT_TRUE(j["symbols"].is_array());

  EXPECT_TRUE(has_symbol(j["symbols"], "MyAction", "Declare"));
  EXPECT_TRUE(has_symbol(j["symbols"], "GlobalX", "GlobalVar"));
  EXPECT_TRUE(has_symbol(j["symbols"], "Main", "Tree"));
}
