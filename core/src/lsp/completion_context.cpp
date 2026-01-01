#include "bt_dsl/lsp/completion_context.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bt_dsl/syntax/completion_query.hpp"

// External declaration for Tree-sitter BT-DSL language
extern "C" const TSLanguage * tree_sitter_bt_dsl();

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

bool contains_half_open(TSNode n, uint32_t b)
{
  if (ts_node_is_null(n)) {
    return false;
  }
  const uint32_t s = ts_node_start_byte(n);
  const uint32_t e = ts_node_end_byte(n);
  return s <= b && b < e;
}

bool contains_inclusive_end(TSNode n, uint32_t b)
{
  if (ts_node_is_null(n)) {
    return false;
  }
  const uint32_t s = ts_node_start_byte(n);
  const uint32_t e = ts_node_end_byte(n);
  return s <= b && b <= e;
}

bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

TSNode find_ancestor_of_type(TSNode n, const char * type)
{
  while (!ts_node_is_null(n)) {
    const char * t = ts_node_type(n);
    if (t && type && std::strcmp(t, type) == 0) {
      return n;
    }
    n = ts_node_parent(n);
  }
  return {};
}

TSNode find_ancestor_of_any(TSNode n, const char * t1, const char * t2)
{
  while (!ts_node_is_null(n)) {
    const char * t = ts_node_type(n);
    if (t) {
      if (t1 && std::strcmp(t, t1) == 0) {
        return n;
      }
      if (t2 && std::strcmp(t, t2) == 0) {
        return n;
      }
    }
    n = ts_node_parent(n);
  }
  return {};
}

struct Capture
{
  std::string name;
  TSNode node{};
  TSNode parent_match_node{};  // optional: the node matched by the pattern root
};

