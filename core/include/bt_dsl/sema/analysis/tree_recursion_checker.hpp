// bt_dsl/sema/analysis/tree_recursion_checker.hpp - Tree call graph + recursion (cycle) detection
//
// Spec §6.3.1: Direct or indirect recursive tree calls are forbidden.
//
// This pass runs after name resolution (NameResolver / ImportAwareResolver),
// because it relies on NodeStmt::resolvedNode to identify tree call sites.
//
#pragma once

#include <cstddef>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"

namespace bt_dsl
{

/**
 * Detect recursion (cycles) in the tree call graph.
 *
 * The checker builds a call graph between TreeDecl nodes by scanning NodeStmt
 * call sites and following those whose resolved node is a TreeDecl.
 */
class TreeRecursionChecker
{
public:
  explicit TreeRecursionChecker(DiagnosticBag * diags = nullptr) : diags_(diags) {}

  // ===========================================================================
  // Entry Points
  // ===========================================================================

  /**
   * Check recursion within a single Program.
   *
   * This detects recursion among trees declared in the same AST program.
   */
  bool check(const Program & program);

  /**
   * Check recursion across a module graph, starting from an entry module.
   *
   * The traversal roots are the trees defined in the entry module.
   */
  bool check(const ModuleGraph & graph, const ModuleInfo & entry);

  // ===========================================================================
  // Error State
  // ===========================================================================

  [[nodiscard]] bool has_errors() const noexcept { return hasErrors_; }
  [[nodiscard]] size_t error_count() const noexcept { return errorCount_; }

  // Internal (exposed for helper routines in the implementation unit).
  void report_error(SourceRange range, std::string_view message);

private:
  DiagnosticBag * diags_ = nullptr;
  bool hasErrors_ = false;
  size_t errorCount_ = 0;
};

}  // namespace bt_dsl
