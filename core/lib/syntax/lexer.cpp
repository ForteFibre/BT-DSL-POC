#include "bt_dsl/syntax/lexer.hpp"

#include <cctype>

namespace bt_dsl::syntax
{
namespace
{

bool is_ident_start(unsigned char c) { return (std::isalpha(c) != 0) || c == '_'; }
bool is_ident_continue(unsigned char c) { return (std::isalnum(c) != 0) || c == '_'; }

}  // namespace

bool Lexer::starts_with(std::string_view s) const noexcept
{
  return src_.size() >= pos_ + s.size() && src_.substr(pos_, s.size()) == s;
}

void Lexer::skip_whitespace()
{
  while (!eof()) {
    const auto c = static_cast<unsigned char>(peek());
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      advance(1);
      continue;
    }
    break;
  }
}

namespace
{

bool is_hex_digit(unsigned char c)
{
  return (std::isdigit(c) != 0) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

bool Lexer::skip_line_comment_or_emit_doc(Token & out)
{
  if (!starts_with("//")) {
    return false;
  }

  // Consume //
  const auto start = static_cast<uint32_t>(pos_);
  advance(2);

  const char kind = peek();
  if (kind == '!' || kind == '/') {
    // Doc comment: //! ... or /// ...
    const TokenKind tk = (kind == '!') ? TokenKind::DocModule : TokenKind::DocLine;
    advance(1);

    // Optional single leading space
    if (peek() == ' ') {
      advance(1);
    }

    const auto payload_start = static_cast<uint32_t>(pos_);

    // Find end of line (excluding CR if present)
    while (!eof() && peek() != '\n') {
      advance(1);
    }
    auto payload_end = static_cast<uint32_t>(pos_);
    if (payload_end > payload_start && src_[payload_end - 1] == '\r') {
      payload_end -= 1;
    }

    out.kind = tk;
    out.range = SourceRange(start, static_cast<uint32_t>(pos_));
    out.text = src_.substr(payload_start, payload_end - payload_start);

    return true;
  }

  // Non-doc line comment: skip to end of line
  while (!eof() && peek() != '\n') {
    advance(1);
  }
  (void)start;
  return false;
}

Token Lexer::lex_identifier_or_keyword()
{
  const auto start = static_cast<uint32_t>(pos_);
  advance(1);
  while (!eof() && is_ident_continue(static_cast<unsigned char>(peek()))) {
    advance(1);
  }
  const auto end = static_cast<uint32_t>(pos_);

  Token t;
  t.kind = TokenKind::Identifier;
  t.range = SourceRange(start, end);
  t.text = src_.substr(start, end - start);
  return t;
}

Token Lexer::lex_number()
{
  const auto start = static_cast<uint32_t>(pos_);

  // Base-prefixed integers: 0x.. 0b.. 0o..
  if (!eof() && peek() == '0') {
    const char p1 = peek(1);
    if (p1 == 'x' || p1 == 'X' || p1 == 'b' || p1 == 'B' || p1 == 'o' || p1 == 'O') {
      int base = 8;
      if (p1 == 'x' || p1 == 'X') {
        base = 16;
      } else if (p1 == 'b' || p1 == 'B') {
        base = 2;
      } else {
        base = 8;
      }
      advance(2);

      bool any = false;
      bool invalid = false;

      while (!eof()) {
        const auto c = static_cast<unsigned char>(peek());
        bool ok = false;
        if (base == 16) {
          ok = is_hex_digit(c);
        } else if (base == 8) {
          ok = (c >= '0' && c <= '7');
        } else {
          ok = (c == '0' || c == '1');
        }

        if (ok) {
          any = true;
          advance(1);
          continue;
        }

        // If it still looks like part of the literal, consume it but mark invalid.
        if (std::isalnum(c) != 0) {
          invalid = true;
          advance(1);
          continue;
        }

        break;
      }

      const auto end = static_cast<uint32_t>(pos_);
      Token t;
      t.range = SourceRange(start, end);
      t.text = src_.substr(start, end - start);
      // If no digits or invalid digits, force a parse-time error by emitting Unknown.
      t.kind = (!any || invalid) ? TokenKind::Unknown : TokenKind::IntLiteral;
      return t;
    }
  }

  while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
    advance(1);
  }

  bool is_float = false;

  // Fractional part
  if (!eof() && peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0) {
    is_float = true;
    advance(1);
    while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
      advance(1);
    }
  }

