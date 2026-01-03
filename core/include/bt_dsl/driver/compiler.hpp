// bt_dsl/driver/compiler.hpp - Compiler driver
//
// Single entry point for the compile pipeline.
// Used by CLI and can be integrated into other tools.
//
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/project/project_config.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"
#include "bt_dsl/sema/types/type.hpp"

namespace bt_dsl
{

// ============================================================================
// Compile Mode
// ============================================================================

enum class CompileMode {
  Check,  ///< Syntax and semantic analysis only (no codegen)
  Build,  ///< Full build including XML generation
};

// ============================================================================
// Compile Options
// ============================================================================

struct CompileOptions
{
  /// Compile mode
  CompileMode mode = CompileMode::Build;

  /// Output directory for generated files (overrides project config)
  std::optional<std::filesystem::path> output_dir;

  /// Target environment (overrides project config)
  std::optional<std::string> target;

  /// Package paths (each path's folder name becomes the package name)
  /// e.g., --pkg /path/to/std registers "std" as a package.
  std::vector<std::filesystem::path> pkg_paths;

  /// Automatically detect and register the standard library.
  /// When true, the compiler will search for stdlib in standard locations.
  bool auto_detect_stdlib = true;

  /// Enable verbose output
  bool verbose = false;
};

// ============================================================================
// Compile Result
// ============================================================================

struct CompileResult
{
  /// Whether compilation succeeded (no errors)
  bool success = false;

  /// Collected diagnostics (errors, warnings, etc.)
  DiagnosticBag diagnostics;

  /// Generated files (only populated for Build mode)
  std::vector<std::filesystem::path> generated_files;

  /// Module graph (for introspection by LSP, etc.)
  std::unique_ptr<ModuleGraph> module_graph;
};

// ============================================================================
// Compiler
// ============================================================================

/**
 * Compiler driver that orchestrates the full compilation pipeline.
 *
 * The pipeline consists of:
 * 1. Module resolution (parsing and import loading)
 * 2. Symbol table building
 * 3. Name resolution
 * 4. Type checking
 * 5. Static safety analysis (init check, null check, tree recursion)
 * 6. Code generation (Build mode only)
 */
class Compiler
{
public:
  /**
   * Compile a single source file.
   *
   * @param file Path to the .bt source file
   * @param options Compile options
   * @return CompileResult with success status and diagnostics
   */
  [[nodiscard]] static CompileResult compile_single_file(
    const std::filesystem::path & file, const CompileOptions & options);

  /**
   * Compile a project defined by a ProjectConfig.
   *
   * This compiles all entry points defined in the project configuration.
   *
   * @param config Project configuration (from btc.yaml)
   * @param options Compile options (may override config settings)
   * @return CompileResult with success status and diagnostics
   */
  [[nodiscard]] static CompileResult compile_project(
    const ProjectConfig & config, const CompileOptions & options);

private:
  /**
   * Run semantic analysis on a module.
   *
   * @param module Module to analyze
   * @param types Shared type context (for canonicalization/inference)
   * @param diags Diagnostic bag to collect errors
   * @return true if no errors occurred
   */
  static bool run_semantic_analysis(
    ModuleInfo & module, TypeContext & types, DiagnosticBag & diags);

  /**
   * Generate XML output for a module.
   *
   * @param module Module to generate XML for
   * @param outputPath Output file path
   * @param diags Diagnostic bag to collect errors
   * @return true if generation succeeded
   */
  static bool generate_xml(
    const ModuleInfo & module, const std::filesystem::path & output_path, DiagnosticBag & diags);
};

}  // namespace bt_dsl
