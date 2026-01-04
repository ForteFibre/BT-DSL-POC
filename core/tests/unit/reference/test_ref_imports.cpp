// tests/unit/reference/test_ref_imports.cpp
// Reference compliance tests for: 4.1.3 Import Resolution (declarations-and-scopes.md)
//
// Tests module resolution rules:
// - Non-transitive imports (A imports B, B imports C -> A cannot see C)
// - Private visibility (underscore prefix not visible)
// - Ambiguous imports (same name from multiple imports -> error)

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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

class ImportTestContext
{
public:
  ImportTestContext() : temp_dir(std::filesystem::temp_directory_path() / "bt_ref_imports") {}

  void create_file(const std::string & filename, const std::string & content) const
  {
    std::ofstream f(temp_dir.path / filename);
    f << content;
  }

  bool resolve(const std::string & main_filename)
  {
    return resolver.resolve(temp_dir.path / main_filename);
  }

  bool resolve_names(const std::string & main_filename)
  {
    if (!resolve(main_filename)) return false;

    ModuleInfo * main_mod = graph.get_module(temp_dir.path / main_filename);
    if (!main_mod) return false;

    // We only test name resolution of the main module for these tests
    NameResolver name_resolver(*main_mod, &diags);
    return name_resolver.resolve();
  }

  TempDir temp_dir;
  ModuleGraph graph;
  DiagnosticBag diags;
  ModuleResolver resolver{graph, &diags};
};

}  // namespace

// ============================================================================
// 4.1.3 Non-Transitive Imports
// Reference: Imports are not transitive.
// ============================================================================

TEST(RefImports, ImportsAreNonTransitive)
{
  // Layout:
  // C.bt: extern action Foo();
  // B.bt: import "C.bt";
  // A.bt: import "B.bt"; tree Main() { Foo(); }
  //
  // Main() in A.bt calling Foo() should FAIL because C's symbols are not visible in A
  // unless explicitly imported.

  ImportTestContext ctx;
  ctx.create_file("C.bt", "extern action Foo();");
  ctx.create_file("B.bt", "import \"./C.bt\";");
  ctx.create_file("A.bt", R"(
    import "./B.bt";
    tree Main() {
      Foo(); // Should fail
    }
  )");

  // Module resolution should succeed (files exist)
  // Name resolution should fail (Foo not found)
  EXPECT_FALSE(ctx.resolve_names("A.bt"));
}

TEST(RefImports, ExplicitImportMakesVisible)
{
  // Same as above but A imports C directly too -> Should SUCCEED
  ImportTestContext ctx;
  ctx.create_file("C.bt", "extern action Foo();");
  ctx.create_file("B.bt", "import \"./C.bt\";");
  ctx.create_file("A.bt", R"(
    import "./B.bt";
    import "./C.bt";
    tree Main() {
      Foo(); // Should succeed
    }
  )");

  EXPECT_TRUE(ctx.resolve_names("A.bt"));
}

// ============================================================================
// 4.1.2 Visibility (Private Definitions)
// Reference: Definitions starting with '_' are private to the file.
// ============================================================================

TEST(RefImports, PrivateSymbolsNotVisible)
{
  ImportTestContext ctx;
  ctx.create_file("Lib.bt", R"(
    extern action _PrivateAction();
    tree _PrivateTree() {}
    var _private_var: int32;
  )");
  ctx.create_file("Main.bt", R"(
    import "./Lib.bt";
    tree Main() {
      _PrivateAction(); // Error
    }
  )");

  EXPECT_FALSE(ctx.resolve_names("Main.bt"));
}

TEST(RefImports, PrivateSymbolsVisibleInSameFile)
{
  ImportTestContext ctx;
  ctx.create_file("Main.bt", R"(
    extern action _PrivateAction();
    tree Main() {
      _PrivateAction(); // OK
    }
  )");

  EXPECT_TRUE(ctx.resolve_names("Main.bt"));
}

// ============================================================================
// 4.1.3 Ambiguous Imports
// Reference: Same name from multiple imports -> Ambiguous Error
// ============================================================================

TEST(RefImports, AmbiguousImportError)
{
  // LibA.bt: extern action Foo();
  // LibB.bt: extern action Foo();
  // Main.bt: import "LibA.bt"; import "LibB.bt"; Foo();

  ImportTestContext ctx;
  ctx.create_file("LibA.bt", "extern action Foo();");
  ctx.create_file("LibB.bt", "extern action Foo();");
  ctx.create_file("Main.bt", R"(
    import "./LibA.bt";
    import "./LibB.bt";
    tree Main() {
      Foo(); // Ambiguous -> Error
    }
  )");

  EXPECT_FALSE(ctx.resolve_names("Main.bt"));
}

TEST(RefImports, AmbiguousImportNoUseOk)
{
  // Ambiguity is only an error if the symbol is USED.
  // Just importing conflicting names is fine if we don't reference them.
  ImportTestContext ctx;
  ctx.create_file("LibA.bt", "extern action Foo();");
  ctx.create_file("LibB.bt", "extern action Foo();");
  ctx.create_file("Main.bt", R"(
    import "./LibA.bt";
    import "./LibB.bt";
    tree Main() {
      // Not calling Foo()
    }
  )");

  EXPECT_TRUE(ctx.resolve_names("Main.bt"));
}