std::vector<Capture> collect_captures(TSNode root, TSQuery * q)
{
  std::vector<Capture> out;
  TSQueryCursor * cursor = ts_query_cursor_new();
  if (!cursor) {
    return out;
  }

  ts_query_cursor_exec(cursor, q, root);

  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; ++i) {
      const TSQueryCapture cap = match.captures[i];
      uint32_t name_len = 0;
      const char * name = ts_query_capture_name_for_id(q, cap.index, &name_len);
      if (!name || name_len == 0) {
        continue;
      }
      Capture c;
      c.name.assign(name, name + name_len);
      c.node = cap.node;

      // Tree-sitter's C API doesn't expose the pattern root node on TSQueryMatch,
      // so we derive a stable grouping node from the capture's ancestry.
      // This is used to associate related captures (e.g. tree_def braces/name,
      // property_block parens, callable name + its args parens).
      if (starts_with(c.name, "bt.tree.")) {
        c.parent_match_node = find_ancestor_of_type(c.node, "tree_def");
      } else if (c.name == "bt.args") {
        c.parent_match_node = find_ancestor_of_type(c.node, "property_block");
      } else if (starts_with(c.name, "bt.call.")) {
        // Calls (leaf_node_call / compound_node_call)
        c.parent_match_node = find_ancestor_of_any(c.node, "leaf_node_call", "compound_node_call");
      } else if (starts_with(c.name, "bt.precondition.")) {
        c.parent_match_node = find_ancestor_of_type(c.node, "precondition");
      } else if (starts_with(c.name, "bt.arg.")) {
        c.parent_match_node = find_ancestor_of_type(c.node, "argument");
      } else if (starts_with(c.name, "bt.bb.")) {
        c.parent_match_node = find_ancestor_of_type(c.node, "inline_blackboard_decl");
      } else if (starts_with(c.name, "bt.port.")) {
        c.parent_match_node = find_ancestor_of_type(c.node, "port_direction");
      } else {
        c.parent_match_node = ts_node_parent(c.node);
      }
      if (ts_node_is_null(c.parent_match_node)) {
        c.parent_match_node = c.node;
      }
      out.push_back(std::move(c));
    }
  }

  ts_query_cursor_delete(cursor);
  return out;
}

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

  TSParser * parser = ts_parser_new();
  if (!parser) {
    return std::nullopt;
  }
  ts_parser_set_language(parser, tree_sitter_bt_dsl());

  TSTree * tree =
    ts_parser_parse_string(parser, nullptr, text.data(), static_cast<uint32_t>(text.size()));
  if (!tree) {
    ts_parser_delete(parser);
    return std::nullopt;
  }

  const TSNode root = ts_tree_root_node(tree);

  // Compile query
  uint32_t err_off = 0;
  TSQueryError err_type = TSQueryErrorNone;
  TSQuery * q = ts_query_new(
    tree_sitter_bt_dsl(), bt_dsl::syntax::k_completion_context_query.data(),
    static_cast<uint32_t>(bt_dsl::syntax::k_completion_context_query.size()), &err_off, &err_type);
  if (!q) {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return std::nullopt;
  }

  const auto caps = collect_captures(root, q);

  // Helper to select smallest containing node for a given capture name.
  auto best_containing = [&](std::string_view cap_name) -> std::optional<TSNode> {
    TSNode best = {};
    bool has = false;
    uint32_t best_len = std::numeric_limits<uint32_t>::max();
    for (const auto & c : caps) {
      if (c.name != cap_name) {
        continue;
      }
      if (!contains_half_open(c.node, byte_offset)) {
        continue;
      }
      const uint32_t s = ts_node_start_byte(c.node);
      const uint32_t e = ts_node_end_byte(c.node);
      const uint32_t len = (e > s) ? (e - s) : 0;
      if (!has || len < best_len) {
        best = c.node;
        best_len = len;
        has = true;
      }
    }
    if (!has) {
      return std::nullopt;
    }
    return best;
  };

  // 1) Import path
  if (auto n = best_containing("bt.import.path")) {
    ctx.kind = CompletionContextKind::ImportPath;
    ts_query_delete(q);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return ctx;
  }

  // 2) Precondition kind context ("@" token also counts, inclusive end)
  {
    for (const auto & c : caps) {
      if (c.name == "bt.precondition.kind" && contains_half_open(c.node, byte_offset)) {
        ctx.kind = CompletionContextKind::PreconditionKind;
        break;
      }
      if (c.name == "bt.precondition.at" && contains_inclusive_end(c.node, byte_offset)) {
        ctx.kind = CompletionContextKind::PreconditionKind;
        break;
      }
    }
  }

  // 3) Node name context
  if (ctx.kind != CompletionContextKind::PreconditionKind) {
    if (auto n = best_containing("bt.node.name")) {
      ctx.kind = CompletionContextKind::NodeName;
    }
  }

  // 4) Determine whether we're inside a tree body (between braces of nearest tree_def)
  bool in_tree_body = false;
  {
    // Match-level grouping: find lbrace/rbrace from same match.node (tree_def)
    struct TreeMatch
    {
      TSNode tree_def{};
      TSNode name{};
      TSNode lbrace{};
      TSNode rbrace{};
    };

    const std::vector<TreeMatch> trees;
    std::unordered_map<uint64_t, TreeMatch> by_match;

    // We don't have stable match IDs, so we approximate grouping by the matched node's start/end.
    auto key_of = [](TSNode n) -> uint64_t {
      return (static_cast<uint64_t>(ts_node_start_byte(n)) << 32) |
             static_cast<uint64_t>(ts_node_end_byte(n));
    };

    for (const auto & c : caps) {
      if (c.name.rfind("bt.tree.", 0) != 0) {
        continue;
      }
      const uint64_t k = key_of(c.parent_match_node);
      auto & tm = by_match[k];
      tm.tree_def = c.parent_match_node;
      if (c.name == "bt.tree.name") {
        tm.name = c.node;
      } else if (c.name == "bt.tree.lbrace") {
        tm.lbrace = c.node;
      } else if (c.name == "bt.tree.rbrace") {
        tm.rbrace = c.node;
      }
    }

    TSNode best_tree = {};
    bool has_best_tree = false;
    uint32_t best_len = std::numeric_limits<uint32_t>::max();

    for (const auto & [_, tm] : by_match) {
      if (
        ts_node_is_null(tm.lbrace) || ts_node_is_null(tm.rbrace) || ts_node_is_null(tm.tree_def)) {
        continue;
      }
      const uint32_t lb_end = ts_node_end_byte(tm.lbrace);
      const uint32_t rb_start = ts_node_start_byte(tm.rbrace);
      if (byte_offset < lb_end || rb_start < byte_offset) {
        continue;
      }
      const uint32_t s = ts_node_start_byte(tm.tree_def);
      const uint32_t e = ts_node_end_byte(tm.tree_def);
      const uint32_t len = (e > s) ? (e - s) : 0;
      if (!has_best_tree || len < best_len) {
        best_tree = tm.tree_def;
        best_len = len;
        has_best_tree = true;

        if (!ts_node_is_null(tm.name)) {
          const uint32_t ns = ts_node_start_byte(tm.name);
          const uint32_t ne = ts_node_end_byte(tm.name);
          if (ne > ns && ne <= text.size()) {
            ctx.tree_name = std::string(text.substr(ns, ne - ns));
          }
        }
      }
    }

    if (has_best_tree) {
      in_tree_body = true;
      // Only set TreeBody if we are not in a more specific context.
      if (ctx.kind == CompletionContextKind::TopLevelKeywords) {
        ctx.kind = CompletionContextKind::TreeBody;
      }
    }
  }

  // 5) Argument/property_block context: find active property_block and classify arg key/value
  bool in_args = false;
  uint32_t args_lparen_end = 0;
  uint32_t args_rparen_start = 0;
  {
    if (auto n = best_containing("bt.args")) {
      in_args = true;
      const uint32_t s = ts_node_start_byte(*n);
      const uint32_t e = ts_node_end_byte(*n);
      // property_block is "(" ... ")".
      // Derive the inner range cheaply (ASCII parens => 1 byte each).
      args_lparen_end = (s < e) ? (s + 1) : s;
      args_rparen_start = (e > 0 && e > s) ? (e - 1) : e;
      if (
        ctx.kind == CompletionContextKind::TopLevelKeywords ||
        ctx.kind == CompletionContextKind::TreeBody) {
        ctx.kind = CompletionContextKind::ArgStart;
      }
    }
  }

  // 6) When in args, pick callable name from captures that own the active property_block.
  if (in_args) {
    struct CallMatch
    {
      TSNode call{};
      TSNode name{};
      TSNode args{};
    };

    std::unordered_map<uint64_t, CallMatch> by_call;
    auto key_of = [](TSNode n) -> uint64_t {
      return (static_cast<uint64_t>(ts_node_start_byte(n)) << 32) |
             static_cast<uint64_t>(ts_node_end_byte(n));
    };

    for (const auto & c : caps) {
      if (c.name != "bt.call.node.name" && c.name != "bt.call.args") {
        continue;
      }
      const uint64_t k = key_of(c.parent_match_node);
      auto & m = by_call[k];
      m.call = c.parent_match_node;
      if (c.name == "bt.call.node.name") {
        m.name = c.node;
      } else {
        m.args = c.node;
      }
    }

    TSNode best_name = {};
    bool has_best = false;
    uint32_t best_len = std::numeric_limits<uint32_t>::max();

    for (const auto & [_, m] : by_call) {
      if (ts_node_is_null(m.call) || ts_node_is_null(m.name) || ts_node_is_null(m.args)) {
        continue;
      }

      const uint32_t as = ts_node_start_byte(m.args);
      const uint32_t ae = ts_node_end_byte(m.args);
      const uint32_t al = (as < ae) ? (as + 1) : as;
      const uint32_t ar = (ae > 0 && ae > as) ? (ae - 1) : ae;
      if (al != args_lparen_end || ar != args_rparen_start) {
        continue;
      }

      const uint32_t s = ts_node_start_byte(m.call);
      const uint32_t e = ts_node_end_byte(m.call);
      const uint32_t len = (e > s) ? (e - s) : 0;
      if (!has_best || len < best_len) {
        best_len = len;
        best_name = m.name;
        has_best = true;
      }
    }

    if (has_best && !ts_node_is_null(best_name)) {
      const uint32_t ns = ts_node_start_byte(best_name);
      const uint32_t ne = ts_node_end_byte(best_name);
      if (ne > ns && ne <= text.size()) {
        ctx.callable_name = std::string(text.substr(ns, ne - ns));
      }
    }
  }

  // 7) ArgName/ArgValue/PortDirection/BlackboardRefName classification
  if (in_args) {
    // Strong signals first.
    for (const auto & c : caps) {
      if (c.name == "bt.arg.value" && contains_half_open(c.node, byte_offset)) {
        ctx.kind = CompletionContextKind::ArgValue;
        break;
      }
      if (c.name == "bt.arg.name" && contains_half_open(c.node, byte_offset)) {
        ctx.kind = CompletionContextKind::ArgName;
        break;
      }
    }

    if (ctx.kind != CompletionContextKind::ArgName && ctx.kind != CompletionContextKind::ArgValue) {
      // Use punctuation-only heuristics confined to the active args parens.
      uint32_t last_colon_end = 0;
      uint32_t last_comma_end = 0;
      const uint32_t last_lparen_end = args_lparen_end;

      uint32_t next_colon_start = std::numeric_limits<uint32_t>::max();
      uint32_t next_comma_start = std::numeric_limits<uint32_t>::max();

      for (const auto & c : caps) {
        if (c.name != "bt.punct.colon" && c.name != "bt.punct.comma") {
          continue;
        }
        const uint32_t s = ts_node_start_byte(c.node);
        const uint32_t e = ts_node_end_byte(c.node);
        if (s < args_lparen_end || args_rparen_start < e) {
          continue;
        }

        if (e <= byte_offset) {
          if (c.name == "bt.punct.colon") {
            last_colon_end = std::max(last_colon_end, e);
          } else {
            last_comma_end = std::max(last_comma_end, e);
          }
        }
        if (s >= byte_offset) {
          if (c.name == "bt.punct.colon") {
            next_colon_start = std::min(next_colon_start, s);
          } else {
            next_comma_start = std::min(next_comma_start, s);
          }
        }
      }

      const uint32_t last_delim = std::max({last_colon_end, last_comma_end, last_lparen_end});
      if (last_delim == last_colon_end && last_colon_end > 0) {
        ctx.kind = CompletionContextKind::ArgValue;
      } else if (
        next_colon_start != std::numeric_limits<uint32_t>::max() &&
        (next_comma_start == std::numeric_limits<uint32_t>::max() ||
         next_colon_start < next_comma_start)) {
        ctx.kind = CompletionContextKind::ArgName;
      } else {
        ctx.kind = CompletionContextKind::ArgStart;
      }
    }
  }

  // Blackboard refs / directions can appear in values.
  {
    for (const auto & c : caps) {
      if (c.name == "bt.port.direction" && contains_half_open(c.node, byte_offset)) {
        ctx.kind = CompletionContextKind::PortDirection;
        break;
      }
      if (c.name == "bt.bb.name" && contains_half_open(c.node, byte_offset)) {
        if (
          ctx.kind == CompletionContextKind::TopLevelKeywords ||
          ctx.kind == CompletionContextKind::TreeBody ||
          ctx.kind == CompletionContextKind::ArgStart ||
          ctx.kind == CompletionContextKind::ArgValue) {
          ctx.kind = CompletionContextKind::BlackboardRefName;
        }
        break;
      }
    }
  }

  // If we're in tree body and still at TopLevelKeywords, treat as TreeBody.
  if (in_tree_body && ctx.kind == CompletionContextKind::TopLevelKeywords) {
    ctx.kind = CompletionContextKind::TreeBody;
  }

  ts_query_delete(q);
  ts_tree_delete(tree);
  ts_parser_delete(parser);

  return ctx;
}

}  // namespace bt_dsl::lsp
