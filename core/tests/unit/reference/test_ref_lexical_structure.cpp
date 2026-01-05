// tests/unit/reference/test_ref_lexical_structure.cpp
// Reference compliance tests for: 1. Lexical Structure (lexical-structure.md)
//
// Tests that the lexer/parser correctly handles:
// - Identifiers and keywords
// - Literals (integer, float, string, boolean, null)
// - Comments (line, block, documentation)
// - Escape sequences in strings

#include <gtest/gtest.h>

#include <string>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/syntax/frontend.hpp"

using namespace bt_dsl;

namespace
{

// Helper to check if parsing succeeds without errors
bool parses_ok(const std::string & src)
{
  auto unit = parse_source(src);
  return unit && unit->diags.empty();
}

// Helper to check if parsing fails with errors
bool parses_with_error(const std::string & src)
{
  auto unit = parse_source(src);
  return !unit || !unit->diags.empty();
}

// Helper to get parsed program
Program * get_program(std::unique_ptr<ParsedUnit> & unit, const std::string & src)
{
  unit = parse_source(src);
  if (!unit || !unit->diags.empty()) {
    return nullptr;
  }
  return unit->program;
}

}  // namespace

// ============================================================================
// 1.3.1 Identifiers
// Reference: identifier = /[a-zA-Z_][a-zA-Z0-9_]*/ - keyword
// ============================================================================

TEST(RefLexicalStructure, IdentifierBasic)
{
  // Valid identifiers
  EXPECT_TRUE(parses_ok("var foo: int32;"));
  EXPECT_TRUE(parses_ok("var Foo: int32;"));
  EXPECT_TRUE(parses_ok("var _foo: int32;"));
  EXPECT_TRUE(parses_ok("var foo123: int32;"));
  EXPECT_TRUE(parses_ok("var foo_bar_baz: int32;"));
  EXPECT_TRUE(parses_ok("var __private: int32;"));
}

TEST(RefLexicalStructure, IdentifierCannotStartWithDigit)
{
  // MUST FAIL: Identifiers cannot start with a digit
  EXPECT_TRUE(parses_with_error("var 123foo: int32;"));
  EXPECT_TRUE(parses_with_error("var 1abc: int32;"));
}

// ============================================================================
// 1.3.2 Keywords
// Reference: Keywords cannot be used as identifiers
// ============================================================================

TEST(RefLexicalStructure, KeywordsReserved)
{
  // MUST FAIL: Keywords cannot be used as identifiers
  EXPECT_TRUE(parses_with_error("var import: int32;"));
  EXPECT_TRUE(parses_with_error("var extern: int32;"));
  EXPECT_TRUE(parses_with_error("var type: int32;"));
  EXPECT_TRUE(parses_with_error("var var: int32;"));
  EXPECT_TRUE(parses_with_error("var const: int32;"));
  EXPECT_TRUE(parses_with_error("var tree: int32;"));
  EXPECT_TRUE(parses_with_error("var true: int32;"));
  EXPECT_TRUE(parses_with_error("var false: int32;"));
  EXPECT_TRUE(parses_with_error("var null: int32;"));
  EXPECT_TRUE(parses_with_error("var action: int32;"));
  EXPECT_TRUE(parses_with_error("var condition: int32;"));
  EXPECT_TRUE(parses_with_error("var control: int32;"));
  EXPECT_TRUE(parses_with_error("var decorator: int32;"));
  EXPECT_TRUE(parses_with_error("var subtree: int32;"));
}

TEST(RefLexicalStructure, KeywordsAsPartOfIdentifierOk)
{
  // Keywords as PART of identifier should be fine
  EXPECT_TRUE(parses_ok("var import_path: int32;"));
  EXPECT_TRUE(parses_ok("var my_tree: int32;"));
  EXPECT_TRUE(parses_ok("var true_value: int32;"));
  EXPECT_TRUE(parses_ok("var null_check: int32;"));
}

// ============================================================================
// 1.4.1 Integer Literals
// Reference: Decimal, hex (0x), binary (0b), octal (0o)
// ============================================================================

TEST(RefLexicalStructure, IntegerLiteralDecimal)
{
  std::unique_ptr<ParsedUnit> unit;
  auto * prog = get_program(unit, "const X = 42;");
  ASSERT_NE(prog, nullptr);
  const auto consts = prog->global_consts();
  ASSERT_EQ(consts.size(), 1U);
  ASSERT_NE(consts[0], nullptr);
  EXPECT_EQ(consts[0]->name, "X");
}

TEST(RefLexicalStructure, IntegerLiteralNegative)
{
  EXPECT_TRUE(parses_ok("const X = -42;"));
  EXPECT_TRUE(parses_ok("const X = -1;"));
}

TEST(RefLexicalStructure, IntegerLiteralHex)
{
  EXPECT_TRUE(parses_ok("const X = 0xFF;"));
  EXPECT_TRUE(parses_ok("const X = 0xDEADBEEF;"));
  EXPECT_TRUE(parses_ok("const X = 0x0;"));
}

TEST(RefLexicalStructure, IntegerLiteralBinary)
{
  EXPECT_TRUE(parses_ok("const X = 0b1010;"));
  EXPECT_TRUE(parses_ok("const X = 0b0;"));
  EXPECT_TRUE(parses_ok("const X = 0b11111111;"));
}

TEST(RefLexicalStructure, IntegerLiteralOctal)
{
  EXPECT_TRUE(parses_ok("const X = 0o777;"));
  EXPECT_TRUE(parses_ok("const X = 0o0;"));
  EXPECT_TRUE(parses_ok("const X = 0o123;"));
}

