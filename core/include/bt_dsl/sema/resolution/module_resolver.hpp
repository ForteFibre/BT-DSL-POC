// bt_dsl/sema/resolution/module_resolver.hpp - Module resolution and dependency loading
//
// Resolves import paths and builds the module dependency graph.
//
#pragma once

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"

namespace bt_dsl
{

// ============================================================================
// Package Registry
// ============================================================================

/**
 * Registry of package names to their filesystem paths.
 *
 * Used for resolving package-style imports like `import "std/nodes.bt"`.
 * The package name (e.g., "std") maps to a directory path.
 */
using PackageRegistry = std::unordered_map<std::string, std::filesystem::path>;

// ============================================================================
// Module Resolver
// ============================================================================

/**
 * Resolves import paths and builds the module dependency graph.
 *
 * This class handles:
 * - Validating import paths per spec §4.1.3
 * - Resolving relative paths to absolute paths
 * - Resolving package-style imports (e.g., "std/nodes.bt")
 * - Parsing modules and building the ModuleGraph
 * - Registering symbols in per-module symbol tables
 *
 * Circular imports are allowed - modules are only parsed once.
 *
 * Reference: docs/reference/semantics.md §4.1.3 (importの解決)
 */
class ModuleResolver
{
public:
  /**
   * Construct a ModuleResolver.
   *
   * @param graph The module graph to populate
   * @param diags DiagnosticBag for error reporting (may be nullptr)
   */
  ModuleResolver(ModuleGraph & graph, DiagnosticBag * diags = nullptr)
  : graph_(graph), diags_(diags)
  {
  }

  /**
   * Register a package path.
   *
   * Package paths allow package-style imports like `import "std/nodes.bt"`.
   * The import path "pkg/file.bt" will be resolved to "<registered_path>/file.bt".
   *
   * @param name Package name (e.g., "std")
   * @param path Filesystem path to the package root directory
   */
  void register_package(std::string_view name, const std::filesystem::path & path)
  {
    packages_[std::string(name)] = path;
  }

  /**
   * Register multiple package paths from a registry.
   *
   * @param registry Map of package names to paths
   */
  void register_packages(const PackageRegistry & registry)
  {
    for (const auto & [name, path] : registry) {
      packages_[name] = path;
    }
  }

  // ===========================================================================
  // Entry Point
  // ===========================================================================

  /**
   * Resolve all modules starting from an entry point.
   *
   * This recursively processes all imports, building the module graph.
   *
   * @param entryPoint Path to the entry point file
   * @return true if all modules were resolved successfully
   */
  bool resolve(const std::filesystem::path & entry_point);

  // ===========================================================================
  // Error State
  // ===========================================================================

  /// Check if any errors occurred during resolution
  [[nodiscard]] bool has_errors() const noexcept { return has_errors_; }

  /// Get the number of errors
  [[nodiscard]] size_t error_count() const noexcept { return error_count_; }

  /// Get the diagnostics bag
  [[nodiscard]] DiagnosticBag * diagnostics() noexcept { return diags_; }

private:
  // ===========================================================================
  // Internal Methods
  // ===========================================================================

  /**
   * Process a single module and its imports.
   *
   * @param path Absolute path to the module
   * @param visited Set of already-visited paths (for cycle handling)
   * @return true if successful
   */
  bool process_module(
    const std::filesystem::path & path, std::unordered_set<std::string> & visited);

  /**
   * Validate an import path per spec §4.1.3.
   *
   * Rules:
   * - Absolute paths (starting with '/') are prohibited
   * - Extension is required
   * - Package format (not starting with ./ or ../) resolved via registered packages
   *
   * @param path The import path string
   * @param range Source range for error reporting
   * @return true if valid
   */
  bool validate_import_path(std::string_view path, SourceRange range);

  /**
   * Check if an import path is a package-style import.
   *
   * Package-style imports don't start with "./" or "../".
   *
   * @param path The import path string
   * @return true if this is a package-style import
   */
  static bool is_package_import(std::string_view path);

  /**
   * Resolve a package-style import path to an absolute path.
   *
   * @param importPath The import path from the source (e.g., "std/nodes.bt")
   * @return Resolved absolute path, or nullopt if package not registered
   */
  std::optional<std::filesystem::path> resolve_package_import(std::string_view import_path);

  /**
   * Resolve a relative import path to an absolute path.
   *
   * @param basePath The directory containing the importing file
   * @param importPath The import path from the source
   * @return Resolved absolute path, or nullopt if invalid
   */
  static std::optional<std::filesystem::path> resolve_import_path(
    const std::filesystem::path & base_path, std::string_view import_path);

  /**
   * Parse a source file and create the AST.
   *
   * @param path Absolute path to the source file
    * @return Parsed ModuleInfo (owned by the graph), or nullptr on fatal error
   */
  ModuleInfo * parse_file(const std::filesystem::path & path);

  /**
   * Register all declarations from a module into its symbol tables.
   *
   * @param module The module to process
   * @return true if successful (no duplicate declarations)
   */
  bool register_declarations(ModuleInfo & module);

  /**
   * Report an error.
   */
  void report_error(SourceRange range, std::string_view message);

  /**
   * Report an error with a file path.
   */
  void report_error(const std::filesystem::path & file, std::string_view message);

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  ModuleGraph & graph_;
  DiagnosticBag * diags_;
  PackageRegistry packages_;
  bool has_errors_ = false;
  size_t error_count_ = 0;
};

}  // namespace bt_dsl
