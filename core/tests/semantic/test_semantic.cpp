// test_semantic.cpp - Semantic analysis tests
// Ported from TypeScript validation.test.ts

#include <gtest/gtest.h>

#include <algorithm>

#include "bt_dsl/core/diagnostic.hpp"
#include "bt_dsl/core/symbol_table.hpp"
#include "bt_dsl/parser/parser.hpp"
#include "bt_dsl/semantic/analyzer.hpp"
#include "bt_dsl/semantic/node_registry.hpp"
#include "bt_dsl/semantic/type_system.hpp"

using namespace bt_dsl;

class SemanticTest : public ::testing::Test
{
protected:
  Parser parser;
  Program stdlib_program;
  bool has_stdlib = false;

  void SetUp() override
  {
    // Minimal stdlib for tests. This mirrors the extension-bundled stdlib and
    // allows tests to use built-in nodes like Sequence/Fallback/Delay/etc
    // without redeclaring them.
    const std::string stdlib_src = R"(
extern type Vector3;
extern type Entry;

extern action AlwaysFailure();
extern action AlwaysSuccess();
extern action Sleep(in msec: int);
extern action WasEntryUpdated(in entry: Entry);

extern control Fallback();
extern control Parallel(in failure_count: int, in success_count: int);
extern control ReactiveFallback();
extern control ReactiveSequence();
extern control Sequence();
extern control SequenceWithMemory();

extern decorator Delay(in delay_msec: int);
extern decorator ForceFailure();
extern decorator ForceSuccess();
extern decorator Inverter();
extern decorator KeepRunningUntilFailure();
extern decorator Repeat(in num_cycles: int);
extern decorator RetryUntilSuccessful(in num_attempts: int);
extern decorator RunOnce(in then_skip: bool);
extern decorator SkipUnlessUpdated(in entry: Entry);
extern decorator Timeout(in msec: int);
extern decorator WaitValueUpdate(in entry: Entry);
)";

    auto parsed = parser.parse(stdlib_src);
    EXPECT_TRUE(parsed.has_value()) << "Failed to parse stdlib for tests";
    if (parsed.has_value()) {
      stdlib_program = std::move(parsed.value());
      has_stdlib = true;
    }
  }

  // Helper to parse and analyze
  AnalysisResult parse_and_analyze(const std::string & source)
  {
    auto parse_result = parser.parse(source);
    EXPECT_TRUE(parse_result.has_value()) << "Parse failed";
    if (!parse_result.has_value()) {
      return AnalysisResult{};
    }
    if (!has_stdlib) {
      return Analyzer::analyze(parse_result.value());
    }
    const std::vector<const Program *> imports = {&stdlib_program};
    return Analyzer::analyze(parse_result.value(), imports);
  }

  // Helper to check for error containing substring
  static bool has_error(const AnalysisResult & result, const std::string & substring)
  {
    const auto errs = result.diagnostics.errors();
    return std::any_of(errs.begin(), errs.end(), [&substring](const Diagnostic & diag) {
      return diag.message.find(substring) != std::string::npos;
    });
  }

  // Helper to check for warning containing substring
  static bool has_warning(const AnalysisResult & result, const std::string & substring)
  {
    const auto warns = result.diagnostics.warnings();
    return std::any_of(warns.begin(), warns.end(), [&substring](const Diagnostic & diag) {
      return diag.message.find(substring) != std::string::npos;
    });
  }

  // Count errors
  static size_t error_count(const AnalysisResult & result)
  {
    return result.diagnostics.errors().size();
  }
};

// ============================================================================
// Duplicate Checks
// ============================================================================

