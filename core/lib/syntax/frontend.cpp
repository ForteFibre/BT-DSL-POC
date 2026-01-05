// bt_dsl/syntax/frontend.cpp - High-level parse pipeline
#include "bt_dsl/syntax/frontend.hpp"

#include "bt_dsl/syntax/lexer.hpp"
#include "bt_dsl/syntax/parser.hpp"

namespace bt_dsl
{

std::unique_ptr<ParsedUnit> parse_source(std::string source_text)
{
  auto unit = std::make_unique<ParsedUnit>();
  unit->source = SourceManager(std::move(source_text));

  bt_dsl::syntax::Lexer lex(unit->source.get_source());
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

  bt_dsl::syntax::Parser parser(unit->ast, unit->source, unit->diags, std::move(parser_tokens));
  unit->program = parser.parse_program();
  return unit;
}

}  // namespace bt_dsl
