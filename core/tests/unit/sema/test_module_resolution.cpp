// tests/sema/test_module_resolution.cpp - Unit tests for module resolution
//
// Tests the ModuleResolver and NameResolver which handle
// cross-module import resolution and symbol visibility.
//

#include <gtest/gtest.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"
#include "bt_dsl/sema/resolution/module_resolver.hpp"
#include "bt_dsl/sema/resolution/name_resolver.hpp"

using namespace bt_dsl;

namespace
{

struct TempDir
{
  std::filesystem::path path;
  explicit TempDir(std::filesystem::path p) : path(std::move(p))
  {
    std::filesystem::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir & operator=(const TempDir &) = delete;
};

}  // namespace

// Get the test files directory
// Note: This path is computed at compile time from __FILE__
static std::filesystem::path get_test_files_dir()
{
  // __FILE__ gives us the absolute path to this source file
  const std::filesystem::path this_file = std::filesystem::absolute(__FILE__);
  // Go up from test_module_resolution.cpp -> sema -> tests -> core
  const std::filesystem::path tests_dir = this_file.parent_path();
  return tests_dir / "module_test_files";
}

// ============================================================================
// ModuleResolver Tests
// ============================================================================

TEST(SemaModuleResolution, BasicImport)
{
  const std::filesystem::path main_path = get_test_files_dir() / "main.bt";

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool ok = resolver.resolve(main_path);
  ASSERT_TRUE(ok) << "main.bt should parse without errors";
  EXPECT_FALSE(resolver.has_errors());

  // Check modules were loaded
  EXPECT_EQ(graph.size(), 2U);  // main.bt and helper.bt

  // Check main module
  ModuleInfo * main_mod = graph.get_module(main_path);
  ASSERT_NE(main_mod, nullptr);
  ASSERT_NE(main_mod->program, nullptr);
  ASSERT_EQ(main_mod->imports.size(), 1U);

  // Check helper was imported
  ModuleInfo * helper_mod = main_mod->imports[0];
  ASSERT_NE(helper_mod, nullptr);
  ASSERT_NE(helper_mod->program, nullptr);
}

TEST(SemaModuleResolution, ImportPathValidationAbsolute)
{
  // Create a temp file with absolute path import
  const TempDir temp_dir(std::filesystem::temp_directory_path() / "bt_test");
  const std::filesystem::path test_file = temp_dir.path / "abs_import.bt";

  {
    std::ofstream f(test_file);
    f << "import \"/absolute/path.bt\";\n";
    f << "extern action DoNothing();\n";
    f << "tree Test() { DoNothing(); }\n";
  }

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool ok = resolver.resolve(test_file);

  // Should fail due to absolute path
  EXPECT_TRUE(!ok || resolver.has_errors());
  EXPECT_TRUE(diags.has_errors());
}

TEST(SemaModuleResolution, ImportPathValidationNoExtension)
{
  const TempDir temp_dir(std::filesystem::temp_directory_path() / "bt_test");
  const std::filesystem::path test_file = temp_dir.path / "no_ext.bt";

  {
    std::ofstream f(test_file);
    f << "import \"./foo\";\n";
    f << "extern action DoNothing();\n";
    f << "tree Test() { DoNothing(); }\n";
  }

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool ok = resolver.resolve(test_file);

  // Should fail due to missing extension
  EXPECT_TRUE(!ok || resolver.has_errors());
  EXPECT_TRUE(diags.has_errors());
}

TEST(SemaModuleResolution, CycleAllowed)
{
  const std::filesystem::path cycle_a_path = get_test_files_dir() / "cycle_a.bt";

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool ok = resolver.resolve(cycle_a_path);

  // Circular imports should be allowed
  if (!ok) {
    for (const auto & d : diags) {
      std::cerr << d.message << "\n";
    }
  }
  ASSERT_TRUE(ok);
  EXPECT_FALSE(resolver.has_errors());

  // Both modules should be loaded
  EXPECT_EQ(graph.size(), 2U);
}

// ============================================================================
// NameResolver (Import-Aware) Tests
// ============================================================================

TEST(SemaModuleResolution, ImportAwareTypeLookup)
{
  // Create test files with extern type
  const TempDir temp_dir(std::filesystem::temp_directory_path() / "bt_test_types");

  const std::filesystem::path types_file = temp_dir.path / "types.bt";
  const std::filesystem::path main_file = temp_dir.path / "main.bt";

  {
    std::ofstream f(types_file);
    f << "extern type Pose;\n";
  }

  {
    std::ofstream f(main_file);
    f << "import \"./types.bt\";\n";
    f << "var pos: Pose;\n";
  }

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool resolve_ok = resolver.resolve(main_file);
  if (!resolve_ok) {
    for (const auto & d : diags) {
      std::cerr << "ModuleResolver: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(resolve_ok);

  // Run name resolution on main module
  ModuleInfo * main_mod = graph.get_module(main_file);
  ASSERT_NE(main_mod, nullptr);

  NameResolver name_resolver(*main_mod, &diags);
  const bool name_resolve_ok = name_resolver.resolve();

  if (!name_resolve_ok) {
    for (const auto & d : diags) {
      std::cerr << "NameResolver: " << d.message << "\n";
    }
  }
  ASSERT_TRUE(name_resolve_ok);
}

TEST(SemaModuleResolution, PrivateNotVisible)
{
  const TempDir temp_dir(std::filesystem::temp_directory_path() / "bt_test_private");

  const std::filesystem::path helper_file = temp_dir.path / "helper.bt";
  const std::filesystem::path main_file = temp_dir.path / "main.bt";

  {
    std::ofstream f(helper_file);
    f << "extern action DoNothing();\n";
    f << "tree _PrivateTree() { DoNothing(); }\n";
  }

  {
    std::ofstream f(main_file);
    f << "import \"./helper.bt\";\n";
    f << "extern action DoNothing();\n";
    f << "tree Main() { _PrivateTree(); DoNothing(); }\n";
  }

  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver(graph, &diags);

  const bool resolve_ok = resolver.resolve(main_file);
  ASSERT_TRUE(resolve_ok);

  // Run name resolution - should fail because _PrivateTree is not visible
  ModuleInfo * main_mod = graph.get_module(main_file);
  ASSERT_NE(main_mod, nullptr);

  NameResolver name_resolver(*main_mod, &diags);
  const bool name_resolve_ok = name_resolver.resolve();

  // Should fail - _PrivateTree is private
  EXPECT_FALSE(name_resolve_ok);
  EXPECT_TRUE(name_resolver.has_errors());
}
