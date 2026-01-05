// bt_dsl/sema/resolution/module_graph.hpp - Module dependency graph
//
// Manages module information and dependencies for cross-module resolution.
//
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"
#include "bt_dsl/sema/resolution/node_registry.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/type_table.hpp"

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
  /// Source file id (owned/managed by ModuleGraph::sources())
  FileId file_id = FileId::invalid();

  /// Parsed AST context (non-movable, owned by this module)
  std::unique_ptr<AstContext> ast;

  /// Diagnostics produced during parsing
  DiagnosticBag parse_diags;

  /// Parsed program root (owned by ast)
  Program * program = nullptr;

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
  [[nodiscard]] SourceRegistry & sources() noexcept { return sources_; }
  [[nodiscard]] const SourceRegistry & sources() const noexcept { return sources_; }

  /**
   * Add a new module to the graph.
   *
   * If a module for the same FileId already exists, returns existing.
   */
  ModuleInfo * add_module(FileId file_id)
  {
    if (!file_id.is_valid()) {
      return nullptr;
    }

    const auto idx = static_cast<size_t>(file_id.value);
    if (modules_.size() <= idx) {
      modules_.resize(idx + 1);
    }
    if (!modules_[idx]) {
      auto info = std::make_unique<ModuleInfo>();
      info->file_id = file_id;
      modules_[idx] = std::move(info);
    }
    return modules_[idx].get();
  }

  /**
   * Get a module by path.
   *
   * @param path Path to look up (will be canonicalized)
   * @return Pointer to ModuleInfo if found, nullptr otherwise
   */
  [[nodiscard]] ModuleInfo * get_module(FileId file_id) const
  {
    if (!file_id.is_valid()) {
      return nullptr;
    }
    const auto idx = static_cast<size_t>(file_id.value);
    if (idx >= modules_.size()) {
      return nullptr;
    }
    return modules_[idx].get();
  }

  [[nodiscard]] ModuleInfo * get_module(const std::filesystem::path & path) const
  {
    const std::optional<FileId> id = sources_.find_by_path(path);
    if (!id) {
      return nullptr;
    }
    return get_module(*id);
  }

  /**
   * Check if a module exists in the graph.
   */
  [[nodiscard]] bool has_module(FileId file_id) const { return get_module(file_id) != nullptr; }
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
    for (const auto & info : modules_) {
      if (!info) continue;
      result.push_back(info.get());
    }
    return result;
  }

  /**
   * Get the number of modules in the graph.
   */
  [[nodiscard]] size_t size() const noexcept
  {
    size_t count = 0;
    for (const auto & m : modules_) {
      if (m) ++count;
    }
    return count;
  }

  /**
   * Check if the graph is empty.
   */
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
  SourceRegistry sources_;
  std::vector<std::unique_ptr<ModuleInfo>> modules_;  // indexed by FileId::value
};

}  // namespace bt_dsl
