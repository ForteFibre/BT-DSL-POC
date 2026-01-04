#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "bt_dsl/syntax/token.hpp"

namespace bt_dsl::syntax
{

class Lexer
{
public:
  explicit Lexer(std::string_view src) : src_(src) {}

  [[nodiscard]] std::vector<Token> lex_all();

private:
  [[nodiscard]] Token next_token();

  [[nodiscard]] bool eof() const noexcept { return pos_ >= src_.size(); }
  [[nodiscard]] char peek(size_t lookahead = 0) const noexcept
  {
    const size_t i = pos_ + lookahead;
    return (i < src_.size()) ? src_[i] : '\0';
  }
  [[nodiscard]] bool starts_with(std::string_view s) const noexcept;

  void advance(size_t n = 1) noexcept { pos_ += n; }

  void skip_whitespace();
  bool skip_line_comment_or_emit_doc(Token & out);

  [[nodiscard]] Token lex_identifier_or_keyword();
  [[nodiscard]] Token lex_number();
  [[nodiscard]] Token lex_string();

  std::string_view src_;
  size_t pos_ = 0;
};

}  // namespace bt_dsl::syntax
