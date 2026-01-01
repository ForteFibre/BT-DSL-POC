#pragma once

#include <array>
#include <string_view>

namespace bt_dsl::syntax
{

// NOTE: These are language-spec surface keywords. Keep them aligned with
// docs/reference/grammar.md and tree-sitter-bt-dsl/grammar.js.

inline constexpr std::array<std::string_view, 6> k_top_level_keywords = {
  "import", "extern", "type", "var", "const", "tree",
};

inline constexpr std::array<std::string_view, 4> k_port_directions = {
  "in",
  "out",
  "ref",
  "mut",
};

inline constexpr std::array<std::string_view, 5> k_precondition_kinds = {
  "success_if", "failure_if", "skip_if", "run_while", "guard",
};

}  // namespace bt_dsl::syntax
