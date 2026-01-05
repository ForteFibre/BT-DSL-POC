// bt_dsl/syntax/frontend.cpp - High-level parse pipeline
#include "bt_dsl/syntax/frontend.hpp"

#include "bt_dsl/syntax/lexer.hpp"
#include "bt_dsl/syntax/parser.hpp"

namespace bt_dsl
{

ParseOutput parse_source(
  SourceRegistry & sources, const std::filesystem::path & path, std::string source_text,
  AstContext & ast, DiagnosticBag & diags)
{
  ParseOutput out;
  out.file_id = sources.register_file(path, "");
  sources.update_content(out.file_id, std::move(source_text));
  const SourceFile * source = sources.get_file(out.file_id);
  if (!source) {
    diags.report_error(
      SourceRange{out.file_id, 0, 0}, "internal error: failed to register source file");
    return out;
  }

  bt_dsl::syntax::Lexer lex(out.file_id, source->content());
  auto tokens = lex.lex_all();

  // The lexer can emit non-doc comment tokens so tools (e.g. formatter) can
  // preserve user text. The recursive-descent parser intentionally ignores
  // non-doc comments, so filter them out here.
  std::vector<bt_dsl::syntax::Token> parser_tokens;
  parser_tokens.reserve(tokens.size());
  for (const auto & t : tokens) {
    if (t.kind == bt_dsl::syntax::TokenKind::LineComment) continue;
    if (t.kind == bt_dsl::syntax::TokenKind::BlockComment) continue;
    parser_tokens.push_back(t);
  }

  bt_dsl::syntax::Parser parser(ast, out.file_id, *source, diags, std::move(parser_tokens));
  out.program = parser.parse_program();
  return out;
}

}  // namespace bt_dsl
