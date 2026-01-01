// test_lsp_diagnostics.cpp - Serverless LSP diagnostics tests

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "bt_dsl/lsp/lsp.hpp"

using json = nlohmann::json;

TEST(LspDiagnosticsTest, IncludesParseAndSemanticDiagnostics)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";

  // Contains a semantic error (unknown variable) and also remains parseable.
  const std::string src = R"(
extern action MyAction(in target: string<256>);
tree Main() {
  MyAction(target: UndefinedVar);
}
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  bool saw_unknown_var = false;
  for (const auto & item : j["items"]) {
    if (!item.contains("message")) {
      continue;
    }
    const std::string msg = item["message"].get<std::string>();
    if (msg.find("Unknown variable") != std::string::npos) {
      saw_unknown_var = true;
      break;
    }
  }

  EXPECT_TRUE(saw_unknown_var) << "Expected semantic diagnostic 'Unknown variable'";
}

TEST(LspDiagnosticsTest, IncludesParserErrors)
{
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
  for (const auto & item : j["items"]) {
    if (item.contains("source") && item["source"] == "parser") {
      saw_parser = true;
      break;
    }
  }

  EXPECT_TRUE(saw_parser) << "Expected at least one parser diagnostic";
}

TEST(LspDiagnosticsTest, SuppressesSemanticDiagnosticsWhenParseErrorsExist)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///broken_with_semantic.bt";

  // Contains a semantic issue (UndefinedVar) but also has a syntax error.
  // We expect parser diagnostics, and we should *not* emit analyzer diagnostics
  // for an incomplete/recovered AST.
  const std::string src = R"(
extern action MyAction(in target: int);
tree Main() {
  MyAction(target: UndefinedVar
}
  )";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  ASSERT_TRUE(j.contains("items"));
  ASSERT_TRUE(j["items"].is_array());

  bool saw_parser = false;
  bool saw_analyzer = false;
  for (const auto & item : j["items"]) {
    if (item.contains("source") && item["source"] == "parser") {
      saw_parser = true;
    }
    if (item.contains("source") && item["source"] == "analyzer") {
      saw_analyzer = true;
    }
  }

  EXPECT_TRUE(saw_parser) << "Expected parser diagnostics";
  EXPECT_FALSE(saw_analyzer) << "Did not expect analyzer diagnostics when parse errors exist";
}

TEST(LspDiagnosticsTest, ErrorsOnNonRelativeImports)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
import "SomeLib.bt"
tree Main() { Sequence {} }
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  bool saw_import_error = false;
  for (const auto & item : j["items"]) {
    if (item.contains("source") && item["source"] == "import" && item.contains("message")) {
      const std::string msg = item["message"].get<std::string>();
      if (msg.find("Cannot resolve package import") != std::string::npos) {
        saw_import_error = true;
        break;
      }
    }
  }

  EXPECT_TRUE(saw_import_error)
    << "Expected a package-import resolution error for a non-relative import when the host does "
       "not provide the package document";
}

TEST(LspDiagnosticsTest, ErrorsOnAbsoluteImportPath)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
import "/abs.bt"
tree Main() { Sequence {} }
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  bool saw_abs_error = false;
  for (const auto & item : j["items"]) {
    if (item.contains("source") && item["source"] == "import" && item.contains("message")) {
      const std::string msg = item["message"].get<std::string>();
      if (msg.find("Absolute import paths") != std::string::npos) {
        saw_abs_error = true;
        break;
      }
    }
  }

  EXPECT_TRUE(saw_abs_error) << "Expected an absolute-import-path error";
}

TEST(LspDiagnosticsTest, ErrorsOnImportMissingExtension)
{
  bt_dsl::lsp::Workspace ws;

  const std::string uri = "file:///main.bt";
  const std::string src = R"(
import "./dep"
tree Main() { Sequence {} }
)";

  ws.set_document(uri, src);

  const auto diag_json = ws.diagnostics_json(uri);
  const auto j = json::parse(diag_json);

  bool saw_ext_error = false;
  for (const auto & item : j["items"]) {
    if (item.contains("source") && item["source"] == "import" && item.contains("message")) {
      const std::string msg = item["message"].get<std::string>();
      if (msg.find("must include an extension") != std::string::npos) {
        saw_ext_error = true;
        break;
      }
    }
  }

  EXPECT_TRUE(saw_ext_error) << "Expected a missing-extension error";
}
