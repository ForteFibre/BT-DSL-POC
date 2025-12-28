// test_lsp_diagnostics.cpp - Serverless LSP diagnostics tests

#include "bt_dsl/lsp.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST(LspDiagnosticsTest, IncludesParseAndSemanticDiagnostics) {
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";

  // Contains a semantic error (unknown variable) and also remains parseable.
  const std::string src = R"(
declare Action MyAction(in target: string)
Tree Main() {
  MyAction(target: UndefinedVar)
}
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  bool saw_unknown_var = false;
  for (const auto &item : j["items"]) {
    if (!item.contains("message")) {
      continue;
    }
    const std::string msg = item["message"].get<std::string>();
    if (msg.find("Unknown variable") != std::string::npos) {
      saw_unknown_var = true;
      break;
    }
  }

  EXPECT_TRUE(saw_unknown_var)
      << "Expected semantic diagnostic 'Unknown variable'";
}

TEST(LspDiagnosticsTest, IncludesParserErrors) {
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///broken.bt";

  // Intentionally broken syntax (missing closing brace)
  const std::string src = "Tree Main() {\n  Sequence {\n";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  bool saw_parser = false;
  for (const auto &item : j["items"]) {
    if (item.contains("source") && item["source"] == "parser") {
      saw_parser = true;
      break;
    }
  }

  EXPECT_TRUE(saw_parser) << "Expected at least one parser diagnostic";
}

TEST(LspDiagnosticsTest, ErrorsOnNonRelativeImports) {
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
import "SomeLib.bt"
Tree Main() { Sequence {} }
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  bool saw_import_error = false;
  for (const auto &item : j["items"]) {
    if (item.contains("source") && item["source"] == "import" &&
        item.contains("message")) {
      const std::string msg = item["message"].get<std::string>();
      if (msg.find("Only relative imports") != std::string::npos) {
        saw_import_error = true;
        break;
      }
    }
  }

  EXPECT_TRUE(saw_import_error)
      << "Expected an import policy error for non-relative imports";
}