  // Exponent
  if (!eof() && (peek() == 'e' || peek() == 'E')) {
    is_float = true;
    advance(1);
    if (peek() == '+' || peek() == '-') {
      advance(1);
    }
    while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
      advance(1);
    }
  }

  const auto end = static_cast<uint32_t>(pos_);
  Token t;
  t.kind = is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral;
  t.range = SourceRange(start, end);
  t.text = src_.substr(start, end - start);
  return t;
}

Token Lexer::lex_string()
{
  const auto start = static_cast<uint32_t>(pos_);
  // opening quote
  advance(1);

  const auto payload_start = static_cast<uint32_t>(pos_);

  while (!eof()) {
    const char c = peek();
    if (c == '"') {
      break;
    }
    // Raw newlines are not allowed inside string literals.
    if (c == '\n' || c == '\r') {
      // Consume the newline so we make forward progress.
      advance(1);
      const auto end = static_cast<uint32_t>(pos_);
      Token t;
      t.kind = TokenKind::Unknown;
      t.range = SourceRange(start, end);
      t.text = src_.substr(start, end - start);
      return t;
    }
    if (c == '\\') {
      // Skip escape sequence in a best-effort way (parser validates)
      advance(1);
      if (!eof()) {
        if (peek() == 'u') {
          // \u{...}
          advance(1);
          if (peek() == '{') {
            advance(1);
            while (!eof() && peek() != '}') {
              advance(1);
            }
            if (peek() == '}') {
              advance(1);
            }
          }
        } else {
          advance(1);
        }
      }
      continue;
    }
    // normal char
    advance(1);
  }

  const auto payload_end = static_cast<uint32_t>(pos_);

  // closing quote (if present)
  if (!eof() && peek() == '"') {
    advance(1);
  }

  const auto end = static_cast<uint32_t>(pos_);

  Token t;
  t.kind = TokenKind::StringLiteral;
  t.range = SourceRange(start, end);
  t.text = src_.substr(payload_start, payload_end - payload_start);
  return t;
}

