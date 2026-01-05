// bt_dsl/test_support/parse_helpers.hpp - helpers for unit/integration tests
//
// These helpers provide a lightweight single-file parsing pipeline for tests.
// They intentionally keep ownership explicit (SourceRegistry + AstContext) while
// offering a convenient wrapper.
//
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"
#include "bt_dsl/syntax/frontend.hpp"

namespace bt_dsl::test_support
{

struct TestParseUnit
{
  SourceRegistry sources;
  FileId file_id = FileId::invalid();
  std::unique_ptr<AstContext> ast;
  DiagnosticBag diags;
  Program * program = nullptr;

  [[nodiscard]] const SourceFile * source_file() const noexcept
  {
    return sources.get_file(file_id);
  }

  [[nodiscard]] std::string_view slice(SourceRange r) const noexcept
  {
    return sources.get_slice(r);
  }

  [[nodiscard]] FullSourceRange full_range(SourceRange r) const noexcept
  {
    return sources.get_full_range(r);
  }
};

[[nodiscard]] inline TestParseUnit parse(
  std::string src, const std::filesystem::path & virtual_path = "<test>.bt")
{
  TestParseUnit out;
  out.ast = std::make_unique<AstContext>();

  // Note: parse_source registers/updates the file content in the registry.
  const ParseOutput parsed =
    parse_source(out.sources, virtual_path, std::move(src), *out.ast, out.diags);
  out.file_id = parsed.file_id;
  out.program = parsed.program;
  return out;
}

}  // namespace bt_dsl::test_support
