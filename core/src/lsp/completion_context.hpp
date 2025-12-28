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
  DecoratorName,
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

  // Name of the node_stmt where the cursor is (if any).
  std::optional<std::string> node_stmt_name;

  // When inside a property_block, the callable (node/decorator) that owns it.
  std::optional<std::string> callable_name;
  bool callable_is_decorator = false;
};

// Classify completion context at a byte offset using Tree-sitter CST + Query.
//
// This function intentionally does NOT use text/brace/AST fallbacks.
std::optional<CompletionContext> classify_completion_context(
  std::string_view text, uint32_t byte_offset);

}  // namespace bt_dsl::lsp