Token Lexer::next_token()
{
  while (true) {
    skip_whitespace();

    if (eof()) {
      Token t;
      t.kind = TokenKind::Eof;
      const auto at = static_cast<uint32_t>(src_.size());
      t.range = SourceRange(at, at);
      t.text = {};
      return t;
    }

    // Comments
    if (starts_with("//")) {
      Token doc;
      if (skip_line_comment_or_emit_doc(doc)) {
        return doc;
      }
      // Non-doc comment: consumed by skip_line_comment_or_emit_doc().
      continue;
    }
    if (starts_with("/*")) {
      // Block comment: skip until */ (best-effort; if unterminated, skip to EOF)
      advance(2);
      while (!eof() && !starts_with("*/")) {
        advance(1);
      }
      if (starts_with("*/")) {
        advance(2);
      }
      continue;
    }

    break;
  }

  const auto c = static_cast<unsigned char>(peek());

  if (is_ident_start(c)) {
    return lex_identifier_or_keyword();
  }
  if (std::isdigit(c) != 0) {
    return lex_number();
  }
  if (peek() == '"') {
    return lex_string();
  }

  const auto start = static_cast<uint32_t>(pos_);

  // Multi-char operators
  if (starts_with("&&")) {
    advance(2);
    return {TokenKind::AndAnd, SourceRange(start, static_cast<uint32_t>(pos_)), "&&"};
  }
  if (starts_with("||")) {
    advance(2);
    return {TokenKind::OrOr, SourceRange(start, static_cast<uint32_t>(pos_)), "||"};
  }
  if (starts_with("==")) {
    advance(2);
    return {TokenKind::EqEq, SourceRange(start, static_cast<uint32_t>(pos_)), "=="};
  }
  if (starts_with("!=")) {
    advance(2);
    return {TokenKind::Ne, SourceRange(start, static_cast<uint32_t>(pos_)), "!="};
  }
  if (starts_with("<=")) {
    advance(2);
    return {TokenKind::Le, SourceRange(start, static_cast<uint32_t>(pos_)), "<="};
  }
  if (starts_with(">=")) {
    advance(2);
    return {TokenKind::Ge, SourceRange(start, static_cast<uint32_t>(pos_)), ">="};
  }
  if (starts_with("+=")) {
    advance(2);
    return {TokenKind::PlusEq, SourceRange(start, static_cast<uint32_t>(pos_)), "+="};
  }
  if (starts_with("-=")) {
    advance(2);
    return {TokenKind::MinusEq, SourceRange(start, static_cast<uint32_t>(pos_)), "-="};
  }
  if (starts_with("*=")) {
    advance(2);
    return {TokenKind::StarEq, SourceRange(start, static_cast<uint32_t>(pos_)), "*="};
  }
  if (starts_with("/=")) {
    advance(2);
    return {TokenKind::SlashEq, SourceRange(start, static_cast<uint32_t>(pos_)), "/="};
  }
  if (starts_with("%=")) {
    advance(2);
    return {TokenKind::PercentEq, SourceRange(start, static_cast<uint32_t>(pos_)), "%="};
  }

  // Single-char tokens
  const char ch = peek();
  advance(1);
  const auto end = static_cast<uint32_t>(pos_);

  switch (ch) {
    case '(':
      return {TokenKind::LParen, SourceRange(start, end), "("};
    case ')':
      return {TokenKind::RParen, SourceRange(start, end), ")"};
    case '{':
      return {TokenKind::LBrace, SourceRange(start, end), "{"};
    case '}':
      return {TokenKind::RBrace, SourceRange(start, end), "}"};
    case '[':
      return {TokenKind::LBracket, SourceRange(start, end), "["};
    case ']':
      return {TokenKind::RBracket, SourceRange(start, end), "]"};
    case ',':
      return {TokenKind::Comma, SourceRange(start, end), ","};
    case ':':
      return {TokenKind::Colon, SourceRange(start, end), ":"};
    case ';':
      return {TokenKind::Semicolon, SourceRange(start, end), ";"};
    case '.':
      return {TokenKind::Dot, SourceRange(start, end), "."};
    case '@':
      return {TokenKind::At, SourceRange(start, end), "@"};
    case '#':
      return {TokenKind::Hash, SourceRange(start, end), "#"};
    case '!':
      return {TokenKind::Bang, SourceRange(start, end), "!"};
    case '?':
      return {TokenKind::Question, SourceRange(start, end), "?"};
    case '+':
      return {TokenKind::Plus, SourceRange(start, end), "+"};
    case '-':
      return {TokenKind::Minus, SourceRange(start, end), "-"};
    case '*':
      return {TokenKind::Star, SourceRange(start, end), "*"};
    case '/':
      return {TokenKind::Slash, SourceRange(start, end), "/"};
    case '%':
      return {TokenKind::Percent, SourceRange(start, end), "%"};
    case '&':
      return {TokenKind::Amp, SourceRange(start, end), "&"};
    case '|':
      return {TokenKind::Pipe, SourceRange(start, end), "|"};
    case '^':
      return {TokenKind::Caret, SourceRange(start, end), "^"};
    case '=':
      return {TokenKind::Eq, SourceRange(start, end), "="};
    case '<':
      return {TokenKind::Lt, SourceRange(start, end), "<"};
    case '>':
      return {TokenKind::Gt, SourceRange(start, end), ">"};
    default:
      break;
  }

  Token t;
  t.kind = TokenKind::Unknown;
  t.range = SourceRange(start, end);
  t.text = src_.substr(start, end - start);
  return t;
}

std::vector<Token> Lexer::lex_all()
{
  std::vector<Token> out;
  while (true) {
    const Token t = next_token();
    out.push_back(t);
    if (t.kind == TokenKind::Eof) {
      break;
    }
  }
  return out;
}

}  // namespace bt_dsl::syntax
