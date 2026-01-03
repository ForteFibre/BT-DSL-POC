#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "bt_dsl/lsp/lsp.hpp"

static uint32_t find_byte_offset(const std::string & text, const std::string & needle)
{
  const auto pos = text.find(needle);
  EXPECT_NE(pos, std::string::npos) << "needle must exist: '" << needle << "'";
  if (pos == std::string::npos) return 0U;
  return static_cast<uint32_t>(pos);
}

static constexpr const char * k_uri = "file:///tmp/test_workspace_basic.bt";

static std::string basic_source()
{
  return "extern control Sequence();\n"
         "extern action DoWork(in x: int32, out y: int32);\n"
         "\n"
         "tree Main() {\n"
         "  var x: int32;\n"
         "  var y: int32;\n"
         "  Sequence {\n"
         "    x = 0;\n"
         "    DoWork(x: in x, y: out y);\n"
         "  }\n"
         "}\n";
}

TEST(LspWorkspace, Diagnostics)
{
  using json = nlohmann::json;

  const std::string src = basic_source();
  bt_dsl::lsp::Workspace ws;
  ws.set_document(k_uri, src);

  const auto j = json::parse(ws.diagnostics_json(k_uri));
  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  if (!j["items"].empty()) {
    for (const auto & it : j["items"]) {
      std::cerr << "diag: " << it.dump() << "\n";
    }
  }
  EXPECT_TRUE(j["items"].empty());
}

TEST(LspWorkspace, CompletionSuggestsNode)
{
  using json = nlohmann::json;

  bt_dsl::lsp::Workspace ws;
  const std::string src2 =
    "extern control Sequence();\n"
    "extern action Action(in x: int32);\n"
    "\n"
    "tree Main() {\n"
    "  Sequence {\n"
    "    \n"
    "  }\n"
    "}\n";
  const std::string uri2 = "file:///tmp/test_workspace_completion.bt";
  ws.set_document(uri2, src2);

  const uint32_t off = find_byte_offset(src2, "    \n") + 4U;  // blank line inside body
  const auto j = json::parse(ws.completion_json(uri2, off));

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  bool has_action = false;
  for (const auto & it : j["items"]) {
    if (it.contains("label") && it["label"].is_string() && it["label"] == "Action") {
      has_action = true;
    }
  }
  EXPECT_TRUE(has_action);
}

TEST(LspWorkspace, HoverShowsVariable)
{
  using json = nlohmann::json;

  const std::string src = basic_source();
  bt_dsl::lsp::Workspace ws;
  ws.set_document(k_uri, src);

  const auto pos = src.rfind("in x");
  ASSERT_NE(pos, std::string::npos);
  const uint32_t off = static_cast<uint32_t>(pos) + 3U;  // points to 'x'
  const auto j = json::parse(ws.hover_json(k_uri, off));

  ASSERT_TRUE(j.contains("contents"));
  if (!j["contents"].is_string()) {
    std::cerr << "hover json: " << j.dump() << "\n";
  }
  ASSERT_TRUE(j["contents"].is_string());
  const std::string md = j["contents"].get<std::string>();
  EXPECT_NE(md.find("**x**"), std::string::npos);
  EXPECT_NE(md.find("int32"), std::string::npos);
}

TEST(LspWorkspace, DefinitionPointsToDecl)
{
  using json = nlohmann::json;

  const std::string src = basic_source();
  bt_dsl::lsp::Workspace ws;
  ws.set_document(k_uri, src);

  const auto pos = src.rfind("in x");
  ASSERT_NE(pos, std::string::npos);
  const uint32_t off = static_cast<uint32_t>(pos) + 3U;  // points to 'x'
  const auto j = json::parse(ws.definition_json(k_uri, off));

  ASSERT_TRUE(j.contains("locations"));
  ASSERT_TRUE(j["locations"].is_array());
  ASSERT_EQ(j["locations"].size(), 1U);

  const auto & loc = j["locations"][0];
  EXPECT_EQ(loc["uri"].get<std::string>(), std::string(k_uri));

  const uint32_t decl_pos = find_byte_offset(src, "var x") + 4U;  // inside identifier
  const uint32_t start_byte = loc["range"]["startByte"].get<uint32_t>();
  const uint32_t end_byte = loc["range"]["endByte"].get<uint32_t>();
  EXPECT_TRUE(start_byte <= decl_pos && decl_pos < end_byte);
}

TEST(LspWorkspace, DocumentSymbols)
{
  using json = nlohmann::json;

  const std::string src = basic_source();
  bt_dsl::lsp::Workspace ws;
  ws.set_document(k_uri, src);

  const auto j = json::parse(ws.document_symbols_json(k_uri));
  ASSERT_TRUE(j.contains("symbols"));
  ASSERT_TRUE(j["symbols"].is_array());

  bool has_tree = false;
  bool has_extern = false;
  for (const auto & s : j["symbols"]) {
    if (!s.contains("name") || !s["name"].is_string()) continue;
    if (s["name"] == "Main") {
      has_tree = true;
    }
    if (s["name"] == "DoWork") {
      has_extern = true;
    }
  }
  EXPECT_TRUE(has_tree);
  EXPECT_TRUE(has_extern);
}
