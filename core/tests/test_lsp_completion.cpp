// test_lsp_completion.cpp - Serverless LSP completion tests

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "bt_dsl/lsp.hpp"

using json = nlohmann::json;

static bool has_label(const json & items, const std::string & label)
{
  for (const auto & it : items) {
    if (it.contains("label") && it["label"].get<std::string>() == label) {
      return true;
    }
  }
  return false;
}

static json completion_items(
  bt_dsl::lsp::Workspace & ws, const std::string & uri, uint32_t pos,
  const std::vector<std::string> & imports = {})
{
  const auto j = imports.empty() ? json::parse(ws.completion_json(uri, pos))
                                 : json::parse(ws.completion_json(uri, pos, imports));
  EXPECT_TRUE(j.contains("items"));
  EXPECT_TRUE(j["items"].is_array());
  return j["items"];
}

TEST(LspCompletionTest, SuggestsDeclaredNodes)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
Tree Main() {
  
}
)";

  ws.set_document(uri, src);

  // Cursor at the empty line inside Main
  const auto pos = static_cast<uint32_t>(src.find("\n  \n") + 3);
  const auto items = completion_items(ws, uri, pos);
  EXPECT_TRUE(has_label(items, "MyAction"));
}

TEST(LspCompletionTest, SuggestsPortsInsideArgumentList)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string, out result: bool)
var MyTarget: string
Tree Main() {
  MyAction()
}
)";

  ws.set_document(uri, src);

  const auto call_pos = src.find("MyAction()") + std::string("MyAction(").size();
  const auto items = completion_items(ws, uri, static_cast<uint32_t>(call_pos));

  // Port names
  EXPECT_TRUE(has_label(items, "target"));
  EXPECT_TRUE(has_label(items, "result"));
}

TEST(LspCompletionTest, SuggestsPortsAtStartOfExistingNamedArg)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string decl_uri = "file:///test-nodes.bt";

  // Mirror the VS Code e2e fixture shape:
  // - import another file that declares a node with ports
  // - call it with existing named args
  // - request completion at the byte position immediately after '('
  //   (i.e. at the start of the existing arg name)
  ws.set_document(decl_uri, R"(
declare Action TestAction(in pos: int, out found: bool)
)");

  const std::string src = R"(
//! Fixture
import "./test-nodes.bt"

Tree Main() {
  Sequence {
    TestAction(pos: 1, found: out Foo)
  }
}
 )";

  ws.set_document(uri, src);

  const auto call_pos = src.find("TestAction(") + std::string("TestAction(").size();
  ASSERT_NE(call_pos, std::string::npos);

  const auto items = completion_items(ws, uri, static_cast<uint32_t>(call_pos), {decl_uri});

  EXPECT_TRUE(has_label(items, "pos"));
  EXPECT_TRUE(has_label(items, "found"));
}

TEST(LspCompletionTest, SuggestsPortsInArgsWithUtf8CommentBeforeTree)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string decl_uri = "file:///test-nodes.bt";

  ws.set_document(decl_uri, R"(
declare Action TestAction(in pos: int, out found: bool)
)");

  // NOTE: This mirrors vscode/test/fixture-workspace/main.bt which contains
  // non-ASCII characters in comments to catch UTF-8/UTF-16 offset issues.
  const std::string src = R"(
//! Fixture for VS Code extension e2e tests
import "./test-nodes.bt"

// 日本語🙂 を入れて UTF-8/UTF-16 変換のズレを検出しやすくする
var Ammo: int
var Found: bool

/// main tree
Tree Main() {
  @TestDeco(enabled: true)
  Sequence {
    TestAction(pos: 1, found: out Found)
  }
}
 )";

  ws.set_document(uri, src);

  const auto call_start = src.find("TestAction(");
  ASSERT_NE(call_start, std::string::npos);

  // Place cursor inside the first arg name ("pos").
  const auto pos = call_start + std::string("TestAction(").size() + 1;
  const auto items = completion_items(ws, uri, static_cast<uint32_t>(pos), {decl_uri});

  EXPECT_TRUE(has_label(items, "pos"));
  EXPECT_TRUE(has_label(items, "found"));
}

