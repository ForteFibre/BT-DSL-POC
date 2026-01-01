// bt_dsl/node_registry.hpp - Node and port definition management
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/semantic/behavior.hpp"

namespace bt_dsl
{

// ============================================================================
// Node Types
// ============================================================================

/**
 * Category of a behavior tree node.
 */
enum class NodeCategory : uint8_t {
  Action,
  Condition,
  Control,
  Decorator,
  SubTree,
};

/**
 * Convert string to NodeCategory.
 * @return std::nullopt if string is not a valid category
 */
std::optional<NodeCategory> node_category_from_string(std::string_view name);

/**
 * Convert NodeCategory to string.
 */
std::string_view node_category_to_string(NodeCategory category);

/**
 * Information about a node port (parameter).
 */
struct PortInfo
{
  std::string name;
  PortDirection direction;
  std::optional<std::string> type_name;
  // Optional default value (const_expr) for input ports.
  // Reference spec: defaults are only allowed for `in` ports.
  std::optional<Expression> default_value;
  std::optional<std::string> description;
  SourceRange definition_range;
};

/**
 * Source of node definition.
 */
enum class NodeSource : uint8_t {
  Tree,     // User-defined Tree
  Declare,  // declare statement
};

/**
 * Information about a behavior tree node type.
 */
struct NodeInfo
{
  std::string id;
  NodeCategory category;
  // Optional behavior definition for nodes that own children.
  // Spec: omitted behavior means All + Chained.
  Behavior behavior;
  std::vector<PortInfo> ports;
  NodeSource source;
  SourceRange definition_range;

  /**
   * Get port by name.
   * @return nullptr if not found
   */
  [[nodiscard]] const PortInfo * get_port(std::string_view name) const;

  /**
   * Get the single port name if the node has exactly one port.
   * @return std::nullopt if node has 0 or >1 ports
   */
  [[nodiscard]] std::optional<std::string> get_single_port_name() const;

  /**
   * Check if this node type can have children.
    * In this DSL, both Control and Decorator category nodes can have children.
   */
  [[nodiscard]] bool can_have_children() const;

  /**
   * Get the number of ports.
   */
  [[nodiscard]] size_t port_count() const { return ports.size(); }
};

// ============================================================================
// Node Registry
// ============================================================================

/**
 * Registry of all known node types in a program.
 *
 * Manages both TreeDef (user-defined subtrees) and DeclareStmt (declared
 * nodes).
 */
class NodeRegistry
{
public:
  NodeRegistry() = default;

  /**
   * Build registry from a parsed program.
   */
  void build_from_program(const Program & program);

  /**
   * Merge another registry into this one.
   * Used for handling imports.
   */
  void merge(const NodeRegistry & other);

  /**
   * Register a node.
   * @return true if registered successfully, false if name already exists
   */
  bool register_node(NodeInfo node);

  /**
   * Insert or overwrite a node definition.
   * Intended for analyzers that need deterministic precedence (e.g. local
   * definitions override imported ones) while still reporting a conflict.
   */
  void upsert_node(NodeInfo node);

  /**
   * Get a node by ID.
   * @return nullptr if not found
   */
  const NodeInfo * get_node(std::string_view id) const;

  /**
   * Get a port from a node.
   * @return nullptr if node or port not found
   */
  const PortInfo * get_port(std::string_view node_id, std::string_view port_name) const;

  /**
   * Get the single port name for a node.
   * @return std::nullopt if node not found or doesn't have exactly one port
   */
  std::optional<std::string> get_single_port_name(std::string_view node_id) const;

  /**
   * Check if a name exists as both TreeDef and DeclareStmt.
   * This is considered an error.
   */
  bool has_conflict(std::string_view id) const;

  /**
   * Check if a node exists.
   */
  bool has_node(std::string_view id) const;

  /**
   * Check if a name is registered as a Tree.
   */
  bool is_tree(std::string_view id) const;

  /**
   * Check if a name is registered as a declared node.
   */
  bool is_declared(std::string_view id) const;

  /**
   * Get all registered nodes.
   */
  std::vector<const NodeInfo *> all_nodes() const;

  /**
   * Get all node names.
   */
  std::vector<std::string> all_node_names() const;

  /**
   * Clear the registry.
   */
  void clear();

private:
  std::unordered_map<std::string, NodeInfo> nodes_;
  std::unordered_set<std::string> tree_names_;
  std::unordered_set<std::string> declare_names_;

  // Helper to create NodeInfo from TreeDef
  static NodeInfo from_tree_def(const TreeDef & tree);

  // Helper to create NodeInfo from DeclareStmt
  static NodeInfo from_declare_stmt(const DeclareStmt & decl);
};

}  // namespace bt_dsl
