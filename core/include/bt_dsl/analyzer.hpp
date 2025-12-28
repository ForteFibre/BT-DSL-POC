// bt_dsl/analyzer.hpp - Main semantic analyzer
#pragma once

#include <unordered_map>
#include <vector>

#include "ast.hpp"
#include "diagnostic.hpp"
#include "node_registry.hpp"
#include "symbol_table.hpp"
#include "type_system.hpp"

namespace bt_dsl
{

// ============================================================================
// Analysis Result
// ============================================================================

/**
 * Result of semantic analysis.
 * Contains all computed information and any diagnostics.
 */
struct AnalysisResult
{
  DiagnosticBag diagnostics;
  SymbolTable symbols;
  NodeRegistry nodes;
  std::unordered_map<std::string, TypeContext> tree_type_contexts;

  /**
   * Check if analysis produced any errors.
   */
  [[nodiscard]] bool has_errors() const { return diagnostics.has_errors(); }

  /**
   * Get the type context for a specific tree.
   * @return nullptr if tree not found
   */
  const TypeContext * get_tree_context(std::string_view tree_name) const;
};

// ============================================================================
// Analyzer
// ============================================================================

/**
 * Semantic analyzer for BT-DSL programs.
 *
 * Performs multiple passes:
 * 1. Collect declarations (symbols, nodes)
 * 2. Check for duplicates and conflicts
 * 3. Resolve types
 * 4. Validate semantics (directions, category constraints, etc.)
 */
class Analyzer
{
public:
  Analyzer();

  /**
   * Analyze a single program.
   * @param program The parsed program to analyze
   * @return Analysis result with diagnostics and computed information
   */
  static AnalysisResult analyze(const Program & program);

  /**
   * Analyze a program with imported programs.
   * Imported programs provide additional node/tree definitions.
   * @param program The main program to analyze
   * @param imported_programs Programs imported by the main program
   * @return Analysis result
   */
  static AnalysisResult analyze(
    const Program & program, const std::vector<const Program *> & imported_programs);

private:
  // Analysis passes
  // Analysis passes
  static void collect_declarations(const Program & program, AnalysisResult & result);
  static void check_duplicates(const Program & program, AnalysisResult & result);
  static void resolve_types(const Program & program, AnalysisResult & result);
  static void validate_semantics(const Program & program, AnalysisResult & result);

  // Individual validation checks
  static void check_duplicate_trees(const Program & program, AnalysisResult & result);
  static void check_duplicate_globals(const Program & program, AnalysisResult & result);
  static void check_duplicate_params(const TreeDef & tree, AnalysisResult & result);
  static void check_duplicate_declares(const Program & program, AnalysisResult & result);
  static void check_declare_conflicts(const Program & program, AnalysisResult & result);

  static void validate_tree(const Program & program, const TreeDef & tree, AnalysisResult & result);
  static void validate_node_stmt(
    const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result);
  static void validate_assignment_stmt(
    const Program & program, const AssignmentStmt & stmt, const TreeDef & tree,
    const TypeContext & ctx, AnalysisResult & result);
  static void validate_node_category(const NodeStmt & node, AnalysisResult & result);
  static void validate_arguments(
    const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result);
  static void validate_decorator(const Decorator & decorator, AnalysisResult & result);
  static void check_direction_permission(
    const Argument & arg, const PortInfo * port, const TreeDef & tree, const TypeContext & ctx,
    AnalysisResult & result);
  static void check_write_param_usage(const TreeDef & tree, AnalysisResult & result);
  static void validate_declare_stmt(const DeclareStmt & decl, AnalysisResult & result);
  static void validate_local_vars(
    const TreeDef & tree, const TypeContext & ctx, AnalysisResult & result);

  // Helper functions
  [[nodiscard]] static const Type * get_global_var_type(
    std::string_view name, const Program & program);

  [[nodiscard]] static bool is_parameter_writable(std::string_view name, const TreeDef & tree);

  static void collect_write_usages(
    const NodeStmt & node, const std::unordered_set<std::string> & writable_params,
    std::unordered_set<std::string> & used_for_write);
};

}  // namespace bt_dsl