TEST(LspCompletionTest, DoesNotReplacePreviousWordWhenCompletingAtWhitespace)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string)
var MyTarget: string
Tree Main() {
  MyAction(target: )
}
)";

  ws.set_document(uri, src);

  // Cursor right after "target: " (at whitespace), before any identifier.
  const auto anchor = src.find("MyAction(target: ");
  ASSERT_NE(anchor, std::string::npos);
  const auto pos = anchor + std::string("MyAction(target: ").size();

  const auto items = completion_items(ws, uri, static_cast<uint32_t>(pos));
  ASSERT_FALSE(items.empty());

  const auto & rr = items[0]["replaceRange"];
  ASSERT_TRUE(rr.contains("startByte"));
  ASSERT_TRUE(rr.contains("endByte"));
  EXPECT_EQ(rr["startByte"].get<uint32_t>(), static_cast<uint32_t>(pos));
  EXPECT_EQ(rr["endByte"].get<uint32_t>(), static_cast<uint32_t>(pos));
}

TEST(LspCompletionTest, ArgValueSuggestsVarsAndDirectionsButNotPorts)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string, out result: bool)
var MyTarget: string
Tree Main() {
  MyAction(target: )
}
)";

  ws.set_document(uri, src);

  // Cursor right after "target: " (value position).
  const auto anchor = src.find("MyAction(target: ");
  ASSERT_NE(anchor, std::string::npos);
  const auto pos = anchor + std::string("MyAction(target: ").size();

  const auto items = completion_items(ws, uri, static_cast<uint32_t>(pos));

  // Value context: variables + direction keywords.
  EXPECT_TRUE(has_label(items, "MyTarget"));
  EXPECT_TRUE(has_label(items, "in"));
  EXPECT_TRUE(has_label(items, "out"));
  EXPECT_TRUE(has_label(items, "ref"));

  // Ports (argument keys) should NOT be suggested in value position.
  EXPECT_FALSE(has_label(items, "target"));
  EXPECT_FALSE(has_label(items, "result"));
}

TEST(LspCompletionTest, ArgNameSuggestsPortsButNotVarsOrDirections)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
declare Action MyAction(in target: string, out result: bool)
var MyTarget: string
Tree Main() {
  MyAction(ta: )
}
)";

  ws.set_document(uri, src);

  // Cursor inside the argument name identifier "ta".
  const auto anchor = src.find("MyAction(ta");
  ASSERT_NE(anchor, std::string::npos);
  const auto pos = anchor + std::string("MyAction(ta").size();

  const auto items = completion_items(ws, uri, static_cast<uint32_t>(pos));

  // Name context: ports only.
  EXPECT_TRUE(has_label(items, "target"));
  EXPECT_TRUE(has_label(items, "result"));
  EXPECT_FALSE(has_label(items, "MyTarget"));
  EXPECT_FALSE(has_label(items, "in"));
  EXPECT_FALSE(has_label(items, "out"));
  EXPECT_FALSE(has_label(items, "ref"));
}

TEST(LspCompletionTest, DecoratorsAreSuggestedOnlyAfterAtSign)
{
  bt_dsl::lsp::Workspace ws;

  const std::string main_uri = "file:///main.bt";
  const std::string std_uri = "file:///stdlib.bt";

  ws.set_document(std_uri, "declare Decorator Repeat()\n");

  const std::string src = R"(
import "./stdlib.bt"
Tree Main() {
  @
  Sequence {
    
  }
}
)";
  ws.set_document(main_uri, src);

  // After '@' -> should suggest Repeat.
  const auto at_pos = src.find("@\n") + 1;
  ASSERT_NE(at_pos, std::string::npos);
  const auto items_at = completion_items(ws, main_uri, static_cast<uint32_t>(at_pos), {std_uri});
  EXPECT_TRUE(has_label(items_at, "Repeat"));

  // Inside the tree body at blank line -> should NOT suggest decorators.
  const auto blank_pos = static_cast<uint32_t>(src.rfind("\n    \n") + 6);
  ASSERT_NE(blank_pos, static_cast<uint32_t>(std::string::npos));
  const auto items_blank = completion_items(ws, main_uri, blank_pos, {std_uri});
  EXPECT_FALSE(has_label(items_blank, "Repeat"));
}

TEST(LspCompletionTest, TopLevelSuggestsKeywordsButNotNodes)
{
  bt_dsl::lsp::Workspace ws;

  const std::string main_uri = "file:///main.bt";
  const std::string std_uri = "file:///stdlib.bt";
  ws.set_document(std_uri, "declare Control Sequence()\n");

  const std::string src = R"(
import "./stdlib.bt"

)";
  ws.set_document(main_uri, src);

  const auto pos = static_cast<uint32_t>(src.size());
  const auto items = completion_items(ws, main_uri, pos, {std_uri});
  EXPECT_TRUE(has_label(items, "Tree"));
  EXPECT_TRUE(has_label(items, "var"));
  EXPECT_TRUE(has_label(items, "import"));
  EXPECT_FALSE(has_label(items, "Sequence"));
}
