// bt_dsl/btcpp_model.hpp - Intermediate BT.CPP structure (AST -> model -> XML)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt_dsl::btcpp
{

// NOTE:
// This is a minimal, serialization-friendly model representing BehaviorTree.CPP
// XML. It intentionally avoids tinyxml2 types to keep conversion testable and
// decoupled.

struct Attribute
{
  std::string key;
  std::string value;
};

struct Node
{
  std::string tag;                    // XML element name (e.g. "Sequence", "Script")
  std::vector<Attribute> attributes;  // XML attributes
  std::vector<Node> children;         // child elements
  std::optional<std::string> text;    // optional text node content
};

enum class PortKind : uint8_t {
  Input,
  Output,
  InOut,
};

enum class NodeModelKind : uint8_t {
  Action,
  Condition,
  Control,
  Decorator,
};

struct PortModel
{
  PortKind kind;
  std::string name;
  std::optional<std::string> type;
};

struct SubTreeModel
{
  std::string id;
  std::vector<PortModel> ports;
};

struct NodeModel
{
  NodeModelKind kind;
  std::string id;
  std::vector<PortModel> ports;
};

struct BehaviorTreeModel
{
  std::string id;
  std::optional<std::string> description;  // -> <Metadata><item key="description" .../></Metadata>
  std::optional<Node> root;                // Root node of the tree
};

struct Document
{
  std::string main_tree_to_execute;
  // TreeNodesModel manifest
  std::vector<NodeModel> node_models;
  std::vector<SubTreeModel> subtree_models;
  std::vector<BehaviorTreeModel> behavior_trees;
};

}  // namespace bt_dsl::btcpp
