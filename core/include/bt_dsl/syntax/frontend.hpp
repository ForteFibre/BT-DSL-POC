// bt_dsl/syntax/frontend.hpp - High-level parse pipeline entry point
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

struct ParsedUnit
{
  SourceManager source;
  AstContext ast;
  DiagnosticBag diags;
  Program * program = nullptr;
};

// Parse pipeline:
// source -> lexer (token stream) -> recursive-descent parser (AST) -> diagnostics
[[nodiscard]] std::unique_ptr<ParsedUnit> parse_source(std::string source_text);

}  // namespace bt_dsl
