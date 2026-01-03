// bt_dsl/syntax/ts_ll.hpp - Low-level Tree-sitter wrapper (CST access)
#pragma once

#include <tree_sitter/api.h>

#include <cstdint>
#include <string_view>

#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl::ts_ll
{

// NOTE: The tree-sitter language entry point is provided by the locally built
// tree_sitter_bt_dsl static library (see CMakeLists.txt).
extern "C" const TSLanguage * tree_sitter_bt_dsl();

//------------------------------------------------------------------------------
// Node - thin wrapper around TSNode
//------------------------------------------------------------------------------
class Node
{
public:
  Node() : node_{} {}
  explicit Node(TSNode n) : node_(n) {}

  [[nodiscard]] bool is_null() const noexcept { return ts_node_is_null(node_); }
  [[nodiscard]] bool has_error() const noexcept { return ts_node_has_error(node_); }
  [[nodiscard]] bool is_error() const noexcept { return ts_node_is_error(node_); }
  [[nodiscard]] bool is_missing() const noexcept { return ts_node_is_missing(node_); }

  [[nodiscard]] std::string_view kind() const noexcept
  {
    const char * t = ts_node_type(node_);
    return t ? std::string_view(t) : std::string_view();
  }

  [[nodiscard]] uint32_t start_byte() const noexcept { return ts_node_start_byte(node_); }
  [[nodiscard]] uint32_t end_byte() const noexcept { return ts_node_end_byte(node_); }

  [[nodiscard]] SourceRange range() const noexcept { return {start_byte(), end_byte()}; }

  [[nodiscard]] std::string_view text(const SourceManager & sm) const noexcept
  {
    return sm.get_source_slice(range());
  }

  [[nodiscard]] uint32_t child_count() const noexcept { return ts_node_child_count(node_); }
  [[nodiscard]] uint32_t named_child_count() const noexcept
  {
    return ts_node_named_child_count(node_);
  }

  [[nodiscard]] Node child(uint32_t i) const noexcept { return Node(ts_node_child(node_, i)); }
  [[nodiscard]] Node named_child(uint32_t i) const noexcept
  {
    return Node(ts_node_named_child(node_, i));
  }

  [[nodiscard]] Node child_by_field(std::string_view field) const noexcept
  {
    return Node(
      ts_node_child_by_field_name(node_, field.data(), static_cast<uint32_t>(field.size())));
  }

  [[nodiscard]] TSNode raw() const noexcept { return node_; }

private:
  TSNode node_;
};

//------------------------------------------------------------------------------
// Cursor - wrapper around TSTreeCursor (named/un-named iteration)
//------------------------------------------------------------------------------
class Cursor
{
public:
  explicit Cursor(Node n) : cursor_(ts_tree_cursor_new(n.raw())) {}
  Cursor(const Cursor &) = delete;
  Cursor & operator=(const Cursor &) = delete;

  Cursor(Cursor && other) noexcept : cursor_(other.cursor_) { other.cursor_ = {}; }
  Cursor & operator=(Cursor && other) noexcept
  {
    if (this != &other) {
      ts_tree_cursor_delete(&cursor_);
      cursor_ = other.cursor_;
      other.cursor_ = {};
    }
    return *this;
  }

  ~Cursor() { ts_tree_cursor_delete(&cursor_); }

  [[nodiscard]] Node current_node() const noexcept
  {
    return Node(ts_tree_cursor_current_node(&cursor_));
  }

  [[nodiscard]] bool goto_first_child() noexcept
  {
    return ts_tree_cursor_goto_first_child(&cursor_);
  }
  [[nodiscard]] bool goto_next_sibling() noexcept
  {
    return ts_tree_cursor_goto_next_sibling(&cursor_);
  }
  [[nodiscard]] bool goto_parent() noexcept { return ts_tree_cursor_goto_parent(&cursor_); }

private:
  TSTreeCursor cursor_;
};

//------------------------------------------------------------------------------
// Parser/Tree - RAII wrappers
//------------------------------------------------------------------------------
class Parser
{
public:
  Parser();
  Parser(const Parser &) = delete;
  Parser & operator=(const Parser &) = delete;
  ~Parser();

  [[nodiscard]] TSTree * parse_string(std::string_view source) const;

private:
  TSParser * parser_ = nullptr;
};

class Tree
{
public:
  explicit Tree(TSTree * t = nullptr) : tree_(t) {}
  Tree(const Tree &) = delete;
  Tree & operator=(const Tree &) = delete;

  Tree(Tree && other) noexcept : tree_(other.tree_) { other.tree_ = nullptr; }
  Tree & operator=(Tree && other) noexcept
  {
    if (this != &other) {
      reset();
      tree_ = other.tree_;
      other.tree_ = nullptr;
    }
    return *this;
  }

  ~Tree() { reset(); }

  void reset(TSTree * t = nullptr)
  {
    if (tree_) ts_tree_delete(tree_);
    tree_ = t;
  }

  [[nodiscard]] bool is_null() const noexcept { return tree_ == nullptr; }

  [[nodiscard]] Node root_node() const noexcept
  {
    return tree_ ? Node(ts_tree_root_node(tree_)) : Node();
  }

private:
  TSTree * tree_ = nullptr;
};

}  // namespace bt_dsl::ts_ll
