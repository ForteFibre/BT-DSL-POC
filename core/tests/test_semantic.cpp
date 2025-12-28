// test_semantic.cpp - Semantic analysis tests
// Ported from TypeScript validation.test.ts

#include <gtest/gtest.h>

#include "bt_dsl/analyzer.hpp"
#include "bt_dsl/diagnostic.hpp"
#include "bt_dsl/node_registry.hpp"
#include "bt_dsl/parser.hpp"
#include "bt_dsl/symbol_table.hpp"
#include "bt_dsl/type_system.hpp"

using namespace bt_dsl;

class SemanticTest : public ::testing::Test
{
protected:
  Parser parser;
  Analyzer analyzer;
  Program stdlib_program;
  bool has_stdlib = false;

  void SetUp() override
  {
    // Minimal stdlib for tests. This mirrors the extension-bundled stdlib and
    // allows tests to use built-in nodes like Sequence/Fallback/Delay/etc
    // without redeclaring them.
    const std::string stdlib_src = R"(
declare Action AlwaysFailure()
declare Action AlwaysSuccess()
declare Action Sleep(in msec: int)
declare Action WasEntryUpdated(in entry: any)

declare Control Fallback()
declare Control Parallel(in failure_count: int, in success_count: int)
declare Control ReactiveFallback()
declare Control ReactiveSequence()
declare Control Sequence()
declare Control SequenceWithMemory()

declare Decorator Delay(in delay_msec: int)
declare Decorator ForceFailure()
declare Decorator ForceSuccess()
declare Decorator Inverter()
declare Decorator KeepRunningUntilFailure()
declare Decorator Repeat(in num_cycles: int)
declare Decorator RetryUntilSuccessful(in num_attempts: int)
declare Decorator RunOnce(in then_skip: bool)
declare Decorator SkipUnlessUpdated(in entry: any)
declare Decorator Timeout(in msec: int)
declare Decorator WaitValueUpdate(in entry: any)
)";

