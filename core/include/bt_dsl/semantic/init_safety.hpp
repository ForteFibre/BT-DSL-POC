// bt_dsl/init_safety.hpp - Blackboard initialization safety analysis
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/core/diagnostic.hpp"
#include "bt_dsl/semantic/node_registry.hpp"

namespace bt_dsl
{

/**
 * Summary of a tree's initialization effects.
 *
 * This is used to analyze SubTree calls without inlining tree bodies.
 *
 * - requires_globals: globals that must be Init at call time (the tree reads them as in/ref/mut)
 * - writes_globals_on_success: globals guaranteed Init after the tree returns success
 * - writes_out_params_on_success: out-params guaranteed Init after the tree returns success
 */
struct InitSafetySummary
{
  std::unordered_set<std::string> requires_globals;
  std::unordered_set<std::string> writes_globals_on_success;
  std::unordered_set<std::string> writes_out_params_on_success;
};

/**
 * Run initialization safety analysis.
 *
 * This will:
 * - infer summaries for all trees (main + direct imports), assuming unknown globals unless
 *   explicitly initialized by a global initializer;
 * - validate the entry tree (the first tree in the main program, matching XML codegen behavior)
 *   with concrete global initialization state.
 */
void run_initialization_safety(
  const Program & program, const std::vector<const Program *> & imported_programs,
  const NodeRegistry & nodes, DiagnosticBag & diagnostics);

}  // namespace bt_dsl
