#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "bt_dsl/syntax/lexer.hpp"
#include "bt_dsl/syntax/token.hpp"

using bt_dsl::syntax::Lexer;
using bt_dsl::syntax::Token;
using bt_dsl::syntax::TokenKind;

TEST(SyntaxLexer, SkipsLineAndBlockComments)
{
  const std::string_view src =
    "// line\n"
    "/* block */\n"
    "const X = 1; // trailing\n"
    "const Y = /* inline */ 2;\n";

  Lexer lex(src);
  const auto toks = lex.lex_all();

  // Should not emit tokens for comments.
  // Just sanity-check that we see two 'const' identifiers and reach EOF.
  int const_count = 0;
  for (const auto & t : toks) {
    if (t.kind == TokenKind::Identifier && t.text == "const") {
      ++const_count;
    }
  }
  EXPECT_EQ(const_count, 2);
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
