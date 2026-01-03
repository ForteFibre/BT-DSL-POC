#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bt_dsl::lsp
{

enum class CompletionContextKind {
  None,
  TopLevelKeywords,
  TreeBody,
  NodeName,
  PreconditionKind,
  ArgStart,
  ArgName,
  ArgValue,
  BlackboardRefName,
  PortDirection,
  ImportPath,
};

struct CompletionContext
{
  CompletionContextKind kind = CompletionContextKind::None;

  // Best-effort enrichment:
  std::optional<std::string> tree_name;

  // When inside a property_block, the callable (node) that owns it.
  std::optional<std::string> callable_name;
};

// Classify completion context at a byte offset using Tree-sitter CST + Query.
std::optional<CompletionContext> classify_completion_context(
  std::string_view text, uint32_t byte_offset);

}  // namespace bt_dsl::lsp