TEST_F(SemanticTest, DuplicateTreeNames)
{
  auto result = parse_and_analyze(R"(
  tree Main() { Sequence {} }
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate tree name"));
}

TEST_F(SemanticTest, DuplicateGlobalVariables)
{
  auto result = parse_and_analyze(R"(
        var Pos: Vector3;
        var Pos: Vector3;
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate global variable"));
}

TEST_F(SemanticTest, DuplicateGlobalConstants)
{
  auto result = parse_and_analyze(R"(
        const X = 1;
        const X = 2;
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate global constant"));
}

TEST_F(SemanticTest, ErrorOnGlobalVarConstNameCollision)
{
  auto result = parse_and_analyze(R"(
        var X: int = 0;
        const X = 1;
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "conflicts with a global"));
}

TEST_F(SemanticTest, DuplicateParameterNames)
{
  auto result = parse_and_analyze(R"(
  tree Main(x: int, x: double) { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate parameter"));
}

TEST_F(SemanticTest, ErrorOnParameterShadowingGlobalValue)
{
  // Spec (docs/reference/declarations-and-scopes.md 4.2.3): shadowing between ancestor
  // scopes is forbidden for value-space declarations.
  auto result = parse_and_analyze(R"(
    var x: int = 0;
    tree Main(x: int) {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Shadowing is forbidden"));
}

TEST_F(SemanticTest, ErrorOnParameterShadowingImportedGlobalValue)
{
  auto imp = parser.parse(R"(
    var x: int = 0;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    tree Main(x: int) {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Shadowing is forbidden"));
}

// ============================================================================
// Symbol Resolution
// ============================================================================

TEST_F(SemanticTest, ResolveGlobalVariableReferenceInNodeArg)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: int);
        var Target: int = 0;
    tree Main() {
      Action(pos: Target);
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, ResolveTreeParameterReferenceInNodeArg)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
    tree Main(target: Vector3) {
      Action(pos: target);
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, ErrorOnUndefinedVariableReferenceInNodeArg)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
    tree Main() {
      Action(pos: UndefinedVar);
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Unknown variable"));
}

TEST_F(SemanticTest, ErrorOnTypeBoundForwardReferenceToLocalConst)
{
  // Reference: docs/reference/declarations-and-scopes.md 4.2.4
  // Tree-local value-space identifiers are not visible before their declaration.
  auto result = parse_and_analyze(R"(
    tree Main() {
      var arr: [int; SIZE] = [0; 1];
      const SIZE: int = 3;
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  const bool ok = has_error(result, "Unknown constant") || has_error(result, "Type bound 'SIZE'") ||
                  has_error(result, "not allowed before its declaration");
  EXPECT_TRUE(ok) << "Expected an error for forward reference to local const in type bound";
}

TEST_F(SemanticTest, LocalConstTypeBoundAfterDeclarationOk)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      const SIZE: int = 3;
      var arr: [int; SIZE] = [0; SIZE];
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, MergesDeclarationsFromMultipleImports)
{
  auto imp1 = parser.parse(R"(
  extern action FromImport1();
    )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
      extern action FromImport2();
    )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
      tree Main() {
            Sequence {
          FromImport1();
          FromImport2();
            }
        }
    )");
  ASSERT_TRUE(main_prog.has_value());

  // Include stdlib + both imports.
  const std::vector<const Program *> imports = {
    has_stdlib ? &stdlib_program : nullptr, &imp1.value(), &imp2.value()};

  // Filter nulls (in case stdlib failed to parse).
  std::vector<const Program *> filtered;
  for (const auto * p : imports) {
    if (p) {
      filtered.push_back(p);
    }
  }

  const auto result = Analyzer::analyze(main_prog.value(), filtered);
  EXPECT_FALSE(result.has_errors()) << "Expected imported declarations to be merged";
}

TEST_F(SemanticTest, OkOnDuplicatePublicTypeAcrossImportsWhenUnused)
{
  // Spec: duplicates across direct imports are not errors by themselves; they
  // become errors only when referenced (ambiguity at reference site).
  auto imp1 = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Duplicate imported types should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousImportedTypeReference)
{
  auto imp1 = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      var x: Pose;
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "ambiguous imported type"));
}

TEST_F(SemanticTest, OkOnDuplicatePublicNodeAcrossImportsWhenUnused)
{
  auto imp1 = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Duplicate imported nodes should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousImportedNodeCall)
{
  auto imp1 = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Do();
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Ambiguous imported node name"));
}

TEST_F(SemanticTest, OkOnDuplicatePublicNodeLocalVsImportWhenUnused)
{
  auto imp = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    extern action Do();
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Local vs import duplicate node should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousLocalVsImportedNodeCall)
{
  auto imp = parser.parse(R"(
    extern action Do();
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    extern action Do();
    tree Main() {
      Do();
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Ambiguous imported node name"));
}

TEST_F(SemanticTest, OkOnDuplicatePublicGlobalAcrossImportsWhenUnused)
{
  auto imp1 = parser.parse(R"(
    var X: int = 0;
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    var X: int = 1;
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Duplicate imported globals should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousImportedGlobalReference)
{
  auto imp1 = parser.parse(R"(
    var X: int = 0;
  )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
    var X: int = 1;
  )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
    extern action Use(in x: int);
    tree Main() {
      Use(x: X);
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp1.value());
  imports.push_back(&imp2.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Ambiguous imported value name"));
}

TEST_F(SemanticTest, OkOnDuplicatePublicGlobalLocalVsImportWhenUnused)
{
  auto imp = parser.parse(R"(
    var X: int = 1;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    var X: int = 0;
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Local vs import duplicate global should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousLocalVsImportedGlobalReference)
{
  auto imp = parser.parse(R"(
    var X: int = 1;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    var X: int = 0;
    extern action Use(in x: int);
    tree Main() {
      Use(x: X);
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Ambiguous imported value name"));
}

// ============================================================================
// Type Visibility Across Imports
// ============================================================================

TEST_F(SemanticTest, ErrorOnUsingPrivateImportedTypeInLocalAnnotation)
{
  auto imp = parser.parse(R"(
    type _Secret = int;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      var x: _Secret;
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "private type '_Secret'"));
}

TEST_F(SemanticTest, OkOnDuplicatePublicTypeLocalVsImportWhenUnused)
{
  auto imp = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    extern type Pose;
    tree Main() {
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Local vs import duplicate type should be ok when unused";
}

TEST_F(SemanticTest, ErrorOnAmbiguousLocalVsImportedTypeUse)
{
  auto imp = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    extern type Pose;
    tree Main() {
      var x: Pose;
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "ambiguous imported type"));
}

TEST_F(SemanticTest, ErrorOnCallingImportedNodeWhosePortUsesPrivateType)
{
  auto imp = parser.parse(R"(
    type _Secret = int;
    extern action Pub(in x: _Secret);
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Pub(x: 1);
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "not visible across imports"));
}

TEST_F(SemanticTest, OkOnCallingImportedNodeWhosePortUsesPublicTypeAlias)
{
  auto imp = parser.parse(R"(
    type Secret = int;
    extern action Pub(in x: Secret);
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      Pub(x: 1);
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_FALSE(result.has_errors()) << "Expected public type alias to be visible across imports";
}

// ============================================================================
// Tree Recursion (Cycle) Detection
// ============================================================================

TEST_F(SemanticTest, ErrorOnDirectTreeRecursion)
{
  auto result = parse_and_analyze(R"(
    tree A() {
      A();
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Recursive tree call is not allowed"));
}

TEST_F(SemanticTest, ErrorOnIndirectTreeRecursion)
{
  auto result = parse_and_analyze(R"(
    tree A() {
      B();
    }
    tree B() {
      C();
    }
    tree C() {
      A();
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Recursive tree call is not allowed"));
}

TEST_F(SemanticTest, ErrorOnRecursionAcrossImportedTrees)
{
  auto imp = parser.parse(R"(
    tree B() {
      C();
    }
    tree C() {
      B();
    }
  )");
  ASSERT_TRUE(imp.has_value());

  auto main_prog = parser.parse(R"(
    tree A() {
      B();
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  const std::vector<const Program *> imports = {&imp.value()};
  const auto result = Analyzer::analyze(main_prog.value(), imports);

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Recursive tree call is not allowed"));
}

// ============================================================================
// Initialization Safety
// ============================================================================

TEST_F(SemanticTest, InitSafety_AllChained_PropagatesOutWrites)
{
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    tree Main() {
      var x: int;
      Sequence {
        Compute(res: out x);
        Log(msg: x);
      }
    }
  )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected errors: init should be propagated in Sequence";
}

TEST_F(SemanticTest, InitSafety_BehaviorNone_DoesNotPropagateChildWrites)
{
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    #[behavior(None)]
    extern decorator ForceSuccessLike();

    tree Main() {
      var x: int;
      Sequence {
        ForceSuccessLike {
          Compute(res: out x);
        }
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, InitSafety_Isolated_DoesNotChainBetweenSiblings)
{
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    #[behavior(All, Isolated)]
    extern control ParallelAll();

    tree Main() {
      var x: int;
      ParallelAll {
        Compute(res: out x);
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, InitSafety_Any_PropagatesIntersectionOnly)
{
  auto result = parse_and_analyze(R"(
    extern action WriteX(out x: int);
    extern action WriteXY(out x: int, out y: int);
    extern action Log(in msg: int);

    #[behavior(Any)]
    extern control FallbackLike();

    tree Main() {
      var x: int;
      var y: int;

      Sequence {
        FallbackLike {
          WriteX(x: out x);
          WriteXY(x: out x, y: out y);
        }
        Log(msg: x);
        Log(msg: y);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  // y is not guaranteed initialized after Any-policy node.
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, InitSafety_SuccessIf_DoesNotGuaranteeOutWrites)
{
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    tree Main(in ok: bool) {
      var x: int;
      Sequence {
        @success_if(ok)
        Compute(res: out x);
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, InitSafety_SuccessIf_DoesNotGuaranteeAssignmentWrites)
{
  auto result = parse_and_analyze(R"(
    extern action Log(in msg: int);

    tree Main(in ok: bool) {
      var x: int;
      Sequence {
        @success_if(ok)
        x = 1;
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, ErrorOnNonBoolAssignmentPrecondition)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var x: int;
      Sequence {
        @success_if(1)
        x = 1;
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Precondition must be of type bool"));
}

TEST_F(SemanticTest, InitSafety_SkipIf_DoesNotGuaranteeOutWrites)
{
  // Reference: docs/reference/execution-model.md
  // @skip_if(cond) can return Skip without executing the node body, and Skip may be
  // treated like Success by control nodes. Therefore, out-writes cannot be assumed.
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    tree Main(in skip: bool) {
      var x: int;
      Sequence {
        @skip_if(skip)
        Compute(res: out x);
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

TEST_F(SemanticTest, InitSafety_RunWhile_DoesNotGuaranteeOutWrites)
{
  // Reference: docs/reference/execution-model.md
  // @run_while(cond) may return Skip immediately if the condition is false, so out-writes
  // cannot be assumed as guaranteed on success for subsequent siblings.
  auto result = parse_and_analyze(R"(
    extern action Compute(out res: int);
    extern action Log(in msg: int);

    tree Main(in run: bool) {
      var x: int;
      Sequence {
        @run_while(run)
        Compute(res: out x);
        Log(msg: x);
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "may be uninitialized"));
}

// ============================================================================
// Declare Statement Validation
// ============================================================================

TEST_F(SemanticTest, DuplicatePortNamesInDeclaration)
{
  auto result = parse_and_analyze(R"(
  extern action MyAction(in target: Vector3, in target: bool);
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate port name"));
}

TEST_F(SemanticTest, InvalidCategory)
{
  // In the new DSL, the parser won't accept an invalid extern category keyword,
  // but the analyzer still validates the category string for robustness.
  Program program;
  DeclareStmt decl;
  decl.category = "InvalidCategory";
  decl.name = "MyNode";
  program.declarations.push_back(std::move(decl));

  const auto result = Analyzer::analyze(program);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Invalid category"));
}

TEST_F(SemanticTest, DuplicateDeclarationNames)
{
  auto result = parse_and_analyze(R"(
  extern action MyAction();
  extern condition MyAction();
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate declaration"));
}

TEST_F(SemanticTest, DeclarationConflictsWithTree)
{
  auto result = parse_and_analyze(R"(
  extern action Main();
  tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "conflicts with a Tree"));
}

TEST_F(SemanticTest, AllowUsingDeclaredNodeInTree)
{
  auto result = parse_and_analyze(R"(
  extern action MyAction(in target: string);
  tree Main() { MyAction(target: "hello"); }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowUsingDeclaredDecorator)
{
  auto result = parse_and_analyze(R"(
    extern decorator MyDecorator(in timeout: double);
    tree Main() {
      MyDecorator(timeout: 5.0) {
        Sequence {}
      }
    }
    )");

  EXPECT_FALSE(result.has_errors());
}

// ============================================================================
// Local Variable Checks
// ============================================================================

TEST_F(SemanticTest, AllowLocalVarWithInitialValueOnly)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var msg = "hello";
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowLocalVarWithMatchingTypeAndValue)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var count: int = 42;
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnTypeMismatchInLocalVar)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var count: int = "hello";
            Sequence {}
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Type mismatch"));
}

TEST_F(SemanticTest, ErrorOnLocalVarWithoutTypeOrValue)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var unknown;
            Sequence {}
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "must have either"));
}

// ============================================================================
// Node Category Validation
// ============================================================================

TEST_F(SemanticTest, DecoratorCanBeUsedAsNode)
{
  auto result = parse_and_analyze(R"(
        tree Main() {
            Delay(delay_msec: 10) {
                Sequence {}
            }
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, DecoratorRequiresChildren)
{
  auto result = parse_and_analyze(R"(
        tree Main() {
            Delay(delay_msec: 10);
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "requires a children block"));
}

TEST_F(SemanticTest, NonControlNodeCannotHaveChildren)
{
  auto result = parse_and_analyze(R"(
    extern action TestAction();
    tree Main() {
            TestAction() {
                Sequence {}
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "cannot have a children block"));
}

TEST_F(SemanticTest, ControlNodeRequiresChildren)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      Fallback();
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "requires a children block"));
}

// ============================================================================
// Direction Permission Checks
// ============================================================================

TEST_F(SemanticTest, ErrorWhenUsingRefOnNonRefParameter)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
        var Target: Vector3;
    tree Main(target: Vector3) {
      Action(pos: ref target);
        }
    )");

  // Reference semantics: passing `ref` to an `in` port is a warning (more
  // permissive than required), not an error.
  EXPECT_FALSE(result.has_errors());
  EXPECT_TRUE(has_warning(result, "more permissive"));
}

TEST_F(SemanticTest, WarnWhenRefParameterNeverUsedForWriteAccess)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
        var Target: Vector3;
    tree Main(ref target: Vector3) {
      Action(pos: target);
        }
    )");

  // Reference semantics: `ref` parameters are read-only and do not require
  // write usage.
  EXPECT_FALSE(result.has_errors());
  EXPECT_FALSE(has_warning(result, "never used for write access"));
}

TEST_F(SemanticTest, AllowRefParameterUsedWithRef)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
        var Target: Vector3;
    tree Main(ref target: Vector3) {
      Action(pos: ref target);
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowOutParameterForOutputPorts)
{
  auto result = parse_and_analyze(R"(
    extern action OutputAction(out result: Vector3);
        var Target: Vector3;
    tree Main(out result: Vector3) {
      OutputAction(result: out result);
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorWhenSubTreeRefParamPassedAsIn)
{
  auto result = parse_and_analyze(R"(
    extern action Action(in pos: Vector3);
        var Target: Vector3;
    tree Main() {
      SubTree(x: Target);
        }
    tree SubTree(ref x: Vector3) {
      Action(pos: x);
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Direction mismatch"));
}

// ============================================================================
// Expression Type Checks (AssignmentStmt)
// ============================================================================

TEST_F(SemanticTest, ErrorOnAddingIntAndBoolInAssignment)
{
  auto result = parse_and_analyze(R"(
    var result: int;
    var flag: bool;
    tree Main() {
            Sequence {
        result = 30 + flag;
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    has_error(result, "cannot be applied") || has_error(result, "Operator cannot be applied"));
}

TEST_F(SemanticTest, AllowAddingIntAndIntInAssignment)
{
  auto result = parse_and_analyze(R"(
    var a: int;
    var b: int = 0;
    tree Main() {
            Sequence {
        a = b + 1;
            }
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnSignedUnsignedMixInArithmetic)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var a: uint32 = 1;
      var b: int32 = 2;
      var c: uint32;
      Sequence {
        c = a + b;
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Signed/unsigned") || has_error(result, "cannot be mixed"));
}

TEST_F(SemanticTest, RepeatInitCountMustBeIntegerConstExpr)
{
  // runtime variable is not allowed as repeat count for static arrays
  {
    auto result = parse_and_analyze(R"(
      tree Main() {
        var n: int = 3;
        var a: [int; 3] = [1; n];
        Sequence {}
      }
    )");
    EXPECT_TRUE(result.has_errors());
    EXPECT_TRUE(has_error(result, "repeat count") || has_error(result, "not a const"));
  }

  // local const is allowed
  {
    auto result = parse_and_analyze(R"(
      tree Main() {
        const N: int = 3;
        var a: [int; 3] = [1; N];
        Sequence {}
      }
    )");
    EXPECT_FALSE(result.has_errors()) << "Unexpected errors";
  }

  // const expression is allowed
  {
    auto result = parse_and_analyze(R"(
      tree Main() {
        var a: [int; 3] = [1; 1 + 2];
        Sequence {}
      }
    )");
    EXPECT_FALSE(result.has_errors()) << "Unexpected errors";
  }

  // non-integer const_expr is rejected
  {
    auto result = parse_and_analyze(R"(
      tree Main() {
        var a: [int; 3] = [1; 1.0];
        Sequence {}
      }
    )");
    EXPECT_TRUE(result.has_errors());
    EXPECT_TRUE(has_error(result, "integer constant") || has_error(result, "repeat count"));
  }
}

TEST_F(SemanticTest, RepeatInitConstExprMustMatchExactArraySize)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      const N: int = 4;
      var a: [int; 3] = [1; N];
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "array length") || has_error(result, "length mismatch"));
}

TEST_F(SemanticTest, RepeatInitConstExprMustRespectBoundedArraySize)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      const N: int = 3;
      var a: [int; <=3] = [1; N + 1];
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "exceeds bound") || has_error(result, "array length"));
}

TEST_F(SemanticTest, StaticArrayConstIndexOutOfBoundsMustError)
{
  // Reference: docs/reference/type-system/expression-typing.md 3.4.4
  // When N and index are both const_expr, bounds must be checked at compile time.
  auto result = parse_and_analyze(R"(
    tree Main() {
      const SIZE = 3;
      const IDX = 3;
      var a: [int; SIZE] = [1, 2, 3];
      var x: int;
      Sequence {
        x = a[IDX];
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "out of bounds") || has_error(result, "Array index"));
}

TEST_F(SemanticTest, ConstExprCastOutOfRangeMustError)
{
  auto result = parse_and_analyze(R"(
    const X: uint8 = 300 as uint8;
    tree Main() {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Cast out of range") || has_error(result, "out of range"));
}

TEST_F(SemanticTest, ConstExprCastToVecMustError)
{
  // Reference: docs/reference/declarations-and-scopes.md 4.3.4
  // Forbidden: dynamic array construction in const_expr (e.g. `as vec<_>`).
  auto result = parse_and_analyze(R"(
    const V = [1, 2, 3] as vec<int>;
    tree Main() {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "vec") && has_error(result, "constant expression"));
}

TEST_F(SemanticTest, ConstExprCastToExternTypeMustError)
{
  auto result = parse_and_analyze(R"(
    extern type Pose;
    const X: int = (0 as Pose) as int;
    tree Main() {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Cannot cast to extern type in constant expression"));
}

TEST_F(SemanticTest, StaticArrayConstIndexWithCastOutOfBoundsMustError)
{
  // Reference: docs/reference/type-system/expression-typing.md 3.4.4
  // Index const_expr may include casts; bounds must still be checked at compile time.
  auto result = parse_and_analyze(R"(
    tree Main() {
      const SIZE = 3;
      const IDX: int = 3 as int;
      var a: [int; SIZE] = [1, 2, 3];
      var x: int;
      Sequence {
        x = a[IDX];
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "out of bounds") || has_error(result, "Array index"));
}

TEST_F(SemanticTest, DeclarePortDefaultFloatDivZeroMustError)
{
  auto result = parse_and_analyze(R"(
    extern action A(in x: float64 = 1.0 / 0.0);
    tree Main() {
      A();
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Float overflow") || has_error(result, "invalid operation"));
}

TEST_F(SemanticTest, ParameterDefaultFloatDivZeroMustError)
{
  auto result = parse_and_analyze(R"(
    tree Main(in x: float64 = 1.0 / 0.0) {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Float overflow") || has_error(result, "invalid operation"));
}

TEST_F(SemanticTest, GlobalConstBoolComparisonAndLogicalConstExprOk)
{
  // Reference: docs/reference/declarations-and-scopes.md 4.3.4
  // const_expr must support comparisons/logical operators and be fully evaluable.
  auto result = parse_and_analyze(R"(
    const B: bool = (1 < 2) && !(false || false);
    tree Main() {
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected errors";
}

TEST_F(SemanticTest, GlobalConstBoolConstExprWithIntDivZeroMustError)
{
  // Regression: previously we only evaluated integer-only const_expr, so an integer
  // division-by-zero nested under comparison/logical operators could slip through.
  auto result = parse_and_analyze(R"(
    const B: bool = true && ((1 / 0) == 0);
    tree Main() {
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    has_error(result, "Division by zero") || has_error(result, "constant expression") ||
    has_error(result, "fully evaluable"));
}

TEST_F(SemanticTest, LocalConstBoolConstExprWithIntDivZeroMustError)
{
  // Same as above, but for tree-local const initializers.
  auto result = parse_and_analyze(R"(
    tree Main() {
      const B: bool = true && ((1 / 0) == 0);
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    has_error(result, "Division by zero") || has_error(result, "constant expression") ||
    has_error(result, "fully evaluable"));
}

TEST_F(SemanticTest, VecRepeatInitCountMustBeInteger)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var v: vec<int> = vec![1; 1.0];
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "vec repeat count") || has_error(result, "must be an integer"));
}

TEST_F(SemanticTest, ErrorOnUnresolvedNullInferenceInLocalVar)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var x = null;
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Unresolved inferred type") || has_error(result, "_?"));
}

TEST_F(SemanticTest, ResolveWildcardTypeFromInitializer)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var a: _ = 1;
      var b: _? = 1.0;
      var c: vec<_> = vec![1, 2, 3];
      var d: [_; 3] = [1, 2, 3];
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected errors";
}

TEST_F(SemanticTest, ErrorOnUnresolvedWildcardWithoutInitializer)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var x: _;
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Unresolved inferred type") || has_error(result, "_"));
}

TEST_F(SemanticTest, ErrorOnLogicalOperatorWithNonBool)
{
  auto result = parse_and_analyze(R"(
    var a: int;
    var result: bool;
    tree Main() {
            Sequence {
        result = a && true;
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "bool operands"));
}

TEST_F(SemanticTest, ErrorOnAssigningStringToInt)
{
  auto result = parse_and_analyze(R"(
    var count: int;
    tree Main() {
            Sequence {
        count = "hello";
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Cannot assign"));
}

TEST_F(SemanticTest, ErrorOnUnknownTypeInCastExpression)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      var x: int = 1;
      var y: int = x as NotAType;
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Unknown type: NotAType") || has_error(result, "cast target type"));
}

TEST_F(SemanticTest, ErrorOnAmbiguousImportedTypeInCastExpression)
{
  auto imp_a = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp_a.has_value());

  auto imp_b = parser.parse(R"(
    extern type Pose;
  )");
  ASSERT_TRUE(imp_b.has_value());

  auto main_prog = parser.parse(R"(
    tree Main() {
      var x: int = 0;
      var y: int;
      Sequence {
        y = x as Pose;
      }
    }
  )");
  ASSERT_TRUE(main_prog.has_value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imp_a.value());
  imports.push_back(&imp_b.value());

  const auto result = Analyzer::analyze(main_prog.value(), imports);
  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    has_error(result, "ambiguous imported type 'Pose'") ||
    has_error(result, "ambiguous imported type"));
}

// ============================================================================
// Argument Validation (named args)
// ============================================================================

TEST_F(SemanticTest, AllowNamedArgumentForSinglePortNode)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      Repeat(num_cycles: 3) {
        Sequence {}
      }
    }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnUnknownPortArgument)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      Repeat(unknown_port: 3) {
        Sequence {}
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Unknown port") && has_error(result, "unknown_port"));
}

TEST_F(SemanticTest, ErrorOnDuplicatePortArgument)
{
  auto result = parse_and_analyze(R"(
    tree Main() {
      Repeat(num_cycles: 3, num_cycles: 4) {
        Sequence {}
      }
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "Duplicate argument") && has_error(result, "num_cycles"));
}

TEST_F(SemanticTest, ErrorOnMissingRequiredInputPort)
{
  auto result = parse_and_analyze(R"(
    extern action MultiPort(in a: string, in b: string);
    tree Main() {
      MultiPort(a: "value");
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    has_error(result, "Missing required input port 'b'") ||
    has_error(result, "Missing required input port"));
}

// ============================================================================
// Symbol Table Tests
// ============================================================================

TEST_F(SemanticTest, SymbolTableBuildsCorrectly)
{
  auto parse_result = parser.parse(R"(
    extern type Entry;
    var GlobalVar: Entry;
    tree Main(param1: int, ref param2: Entry) {
      var localVar = 42;
            Sequence {}
        }
    )");
  ASSERT_TRUE(parse_result.has_value());

  SymbolTable symbols;
  symbols.build_from_program(parse_result.value());

  // Check global scope
  EXPECT_TRUE(symbols.has_global("GlobalVar"));
  // Trees are not part of the value-space symbol table.
  EXPECT_FALSE(symbols.has_global("Main"));

  // Check tree scope
  auto * main_scope = symbols.tree_scope("Main");
  ASSERT_NE(main_scope, nullptr);

  const auto * param1 = main_scope->lookup("param1");
  ASSERT_NE(param1, nullptr);
  EXPECT_EQ(param1->kind, SymbolKind::Parameter);
  EXPECT_EQ(param1->type_name, "int");

  const auto * param2 = main_scope->lookup("param2");
  ASSERT_NE(param2, nullptr);
  EXPECT_EQ(param2->direction, PortDirection::Ref);

  const auto * local = main_scope->lookup("localVar");
  ASSERT_NE(local, nullptr);
  EXPECT_EQ(local->kind, SymbolKind::LocalVariable);
}

// ============================================================================
// Node Registry Tests
// ============================================================================

TEST_F(SemanticTest, NodeRegistryBuildsFromProgram)
{
  auto parse_result = parser.parse(R"(
  extern action MyAction(in target: Vector3, out result: bool);
  extern control MyControl();
  tree SubTree(param: int) { Sequence {} }
    )");
  ASSERT_TRUE(parse_result.has_value());

  NodeRegistry registry;
  registry.build_from_program(parse_result.value());

  // Check declared action
  const auto * action = registry.get_node("MyAction");
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->category, NodeCategory::Action);
  EXPECT_EQ(action->port_count(), 2);

  const auto * target_port = action->get_port("target");
  ASSERT_NE(target_port, nullptr);
  EXPECT_EQ(target_port->direction, PortDirection::In);

  // Check control
  const auto * control = registry.get_node("MyControl");
  ASSERT_NE(control, nullptr);
  EXPECT_TRUE(control->can_have_children());

  // Check tree as subtree
  const auto * subtree = registry.get_node("SubTree");
  ASSERT_NE(subtree, nullptr);
  EXPECT_EQ(subtree->category, NodeCategory::SubTree);
  EXPECT_TRUE(registry.is_tree("SubTree"));
}

// ============================================================================
// Type System Tests
// ============================================================================

TEST_F(SemanticTest, TypeCompatibility)
{
  // Same types
  EXPECT_TRUE(Type::int_type().is_compatible_with(Type::int_type()));
  EXPECT_TRUE(Type::string_type().is_compatible_with(Type::string_type()));

  // Any is compatible with everything
  EXPECT_TRUE(Type::any_type().is_compatible_with(Type::int_type()));
  EXPECT_TRUE(Type::int_type().is_compatible_with(Type::any_type()));

  // Unknown is compatible (for partial analysis)
  EXPECT_TRUE(Type::unknown().is_compatible_with(Type::string_type()));

  // Widening is compatible (implicit)
  EXPECT_TRUE(Type::int_type(true, 32).is_compatible_with(Type::int_type(true, 8)));
  EXPECT_TRUE(Type::float_type(64).is_compatible_with(Type::float_type(32)));

  // Narrowing is NOT compatible without explicit cast
  EXPECT_FALSE(Type::int_type(true, 8).is_compatible_with(Type::int_type(true, 32)));
  EXPECT_FALSE(Type::float_type(32).is_compatible_with(Type::float_type(64)));

  // Int and float are NOT implicitly compatible
  EXPECT_FALSE(Type::int_type().is_compatible_with(Type::double_type()));
  EXPECT_FALSE(Type::double_type().is_compatible_with(Type::int_type()));

  // Different types are not compatible
  EXPECT_FALSE(Type::int_type().is_compatible_with(Type::string_type()));
  EXPECT_FALSE(Type::bool_type().is_compatible_with(Type::int_type()));
}

TEST_F(SemanticTest, TypeFromString)
{
  EXPECT_TRUE(Type::from_string("int").equals(Type::int_type()));
  EXPECT_TRUE(Type::from_string("double").equals(Type::double_type()));
  EXPECT_TRUE(Type::from_string("bool").equals(Type::bool_type()));
  EXPECT_TRUE(Type::from_string("string").equals(Type::string_type()));
  EXPECT_TRUE(Type::from_string("any").equals(Type::unknown()));

  // Custom types
  auto custom = Type::from_string("Vector3");
  EXPECT_TRUE(custom.is_custom());
  EXPECT_EQ(custom.to_string(), "Vector3");
}

TEST_F(SemanticTest, TypeContextResolution)
{
  auto parse_result = parser.parse(R"(
    tree Main(x: int) {
      var y: int;
      var z = 3.14;
            Sequence {}
        }
    )");
  ASSERT_TRUE(parse_result.has_value());

  SymbolTable symbols;
  symbols.build_from_program(parse_result.value());

  NodeRegistry nodes;
  nodes.build_from_program(parse_result.value());

  TypeResolver resolver(symbols, nodes);

  const auto & tree = parse_result.value().trees[0];
  TypeContext ctx = resolver.resolve_tree_types(tree);

  // Check explicit types
  const auto * x_type = ctx.get_type("x");
  ASSERT_NE(x_type, nullptr);
  EXPECT_TRUE(x_type->equals(Type::int_type()));

  const auto * y_type = ctx.get_type("y");
  ASSERT_NE(y_type, nullptr);
  EXPECT_TRUE(y_type->equals(Type::int_type()));

  // Check inferred type
  const auto * z_type = ctx.get_type("z");
  ASSERT_NE(z_type, nullptr);
  EXPECT_TRUE(z_type->equals(Type::double_type()));
}

TEST_F(SemanticTest, BoundedStringSizeFromConstIdentifier)
{
  auto result = parse_and_analyze(R"(
    const SIZE = 4;
    tree Main() {
      var s: string<SIZE> = "abcd";
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, BoundedArraySizeFromConstIdentifier)
{
  auto result = parse_and_analyze(R"(
    const SIZE = 4;
    tree Main() {
      var a: [int32; <=SIZE] = [1, 2, 3, 4];
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, BoundedTypesCompareUsingEvaluatedConstValues)
{
  auto result = parse_and_analyze(R"(
    const A = 4;
    const B = 2 + 2;

    tree Main() {
      var x: string<A> = "abcd";
      var y: string<B> = x;
      Sequence {}
    }
  )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, BoundedStringRejectsTooLongLiteralAfterConstEval)
{
  auto result = parse_and_analyze(R"(
    const SIZE = 2;
    tree Main() {
      var s: string<SIZE> = "abc";
      Sequence {}
    }
  )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(has_error(result, "string<2>"));
}

TEST_F(SemanticTest, BoundedTypesCanUseImportedConstIdentifier)
{
  auto imported_parse = parser.parse(R"(
    const SIZE = 4;
  )");
  ASSERT_TRUE(imported_parse.has_value());
  Program imported_prog = std::move(imported_parse.value());

  auto main_parse = parser.parse(R"(
    tree Main() {
      var s: string<SIZE> = "abcd";
      Sequence {}
    }
  )");
  ASSERT_TRUE(main_parse.has_value());
  Program main_prog = std::move(main_parse.value());

  std::vector<const Program *> imports;
  if (has_stdlib) {
    imports.push_back(&stdlib_program);
  }
  imports.push_back(&imported_prog);

  const auto result = Analyzer::analyze(main_prog, imports);
  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, GuardNarrowing_AllowsNullableVarAsNonNullInsideChildrenBlock)
{
  auto result = parse_and_analyze(R"(
    extern type Pose;
    extern action MoveTo(in target: Pose);

    tree Main() {
      var target: Pose? = null;

      @guard(target != null)
      Sequence {
        MoveTo(target: target);
      }
    }
  )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, GuardNarrowing_ConjunctionNarrowsBothSides)
{
  auto result = parse_and_analyze(R"(
    extern type Pose;
    extern action UseBoth(in a: Pose, in b: Pose);

    tree Main() {
      var a: Pose? = null;
      var b: Pose? = null;

      @guard(a != null && b != null)
      Sequence {
        UseBoth(a: a, b: b);
      }
    }
  )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, GuardNarrowing_NegationOfEqNullNarrows)
{
  auto result = parse_and_analyze(R"(
    extern type Pose;
    extern action MoveTo(in target: Pose);

    tree Main() {
      var target: Pose? = null;

      @guard(!(target == null))
      Sequence {
        MoveTo(target: target);
      }
    }
  )");

  EXPECT_FALSE(result.has_errors());
}
