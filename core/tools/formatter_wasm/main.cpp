// WASM bindings for BT-DSL core
// Provides parseToAstJson for the Prettier plugin
//
#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
#endif

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/ast/json_visitor.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"
#include "bt_dsl/syntax/lexer.hpp"
#include "bt_dsl/syntax/parser.hpp"
#include "bt_dsl/syntax/token.hpp"

namespace bt_dsl::wasm
{
namespace
{
using bt_dsl::syntax::Lexer;
using bt_dsl::syntax::Parser;
using bt_dsl::syntax::Token;
using bt_dsl::syntax::TokenKind;
using nlohmann::json;

uint32_t begin_off(SourceRange r) { return r.get_begin().get_offset(); }
uint32_t end_off(SourceRange r) { return r.get_end().get_offset(); }

json j_range(SourceRange r)
{
  if (r.is_invalid()) {
    return json{{"start", nullptr}, {"end", nullptr}};
  }
  return json{{"start", begin_off(r)}, {"end", end_off(r)}};
}

std::string_view slice(std::string_view src, SourceRange r)
{
  if (r.is_invalid()) return {};
  const uint32_t s = std::min<uint32_t>(begin_off(r), static_cast<uint32_t>(src.size()));
  const uint32_t e = std::min<uint32_t>(end_off(r), static_cast<uint32_t>(src.size()));
  if (e <= s) return {};
  return src.substr(s, e - s);
}
}  // namespace

/// Parse source text and return JSON with AST, comments, and diagnostics.
/// This is the main entry point for the Prettier plugin.
std::string parse_to_ast_json(const std::string & source_text)
{
  SourceManager source{std::string{source_text}};
  const std::string_view src = source.get_source();

  // Lex (keep comments so JS can preserve them).
  Lexer lex{src};
  const auto all_tokens = lex.lex_all();

  json comments = json::array();
  for (const auto & t : all_tokens) {
    if (
      t.kind == TokenKind::DocLine || t.kind == TokenKind::DocModule ||
      t.kind == TokenKind::LineComment || t.kind == TokenKind::BlockComment) {
      comments.push_back(json{
        {"kind", std::string(bt_dsl::syntax::to_string(t.kind))},
        {"range", j_range(t.range)},
        {"text", std::string(slice(src, t.range))}});
    }
  }

  // Filter out non-doc comments for the parser.
  std::vector<Token> parser_tokens;
  parser_tokens.reserve(all_tokens.size());
  for (const auto & t : all_tokens) {
    if (t.kind == TokenKind::LineComment) continue;
    if (t.kind == TokenKind::BlockComment) continue;
    parser_tokens.push_back(t);
  }

  AstContext ast;
  DiagnosticBag diags;
  Parser parser(ast, source, diags, std::move(parser_tokens));
  Program * program = parser.parse_program();

  // Use the AST JSON serialization
  json program_json = to_json(program);

  json diagnostics = json::array();
  for (const auto & d : diags.all()) {
    std::string sev = "Error";
    switch (d.severity) {
      case Severity::Error:
        sev = "Error";
        break;
      case Severity::Warning:
        sev = "Warning";
        break;
      case Severity::Info:
        sev = "Info";
        break;
      case Severity::Hint:
        sev = "Hint";
        break;
    }

    diagnostics.push_back(json{
      {"severity", sev}, {"range", j_range(d.range)}, {"message", d.message}, {"code", d.code}});
  }

  // Merge: use AST JSON as base, add comments and diagnostics
  program_json["btDslComments"] = comments;
  program_json["diagnostics"] = diagnostics;

  return program_json.dump();
}

}  // namespace bt_dsl::wasm

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_BINDINGS(formatter_wasm)
{
  emscripten::function("parseToAstJson", &bt_dsl::wasm::parse_to_ast_json);
}
#endif
