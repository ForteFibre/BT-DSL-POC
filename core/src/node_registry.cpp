// bt_dsl/node_registry.cpp - Node registry implementation
#include "bt_dsl/node_registry.hpp"

#include <algorithm>

namespace bt_dsl
{

// ============================================================================
// Category Conversion
// ============================================================================

std::optional<NodeCategory> node_category_from_string(std::string_view name)
{
  if (name == "Action") return NodeCategory::Action;
  if (name == "Condition") return NodeCategory::Condition;
  if (name == "Control") return NodeCategory::Control;
  if (name == "Decorator") return NodeCategory::Decorator;
  if (name == "SubTree") return NodeCategory::SubTree;
  return std::nullopt;
}

std::string_view node_category_to_string(NodeCategory category)
{
  switch (category) {
    case NodeCategory::Action:
      return "Action";
    case NodeCategory::Condition:
      return "Condition";
    case NodeCategory::Control:
      return "Control";
    case NodeCategory::Decorator:
      return "Decorator";
    case NodeCategory::SubTree:
      return "SubTree";
  }
  return "Unknown";
}

// ============================================================================
// NodeInfo Implementation
// ============================================================================

const PortInfo * NodeInfo::get_port(std::string_view name) const
{
  auto it = std::find_if(
    ports.begin(), ports.end(), [&name](const PortInfo & p) { return p.name == name; });
  if (it != ports.end()) {
    return &(*it);
  }
  return nullptr;
}

std::optional<std::string> NodeInfo::get_single_port_name() const
{
  if (ports.size() == 1) {
    return ports[0].name;
  }
  return std::nullopt;
}

bool NodeInfo::can_have_children() const { return category == NodeCategory::Control; }

// ============================================================================
// NodeRegistry Implementation
// ============================================================================

void NodeRegistry::build_from_program(const Program & program)
{
  clear();

  // Register Tree definitions as SubTree nodes
  for (const auto & tree : program.trees) {
    NodeInfo node = from_tree_def(tree);
    tree_names_.insert(tree.name);
    nodes_.emplace(tree.name, std::move(node));
  }

  // Register declared nodes
  for (const auto & decl : program.declarations) {
    NodeInfo node = from_declare_stmt(decl);
    declare_names_.insert(decl.name);
    // Only add if not already present (trees take precedence for conflicts)
    if (nodes_.find(decl.name) == nodes_.end()) {
      nodes_.emplace(decl.name, std::move(node));
    }
  }
}

void NodeRegistry::merge(const NodeRegistry & other)
{
  for (const auto & [name, node] : other.nodes_) {
    if (nodes_.find(name) == nodes_.end()) {
      nodes_.emplace(name, node);
      if (other.tree_names_.count(name)) {
        tree_names_.insert(name);
      }
      if (other.declare_names_.count(name)) {
        declare_names_.insert(name);
      }
    }
  }
}

bool NodeRegistry::register_node(NodeInfo node)
{
  const std::string name = node.id;
  auto [it, inserted] = nodes_.emplace(name, std::move(node));
  if (inserted) {
    if (it->second.source == NodeSource::Tree) {
      tree_names_.insert(name);
    } else {
      declare_names_.insert(name);
    }
  }
  return inserted;
}

const NodeInfo * NodeRegistry::get_node(std::string_view id) const
{
  const std::string key(id);
  auto it = nodes_.find(key);
  if (it != nodes_.end()) {
    return &it->second;
  }
  return nullptr;
}

const PortInfo * NodeRegistry::get_port(std::string_view node_id, std::string_view port_name) const
{
  const NodeInfo * node = get_node(node_id);
  if (node) {
    return node->get_port(port_name);
  }
  return nullptr;
}

std::optional<std::string> NodeRegistry::get_single_port_name(std::string_view node_id) const
{
  const NodeInfo * node = get_node(node_id);
  if (node) {
    return node->get_single_port_name();
  }
  return std::nullopt;
}

bool NodeRegistry::has_conflict(std::string_view id) const
{
  const std::string key(id);
  return tree_names_.count(key) > 0 && declare_names_.count(key) > 0;
}

bool NodeRegistry::has_node(std::string_view id) const { return get_node(id) != nullptr; }

bool NodeRegistry::is_tree(std::string_view id) const
{
  const std::string key(id);
  return tree_names_.count(key) > 0;
}

bool NodeRegistry::is_declared(std::string_view id) const
{
  const std::string key(id);
  return declare_names_.count(key) > 0;
}

std::vector<const NodeInfo *> NodeRegistry::all_nodes() const
{
  std::vector<const NodeInfo *> result;
  result.reserve(nodes_.size());
  for (const auto & [_, node] : nodes_) {
    result.push_back(&node);
  }
  return result;
}

std::vector<std::string> NodeRegistry::all_node_names() const
{
  std::vector<std::string> result;
  result.reserve(nodes_.size());
  for (const auto & [name, _] : nodes_) {
    result.push_back(name);
  }
  return result;
}

void NodeRegistry::clear()
{
  nodes_.clear();
  tree_names_.clear();
  declare_names_.clear();
}

NodeInfo NodeRegistry::from_tree_def(const TreeDef & tree)
{
  NodeInfo node;
  node.id = tree.name;
  node.category = NodeCategory::SubTree;
  node.source = NodeSource::Tree;
  node.definition_range = tree.range;

  // Convert parameters to ports
  for (const auto & param : tree.params) {
    PortInfo port;
    port.name = param.name;
    port.direction = param.direction.value_or(PortDirection::In);
    port.type_name = param.type_name;
    port.description = std::nullopt;
    port.definition_range = param.range;
    node.ports.push_back(std::move(port));
  }

  return node;
}

NodeInfo NodeRegistry::from_declare_stmt(const DeclareStmt & decl)
{
  NodeInfo node;
  node.id = decl.name;
  node.source = NodeSource::Declare;
  node.definition_range = decl.range;

  // Convert category string to enum
  auto category = node_category_from_string(decl.category);
  node.category = category.value_or(NodeCategory::Action);

  // Convert declared ports
  // DeclarePort has: name, optional<PortDirection> direction, string type_name,
  // vector<string> docs
  for (const auto & dp : decl.ports) {
    PortInfo port;
    port.name = dp.name;
    port.direction = dp.direction.value_or(PortDirection::In);
    port.type_name = dp.type_name;  // type_name is std::string, not optional
    // Convert docs vector to description string
    if (!dp.docs.empty()) {
      std::string desc;
      for (const auto & doc : dp.docs) {
        if (!desc.empty()) desc += " ";
        desc += doc;
      }
      port.description = desc;
    }
    port.definition_range = dp.range;
    node.ports.push_back(std::move(port));
  }

  return node;
}

}  // namespace bt_dsl
