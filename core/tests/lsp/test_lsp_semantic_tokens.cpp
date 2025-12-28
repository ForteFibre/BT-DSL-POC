// test_lsp_semantic_tokens.cpp - Serverless LSP semantic tokens tests

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "bt_dsl/lsp/lsp.hpp"

using json = nlohmann::json;

static std::string slice_by_bytes(const std::string & src, uint32_t start, uint32_t end)
{
  if (start > src.size()) {
    return "";
  }
  end = std::min<uint32_t>(end, static_cast<uint32_t>(src.size()));
  if (end <= start) {
    return "";
  }
  return src.substr(start, end - start);
}

TEST(LspSemanticTokensTest, ClassifiesSubTreeCallsAsClass)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
Tree Sub() {
  Sequence()
}

Tree Main() {
  Sub()
}
)";

  ws.set_document(uri, src);

  const auto j = json::parse(ws.semantic_tokens_json(uri));
  ASSERT_TRUE(j.contains("tokens"));
  ASSERT_TRUE(j["tokens"].is_array());

  bool found_sub_call = false;
  for (const auto & t : j["tokens"]) {
    if (!t.contains("type") || !t.contains("range")) {
      continue;
    }
    const std::string type = t["type"].get<std::string>();
    const uint32_t start = t["range"]["startByte"].get<uint32_t>();
    const uint32_t end = t["range"]["endByte"].get<uint32_t>();
    const std::string text = slice_by_bytes(src, start, end);

    if (text == "Sub" && type == "class") {
      found_sub_call = true;
      break;
    }
  }

  EXPECT_TRUE(found_sub_call);
}

TEST(LspSemanticTokensTest, MarksTreeDefinitionsAsDeclarations)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
Tree Main() {
  Sequence()
}
)";

  ws.set_document(uri, src);

  const auto j = json::parse(ws.semantic_tokens_json(uri));
  ASSERT_TRUE(j.contains("tokens"));

  bool found_decl = false;
  for (const auto & t : j["tokens"]) {
    if (!t.contains("type") || !t.contains("range")) {
      continue;
    }
    const uint32_t start = t["range"]["startByte"].get<uint32_t>();
    const uint32_t end = t["range"]["endByte"].get<uint32_t>();
    const std::string text = slice_by_bytes(src, start, end);
    if (text != "Main") {
      continue;
    }

    if (t["type"].get<std::string>() != "function") {
      continue;
    }
    if (t.contains("modifiers") && t["modifiers"].is_array()) {
      for (const auto & m : t["modifiers"]) {
        if (m.is_string() && m.get<std::string>() == "declaration") {
          found_decl = true;
          break;
        }
      }
    }
    if (found_decl) {
      break;
    }
  }

  EXPECT_TRUE(found_decl);
}

TEST(LspSemanticTokensTest, MarksGlobalVarsAsVariableDeclarations)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
var TargetPos: Vector3
Tree Main() {
  Sequence()
}
)";

  ws.set_document(uri, src);

  const auto j = json::parse(ws.semantic_tokens_json(uri));
  ASSERT_TRUE(j.contains("tokens"));
  ASSERT_TRUE(j["tokens"].is_array());

  bool found_global_decl = false;
  for (const auto & t : j["tokens"]) {
    if (!t.contains("type") || !t.contains("range")) {
      continue;
    }
    const uint32_t start = t["range"]["startByte"].get<uint32_t>();
    const uint32_t end = t["range"]["endByte"].get<uint32_t>();
    const std::string text = slice_by_bytes(src, start, end);
    if (text != "TargetPos") {
      continue;
    }
    if (t["type"].get<std::string>() != "variable") {
      continue;
    }
    if (t.contains("modifiers") && t["modifiers"].is_array()) {
      for (const auto & m : t["modifiers"]) {
        if (m.is_string() && m.get<std::string>() == "declaration") {
          found_global_decl = true;
          break;
        }
      }
    }
    if (found_global_decl) {
      break;
    }
  }

  EXPECT_TRUE(found_global_decl);
}
