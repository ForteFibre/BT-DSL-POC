// bt_dsl/lsp.hpp - LSP-like language service APIs (serverless)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bt_dsl::lsp
{

struct ByteRange
{
  uint32_t startByte = 0;
  uint32_t endByte = 0;
};

/**
 * Serverless language service for BT-DSL.
 *
 * This provides LSP-equivalent features
 * (diagnostics/completion/hover/definition/outline) without implementing an LSP
 * server. It is intended to be called from a host (e.g. VS Code) and can be
 * exposed via WASM.
 *
 * All positions are expressed in UTF-8 byte offsets to avoid UTF-16/Unicode
 * ambiguities at the WASM boundary. The host is responsible for converting byte
 * offsets to editor positions.
 */
class Workspace
{
public:
  Workspace();
  ~Workspace();

  Workspace(const Workspace &) = delete;
  Workspace & operator=(const Workspace &) = delete;

  Workspace(Workspace && other) noexcept;
  Workspace & operator=(Workspace && other) noexcept;

  void set_document(std::string uri, std::string text);
  void remove_document(std::string_view uri);
  [[nodiscard]] bool has_document(std::string_view uri) const;

  // Diagnostics (parse + semantic)
  std::string diagnostics_json(std::string_view uri);
  std::string diagnostics_json(
    std::string_view uri, const std::vector<std::string> & imported_uris);

  // Import resolution (host-driven loading)
  //
  // Resolves relative import specs against each document's URI and returns a
  // JSON payload describing the transitive import closure that can be
  // discovered from documents currently present in the workspace.
  //
  // The host may call this repeatedly: if new documents are added via
  // set_document(), the returned closure can expand.
  //
  // If stdlib_uri is non-empty, it will be included as an implicit import.
  std::string resolve_imports_json(std::string_view uri, std::string_view stdlib_uri = {});

  // Completion
  std::string completion_json(std::string_view uri, uint32_t byte_offset);
  std::string completion_json(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris,
    std::string_view trigger = {});

  // Hover
  std::string hover_json(std::string_view uri, uint32_t byte_offset);
  std::string hover_json(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris);

  // Go-to-definition
  std::string definition_json(std::string_view uri, uint32_t byte_offset);
  std::string definition_json(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris);

  // Document symbols (outline)
  std::string document_symbols_json(std::string_view uri);

  // Document highlights (like LSP textDocument/documentHighlight)
  std::string document_highlights_json(std::string_view uri, uint32_t byte_offset);
  std::string document_highlights_json(
    std::string_view uri, uint32_t byte_offset, const std::vector<std::string> & imported_uris);

  // Semantic tokens (like LSP textDocument/semanticTokens/full)
  //
  // The returned JSON contains UTF-8 byte ranges and semantic classifications
  // derived from the analyzer/node registry.
  std::string semantic_tokens_json(std::string_view uri);
  std::string semantic_tokens_json(
    std::string_view uri, const std::vector<std::string> & imported_uris);

private:
  struct Impl;
  Impl * impl_;
};

}  // namespace bt_dsl::lsp
