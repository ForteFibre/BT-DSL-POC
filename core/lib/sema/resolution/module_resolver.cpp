// bt_dsl/sema/module_resolver.cpp - Module resolution implementation
//
#include "bt_dsl/sema/resolution/module_resolver.hpp"

#include <fstream>
#include <sstream>

#include "bt_dsl/syntax/frontend.hpp"

namespace bt_dsl
{

// ============================================================================
// Entry Point
// ============================================================================

bool ModuleResolver::resolve(const std::filesystem::path & entry_point)
{
  has_errors_ = false;
  error_count_ = 0;

  // Normalize the entry point path
  std::filesystem::path abs_path;
  try {
    abs_path = std::filesystem::absolute(entry_point);
    abs_path = std::filesystem::weakly_canonical(abs_path);
  } catch (const std::filesystem::filesystem_error & e) {
    report_error(entry_point, "cannot resolve path: " + std::string(e.what()));
    return false;
  }

  // Check if file exists
  if (!std::filesystem::exists(abs_path)) {
    report_error(abs_path, "file not found");
    return false;
  }

  // Process the entry module and all its imports
  std::unordered_set<std::string> visited;
  if (!process_module(abs_path, visited)) {
    return false;
  }

  return !has_errors_;
}

// ============================================================================
// Module Processing
// ============================================================================

bool ModuleResolver::process_module(
  const std::filesystem::path & path, std::unordered_set<std::string> & visited)
{
  const std::string path_str = path.string();

  // Check if already visited (circular import is allowed)
  if (visited.count(path_str) > 0) {
    // Module already processed, just return success
    return true;
  }

  // Check if already in graph (may have been processed via different path)
  if (graph_.has_module(path)) {
    return true;
  }

  // Mark as visited
  visited.insert(path_str);

  // Add module to graph
  ModuleInfo * module = graph_.add_module(path);

  // Parse the file
  if (!parse_file(path, *module)) {
    return false;
  }

  // Register declarations into symbol tables
  if (!register_declarations(*module)) {
    // Continue processing to report more errors
  }

  // Process imports
  if (module->program) {
    const std::filesystem::path base_dir = path.parent_path();

    for (auto * import_decl : module->program->imports) {
      // Validate import path
      if (!validate_import_path(import_decl->path, import_decl->get_range())) {
        continue;
      }

      // Resolve import path - use appropriate resolver based on import type
      std::optional<std::filesystem::path> resolved_path;
      if (is_package_import(import_decl->path)) {
        resolved_path = resolve_package_import(import_decl->path);
        if (!resolved_path) {
          // Extract package name for error message
          const std::string_view import_path = import_decl->path;
          auto slash_pos = import_path.find('/');
          const std::string_view pkg_name =
            (slash_pos != std::string_view::npos) ? import_path.substr(0, slash_pos) : import_path;
          report_error(
            import_decl->get_range(), "unknown package: '" + std::string(pkg_name) +
                                        "' (use relative imports or register the package)");
          continue;
        }
      } else {
        resolved_path = resolve_import_path(base_dir, import_decl->path);
        if (!resolved_path) {
          report_error(
            import_decl->get_range(),
            "cannot resolve import path: " + std::string(import_decl->path));
          continue;
        }
      }

      // Check if file exists
      if (!std::filesystem::exists(*resolved_path)) {
        report_error(
          import_decl->get_range(), "imported file not found: " + resolved_path->string());
        continue;
      }

      // Process the imported module recursively
      if (!process_module(*resolved_path, visited)) {
        // Continue to process other imports
      }

      // Link the import
      ModuleInfo * imported_module = graph_.get_module(*resolved_path);
      if (imported_module) {
        module->imports.push_back(imported_module);
      }
    }
  }

  return !has_errors_;
}

// ============================================================================
// Path Validation
// ============================================================================

bool ModuleResolver::validate_import_path(std::string_view path, SourceRange range)
{
  if (path.empty()) {
    report_error(range, "import path cannot be empty");
    return false;
  }

  // Rule: Absolute paths are prohibited
  if (path[0] == '/') {
    report_error(range, "absolute import paths are prohibited");
    return false;
  }

  // Rule: Extension is required
  if (path.find('.') == std::string_view::npos) {
    report_error(range, "import path must have file extension (e.g., '.bt')");
    return false;
  }

  // Both relative paths (./ or ../) and package-style imports are allowed
  // Package-style imports are validated during resolution
  return true;
}

bool ModuleResolver::is_package_import(std::string_view path)
{
  // Package-style imports don't start with "./" or "../"
  return path.substr(0, 2) != "./" && path.substr(0, 3) != "../";
}

std::optional<std::filesystem::path> ModuleResolver::resolve_package_import(
  std::string_view import_path)
{
  // Find the package name (everything before the first '/')
  auto slash_pos = import_path.find('/');
  if (slash_pos == std::string_view::npos) {
    // No slash in path - this is an error (package imports should have pkg/file.bt format)
    return std::nullopt;
  }

  const std::string pkg_name(import_path.substr(0, slash_pos));
  const std::string_view remaining = import_path.substr(slash_pos + 1);

  // Look up the package in the registry
  auto it = packages_.find(pkg_name);
  if (it == packages_.end()) {
    return std::nullopt;
  }

  // Construct the full path
  try {
    const std::filesystem::path resolved = it->second / std::filesystem::path(remaining);
    return std::filesystem::weakly_canonical(resolved);
  } catch (const std::filesystem::filesystem_error &) {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path> ModuleResolver::resolve_import_path(
  const std::filesystem::path & base_path, std::string_view import_path)
{
  try {
    const std::filesystem::path resolved = base_path / std::filesystem::path(import_path);
    return std::filesystem::weakly_canonical(resolved);
  } catch (const std::filesystem::filesystem_error &) {
    return std::nullopt;
  }
}

// ============================================================================
// File Parsing
// ============================================================================

bool ModuleResolver::parse_file(const std::filesystem::path & path, ModuleInfo & module)
{
  // Read file contents
  std::ifstream file(path);
  if (!file.is_open()) {
    report_error(path, "cannot open file");
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // Parse using frontend
  auto unit = parse_source(std::move(source));
  if (!unit) {
    report_error(path, "parse failed");
    return false;
  }

  // Check for parse errors
  if (!unit->diags.empty()) {
    if (diags_) {
      diags_->merge(unit->diags);
    }
    if (unit->diags.has_errors()) {
      has_errors_ = true;
      error_count_ += unit->diags.errors().size();
    }
  }

  // Transfer ownership
  module.program = unit->program;
  module.parsedUnit = std::move(unit);

  return module.program != nullptr;
}

// ============================================================================
// Declaration Registration
// ============================================================================

bool ModuleResolver::register_declarations(ModuleInfo & module)
{
  if (!module.program) {
    return true;
  }

  bool success = true;
  const Program & program = *module.program;

  // Register built-in types
  module.types.register_builtins();

  // Register extern types
  for (const auto * ext_type : program.externTypes) {
    TypeSymbol sym;
    sym.name = ext_type->name;
    sym.decl = ext_type;
    sym.is_builtin = false;
    if (!module.types.define(sym)) {
      report_error(ext_type->get_range(), "redefinition of type");
      success = false;
    }
  }

  // Register type aliases
  for (const auto * alias : program.typeAliases) {
    TypeSymbol sym;
    sym.name = alias->name;
    sym.decl = alias;
    sym.is_builtin = false;
    if (!module.types.define(sym)) {
      report_error(alias->get_range(), "redefinition of type");
      success = false;
    }
  }

  // Register extern nodes
  for (const auto * ext : program.externs) {
    NodeSymbol sym;
    sym.name = ext->name;
    sym.decl = ext;
    if (!module.nodes.define(sym)) {
      report_error(ext->get_range(), "redefinition of node");
      success = false;
    }
  }

  // Register trees
  for (const auto * tree : program.trees) {
    NodeSymbol sym;
    sym.name = tree->name;
    sym.decl = tree;
    if (!module.nodes.define(sym)) {
      report_error(tree->get_range(), "redefinition of node");
      success = false;
    }
  }

  // Build value symbol table
  module.values.build_from_program(program);

  return success;
}

// ============================================================================
// Error Reporting
// ============================================================================

void ModuleResolver::report_error(SourceRange range, std::string_view message)
{
  has_errors_ = true;
  error_count_++;

  if (diags_) {
    diags_->error(range, message);
  }
}

void ModuleResolver::report_error(const std::filesystem::path & file, std::string_view message)
{
  has_errors_ = true;
  error_count_++;

  if (diags_) {
    const std::string full_message = file.string() + ": " + std::string(message);
    diags_->error({}, full_message);
  }
}

}  // namespace bt_dsl
