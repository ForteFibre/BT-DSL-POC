#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bt_dsl::btcpp
{

struct Attribute
{
  std::string key;
  std::string value;
};

struct Node
{
  std::string tag;
  std::vector<Attribute> attributes;
  std::vector<Node> children;
  std::optional<std::string> text;
};

enum class NodeModelKind {
  Action,
  Condition,
  Control,
  Decorator,
};

enum class PortKind {
  Input,
  Output,
  InOut,
};

struct PortModel
{
  PortKind kind;
  std::string name;
  std::optional<std::string> type;
};

struct NodeModel
{
  NodeModelKind kind;
  std::string id;
  std::vector<PortModel> ports;
};

struct SubTreeModel
{
  std::string id;
  std::vector<PortModel> ports;
};

struct BehaviorTreeModel
{
  std::string id;
  std::optional<Node> root;
};

struct Document
{
  std::string main_tree_to_execute;
  std::vector<BehaviorTreeModel> behavior_trees;
  std::vector<NodeModel> node_models;
  std::vector<SubTreeModel> subtree_models;
};

}  // namespace bt_dsl::btcpp