    auto parsed = parser.parse(stdlib_src);
    EXPECT_TRUE(parsed.has_value()) << "Failed to parse stdlib for tests";
    if (parsed.has_value()) {
      stdlib_program = std::move(parsed.value());
      has_stdlib = true;
    }
  }

  // Helper to parse and analyze
  AnalysisResult parseAndAnalyze(const std::string & source)
  {
    auto parse_result = parser.parse(source);
    EXPECT_TRUE(parse_result.has_value()) << "Parse failed";
    if (!parse_result.has_value()) {
      return AnalysisResult{};
    }
    if (!has_stdlib) {
      return analyzer.analyze(parse_result.value());
    }
    const std::vector<const Program *> imports = {&stdlib_program};
    return analyzer.analyze(parse_result.value(), imports);
  }

  // Helper to check for error containing substring
  bool hasError(const AnalysisResult & result, const std::string & substring)
  {
    for (const auto & diag : result.diagnostics.errors()) {
      if (diag.message.find(substring) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  // Helper to check for warning containing substring
  bool hasWarning(const AnalysisResult & result, const std::string & substring)
  {
    for (const auto & diag : result.diagnostics.warnings()) {
      if (diag.message.find(substring) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  // Count errors
  size_t errorCount(const AnalysisResult & result) { return result.diagnostics.errors().size(); }
};

// ============================================================================
// Duplicate Checks
// ============================================================================

TEST_F(SemanticTest, DuplicateTreeNames)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() { Sequence {} }
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Duplicate tree name"));
}

TEST_F(SemanticTest, DuplicateGlobalVariables)
{
  auto result = parseAndAnalyze(R"(
        var Pos: Vector3
        var Pos: Vector3
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Duplicate global variable"));
}

TEST_F(SemanticTest, DuplicateParameterNames)
{
  auto result = parseAndAnalyze(R"(
        Tree Main(x: Int, x: Float) { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Duplicate parameter"));
}

// ============================================================================
// Symbol Resolution
// ============================================================================

TEST_F(SemanticTest, ResolveGlobalVariableReferenceInNodeArg)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main() {
            Action(pos: Target)
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, ResolveTreeParameterReferenceInNodeArg)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        Tree Main(target: Vector3) {
            Action(pos: target)
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, ErrorOnUndefinedVariableReferenceInNodeArg)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        Tree Main() {
            Action(pos: UndefinedVar)
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Unknown variable"));
}

TEST_F(SemanticTest, MergesDeclarationsFromMultipleImports)
{
  auto imp1 = parser.parse(R"(
        declare Action FromImport1()
    )");
  ASSERT_TRUE(imp1.has_value());

  auto imp2 = parser.parse(R"(
        declare Action FromImport2()
    )");
  ASSERT_TRUE(imp2.has_value());

  auto main_prog = parser.parse(R"(
        Tree Main() {
            Sequence {
                FromImport1()
                FromImport2()
            }
        }
    )");
  ASSERT_TRUE(main_prog.has_value());

  // Include stdlib + both imports.
  const std::vector<const Program *> imports = {
    has_stdlib ? &stdlib_program : nullptr, &imp1.value(), &imp2.value()};

  // Filter nulls (in case stdlib failed to parse).
  std::vector<const Program *> filtered;
  for (auto * p : imports) {
    if (p) {
      filtered.push_back(p);
    }
  }

  const auto result = analyzer.analyze(main_prog.value(), filtered);
  EXPECT_FALSE(result.has_errors()) << "Expected imported declarations to be merged";
}

// ============================================================================
// Declare Statement Validation
// ============================================================================

TEST_F(SemanticTest, DuplicatePortNamesInDeclaration)
{
  auto result = parseAndAnalyze(R"(
        declare Action MyAction(in target: Vector3, in target: bool)
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Duplicate port name"));
}

TEST_F(SemanticTest, InvalidCategory)
{
  auto result = parseAndAnalyze(R"(
        declare InvalidCategory MyNode()
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Invalid category"));
}

TEST_F(SemanticTest, DuplicateDeclarationNames)
{
  auto result = parseAndAnalyze(R"(
        declare Action MyAction()
        declare Condition MyAction()
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Duplicate declaration"));
}

TEST_F(SemanticTest, DeclarationConflictsWithTree)
{
  auto result = parseAndAnalyze(R"(
        declare Action Main()
        Tree Main() { Sequence {} }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "conflicts with a Tree"));
}

TEST_F(SemanticTest, AllowUsingDeclaredNodeInTree)
{
  auto result = parseAndAnalyze(R"(
        declare Action MyAction(in target: string)
        Tree Main() { MyAction(target: "hello") }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowUsingDeclaredDecorator)
{
  auto result = parseAndAnalyze(R"(
        declare Decorator MyDecorator(in timeout: double)
        Tree Main() {
            @MyDecorator(timeout: 5.0)
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

// ============================================================================
// Local Variable Checks
// ============================================================================

TEST_F(SemanticTest, AllowLocalVarWithInitialValueOnly)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            var msg = "hello"
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowLocalVarWithMatchingTypeAndValue)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            var count: int = 42
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnTypeMismatchInLocalVar)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            var count: int = "hello"
            Sequence {}
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Type mismatch"));
}

TEST_F(SemanticTest, ErrorOnLocalVarWithoutTypeOrValue)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            var unknown
            Sequence {}
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "must have either"));
}

// ============================================================================
// Node Category Validation
// ============================================================================

TEST_F(SemanticTest, DecoratorCannotBeUsedAsNode)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            Delay()
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Decorator"));
}

TEST_F(SemanticTest, OnlyDecoratorNodesAllowedInDecoratorPosition)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main() {
            @Action
            Action(pos: Target)
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "not a Decorator"));
}

TEST_F(SemanticTest, ResolveDecoratorNodeInDecoratorPosition)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main() {
            @Delay
            Action(pos: Target)
        }
    )");

  EXPECT_FALSE(result.has_errors()) << "Unexpected semantic errors";
}

TEST_F(SemanticTest, NonControlNodeCannotHaveChildren)
{
  auto result = parseAndAnalyze(R"(
        declare Action TestAction()
        Tree Main() {
            TestAction() {
                Sequence {}
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "cannot have a children block"));
}

TEST_F(SemanticTest, ControlNodeRequiresChildren)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            Fallback()
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "requires a children block"));
}

// ============================================================================
// Direction Permission Checks
// ============================================================================

TEST_F(SemanticTest, ErrorWhenUsingRefOnNonRefParameter)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main(target: Vector3) {
            Action(pos: ref target)
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "input-only"));
}

TEST_F(SemanticTest, WarnWhenRefParameterNeverUsedForWriteAccess)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main(ref target: Vector3) {
            Action(pos: target)
        }
    )");

  EXPECT_FALSE(result.has_errors());
  EXPECT_TRUE(hasWarning(result, "never used for write access"));
}

TEST_F(SemanticTest, AllowRefParameterUsedWithRef)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main(ref target: Vector3) {
            Action(pos: ref target)
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, AllowOutParameterForOutputPorts)
{
  auto result = parseAndAnalyze(R"(
        declare Action OutputAction(out result: Vector3)
        var Target: Vector3
        Tree Main(out result: Vector3) {
            OutputAction(result: out result)
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorWhenSubTreeRefParamPassedAsIn)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        var Target: Vector3
        Tree Main() {
            SubTree(x: Target)
        }
        Tree SubTree(ref x: Vector3) {
            Action(pos: x)
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "requires"));
}

// ============================================================================
// Expression Type Checks (AssignmentStmt)
// ============================================================================

