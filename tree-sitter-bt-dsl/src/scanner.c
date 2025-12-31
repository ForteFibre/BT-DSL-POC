#include <stdbool.h>
#include <string.h>

#include "tree_sitter/parser.h"

// External scanner for BT-DSL.
//
// Purpose:
// - Enforce the reference lexical rule:
//     identifier = /[a-zA-Z_][a-zA-Z0-9_]*/ - keyword
//   Tree-sitter's regex engine does not support lookahead, and contextual lexing
//   would otherwise allow keywords where `identifier` is expected.
//
// This scanner only produces the `identifier` token and rejects reserved keywords.

enum TokenType {
  IDENTIFIER,
};

static bool is_alpha_(int32_t c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_alnum_(int32_t c) { return is_alpha_(c) || (c >= '0' && c <= '9'); }

static bool is_space_(int32_t c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_keyword(const char * s, size_t n)
{
  // Keep this list in sync with docs/reference/lexical-structure.md (Keywords).
  // NOTE: We do exact, length-sensitive matches.
  static const char * const keywords[] = {
    "import",
    "extern",
    "type",
    "var",
    "const",
    "tree",
    "as",
    "in",
    "out",
    "ref",
    "mut",
    "true",
    "false",
    "null",
    "vec",
    // `string` participates in the bounded string type syntax: `string<N>`.
    // Treat it as reserved at the lexer level so it can be tokenized as the literal
    // `string` token when the grammar expects it.
    "string",
    "action",
    "subtree",
    "condition",
    "control",
    "decorator",
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    const char * kw = keywords[i];
    size_t kwn = strlen(kw);
    if (kwn == n && memcmp(kw, s, n) == 0) {
      return true;
    }
  }
  return false;
}

void * tree_sitter_bt_dsl_external_scanner_create(void) { return NULL; }

void tree_sitter_bt_dsl_external_scanner_destroy(void * payload) { (void)payload; }

unsigned tree_sitter_bt_dsl_external_scanner_serialize(void * payload, char * buffer)
{
  (void)payload;
  (void)buffer;
  return 0;
}

void tree_sitter_bt_dsl_external_scanner_deserialize(
  void * payload, const char * buffer, unsigned length)
{
  (void)payload;
  (void)buffer;
  (void)length;
}

bool tree_sitter_bt_dsl_external_scanner_scan(
  void * payload, TSLexer * lexer, const bool * valid_symbols)
{
  (void)payload;

  if (!valid_symbols[IDENTIFIER]) {
    return false;
  }

  // External scanners are invoked before `extras` are necessarily skipped, so we must
  // defensively skip whitespace.
  while (is_space_(lexer->lookahead)) {
    lexer->advance(lexer, true);
  }

  if (!is_alpha_(lexer->lookahead)) {
    return false;
  }

  // Capture the identifier lexeme into a small buffer.
  // Keywords are short; we only need to store up to the longest keyword.
  char buf[32];
  size_t len = 0;

  lexer->mark_end(lexer);

  while (is_alnum_(lexer->lookahead)) {
    if (len + 1 < sizeof(buf)) {
      buf[len++] = (char)lexer->lookahead;
    } else {
      // Too long to be a keyword; still consume, but stop buffering.
      // (Identifier itself can be longer than buf.)
      // Keep len capped at buf size - 1.
      len = sizeof(buf) - 1;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);

    // If we've already exceeded our keyword buffer, no need to keep checking.
    // (But we still need to consume the rest of the identifier.)
    if (len == sizeof(buf) - 1) {
      while (is_alnum_(lexer->lookahead)) {
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
      }
      lexer->result_symbol = IDENTIFIER;
      return true;
    }
  }

  // `_` is the infer-type wildcard token in the grammar (infer_type), not an identifier.
  if (len == 1 && buf[0] == '_') {
    return false;
  }

  // If the lexeme is a reserved keyword, do not emit IDENTIFIER.
  if (is_keyword(buf, len)) {
    return false;
  }

  lexer->result_symbol = IDENTIFIER;
  return true;
}
