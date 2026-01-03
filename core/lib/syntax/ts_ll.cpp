// bt_dsl/syntax/ts_ll.cpp - Low-level Tree-sitter wrapper implementation
#include "bt_dsl/syntax/ts_ll.hpp"

#include <cassert>
#include <cstdlib>

namespace bt_dsl::ts_ll
{

Parser::Parser()
{
  parser_ = ts_parser_new();
  assert(parser_ && "ts_parser_new() failed");

  const TSLanguage * lang = tree_sitter_bt_dsl();
  assert(lang && "tree_sitter_bt_dsl() returned null");

  // NOTE: This must be checked even in Release builds.
  // When NDEBUG is set, assert() is compiled out, so relying on assert(ok)
  // triggers clang-analyzer dead-store warnings and can hide real failures.
  if (!ts_parser_set_language(parser_, lang)) {
    assert(false && "ts_parser_set_language() failed");
    std::abort();
  }
}

Parser::~Parser()
{
  if (parser_) ts_parser_delete(parser_);
  parser_ = nullptr;
}

TSTree * Parser::parse_string(std::string_view source) const
{
  assert(parser_);
  // Tree-sitter consumes bytes; the grammar expects UTF-8.
  return ts_parser_parse_string(
    parser_, /*old_tree*/ nullptr, source.data(), static_cast<uint32_t>(source.size()));
}

}  // namespace bt_dsl::ts_ll