TEST_F(SemanticTest, ErrorOnAddingIntAndBoolInAssignment)
{
  auto result = parseAndAnalyze(R"(
        var result: int
        var flag: bool
        Tree Main() {
            Sequence {
                result = 30 + flag
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(
    hasError(result, "cannot be applied") || hasError(result, "Operator cannot be applied"));
}

TEST_F(SemanticTest, AllowAddingIntAndIntInAssignment)
{
  auto result = parseAndAnalyze(R"(
        var a: int
        var b: int
        Tree Main() {
            Sequence {
                a = b + 1
            }
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnLogicalOperatorWithNonBool)
{
  auto result = parseAndAnalyze(R"(
        var a: int
        var result: bool
        Tree Main() {
            Sequence {
                result = a && true
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "bool operands"));
}

TEST_F(SemanticTest, ErrorOnAssigningStringToInt)
{
  auto result = parseAndAnalyze(R"(
        var count: int
        Tree Main() {
            Sequence {
                count = "hello"
            }
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Cannot assign"));
}

// ============================================================================
// Positional Argument Validation
// ============================================================================

TEST_F(SemanticTest, AllowPositionalArgumentForSinglePortNode)
{
  auto result = parseAndAnalyze(R"(
        Tree Main() {
            @Repeat(3)
            Sequence {}
        }
    )");

  EXPECT_FALSE(result.has_errors());
}

TEST_F(SemanticTest, ErrorOnPositionalArgumentForMultiPortNode)
{
  auto result = parseAndAnalyze(R"(
        declare Action MultiPort(in a: any, in b: any)
        Tree Main() {
            MultiPort("value")
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "2 ports"));
}

TEST_F(SemanticTest, ErrorOnMultiplePositionalArguments)
{
  auto result = parseAndAnalyze(R"(
        declare Action Action(in pos: Vector3)
        Tree Main() {
            Action("a", "b")
        }
    )");

  EXPECT_TRUE(result.has_errors());
  EXPECT_TRUE(hasError(result, "Only one positional argument"));
}

// ============================================================================
// Symbol Table Tests
// ============================================================================

TEST_F(SemanticTest, SymbolTableBuildsCorrectly)
{
  auto parse_result = parser.parse(R"(
        var GlobalVar: string
        Tree Main(param1: int, ref param2: string) {
            var localVar = 42
            Sequence {}
        }
    )");
  ASSERT_TRUE(parse_result.has_value());

  SymbolTable symbols;
  symbols.build_from_program(parse_result.value());

  // Check global scope
  EXPECT_TRUE(symbols.has_global("GlobalVar"));
  EXPECT_TRUE(symbols.has_global("Main"));

  // Check tree scope
  auto * main_scope = symbols.tree_scope("Main");
  ASSERT_NE(main_scope, nullptr);

  auto * param1 = main_scope->lookup("param1");
  ASSERT_NE(param1, nullptr);
  EXPECT_EQ(param1->kind, SymbolKind::Parameter);
  EXPECT_EQ(param1->type_name, "int");

  auto * param2 = main_scope->lookup("param2");
  ASSERT_NE(param2, nullptr);
  EXPECT_EQ(param2->direction, PortDirection::Ref);

  auto * local = main_scope->lookup("localVar");
  ASSERT_NE(local, nullptr);
  EXPECT_EQ(local->kind, SymbolKind::LocalVariable);
}

// ============================================================================
// Node Registry Tests
// ============================================================================

TEST_F(SemanticTest, NodeRegistryBuildsFromProgram)
{
  auto parse_result = parser.parse(R"(
        declare Action MyAction(in target: Vector3, out result: bool)
        declare Control MyControl()
        Tree SubTree(param: int) { Sequence {} }
    )");
  ASSERT_TRUE(parse_result.has_value());

  NodeRegistry registry;
  registry.build_from_program(parse_result.value());

  // Check declared action
  auto * action = registry.get_node("MyAction");
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->category, NodeCategory::Action);
  EXPECT_EQ(action->port_count(), 2);

  auto * target_port = action->get_port("target");
  ASSERT_NE(target_port, nullptr);
  EXPECT_EQ(target_port->direction, PortDirection::In);

  // Check control
  auto * control = registry.get_node("MyControl");
  ASSERT_NE(control, nullptr);
  EXPECT_TRUE(control->can_have_children());

  // Check tree as subtree
  auto * subtree = registry.get_node("SubTree");
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

  // Int and double are compatible (promotion)
  EXPECT_TRUE(Type::int_type().is_compatible_with(Type::double_type()));
  EXPECT_TRUE(Type::double_type().is_compatible_with(Type::int_type()));

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
  EXPECT_TRUE(Type::from_string("any").equals(Type::any_type()));

  // Custom types
  auto custom = Type::from_string("Vector3");
  EXPECT_TRUE(custom.is_custom());
  EXPECT_EQ(custom.to_string(), "Vector3");
}

TEST_F(SemanticTest, TypeContextResolution)
{
  auto parse_result = parser.parse(R"(
        Tree Main(x: int) {
            var y: string
            var z = 3.14
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
  auto * x_type = ctx.get_type("x");
  ASSERT_NE(x_type, nullptr);
  EXPECT_TRUE(x_type->equals(Type::int_type()));

  auto * y_type = ctx.get_type("y");
  ASSERT_NE(y_type, nullptr);
  EXPECT_TRUE(y_type->equals(Type::string_type()));

  // Check inferred type
  auto * z_type = ctx.get_type("z");
  ASSERT_NE(z_type, nullptr);
  EXPECT_TRUE(z_type->equals(Type::double_type()));
}
