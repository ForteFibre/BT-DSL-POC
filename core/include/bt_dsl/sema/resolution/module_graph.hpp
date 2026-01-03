// bt_dsl/sema/resolution/module_graph.hpp - Module dependency graph
//
// Manages module information and dependencies for cross-module resolution.
//
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/type_table.hpp"
#include "bt_dsl/syntax/frontend.hpp"

namespace bt_dsl
{

// ============================================================================
// Module Info
// ============================================================================

/**
 * Information about a single module (source file).
 *
 * Each module has its own symbol tables for types, nodes, and values.
 * The imports list contains resolved ModuleInfo pointers for direct imports.
 */
struct ModuleInfo
{
  /// Absolute path to the source file
  std::filesystem::path absolutePath;

  /// Parsed AST (owned by parsedUnit)
  Program * program = nullptr;

  /// Full parsed unit (owns AstContext, SourceManager, DiagnosticBag)
  std::unique_ptr<struct ParsedUnit> parsedUnit;

  /// Per-module symbol tables
  TypeTable types;
  NodeRegistry nodes;
  SymbolTable values;

  /// Direct imports (resolved ModuleInfo pointers)
  std::vector<ModuleInfo *> imports;

  // ===========================================================================
  // Visibility Helpers
  // ===========================================================================

  /**
   * Check if a name is public (visible to importing modules).
   *
   * Per spec §4.1.2: names starting with '_' are private.
   */
  [[nodiscard]] static bool is_public(std::string_view name) noexcept
  {
    return !name.empty() && name[0] != '_';
  }

  /**
   * Check if a name is private (not visible to importing modules).
   */
  [[nodiscard]] static bool is_private(std::string_view name) noexcept
  {
    return !name.empty() && name[0] == '_';
  }
};

// ============================================================================
// Module Graph
// ============================================================================

/**
 * Graph of all modules in a compilation.
 *
 * Manages module lifetime and provides lookup by path.
 */
class ModuleGraph
{
public:
  ModuleGraph() = default;

  // Non-copyable, non-movable (owns modules)
  ModuleGraph(const ModuleGraph &) = delete;
  ModuleGraph & operator=(const ModuleGraph &) = delete;
  ModuleGraph(ModuleGraph &&) = default;
  ModuleGraph & operator=(ModuleGraph &&) = default;

  // ===========================================================================
  // Module Management
  // ===========================================================================

  /**
   * Add a new module to the graph.
   *
   * If a module with the same path already exists, returns existing.
   *
   * @param path Absolute path to the module
   * @return Pointer to the ModuleInfo (never null)
   */
  ModuleInfo * add_module(const std::filesystem::path & path)
  {
    auto canonical = std::filesystem::weakly_canonical(path);
    auto it = modules_.find(canonical);
    if (it != modules_.end()) {
      return it->second.get();
    }
    auto info = std::make_unique<ModuleInfo>();
    info->absolutePath = canonical;
    auto * ptr = info.get();
    modules_.emplace(canonical, std::move(info));
    return ptr;
  }

  /**
   * Get a module by path.
   *
   * @param path Path to look up (will be canonicalized)
   * @return Pointer to ModuleInfo if found, nullptr otherwise
   */
  [[nodiscard]] ModuleInfo * get_module(const std::filesystem::path & path) const
  {
    auto canonical = std::filesystem::weakly_canonical(path);
    auto it = modules_.find(canonical);
    return it != modules_.end() ? it->second.get() : nullptr;
  }

  /**
   * Check if a module exists in the graph.
   */
  [[nodiscard]] bool has_module(const std::filesystem::path & path) const
  {
    return get_module(path) != nullptr;
  }

  /**
   * Get all modules in the graph.
   */
  [[nodiscard]] std::vector<ModuleInfo *> get_all_modules() const
  {
    std::vector<ModuleInfo *> result;
    result.reserve(modules_.size());
    for (const auto & [path, info] : modules_) {
      result.push_back(info.get());
    }
    return result;
  }

  /**
   * Get the number of modules in the graph.
   */
  [[nodiscard]] size_t size() const noexcept { return modules_.size(); }

  /**
   * Check if the graph is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return modules_.empty(); }

private:
  std::map<std::filesystem::path, std::unique_ptr<ModuleInfo>> modules_;
};

}  // namespace bt_dsl
