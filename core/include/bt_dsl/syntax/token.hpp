#pragma once

#include <cstdint>
#include <string_view>

#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl::syntax
{

enum class TokenKind : uint8_t {
  Eof,
  Unknown,

  // Trivia-like tokens that the parser may attach to nodes
  DocLine,    // /// ...
  DocModule,  // //! ...

  // Non-doc comments (currently ignored by the parser, but useful for tools
  // like formatters that must preserve user text)
  LineComment,   // // ...
  BlockComment,  // /* ... */

  Identifier,
  IntLiteral,
  FloatLiteral,
  StringLiteral,  // token.text is the string *contents* (without quotes)

  // Punctuation / operators
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,

  Comma,
  Colon,
  Semicolon,
  Dot,

  At,
  Hash,
  Bang,
  Question,

  Plus,
  Minus,
  Star,
  Slash,
  Percent,

  Amp,
  Pipe,
  Caret,

  AndAnd,
  OrOr,

  Eq,
  EqEq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,

  PlusEq,
  MinusEq,
  StarEq,
  SlashEq,
  PercentEq,
};

struct Token
{
  TokenKind kind = TokenKind::Unknown;
  SourceRange range;      // byte range in the original source (including quotes for strings)
  std::string_view text;  // slice view (for StringLiteral: interior)

  [[nodiscard]] uint32_t begin() const noexcept { return range.get_begin().get_offset(); }
  [[nodiscard]] uint32_t end() const noexcept { return range.get_end().get_offset(); }
};

[[nodiscard]] constexpr std::string_view to_string(TokenKind k) noexcept
{
  switch (k) {
    case TokenKind::Eof:
      return "<eof>";
    case TokenKind::Unknown:
      return "<unknown>";
    case TokenKind::DocLine:
      return "<doc_line>";
    case TokenKind::DocModule:
      return "<doc_module>";
    case TokenKind::LineComment:
      return "<line_comment>";
    case TokenKind::BlockComment:
      return "<block_comment>";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::IntLiteral:
      return "int";
    case TokenKind::FloatLiteral:
      return "float";
    case TokenKind::StringLiteral:
      return "string";
    case TokenKind::LParen:
      return "(";
    case TokenKind::RParen:
      return ")";
    case TokenKind::LBrace:
      return "{";
    case TokenKind::RBrace:
      return "}";
    case TokenKind::LBracket:
      return "[";
    case TokenKind::RBracket:
      return "]";
    case TokenKind::Comma:
      return ",";
    case TokenKind::Colon:
      return ":";
    case TokenKind::Semicolon:
      return ";";
    case TokenKind::Dot:
      return ".";
    case TokenKind::At:
      return "@";
    case TokenKind::Hash:
      return "#";
    case TokenKind::Bang:
      return "!";
    case TokenKind::Question:
      return "?";
    case TokenKind::Plus:
      return "+";
    case TokenKind::Minus:
      return "-";
    case TokenKind::Star:
      return "*";
    case TokenKind::Slash:
      return "/";
    case TokenKind::Percent:
      return "%";
    case TokenKind::Amp:
      return "&";
    case TokenKind::Pipe:
      return "|";
    case TokenKind::Caret:
      return "^";
    case TokenKind::AndAnd:
      return "&&";
    case TokenKind::OrOr:
      return "||";
    case TokenKind::Eq:
      return "=";
    case TokenKind::EqEq:
      return "==";
    case TokenKind::Ne:
      return "!=";
    case TokenKind::Lt:
      return "<";
    case TokenKind::Le:
      return "<=";
    case TokenKind::Gt:
      return ">";
    case TokenKind::Ge:
      return ">=";
    case TokenKind::PlusEq:
      return "+=";
    case TokenKind::MinusEq:
      return "-=";
    case TokenKind::StarEq:
      return "*=";
    case TokenKind::SlashEq:
      return "/=";
    case TokenKind::PercentEq:
      return "%=";
  }
  return "";
}

}  // namespace bt_dsl::syntax
