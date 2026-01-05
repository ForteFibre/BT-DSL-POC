
#include "bt_dsl/lsp/completion_context.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/syntax/lexer.hpp"

namespace bt_dsl::lsp
{
namespace
{

uint32_t clamp_byte_offset(uint32_t off, size_t text_size)
{
  if (off > text_size) {
    return static_cast<uint32_t>(text_size);
  }
  return off;
}

bool contains_half_open(const bt_dsl::SourceRange & r, uint32_t b)
{
  if (r.is_invalid()) return false;
  const uint32_t s = r.get_begin().get_offset();
  const uint32_t e = r.get_end().get_offset();
  return s <= b && b < e;
}

struct OpenBrace
{
  uint32_t openByte = 0;
  bool after_tree = false;
  bool after_node = false;
  std::optional<std::string> tree_name;
  std::optional<std::string> node_name;
};

}  // namespace

std::optional<CompletionContext> classify_completion_context(
  std::string_view text, uint32_t byte_offset)
{
  CompletionContext ctx;
  ctx.kind = CompletionContextKind::TopLevelKeywords;

  if (text.empty()) {
    return ctx;
  }

  byte_offset = clamp_byte_offset(byte_offset, text.size());

  // Completion context classification is purely offset-based.
  // Use a dummy-but-valid FileId so SourceRange is considered valid.
  bt_dsl::syntax::Lexer lex(bt_dsl::FileId{0}, text);
  const std::vector<bt_dsl::syntax::Token> toks = lex.lex_all();

  std::vector<OpenBrace> brace_stack;
  bool pending_tree = false;
  std::optional<std::string_view> last_ident;
  int paren_depth = 0;
  bool saw_import_kw = false;
  std::optional<std::string> paren_callable_name;  // Node name for which paren is open

  for (const auto & t : toks) {
    if (t.begin() > byte_offset) {
      break;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::StringLiteral) {
      if (saw_import_kw && contains_half_open(t.range, byte_offset)) {
        ctx.kind = CompletionContextKind::ImportPath;
        return ctx;
      }
      saw_import_kw = false;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::Identifier) {
      last_ident = t.text;
      if (t.text == "import") {
        saw_import_kw = true;
      } else if (t.text == "tree") {
        pending_tree = true;
      }
      continue;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::At) {
      if (t.end() <= byte_offset) {
        ctx.kind = CompletionContextKind::PreconditionKind;
      }
      continue;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::LParen) {
      ++paren_depth;
      // Record the callable name when entering the first paren (e.g., NodeName(...))
      if (paren_depth == 1 && last_ident) {
        paren_callable_name = std::string(*last_ident);
      }
      continue;
    }
    if (t.kind == bt_dsl::syntax::TokenKind::RParen) {
      paren_depth = std::max(0, paren_depth - 1);
      if (paren_depth == 0) {
        paren_callable_name.reset();
      }
      continue;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::LBrace) {
      OpenBrace ob;
      ob.openByte = t.begin();

      if (pending_tree) {
        ob.after_tree = true;
        if (last_ident && *last_ident != "tree") {
          ob.tree_name = std::string(*last_ident);
        }
      } else if (last_ident) {
        ob.after_node = true;
        ob.node_name = std::string(*last_ident);
      }

      brace_stack.push_back(std::move(ob));
      pending_tree = false;
      continue;
    }

    if (t.kind == bt_dsl::syntax::TokenKind::RBrace) {
      if (!brace_stack.empty()) {
        brace_stack.pop_back();
      }
      pending_tree = false;
      continue;
    }

    if (
      t.kind != bt_dsl::syntax::TokenKind::DocLine &&
      t.kind != bt_dsl::syntax::TokenKind::DocModule) {
      pending_tree = false;
    }
  }

  // If we observed an '@' before the cursor, prefer completing precondition kinds
  // even when nested inside a tree body.
  if (ctx.kind == CompletionContextKind::PreconditionKind) {
    if (!brace_stack.empty()) {
      const OpenBrace & innermost = brace_stack.back();
      if (innermost.tree_name) {
        ctx.tree_name = innermost.tree_name.value();
      }
    }
    return ctx;
  }

  if (paren_depth > 0) {
    bt_dsl::syntax::TokenKind prev = bt_dsl::syntax::TokenKind::Unknown;
    for (const auto & t : toks) {
      if (t.end() <= byte_offset) {
        prev = t.kind;
      } else {
        break;
      }
    }

    if (prev == bt_dsl::syntax::TokenKind::LParen || prev == bt_dsl::syntax::TokenKind::Comma) {
      ctx.kind = CompletionContextKind::ArgStart;
    } else if (prev == bt_dsl::syntax::TokenKind::Colon) {
      ctx.kind = CompletionContextKind::ArgValue;
    } else {
      ctx.kind = CompletionContextKind::ArgName;
    }

    // Search brace stack from innermost to outermost for tree_name
    for (auto it = brace_stack.rbegin(); it != brace_stack.rend(); ++it) {
      if (it->tree_name) {
        ctx.tree_name = it->tree_name.value_or(std::string{});
        break;
      }
    }
    // Set callable_name so port suggestions work
    if (paren_callable_name) {
      ctx.callable_name = *paren_callable_name;
    }
    return ctx;
  }

  if (!brace_stack.empty()) {
    const OpenBrace & innermost = brace_stack.back();
    if (innermost.after_tree || innermost.after_node) {
      ctx.kind = CompletionContextKind::TreeBody;
      // Search brace stack for tree_name
      for (auto it = brace_stack.rbegin(); it != brace_stack.rend(); ++it) {
        if (it->tree_name) {
          ctx.tree_name = it->tree_name.value_or(std::string{});
          break;
        }
      }
      if (innermost.node_name) {
        ctx.callable_name = innermost.node_name.value();
      }
      return ctx;
    }
  }

  return ctx;
}

}  // namespace bt_dsl::lsp
