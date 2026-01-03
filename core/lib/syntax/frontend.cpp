// bt_dsl/syntax/frontend.cpp - High-level parse pipeline
#include "bt_dsl/syntax/frontend.hpp"

#include "bt_dsl/syntax/ast_builder.hpp"
#include "bt_dsl/syntax/ts_ll.hpp"

namespace bt_dsl
{

namespace
{

void collect_syntax_diagnostics(const ts_ll::Node n, DiagnosticBag & diags, size_t & count)
{
  constexpr size_t max_syntax_diags = 64;
  if (count >= max_syntax_diags) return;
  if (n.is_null()) return;

  if (n.is_error()) {
    diags.error(n.range(), "Syntax error");
    ++count;
  } else if (n.is_missing()) {
    diags.error(n.range(), "Missing token");
    ++count;
  }

  if (count >= max_syntax_diags) return;

  for (uint32_t i = 0; i < n.child_count(); ++i) {
    collect_syntax_diagnostics(n.child(i), diags, count);
    if (count >= max_syntax_diags) return;
  }
}

}  // namespace

std::unique_ptr<ParsedUnit> parse_source(std::string source_text)
{
  auto unit = std::make_unique<ParsedUnit>();
  unit->source = SourceManager(std::move(source_text));

  const ts_ll::Parser parser;
  const ts_ll::Tree tree(parser.parse_string(unit->source.get_source()));

  if (tree.is_null()) {
    unit->diags.error({}, "tree-sitter parse failed (null tree)");
    unit->program = unit->ast.create<Program>();
    return unit;
  }

  const ts_ll::Node root = tree.root_node();

  // Tree-sitter can recover from syntax errors and still return a non-null tree.
  // The reference tests require that any recovery (ERROR/MISSING nodes) surfaces
  // as diagnostics.
  if (root.has_error()) {
    size_t count = 0;
    collect_syntax_diagnostics(root, unit->diags, count);
  }

  AstBuilder builder(unit->ast, unit->source, unit->diags);
  unit->program = builder.build_program(root);
  return unit;
}

}  // namespace bt_dsl
