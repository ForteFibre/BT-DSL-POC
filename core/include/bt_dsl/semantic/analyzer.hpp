// bt_dsl/analyzer.hpp - Main semantic analyzer
#pragma once

#include <unordered_map>
#include <vector>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/core/diagnostic.hpp"
#include "bt_dsl/core/symbol_table.hpp"
#include "bt_dsl/semantic/node_registry.hpp"
#include "bt_dsl/semantic/type_system.hpp"

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
  // Nodes visible from the analyzed program (local + imported public).
  NodeRegistry nodes;
  // All nodes/trees available in the analysis set (local + imported, including private).
  // This is useful for whole-program analyses; name resolution for the current file
  // must still use `nodes` to enforce visibility.
  NodeRegistry all_nodes;
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

  /**
   * Get the type context for a specific tree (mutable).
   * This is used during semantic validation to refine/normalize types in a
   * reference-site sensitive way.
   * @return nullptr if tree not found
   */
  TypeContext * get_tree_context_mut(std::string_view tree_name);
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

  enum class StatementListKind {
    TreeBody,
    ChildrenBlock,
  };

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
  static void resolve_types(
    const Program & program, const std::vector<const Program *> & imported_programs,
    AnalysisResult & result);
  static void validate_semantics(
    const Program & program, const std::vector<const Program *> & imported_programs,
    AnalysisResult & result);

  // Individual validation checks
  static void check_duplicate_trees(const Program & program, AnalysisResult & result);
  static void check_duplicate_globals(const Program & program, AnalysisResult & result);
  static void check_duplicate_params(const TreeDef & tree, AnalysisResult & result);
  static void check_duplicate_declares(const Program & program, AnalysisResult & result);
  static void check_declare_conflicts(const Program & program, AnalysisResult & result);

  static void validate_tree(
    const Program & program, const std::vector<const Program *> & imported_programs,
    const TreeDef & tree, AnalysisResult & result);
  static void validate_node_stmt(
    const Program & program, const NodeStmt & node, const TreeDef & tree, TypeContext & ctx,
    Scope * scope, StatementListKind list_kind, AnalysisResult & result);
  static void validate_assignment_stmt(
    const Program & program, const AssignmentStmt & stmt, const TreeDef & tree, TypeContext & ctx,
    Scope * scope, AnalysisResult & result);
  static void validate_statement_block(
    const Program & program, const std::vector<Statement> & stmts, const TreeDef & tree,
    TypeContext & ctx, Scope * scope, AnalysisResult & result);
  static void validate_statement(
    const Program & program, const Statement & stmt, const TreeDef & tree, TypeContext & ctx,
    Scope * scope, StatementListKind list_kind, AnalysisResult & result);
  static void validate_node_category(const NodeStmt & node, AnalysisResult & result);
  static void validate_arguments(
    const Program & program, const NodeStmt & node, const TreeDef & tree, const TypeContext & ctx,
    Scope * scope, StatementListKind list_kind, AnalysisResult & result);
  static void check_direction_permission(
    const Argument & arg, const PortInfo * port, const TreeDef & tree, const TypeContext & ctx,
    AnalysisResult & result);
  static void check_write_param_usage(const TreeDef & tree, AnalysisResult & result);
  static void validate_declare_stmt(
    const Program & program, const DeclareStmt & decl, AnalysisResult & result);

  static void collect_write_usages_in_block(
    const std::vector<Statement> & stmts, const std::unordered_set<std::string> & writable_params,
    std::unordered_set<std::string> & used_for_write);
};

}  // namespace bt_dsl
