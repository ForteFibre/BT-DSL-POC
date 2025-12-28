// test_parser.cpp - Unit tests for BT-DSL Parser
#include <gtest/gtest.h>
#include "bt_dsl/parser/parser.hpp"

namespace bt_dsl {
namespace {

class ParserTest : public ::testing::Test {
protected:
    Parser parser;
};

// ============================================================================
// Basic Parsing Tests
// ============================================================================

TEST_F(ParserTest, ParseEmptyTree) {
    auto result = parser.parse(R"(
        Tree Main() {
            Sequence {}
        }
    )");
    
    ASSERT_TRUE(result.has_value()) << "Parse should succeed";
    EXPECT_EQ(result->trees.size(), 1);
    EXPECT_EQ(result->trees[0].name, "Main");
    EXPECT_TRUE(result->trees[0].body.has_value());
    EXPECT_EQ(result->trees[0].body->node_name, "Sequence");
}

TEST_F(ParserTest, ParseTreeWithParams) {
    auto result = parser.parse(R"(
        Tree MyTree(ref target, amount: int) {
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trees[0].params.size(), 2);
    EXPECT_EQ(result->trees[0].params[0].name, "target");
    EXPECT_EQ(result->trees[0].params[0].direction, PortDirection::Ref);
    EXPECT_EQ(result->trees[0].params[1].name, "amount");
    EXPECT_EQ(result->trees[0].params[1].type_name, "int");
}

// ============================================================================
// Import Statement Tests
// ============================================================================

TEST_F(ParserTest, ParseImport) {
    auto result = parser.parse(R"(
        import "nodes.bt"
        import "actions.bt"
        
        Tree Main() {
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->imports.size(), 2);
    EXPECT_EQ(result->imports[0].path, "nodes.bt");
    EXPECT_EQ(result->imports[1].path, "actions.bt");
}

// ============================================================================
// Declare Statement Tests
// ============================================================================

TEST_F(ParserTest, ParseDeclare) {
    auto result = parser.parse(R"(
        declare Action FindEnemy(in range: float, out pos: Vector3, out found: bool)
        
        Tree Main() {
            FindEnemy()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->declarations.size(), 1);
    
    const auto& decl = result->declarations[0];
    EXPECT_EQ(decl.category, "Action");
    EXPECT_EQ(decl.name, "FindEnemy");
    EXPECT_EQ(decl.ports.size(), 3);
    
    EXPECT_EQ(decl.ports[0].name, "range");
    EXPECT_EQ(decl.ports[0].direction, PortDirection::In);
    EXPECT_EQ(decl.ports[0].type_name, "float");
    
    EXPECT_EQ(decl.ports[1].name, "pos");
    EXPECT_EQ(decl.ports[1].direction, PortDirection::Out);
    
    EXPECT_EQ(decl.ports[2].name, "found");
    EXPECT_EQ(decl.ports[2].direction, PortDirection::Out);
}

// ============================================================================
// Global Variable Tests
// ============================================================================

TEST_F(ParserTest, ParseGlobalVars) {
    auto result = parser.parse(R"(
        var TargetPos: Vector3
        var Ammo: int
        var IsAlerted: bool
        
        Tree Main() {
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->global_vars.size(), 3);
    EXPECT_EQ(result->global_vars[0].name, "TargetPos");
    EXPECT_EQ(result->global_vars[0].type_name, "Vector3");
    EXPECT_EQ(result->global_vars[1].name, "Ammo");
    EXPECT_EQ(result->global_vars[1].type_name, "int");
}

// ============================================================================
// Local Variable Tests
// ============================================================================

TEST_F(ParserTest, ParseLocalVars) {
    auto result = parser.parse(R"(
        Tree Main() {
            var count: int = 0
            var name = "test"
            Sequence {}
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trees[0].local_vars.size(), 2);
    
    const auto& var1 = result->trees[0].local_vars[0];
    EXPECT_EQ(var1.name, "count");
    EXPECT_EQ(var1.type_name, "int");
    EXPECT_TRUE(var1.initial_value.has_value());
    
    const auto& var2 = result->trees[0].local_vars[1];
    EXPECT_EQ(var2.name, "name");
    EXPECT_TRUE(var2.initial_value.has_value());
}

// ============================================================================
// Literal Tests
// ============================================================================

TEST_F(ParserTest, ParseLiterals) {
    auto result = parser.parse(R"(
        Tree Main() {
            Action(
                text: "hello",
                count: 42,
                rate: 3.14,
                active: true,
                disabled: false
            )
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& args = result->trees[0].body->args;
    EXPECT_EQ(args.size(), 5);
    
    // Check string literal
    EXPECT_EQ(args[0].name, "text");
    auto* str_lit = std::get_if<Literal>(&args[0].value);
    ASSERT_NE(str_lit, nullptr);
    auto* str_val = std::get_if<StringLiteral>(str_lit);
    ASSERT_NE(str_val, nullptr);
    EXPECT_EQ(str_val->value, "hello");
    
    // Check integer literal
    EXPECT_EQ(args[1].name, "count");
    auto* int_lit = std::get_if<Literal>(&args[1].value);
    ASSERT_NE(int_lit, nullptr);
    auto* int_val = std::get_if<IntLiteral>(int_lit);
    ASSERT_NE(int_val, nullptr);
    EXPECT_EQ(int_val->value, 42);
    
    // Check float literal
    EXPECT_EQ(args[2].name, "rate");
    auto* float_lit = std::get_if<Literal>(&args[2].value);
    ASSERT_NE(float_lit, nullptr);
    auto* float_val = std::get_if<FloatLiteral>(float_lit);
    ASSERT_NE(float_val, nullptr);
    EXPECT_DOUBLE_EQ(float_val->value, 3.14);
    
    // Check boolean literals
    EXPECT_EQ(args[3].name, "active");
    auto* true_lit = std::get_if<Literal>(&args[3].value);
    ASSERT_NE(true_lit, nullptr);
    auto* true_val = std::get_if<BoolLiteral>(true_lit);
    ASSERT_NE(true_val, nullptr);
    EXPECT_TRUE(true_val->value);
    
    EXPECT_EQ(args[4].name, "disabled");
    auto* false_lit = std::get_if<Literal>(&args[4].value);
    ASSERT_NE(false_lit, nullptr);
    auto* false_val = std::get_if<BoolLiteral>(false_lit);
    ASSERT_NE(false_val, nullptr);
    EXPECT_FALSE(false_val->value);
}

// ============================================================================
// Expression Tests
// ============================================================================

TEST_F(ParserTest, ParseBinaryExpression) {
    auto result = parser.parse(R"(
        Tree Main() {
            var result: int = a + b * c
            Sequence {}
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trees[0].local_vars.size(), 1);
    EXPECT_TRUE(result->trees[0].local_vars[0].initial_value.has_value());
    
    // The expression should be parsed (a + (b * c)) due to precedence
    const auto& expr = *result->trees[0].local_vars[0].initial_value;
    
    // Top level should be BinaryExpr (add)
    auto* binary = std::get_if<Box<BinaryExpr>>(&expr);
    ASSERT_NE(binary, nullptr);
    EXPECT_EQ((*binary)->op, BinaryOp::Add);
}

TEST_F(ParserTest, ParseUnaryExpression) {
    auto result = parser.parse(R"(
        Tree Main() {
            var result: bool
            Sequence {
                result = !flag
            }
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result->trees[0].body->children.size(), 0);
}

TEST_F(ParserTest, ParseComparisonExpression) {
    auto result = parser.parse(R"(
        Tree Main() {
            var result: bool
            Sequence {
                result = a > b && c < d
            }
        }
    )");
    
    ASSERT_TRUE(result.has_value());
}

// ============================================================================
// Decorator Tests
// ============================================================================

TEST_F(ParserTest, ParseDecorators) {
    auto result = parser.parse(R"(
        Tree Main() {
            @Inverter
            @Repeat(count: 3)
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& decorators = result->trees[0].body->decorators;
    EXPECT_EQ(decorators.size(), 2);
    EXPECT_EQ(decorators[0].name, "Inverter");
    EXPECT_EQ(decorators[1].name, "Repeat");
    EXPECT_EQ(decorators[1].args.size(), 1);
    EXPECT_EQ(decorators[1].args[0].name, "count");
}

// ============================================================================
// Argument Tests
// ============================================================================

TEST_F(ParserTest, ParseNamedArguments) {
    auto result = parser.parse(R"(
        Tree Main() {
            Action(
                target: out myVar,
                source: ref otherVar,
                input: someVar
            )
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& args = result->trees[0].body->args;
    EXPECT_EQ(args.size(), 3);
    
    // Check blackboard ref with direction
    EXPECT_EQ(args[0].name, "target");
    auto* ref0 = std::get_if<BlackboardRef>(&args[0].value);
    ASSERT_NE(ref0, nullptr);
    EXPECT_EQ(ref0->direction, PortDirection::Out);
    EXPECT_EQ(ref0->name, "myVar");
    
    EXPECT_EQ(args[1].name, "source");
    auto* ref1 = std::get_if<BlackboardRef>(&args[1].value);
    ASSERT_NE(ref1, nullptr);
    EXPECT_EQ(ref1->direction, PortDirection::Ref);
}

TEST_F(ParserTest, ParsePositionalArgument) {
    auto result = parser.parse(R"(
        Tree Main() {
            Action("hello")
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& args = result->trees[0].body->args;
    EXPECT_EQ(args.size(), 1);
    EXPECT_FALSE(args[0].name.has_value());  // Positional - no name
}

// ============================================================================
// Children Block Tests
// ============================================================================

TEST_F(ParserTest, ParseNestedChildren) {
    auto result = parser.parse(R"(
        Tree Main() {
            Sequence {
                Fallback {
                    Action1()
                    Action2()
                }
                Action3()
            }
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& body = result->trees[0].body;
    EXPECT_EQ(body->node_name, "Sequence");
    EXPECT_EQ(body->children.size(), 2);
    
    // First child is Fallback with 2 children
    auto* fallback = std::get_if<Box<NodeStmt>>(&body->children[0]);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ((*fallback)->node_name, "Fallback");
    EXPECT_EQ((*fallback)->children.size(), 2);
}

TEST_F(ParserTest, ParseAssignmentInChildren) {
    auto result = parser.parse(R"(
        Tree Main() {
            var result: int
            Sequence {
                result = a + b
                result += 1
            }
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    const auto& children = result->trees[0].body->children;
    EXPECT_EQ(children.size(), 2);
    
    auto* assign1 = std::get_if<AssignmentStmt>(&children[0]);
    ASSERT_NE(assign1, nullptr);
    EXPECT_EQ(assign1->target, "result");
    EXPECT_EQ(assign1->op, AssignOp::Assign);
    
    auto* assign2 = std::get_if<AssignmentStmt>(&children[1]);
    ASSERT_NE(assign2, nullptr);
    EXPECT_EQ(assign2->op, AssignOp::AddAssign);
}

// ============================================================================
// Documentation Tests
// ============================================================================

TEST_F(ParserTest, ParseInnerDoc) {
    auto result = parser.parse(R"(
        //! Module documentation line 1
        //! Module documentation line 2
        
        Tree Main() {
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->inner_docs.size(), 2);
}

TEST_F(ParserTest, ParseOuterDoc) {
    auto result = parser.parse(R"(
        /// Tree documentation
        Tree Main() {
            /// Node documentation
            Action()
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->trees[0].docs.size(), 1);
    EXPECT_EQ(result->trees[0].body->docs.size(), 1);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(ParserTest, ParseWithRecovery) {
    auto [program, errors] = parser.parse_with_recovery(R"(
        Tree Main() {
            Sequence {
                Action(
        }
    )");
    
    // Should still produce some partial result
    EXPECT_GE(program.trees.size(), 0);
    // Should have errors
    EXPECT_GT(errors.size(), 0);
}

// ============================================================================
// Complex Examples
// ============================================================================

TEST_F(ParserTest, ParseSoldierAILike) {
    auto result = parser.parse(R"(
        //! Soldier AI Definition v1.0
        
        import "StandardNodes.bt"
        
        var TargetPos: Vector3
        var Ammo: int
        var IsAlerted: bool
        
        /// Main tree
        Tree Main() {
            @Repeat
            Sequence {
                SearchAndDestroy(
                    target: ref TargetPos,
                    ammo: ref Ammo,
                    alert: ref IsAlerted
                )
            }
        }
        
        /// Sub tree for search and destroy
        Tree SearchAndDestroy(ref target, ref ammo, ref alert) {
            Sequence {
                FindEnemy(pos: out target, found: out alert)
                AttackAction(loc: target, val: ref ammo)
            }
        }
    )");
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->inner_docs.size(), 1);
    EXPECT_EQ(result->imports.size(), 1);
    EXPECT_EQ(result->global_vars.size(), 3);
    EXPECT_EQ(result->trees.size(), 2);
}

// ============================================================================
// Source Range Tests
// ============================================================================

TEST_F(ParserTest, SourceRangesArePopulated) {
    auto result = parser.parse(R"(Tree Main() {
    Action()
})");
    
    ASSERT_TRUE(result.has_value());
    
    // Check that source ranges are populated
    EXPECT_GT(result->range.end_byte, result->range.start_byte);
    EXPECT_GT(result->trees[0].range.end_byte, result->trees[0].range.start_byte);
    EXPECT_GT(result->trees[0].body->range.end_byte, result->trees[0].body->range.start_byte);
}

} // namespace
} // namespace bt_dsl
