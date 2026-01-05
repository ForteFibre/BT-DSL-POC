// bt_dsl/syntax/frontend.hpp - High-level parse pipeline entry point
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

struct ParseOutput
{
  FileId file_id = FileId::invalid();
  Program * program = nullptr;
};

// Parse pipeline:
// source -> lexer (token stream) -> recursive-descent parser (AST) -> diagnostics
[[nodiscard]] ParseOutput parse_source(
  SourceRegistry & sources, const std::filesystem::path & path, std::string source_text,
  AstContext & ast, DiagnosticBag & diags);

}  // namespace bt_dsl
