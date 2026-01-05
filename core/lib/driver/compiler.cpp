// bt_dsl/driver/compiler.cpp - Compiler driver implementation
//
#include "bt_dsl/driver/compiler.hpp"

#include <fstream>

#include "bt_dsl/codegen/xml_generator.hpp"
#include "bt_dsl/driver/stdlib_finder.hpp"
#include "bt_dsl/sema/analysis/init_checker.hpp"
#include "bt_dsl/sema/analysis/null_checker.hpp"
#include "bt_dsl/sema/analysis/tree_recursion_checker.hpp"
#include "bt_dsl/sema/resolution/module_resolver.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"
#include "bt_dsl/sema/types/const_evaluator.hpp"
#include "bt_dsl/sema/types/type_checker.hpp"

namespace bt_dsl
{

namespace
{

}  // namespace

CompileResult Compiler::compile_single_file(
  const std::filesystem::path & file, const CompileOptions & options)
{
  CompileResult result;
  result.module_graph = std::make_unique<ModuleGraph>();

  namespace fs = std::filesystem;

  // Ensure file exists
  if (!fs::exists(file)) {
    result.diagnostics.report_error(SourceRange{}, "file not found: " + file.string());
    return result;
  }

  // Resolve modules (parse entry point and imports)
  ModuleResolver resolver(*result.module_graph, &result.diagnostics);

  // Auto-detect and register stdlib if enabled
  if (options.auto_detect_stdlib) {
    if (auto stdlib = find_stdlib()) {
      resolver.register_package("std", *stdlib);
    }
  }

  // Register packages from pkg_paths (folder name becomes package name)
  for (const auto & pkg_path : options.pkg_paths) {
    const std::string pkg_name = pkg_path.filename().string();
    resolver.register_package(pkg_name, pkg_path);
  }

  if (!resolver.resolve(file)) {
    return result;
  }

  // Shared type context for this compile invocation.
  TypeContext types;

  // Get entry point module
  ModuleInfo * entry = result.module_graph->get_module(file);
  if (!entry || !entry->program) {
    result.diagnostics.report_error(SourceRange{}, "failed to load entry point");
    return result;
  }

  // Run semantic analysis on all modules
  for (auto * module : result.module_graph->get_all_modules()) {
    // Merge parse diagnostics - REMOVED: Managed by ModuleResolver
    // if (module->parsedUnit) {
    //   merge_diagnostics(result.diagnostics, module->parsedUnit->diags);
    // }

    if (!run_semantic_analysis(*module, types, result.diagnostics)) {
      // Continue to collect more errors from other modules
    }
  }

  // Check for errors before codegen
  if (result.diagnostics.has_errors()) {
    return result;
  }

  // Generate XML if in Build mode
  if (options.mode == CompileMode::Build) {
    fs::path output_path;
    if (options.output_dir) {
      fs::create_directories(*options.output_dir);
      output_path = *options.output_dir / (file.stem().string() + ".xml");
    } else {
      output_path = file.parent_path() / (file.stem().string() + ".xml");
    }

    if (!generate_xml(*entry, output_path, result.diagnostics)) {
      return result;
    }

    result.generated_files.push_back(output_path);
  }

  result.success = !result.diagnostics.has_errors();
  return result;
}

CompileResult Compiler::compile_project(
  const ProjectConfig & config, const CompileOptions & options)
{
  CompileResult result;
  result.module_graph = std::make_unique<ModuleGraph>();

  namespace fs = std::filesystem;

  // Handle empty entry points
  if (config.compiler.entry_points.empty()) {
    result.diagnostics.report_error(
      SourceRange{}, "no entry points defined in project configuration");
    return result;
  }

  // Determine output directory
  const fs::path output_dir =
    options.output_dir.value_or(config.project_root / config.compiler.output_dir);
  if (options.mode == CompileMode::Build) {
    fs::create_directories(output_dir);
  }

  // Process each entry point
  // Shared type context for this compile invocation.
  TypeContext types;
  for (const auto & entry_rel : config.compiler.entry_points) {
    const fs::path entry_path = config.project_root / entry_rel;

    if (!fs::exists(entry_path)) {
      result.diagnostics.report_error(
        SourceRange{}, "entry point not found: " + entry_path.string());
      continue;
    }

    // Resolve modules for this entry point
    ModuleResolver resolver(*result.module_graph, &result.diagnostics);

    // Auto-detect and register stdlib if enabled
    if (options.auto_detect_stdlib) {
      if (auto stdlib = find_stdlib()) {
        resolver.register_package("std", *stdlib);
      }
    }

    // Register packages from pkg_paths (folder name becomes package name)
    for (const auto & pkg_path : options.pkg_paths) {
      const std::string pkg_name = pkg_path.filename().string();
      resolver.register_package(pkg_name, pkg_path);
    }

    if (!resolver.resolve(entry_path)) {
      continue;
    }

    // Get entry point module
    ModuleInfo * entry = result.module_graph->get_module(entry_path);
    if (!entry || !entry->program) {
      result.diagnostics.report_error(
        SourceRange{}, "failed to load entry point: " + entry_path.string());
      continue;
    }

    // Run semantic analysis on all modules
    for (auto * module : result.module_graph->get_all_modules()) {
      if (!run_semantic_analysis(*module, types, result.diagnostics)) {
        // Continue to collect more errors from other modules
      }
    }

    // Generate XML if no errors and in Build mode
    if (!result.diagnostics.has_errors() && options.mode == CompileMode::Build) {
      const fs::path output_path = output_dir / (entry_path.stem().string() + ".xml");
      if (generate_xml(*entry, output_path, result.diagnostics)) {
        result.generated_files.push_back(output_path);
      }
    }
  }

  result.success = !result.diagnostics.has_errors();
  return result;
}

bool Compiler::run_semantic_analysis(
  ModuleInfo & module, TypeContext & types, DiagnosticBag & diags)
{
  if (!module.program) {
    return false;
  }

  bool success = true;

  // 1. Build symbol table
  // Managed by ModuleResolver during registration to ensure imports work correctly.
  // Running it again here causes duplicate redefinition errors because SymbolTableBuilder
  // is not idempotent for redefinitions on the same module instance.
  // SymbolTableBuilder builder(module.values, module.types, module.nodes, &diags);
  // if (!builder.build(*module.program)) {
  //   success = false;
  // }

  // 2. Name resolution
  NameResolver name_resolver(module, &diags);
  if (!name_resolver.resolve()) {
    success = false;
  }

  // 3. Constant evaluation (const decls + default arguments)
  // Reference: docs/reference/declarations-and-scopes.md §4.3
  if (module.ast) {
    ConstEvaluator eval(*module.ast, types, module.values, &diags);
    if (!eval.evaluate_program(*module.program)) {
      success = false;
    }
  } else {
    diags.report_error(SourceRange{}, "internal error: missing AST for constant evaluation");
    success = false;
  }

  // 4. Type checking
  // Reference: docs/reference/compiler.md §4 (Resolve & Validate)
  TypeChecker type_checker(types, module.types, module.values, &diags);
  if (!type_checker.check(*module.program)) {
    success = false;
  }

  // 5. Init checking (variable initialization before use)
  InitializationChecker init_checker(module.values, module.nodes, &diags);
  if (!init_checker.check(*module.program)) {
    success = false;
  }

  // 6. Null checking
  NullChecker null_checker(module.values, module.nodes, &diags);
  if (!null_checker.check(*module.program)) {
    success = false;
  }

  // 7. Tree recursion checking
  TreeRecursionChecker recursion_checker(&diags);
  if (!recursion_checker.check(*module.program)) {
    success = false;
  }

  return success;
}

bool Compiler::generate_xml(
  const ModuleInfo & module, const std::filesystem::path & output_path, DiagnosticBag & diags)
{
  try {
    const std::string xml = XmlGenerator::generate_single_output(module);

    std::ofstream out(output_path);
    if (!out.is_open()) {
      diags.report_error(SourceRange{}, "failed to open output file: " + output_path.string());
      return false;
    }

    out << xml;
    return true;
  } catch (const std::exception & e) {
    diags.report_error(SourceRange{}, "XML generation failed: " + std::string(e.what()));
    return false;
  }
}

}  // namespace bt_dsl
