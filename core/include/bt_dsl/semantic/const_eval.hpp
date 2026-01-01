// bt_dsl/const_eval.hpp - Compile-time constant evaluation helpers
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/core/diagnostic.hpp"
#include "bt_dsl/core/symbol_table.hpp"
#include "bt_dsl/semantic/type_system.hpp"

namespace bt_dsl
{

struct ConstArrayValue;
using ConstArrayPtr = std::shared_ptr<ConstArrayValue>;

// NOTE: This is an internal semantic-eval representation used to enforce
// reference-spec const_expr requirements (docs/reference/declarations-and-scopes.md 4.3.4).
using ConstValue = std::variant<int64_t, double, bool, std::string, std::monostate, ConstArrayPtr>;

struct ConstArrayValue
{
  // If repeat_init is used, elements is empty.
  std::vector<ConstValue> elements;

  // repeat_init := value ; count
  std::optional<ConstValue> repeat_value;
  uint64_t repeat_count = 0;
};

struct ConstEvalContext
{
  // Only global consts (top-level) participate in forward-reference and cycle evaluation.
  std::unordered_map<std::string, const ConstDeclStmt *> global_consts;

  // Memoized integer results for globals.
  std::unordered_map<std::string, std::optional<int64_t>> memo_i64;
  // Memoized float results for globals.
  std::unordered_map<std::string, std::optional<double>> memo_f64;
  // Memoized fully-evaluated const values for globals.
  std::unordered_map<std::string, std::optional<ConstValue>> memo_value;

  // DFS stack to detect cycles during evaluation.
  std::unordered_set<std::string> in_stack;
  // Local const recursion guard (best-effort).
  std::unordered_set<const void *> local_in_stack;
};

// Build a map of visible global const declarations.
// Visibility follows value-space rules: local (all) + direct imports (public-only).
void build_visible_global_const_map(
  const Program & program, const std::vector<const Program *> & imported_programs,
  std::unordered_map<std::string, const ConstDeclStmt *> & out);

bool const_values_equal(const ConstValue & a, const ConstValue & b);

std::optional<ConstValue> eval_global_const_value(
  std::string_view name, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, const TypeEnvironment * type_env = nullptr);

std::optional<ConstValue> eval_const_value(
  const Expression & expr, const Scope * scope, const SymbolTable & symbols, ConstEvalContext & ctx,
  DiagnosticBag & diagnostics, const TypeEnvironment * type_env = nullptr,
  std::optional<std::string_view> current_const_name = std::nullopt);

}  // namespace bt_dsl
