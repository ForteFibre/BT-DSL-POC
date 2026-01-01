// test_parser.cpp - Unit tests for BT-DSL Parser
#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

#include "bt_dsl/parser/parser.hpp"

namespace bt_dsl
{
namespace
{

const NodeStmt * first_node_stmt(const std::vector<Statement> & block)
{
  for (const auto & s : block) {
    if (const auto * n = std::get_if<Box<NodeStmt>>(&s)) {
      return n->get();
    }
  }
  return nullptr;
}

const NodeStmt * find_node_stmt(const std::vector<Statement> & block, std::string_view name)
{
  for (const auto & s : block) {
    if (const auto * n = std::get_if<Box<NodeStmt>>(&s)) {
      if ((*n)->node_name == name) {
        return n->get();
      }
    }
  }
  return nullptr;
}

class ParserTest : public ::testing::Test
{
protected:
  Parser parser;
};

bool has_parse_error(const ParseResult<Program> & r, const std::string & substring)
{
  if (!r.has_error()) {
    return false;
  }
  const auto & errs = r.error();
  return std::any_of(errs.begin(), errs.end(), [&substring](const ParseError & e) {
    return e.message.find(substring) != std::string::npos;
  });
}

// ============================================================================
// Basic Parsing Tests
// ============================================================================

TEST_F(ParserTest, ParseEmptyTree)
{
  auto result = parser.parse(R"(
        tree Main() {
            Sequence {}
        }
    )");

  ASSERT_TRUE(result.has_value()) << "Parse should succeed";
  EXPECT_EQ(result->trees.size(), 1);
  EXPECT_EQ(result->trees[0].name, "Main");
  ASSERT_GE(result->trees[0].body.size(), 1U);
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->node_name, "Sequence");
}

TEST_F(ParserTest, ParseTreeWithParams)
{
  auto result = parser.parse(R"(
        tree MyTree(ref target: any, amount: int) {
            Action();
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

TEST_F(ParserTest, ParseImport)
{
  auto result = parser.parse(R"(
        import "nodes.bt"
        import "actions.bt"
        
        tree Main() {
            Action();
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

TEST_F(ParserTest, ParseExtern)
{
  auto result = parser.parse(R"(
        extern action FindEnemy(in range: float, out pos: Vector3, out found: bool);
        
        tree Main() {
            FindEnemy();
        }
    )");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->declarations.size(), 1);

  const auto & decl = result->declarations[0];
  EXPECT_EQ(decl.category, "action");
  EXPECT_EQ(decl.name, "FindEnemy");
  EXPECT_EQ(decl.ports.size(), 3);

  EXPECT_EQ(decl.ports[0].name, "range");
  ASSERT_TRUE(decl.ports[0].direction.has_value());
  EXPECT_EQ(*decl.ports[0].direction, PortDirection::In);
  EXPECT_EQ(decl.ports[0].type_name, "float");

  EXPECT_EQ(decl.ports[1].name, "pos");
  ASSERT_TRUE(decl.ports[1].direction.has_value());
  EXPECT_EQ(*decl.ports[1].direction, PortDirection::Out);

  EXPECT_EQ(decl.ports[2].name, "found");
  ASSERT_TRUE(decl.ports[2].direction.has_value());
  EXPECT_EQ(*decl.ports[2].direction, PortDirection::Out);
}

// ============================================================================
// Global Variable Tests
// ============================================================================

TEST_F(ParserTest, ParseGlobalVars)
{
  auto result = parser.parse(R"(
    var TargetPos: Vector3;
    var Ammo: int;
    var IsAlerted: bool;
        
        tree Main() {
            Action();
        }
    )");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->global_vars.size(), 3);
  EXPECT_EQ(result->global_vars[0].name, "TargetPos");
  ASSERT_TRUE(result->global_vars[0].type_name.has_value());
  EXPECT_EQ(*result->global_vars[0].type_name, "Vector3");
  EXPECT_EQ(result->global_vars[1].name, "Ammo");
  ASSERT_TRUE(result->global_vars[1].type_name.has_value());
  EXPECT_EQ(*result->global_vars[1].type_name, "int");
}

// ============================================================================
// Local Variable Tests
// ============================================================================

TEST_F(ParserTest, ParseLocalVars)
{
  auto result = parser.parse(R"(
        tree Main() {
            var count: int = 0;
            var name = "test";
            Sequence {}
        }
    )");

  ASSERT_TRUE(result.has_value());
  ASSERT_GE(result->trees[0].body.size(), 3U);

  const auto * var1 = std::get_if<BlackboardDeclStmt>(result->trees[0].body.data());
  ASSERT_NE(var1, nullptr);
  EXPECT_EQ(var1->name, "count");
  ASSERT_TRUE(var1->type_name.has_value());
  EXPECT_EQ(*var1->type_name, "int");
  EXPECT_TRUE(var1->initial_value.has_value());

  const auto * var2 = std::get_if<BlackboardDeclStmt>(result->trees[0].body.data() + 1);
  ASSERT_NE(var2, nullptr);
  EXPECT_EQ(var2->name, "name");
  EXPECT_TRUE(var2->initial_value.has_value());
}

TEST_F(ParserTest, AllowsUnderscoreIdentifier)
{
  auto result = parser.parse(R"(
        tree Main() {
            var _: int = 0;
            var x: _ = 1;
            _ = 2;
            Sequence {}
        }
    )");

  ASSERT_TRUE(result.has_value()) << "Parse should succeed";
  ASSERT_GE(result->trees.size(), 1U);
  ASSERT_GE(result->trees[0].body.size(), 4U);

  const auto * v0 = std::get_if<BlackboardDeclStmt>(result->trees[0].body.data());
  ASSERT_NE(v0, nullptr);
  EXPECT_EQ(v0->name, "_");
  ASSERT_TRUE(v0->type_name.has_value());
  EXPECT_EQ(*v0->type_name, "int");

  const auto * v1 = std::get_if<BlackboardDeclStmt>(result->trees[0].body.data() + 1);
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(v1->name, "x");
  ASSERT_TRUE(v1->type_name.has_value());
  EXPECT_EQ(*v1->type_name, "_");
}

// ============================================================================
// Literal Tests
// ============================================================================

TEST_F(ParserTest, ParseLiterals)
{
  auto result = parser.parse(R"(
        tree Main() {
            Action(
                text: "hello",
                count: 42,
                rate: 3.14,
                active: true,
                disabled: false
            );
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  const auto & args = root->args;
  EXPECT_EQ(args.size(), 5);

  // Check string literal
  EXPECT_EQ(args[0].name, "text");
  const auto * expr0 = std::get_if<Expression>(&args[0].value);
  ASSERT_NE(expr0, nullptr);
  const auto * str_lit = std::get_if<Literal>(expr0);
  ASSERT_NE(str_lit, nullptr);
  const auto * str_val = std::get_if<StringLiteral>(str_lit);
  ASSERT_NE(str_val, nullptr);
  EXPECT_EQ(str_val->value, "hello");

  // Check integer literal
  EXPECT_EQ(args[1].name, "count");
  const auto * expr1 = std::get_if<Expression>(&args[1].value);
  ASSERT_NE(expr1, nullptr);
  const auto * int_lit = std::get_if<Literal>(expr1);
  ASSERT_NE(int_lit, nullptr);
  const auto * int_val = std::get_if<IntLiteral>(int_lit);
  ASSERT_NE(int_val, nullptr);
  EXPECT_EQ(int_val->value, 42);

  // Check float literal
  EXPECT_EQ(args[2].name, "rate");
  const auto * expr2 = std::get_if<Expression>(&args[2].value);
  ASSERT_NE(expr2, nullptr);
  const auto * float_lit = std::get_if<Literal>(expr2);
  ASSERT_NE(float_lit, nullptr);
  const auto * float_val = std::get_if<FloatLiteral>(float_lit);
  ASSERT_NE(float_val, nullptr);
  EXPECT_DOUBLE_EQ(float_val->value, 3.14);

  // Check boolean literals
  EXPECT_EQ(args[3].name, "active");
  const auto * expr3 = std::get_if<Expression>(&args[3].value);
  ASSERT_NE(expr3, nullptr);
  const auto * true_lit = std::get_if<Literal>(expr3);
  ASSERT_NE(true_lit, nullptr);
  const auto * true_val = std::get_if<BoolLiteral>(true_lit);
  ASSERT_NE(true_val, nullptr);
  EXPECT_TRUE(true_val->value);

  EXPECT_EQ(args[4].name, "disabled");
  const auto * expr4 = std::get_if<Expression>(&args[4].value);
  ASSERT_NE(expr4, nullptr);
  const auto * false_lit = std::get_if<Literal>(expr4);
  ASSERT_NE(false_lit, nullptr);
  const auto * false_val = std::get_if<BoolLiteral>(false_lit);
  ASSERT_NE(false_val, nullptr);
  EXPECT_FALSE(false_val->value);
}

TEST_F(ParserTest, RejectPositionalArgumentSyntax)
{
  // Reference: docs/reference/syntax.md 2.6.4 (argument := identifier ':' argument_expr)
  // Ensure positional arguments are rejected by the parser.
  auto result = parser.parse(R"(
    tree Main() {
      Repeat(3) {
        Sequence {}
      }
    }
  )");

  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(result.has_error());
  EXPECT_TRUE(
    has_parse_error(result, "Missing expected syntax") || has_parse_error(result, "Syntax error"));
}

TEST_F(ParserTest, ParseFloatExponentLiteral)
{
  auto result = parser.parse(R"(
    tree Main() {
      Action(x: 1e3);
    }
  )");

  ASSERT_TRUE(result.has_value()) << "Exponent float literal should parse";
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->args.size(), 1U);
  const auto * expr = std::get_if<Expression>(&root->args[0].value);
  ASSERT_NE(expr, nullptr);
  const auto * lit = std::get_if<Literal>(expr);
  ASSERT_NE(lit, nullptr);
  const auto * fv = std::get_if<FloatLiteral>(lit);
  ASSERT_NE(fv, nullptr);
  EXPECT_DOUBLE_EQ(fv->value, 1000.0);
}

TEST_F(ParserTest, RejectIntegerLiteralOverflow)
{
  auto result = parser.parse(R"(
    tree Main() {
      Action(x: 999999999999999999999999999999999999999);
    }
  )");

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(result.has_error());
  bool found = false;
  for (const auto & e : result.error()) {
    if (
      e.message.find("Integer literal out of range") != std::string::npos ||
      e.message.find("Invalid integer literal") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected integer overflow to be rejected";
}

TEST_F(ParserTest, RejectFloatLiteralOverflow)
{
  auto result = parser.parse(R"(
    tree Main() {
      Action(x: 1e999999999999999999999);
    }
  )");

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(result.has_error());
  bool found = false;
  for (const auto & e : result.error()) {
    if (e.message.find("Invalid float literal") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected float overflow to be rejected";
}

TEST_F(ParserTest, ParseStringEscapes)
{
  auto result = parser.parse(R"(
        tree Main() {
            Action(
        a: "\n",
        b: "\t",
        c: "\r",
        d: "\0",
        e: "\b",
        f: "\f",
        g: "\"",
        h: "\\",
        i: "\u{41}",
        j: "\u{1F600}"
            );
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  const auto & args = root->args;
  ASSERT_EQ(args.size(), 10U);

  auto get_str = [&](size_t idx) -> std::string {
    const auto * expr = std::get_if<Expression>(&args[idx].value);
    EXPECT_NE(expr, nullptr);
    const auto * lit = std::get_if<Literal>(expr);
    EXPECT_NE(lit, nullptr);
    const auto * s = std::get_if<StringLiteral>(lit);
    EXPECT_NE(s, nullptr);
    return s ? s->value : std::string{};
  };

  EXPECT_EQ(get_str(0), std::string("\n"));
  EXPECT_EQ(get_str(1), std::string("\t"));
  EXPECT_EQ(get_str(2), std::string("\r"));
  EXPECT_EQ(get_str(3), std::string(1, '\0'));
  EXPECT_EQ(get_str(4), std::string("\b"));
  EXPECT_EQ(get_str(5), std::string("\f"));
  EXPECT_EQ(get_str(6), std::string("\""));
  EXPECT_EQ(get_str(7), std::string("\\"));
  EXPECT_EQ(get_str(8), std::string("A"));
  EXPECT_EQ(get_str(9), std::string("\xF0\x9F\x98\x80"));
}

// ============================================================================
// Expression Tests
// ============================================================================

TEST_F(ParserTest, ParseBinaryExpression)
{
  auto result = parser.parse(R"(
        tree Main() {
            var result: int = a + b * c;
            Sequence {}
        }
    )");

  ASSERT_TRUE(result.has_value());
  ASSERT_GE(result->trees[0].body.size(), 2U);
  const auto * decl = std::get_if<BlackboardDeclStmt>(result->trees[0].body.data());
  ASSERT_NE(decl, nullptr);
  ASSERT_TRUE(decl->initial_value.has_value());

  // The expression should be parsed (a + (b * c)) due to precedence
  const auto & expr = *decl->initial_value;

  // Top level should be BinaryExpr (add)
  const auto * binary = std::get_if<Box<BinaryExpr>>(&expr);
  ASSERT_NE(binary, nullptr);
  EXPECT_EQ((*binary)->op, BinaryOp::Add);
}

TEST_F(ParserTest, ParseUnaryExpression)
{
  auto result = parser.parse(R"(
        tree Main() {
            var result: bool;
            Sequence {
                result = !flag;
            }
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * seq = find_node_stmt(result->trees[0].body, "Sequence");
  ASSERT_NE(seq, nullptr);
  EXPECT_GT(seq->children.size(), 0U);
}

TEST_F(ParserTest, ParseComparisonExpression)
{
  auto result = parser.parse(R"(
        tree Main() {
            var result: bool;
            Sequence {
                result = a > b && c < d;
            }
        }
    )");

  ASSERT_TRUE(result.has_value());
}

TEST_F(ParserTest, RejectChainedComparisonOperators)
{
  // Spec: chained comparisons are a syntax error (docs/reference/syntax.md).
  auto result = parser.parse(R"(
    tree Main() {
      var result: bool;
      Sequence {
        result = a < b < c;
      }
    }
  )");

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(result.has_error());

  bool found = false;
  for (const auto & e : result.error()) {
    if (
      e.message.find("Chained comparison operators") != std::string::npos ||
      e.message.find("Syntax error") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected chained-comparison syntax error";
}

TEST_F(ParserTest, RejectChainedEqualityOperators)
{
  // Spec: chained equality is a syntax error (docs/reference/syntax.md).
  auto result = parser.parse(R"(
    tree Main() {
      var result: bool;
      Sequence {
        result = a == b == c;
      }
    }
  )");

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(result.has_error());

  bool found = false;
  for (const auto & e : result.error()) {
    if (
      e.message.find("Chained equality operators") != std::string::npos ||
      e.message.find("Syntax error") != std::string::npos) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Expected chained-equality syntax error";
}

// ============================================================================
// Precondition Tests (new DSL)
// ============================================================================

TEST_F(ParserTest, ParsePreconditions)
{
  auto result = parser.parse(R"(
        tree Main() {
            @guard(target != null)
            Action();
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->node_name, "Action");
  EXPECT_EQ(root->preconditions.size(), 1U);
}

// ============================================================================
// Argument Tests
// ============================================================================

TEST_F(ParserTest, ParseNamedArguments)
{
  auto result = parser.parse(R"(
        tree Main() {
            Action(
                target: out var myVar,
                source: ref otherVar,
                input: someVar
            );
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  const auto & args = root->args;
  EXPECT_EQ(args.size(), 3);

  // Check inline out var decl
  EXPECT_EQ(args[0].name, "target");
  ASSERT_TRUE(args[0].direction.has_value());
  EXPECT_EQ(*args[0].direction, PortDirection::Out);
  const auto * decl0 = std::get_if<InlineBlackboardDecl>(&args[0].value);
  ASSERT_NE(decl0, nullptr);
  EXPECT_EQ(decl0->name, "myVar");

  EXPECT_EQ(args[1].name, "source");
  ASSERT_TRUE(args[1].direction.has_value());
  EXPECT_EQ(*args[1].direction, PortDirection::Ref);
  const auto * expr1 = std::get_if<Expression>(&args[1].value);
  ASSERT_NE(expr1, nullptr);
  const auto * ref1 = std::get_if<VarRef>(expr1);
  ASSERT_NE(ref1, nullptr);
  EXPECT_EQ(ref1->name, "otherVar");
}

TEST_F(ParserTest, ParsePositionalArgument)
{
  auto result = parser.parse(R"(
        tree Main() {
            Action("hello");
        }
    )");

  // Reference syntax requires named arguments only.
  EXPECT_FALSE(result.has_value());
}

TEST_F(ParserTest, RejectMultiplePositionalArguments)
{
  auto result = parser.parse(R"(
        tree Main() {
            Action("a", "b");
        }
    )");

  // Reference syntax requires named arguments only.
  EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Children Block Tests
// ============================================================================

TEST_F(ParserTest, ParseNestedChildren)
{
  auto result = parser.parse(R"(
        tree Main() {
            Sequence {
                Fallback {
                    Action1();
                    Action2();
                }
                Action3();
            }
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * body = first_node_stmt(result->trees[0].body);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->node_name, "Sequence");
  EXPECT_EQ(body->children.size(), 2U);

  // First child is Fallback with 2 children
  const auto * fallback = std::get_if<Box<NodeStmt>>(body->children.data());
  ASSERT_NE(fallback, nullptr);
  EXPECT_EQ((*fallback)->node_name, "Fallback");
  EXPECT_EQ((*fallback)->children.size(), 2U);
}

TEST_F(ParserTest, ParseAssignmentInChildren)
{
  auto result = parser.parse(R"(
        tree Main() {
            var result: int;
            Sequence {
                result = a + b;
                result += 1;
            }
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * seq = find_node_stmt(result->trees[0].body, "Sequence");
  ASSERT_NE(seq, nullptr);
  const auto & children = seq->children;
  EXPECT_EQ(children.size(), 2U);

  const auto * assign1 = std::get_if<AssignmentStmt>(children.data());
  ASSERT_NE(assign1, nullptr);
  EXPECT_EQ(assign1->target, "result");
  EXPECT_EQ(assign1->op, AssignOp::Assign);

  const auto * assign2 = std::get_if<AssignmentStmt>(children.data() + 1);
  ASSERT_NE(assign2, nullptr);
  EXPECT_EQ(assign2->op, AssignOp::AddAssign);
}

TEST_F(ParserTest, ParseAssignmentDocsAndPreconditions)
{
  auto result = parser.parse(R"(
        tree Main() {
            var result: int;
            Sequence {
                /// assignment doc
                @success_if(result == 0)
                result = 1;
            }
        }
    )");

  ASSERT_TRUE(result.has_value());
  const NodeStmt * seq = find_node_stmt(result->trees[0].body, "Sequence");
  ASSERT_NE(seq, nullptr);
  ASSERT_EQ(seq->children.size(), 1U);

  const auto * assign = std::get_if<AssignmentStmt>(seq->children.data());
  ASSERT_NE(assign, nullptr);
  EXPECT_EQ(assign->docs.size(), 1U);
  EXPECT_EQ(assign->preconditions.size(), 1U);
  EXPECT_EQ(assign->preconditions[0].kind, "success_if");
}

// ============================================================================
// Documentation Tests
// ============================================================================

TEST_F(ParserTest, ParseInnerDoc)
{
  auto result = parser.parse(R"(
        //! Module documentation line 1
        //! Module documentation line 2
        
        tree Main() {
            Action();
        }
    )");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->inner_docs.size(), 2);
}

TEST_F(ParserTest, ParseOuterDoc)
{
  auto result = parser.parse(R"(
        /// Tree documentation
        tree Main() {
            /// Node documentation
            Action();
        }
    )");

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->trees[0].docs.size(), 1);
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->docs.size(), 1U);
}

TEST_F(ParserTest, DocCommentsDoNotContainCarriageReturnWithCRLF)
{
  const std::string src = std::string("//! Module doc\r\n") + std::string("/// Tree doc\r\n") +
                          std::string("tree Main() {\r\n") + std::string("  /// Node doc\r\n") +
                          std::string("  Action();\r\n") + std::string("}\r\n");

  auto result = parser.parse(src);
  ASSERT_TRUE(result.has_value());

  ASSERT_EQ(result->inner_docs.size(), 1U);
  EXPECT_EQ(result->inner_docs[0].find('\r'), std::string::npos);

  ASSERT_EQ(result->trees.size(), 1U);
  ASSERT_EQ(result->trees[0].docs.size(), 1U);
  EXPECT_EQ(result->trees[0].docs[0].find('\r'), std::string::npos);

  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->docs.size(), 1U);
  EXPECT_EQ(root->docs[0].find('\r'), std::string::npos);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(ParserTest, ParseWithRecovery)
{
  auto [program, errors] = parser.parse_with_recovery(R"(
        tree Main() {
            Sequence {
                Action(
        }
    )");

  // Should still produce some partial result
  EXPECT_GE(program.trees.size(), 0);
  // Should have errors
  EXPECT_GT(errors.size(), 0);
}

TEST_F(ParserTest, RecoveryUsesMissingExprForMissingPreconditionExpression)
{
  auto [program, errors] = parser.parse_with_recovery(R"(
        tree Main() {
            @success_if() Action();
        }
    )");

  ASSERT_FALSE(program.trees.empty());
  ASSERT_GT(errors.size(), 0U);

  const NodeStmt * n = first_node_stmt(program.trees[0].body);
  ASSERT_NE(n, nullptr);
  ASSERT_FALSE(n->preconditions.empty());
  EXPECT_TRUE(std::holds_alternative<MissingExpr>(n->preconditions[0].condition));
}

TEST_F(ParserTest, RecoveryUsesMissingExprForMissingArgumentExpression)
{
  auto [program, errors] = parser.parse_with_recovery(R"(
        tree Main() {
            Action(x:);
        }
    )");

  ASSERT_FALSE(program.trees.empty());
  ASSERT_GT(errors.size(), 0U);

  const NodeStmt * n = first_node_stmt(program.trees[0].body);
  ASSERT_NE(n, nullptr);
  ASSERT_FALSE(n->args.empty());

  const auto * expr = std::get_if<Expression>(&n->args[0].value);
  ASSERT_NE(expr, nullptr);
  EXPECT_TRUE(std::holds_alternative<MissingExpr>(*expr));
}

TEST_F(ParserTest, RecoveryUsesMissingExprForMissingAssignmentRhs)
{
  auto [program, errors] = parser.parse_with_recovery(R"(
        tree Main() {
            var x: int = 0;
            x = ;
        }
    )");

  ASSERT_FALSE(program.trees.empty());
  ASSERT_GT(errors.size(), 0U);
  ASSERT_GE(program.trees[0].body.size(), 2U);

  const auto * assign = std::get_if<AssignmentStmt>(program.trees[0].body.data() + 1);
  ASSERT_NE(assign, nullptr);
  EXPECT_TRUE(std::holds_alternative<MissingExpr>(assign->value));
}

TEST_F(ParserTest, RecoveryUsesMissingExprForMissingConstInitializer)
{
  auto [program, errors] = parser.parse_with_recovery(R"(
        tree Main() {
            const C = ;
            Action();
        }
    )");

  ASSERT_FALSE(program.trees.empty());
  ASSERT_GT(errors.size(), 0U);
  ASSERT_GE(program.trees[0].body.size(), 1U);

  const auto * c = std::get_if<ConstDeclStmt>(program.trees[0].body.data() + 0);
  ASSERT_NE(c, nullptr);
  EXPECT_TRUE(std::holds_alternative<MissingExpr>(c->value));
}

// ============================================================================
// Complex Examples
// ============================================================================

TEST_F(ParserTest, ParseSoldierAILike)
{
  auto result = parser.parse(R"(
        //! Soldier AI Definition v1.0
        
        import "StandardNodes.bt"
        
  var TargetPos: Vector3;
  var Ammo: int;
  var IsAlerted: bool;
        
        /// Main tree
        tree Main() {
            Repeat {
            Sequence {
                SearchAndDestroy(
                    target: ref TargetPos,
                    ammo: ref Ammo,
                    alert: ref IsAlerted
                );
            }
            }
        }
        
        /// Sub tree for search and destroy
        tree SearchAndDestroy(ref target: Vector3, ref ammo: int, ref alert: bool) {
            Sequence {
                FindEnemy(pos: out target, found: out alert);
                AttackAction(loc: target, val: ref ammo);
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

TEST_F(ParserTest, SourceRangesArePopulated)
{
  auto result = parser.parse(R"(tree Main() {
    Action();
})");

  ASSERT_TRUE(result.has_value());

  // Check that source ranges are populated
  EXPECT_GT(result->range.end_byte, result->range.start_byte);
  EXPECT_GT(result->trees[0].range.end_byte, result->trees[0].range.start_byte);
  const NodeStmt * root = first_node_stmt(result->trees[0].body);
  ASSERT_NE(root, nullptr);
  EXPECT_GT(root->range.end_byte, root->range.start_byte);
}

}  // namespace
}  // namespace bt_dsl
