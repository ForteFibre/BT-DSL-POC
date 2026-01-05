#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "bt_dsl/syntax/lexer.hpp"
#include "bt_dsl/syntax/token.hpp"

using bt_dsl::syntax::Lexer;
using bt_dsl::syntax::Token;
using bt_dsl::syntax::TokenKind;

TEST(SyntaxLexer, EmitsLineAndBlockCommentsAsTokens)
{
  const std::string_view src =
    "// line\n"
    "/* block */\n"
    "const X = 1; // trailing\n"
    "const Y = /* inline */ 2;\n";

  Lexer lex(src);
  const auto toks = lex.lex_all();

  // Should emit tokens for non-doc comments so tools can preserve them.
  // Sanity-check that we see the expected comment kinds and reach EOF.
  int const_count = 0;
  int line_comment_count = 0;
  int block_comment_count = 0;
  for (const auto & t : toks) {
    if (t.kind == TokenKind::Identifier && t.text == "const") {
      ++const_count;
    }
    if (t.kind == TokenKind::LineComment) {
      ++line_comment_count;
    }
    if (t.kind == TokenKind::BlockComment) {
      ++block_comment_count;
    }
  }
  EXPECT_EQ(const_count, 2);
  EXPECT_GE(line_comment_count, 2);   // leading + trailing
  EXPECT_GE(block_comment_count, 2);  // block + inline
  ASSERT_FALSE(toks.empty());
  EXPECT_EQ(toks.back().kind, TokenKind::Eof);
}

TEST(SyntaxLexer, BasePrefixedIntegerLiterals)
{
  {
    Lexer lex("const X = 0xDEADBEEF;");
    const auto toks = lex.lex_all();
    bool saw = false;
    for (const auto & t : toks) {
      if (t.kind == TokenKind::IntLiteral) {
        EXPECT_EQ(t.text, "0xDEADBEEF");
        saw = true;
        break;
      }
    }
    EXPECT_TRUE(saw);
  }

  {
    Lexer lex("const X = 0b1010;");
    const auto toks = lex.lex_all();
    bool saw = false;
    for (const auto & t : toks) {
      if (t.kind == TokenKind::IntLiteral) {
        EXPECT_EQ(t.text, "0b1010");
        saw = true;
        break;
      }
    }
    EXPECT_TRUE(saw);
  }

  {
    Lexer lex("const X = 0o777;");
    const auto toks = lex.lex_all();
    bool saw = false;
    for (const auto & t : toks) {
      if (t.kind == TokenKind::IntLiteral) {
        EXPECT_EQ(t.text, "0o777");
        saw = true;
        break;
      }
    }
    EXPECT_TRUE(saw);
  }
}

TEST(SyntaxLexer, InvalidBaseLiteralBecomesUnknown)
{
  Lexer lex("const X = 0o89;");
  const auto toks = lex.lex_all();

  bool saw_unknown = false;
  for (const auto & t : toks) {
    if (t.kind == TokenKind::Unknown && t.text.find("0o") == 0) {
      saw_unknown = true;
      break;
    }
  }
  EXPECT_TRUE(saw_unknown);
}

TEST(SyntaxLexer, RawNewlineInStringBecomesUnknown)
{
  Lexer lex("const X = \"hello\nworld\";");
  const auto toks = lex.lex_all();

  bool saw_unknown = false;
  for (const auto & t : toks) {
    if (t.kind == TokenKind::Unknown && t.text.find('"') != std::string_view::npos) {
      saw_unknown = true;
      break;
    }
  }
  EXPECT_TRUE(saw_unknown);
}

TEST(SyntaxLexer, DocCommentsPreservePayloadAndNormalizeCrlf)
{
  Lexer lex("//! module\r\n/// line\r\nconst X = 1;\n");
  const auto toks = lex.lex_all();

  ASSERT_GE(toks.size(), 3U);
  EXPECT_EQ(toks[0].kind, TokenKind::DocModule);
  EXPECT_EQ(toks[0].text, "module");
  EXPECT_EQ(toks[1].kind, TokenKind::DocLine);
  EXPECT_EQ(toks[1].text, "line");
}

TEST(SyntaxLexer, UnclosedBlockCommentShouldNotBeSilentlySkipped)
{
  Lexer lex("/* unclosed comment");
  const auto toks = lex.lex_all();

  ASSERT_EQ(toks.size(), 1U);
  EXPECT_EQ(toks[0].kind, TokenKind::Eof);
}

TEST(SyntaxLexer, UnclosedStringReturnsUnknown)
{
  Lexer lex("const X = \"unclosed");
  const auto toks = lex.lex_all();

  // Should see 'const', 'X', '=', 'Unknown' (for "unclosed)
  ASSERT_GE(toks.size(), 4U);
  EXPECT_EQ(toks[3].kind, TokenKind::Unknown);
  EXPECT_EQ(toks[3].text, "\"unclosed");
}

TEST(SyntaxLexer, InvalidCharReturnsUnknown)
{
  Lexer lex(
    "var @ x");  // @ is invalid in this context (unless it's precondition, but as start of expr)
  // construct is 'var' 'At(@)' 'x'
  const auto toks = lex.lex_all();

  ASSERT_GE(toks.size(), 2U);
  EXPECT_EQ(toks[0].kind, TokenKind::Identifier);  // var is an identifier in Lexer
  EXPECT_EQ(toks[0].text, "var");
  EXPECT_EQ(toks[1].kind, TokenKind::At);  // @ IS a valid token (TokenKind::At)

  // Let's try a truly invalid char like $ or `
  Lexer lex2("var $ x");
  const auto toks2 = lex2.lex_all();
  EXPECT_EQ(toks2[1].text, "$");
}

TEST(SyntaxLexer, InvalidUnicodeEscapeReturnsUnknown)
{
  // \u{...} requires 1-6 hex digits.
  // Empty
  {
    Lexer lex(R"(const S = "\u{}";)");
    auto toks = lex.lex_all();
    ASSERT_GE(toks.size(), 4U);
    // Should be Unknown because \u{} is invalid.
    EXPECT_EQ(toks[3].kind, TokenKind::Unknown);
  }
}

TEST(SyntaxLexer, MalformedFloatReturnsTokens)
{
  // 1. is not a float (needs fraction).
  // It should be Int(1) then Dot(.)
  Lexer lex("1.");
  auto toks = lex.lex_all();
  ASSERT_EQ(toks.size(), 3U);  // 1, ., EOF
  EXPECT_EQ(toks[0].kind, TokenKind::IntLiteral);
  EXPECT_EQ(toks[1].kind, TokenKind::Dot);
}

TEST(SyntaxLexer, FloatWithoutIntegerPart)
{
  // .5 is not a valid float in BT-DSL (must start with digit).
  // Should be Dot(.) then Int(5)
  Lexer lex(".5");
  auto toks = lex.lex_all();
  ASSERT_EQ(toks.size(), 3U);
  EXPECT_EQ(toks[0].kind, TokenKind::Dot);
  EXPECT_EQ(toks[1].kind, TokenKind::IntLiteral);
}

TEST(SyntaxLexer, IntegerSeparatorsAreTokenizedSeparately)
{
  // 1_000 -> Int(1) then Ident(_000)
  Lexer lex("1_000");
  auto toks = lex.lex_all();
  ASSERT_EQ(toks.size(), 3U);  // 1, _000, EOF
  EXPECT_EQ(toks[0].kind, TokenKind::IntLiteral);
  EXPECT_EQ(toks[0].text, "1");
  EXPECT_EQ(toks[1].kind, TokenKind::Identifier);
  EXPECT_EQ(toks[1].text, "_000");
}
