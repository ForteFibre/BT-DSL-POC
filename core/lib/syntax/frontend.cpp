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

  bt_dsl::syntax::Parser parser(unit->ast, unit->source, unit->diags, std::move(tokens));
  unit->program = parser.parse_program();
  return unit;
}

}  // namespace bt_dsl