TEST(RefLexicalStructure, IntegerLiteralInvalidOctalDigit)
{
  // MUST FAIL: Invalid octal digits
  EXPECT_TRUE(parses_with_error("const X = 0o89;"));
}

TEST(RefLexicalStructure, IntegerLiteralSeparatorUnsupported)
{
  // MUST FAIL: Separators are not supported (1.4.2)
  EXPECT_TRUE(parses_with_error("const X = 1_000;"));
}

TEST(RefLexicalStructure, IntegerLiteralInvalidBinaryDigit)
{
  // MUST FAIL: Invalid binary digits
  EXPECT_TRUE(parses_with_error("const X = 0b123;"));
}

// ============================================================================
// 1.4.3 Float Literals
// Reference: -?, integer part, '.', decimal part, optional exponent
// ============================================================================

TEST(RefLexicalStructure, FloatLiteralBasic)
{
  EXPECT_TRUE(parses_ok("const X = 3.14;"));
  EXPECT_TRUE(parses_ok("const X = 0.5;"));
  EXPECT_TRUE(parses_ok("const X = 123.456;"));
}

TEST(RefLexicalStructure, FloatLiteralNegative)
{
  EXPECT_TRUE(parses_ok("const X = -3.14;"));
  EXPECT_TRUE(parses_ok("const X = -0.5;"));
}

TEST(RefLexicalStructure, FloatLiteralExponent)
{
  EXPECT_TRUE(parses_ok("const X = 1e3;"));
  EXPECT_TRUE(parses_ok("const X = 1E3;"));
  EXPECT_TRUE(parses_ok("const X = 1.5e-2;"));
  EXPECT_TRUE(parses_ok("const X = 1.5E+10;"));
}

// ============================================================================
// 1.4.4 String Literals
// Reference: Escape sequences must be supported
// ============================================================================

TEST(RefLexicalStructure, StringLiteralBasic)
{
  EXPECT_TRUE(parses_ok(R"(const X = "hello";)"));
  EXPECT_TRUE(parses_ok(R"(const X = "";)"));
  EXPECT_TRUE(parses_ok(R"(const X = "hello world";)"));
}

TEST(RefLexicalStructure, StringLiteralEscapeSequences)
{
  // Required escape sequences per reference
  EXPECT_TRUE(parses_ok(R"(const X = "hello\nworld";)"));  // \n
  EXPECT_TRUE(parses_ok(R"(const X = "hello\tworld";)"));  // \t
  EXPECT_TRUE(parses_ok(R"(const X = "hello\"world";)"));  // \"
  EXPECT_TRUE(parses_ok(R"(const X = "hello\\world";)"));  // backslash
  EXPECT_TRUE(parses_ok(R"(const X = "hello\rworld";)"));  // \r
  EXPECT_TRUE(parses_ok(R"(const X = "hello\0world";)"));  // \0
}

TEST(RefLexicalStructure, StringLiteralNoRawNewline)
{
  // MUST FAIL: Raw newlines in strings are not allowed
  EXPECT_TRUE(parses_with_error("const X = \"hello\nworld\";"));
}

TEST(RefLexicalStructure, StringLiteralEscapeBackspace)
{
  // \b escape sequence
  EXPECT_TRUE(parses_ok(R"(const X = "hello\bworld";)"));
}

TEST(RefLexicalStructure, StringLiteralEscapeFormFeed)
{
  // \f escape sequence
  EXPECT_TRUE(parses_ok(R"(const X = "hello\fworld";)"));
}

TEST(RefLexicalStructure, StringLiteralUnicodeEscape)
{
  // \u{XXXX} Unicode escape (1-6 hex digits)
  EXPECT_TRUE(parses_ok(R"(const X = "\u{0041}";)"));   // 'A'
  EXPECT_TRUE(parses_ok(R"(const X = "\u{1F600}";)"));  // emoji
  EXPECT_TRUE(parses_ok(R"(const X = "\u{0}";)"));      // NUL
}

// ============================================================================
// 1.4.5 Boolean and Null Literals
// ============================================================================

TEST(RefLexicalStructure, BooleanLiterals)
{
  EXPECT_TRUE(parses_ok("const X = true;"));
  EXPECT_TRUE(parses_ok("const X = false;"));
}

TEST(RefLexicalStructure, NullLiteral) { EXPECT_TRUE(parses_ok("var x: int32? = null;")); }

// ============================================================================
// 1.2.2 Comments
// ============================================================================

TEST(RefLexicalStructure, LineComment)
{
  EXPECT_TRUE(parses_ok("// This is a comment\nconst X = 1;"));
  EXPECT_TRUE(parses_ok("const X = 1; // trailing comment"));
}

TEST(RefLexicalStructure, BlockComment)
{
  EXPECT_TRUE(parses_ok("/* block comment */ const X = 1;"));
  EXPECT_TRUE(parses_ok("const X = /* inline */ 1;"));
  EXPECT_TRUE(parses_ok("/* multi\nline\ncomment */ const X = 1;"));
}

// ============================================================================
// 1.2.3 Documentation Comments
// Reference: //! (inner) and /// (outer) must be recognized separately
// ============================================================================

TEST(RefLexicalStructure, InnerDocComment)
{
  EXPECT_TRUE(parses_ok("//! Module documentation\nconst X = 1;"));
}

TEST(RefLexicalStructure, OuterDocComment)
{
  EXPECT_TRUE(parses_ok("/// Const documentation\nconst X = 1;"));
  EXPECT_TRUE(parses_ok("/// Tree documentation\ntree Foo() {}"));
  EXPECT_TRUE(parses_ok("/// Extern documentation\nextern action Bar();"));
}
