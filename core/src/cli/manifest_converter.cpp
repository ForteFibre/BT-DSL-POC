#include "manifest_converter.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "tinyxml2.h"

namespace bt_dsl
{

namespace
{

std::string sanitize_type_name(const std::string & type_name)
{
  // Replace unsupported characters with underscore
  std::string cleaned;
  cleaned.reserve(type_name.size());
  for (const char c : type_name) {
    if (isalnum(c) || c == '_') {
      cleaned.push_back(c);
    } else {
      cleaned.push_back('_');
    }
  }

  // Ensure it starts with [A-Za-z_]
  if (!cleaned.empty() && !isalpha(cleaned[0]) && cleaned[0] != '_') {
    return "_" + cleaned;
  }
  return cleaned;
}

std::string get_string_attr(const tinyxml2::XMLElement * elem, const char * name)
{
  const char * val = elem->Attribute(name);
  return val ? std::string(val) : std::string();
}

void parse_ports(
  const tinyxml2::XMLElement * node_elem, const char * tag_name, ManifestPortDirection dir,
  std::vector<ManifestPort> & out_ports)
{
  for (const auto * child = node_elem->FirstChildElement(tag_name); child;
       child = child->NextSiblingElement(tag_name)) {
    const std::string name = get_string_attr(child, "name");
    if (name.empty()) continue;

    const std::string type = get_string_attr(child, "type");
    const std::string attr_desc = get_string_attr(child, "description");

    std::string text_desc;
    if (child->GetText()) {
      text_desc = child->GetText();
      // Trim whitespace
      text_desc.erase(0, text_desc.find_first_not_of(" \t\n\r"));
      text_desc.erase(text_desc.find_last_not_of(" \t\n\r") + 1);
    }

    const std::string description = (!text_desc.empty()) ? text_desc : attr_desc;

    out_ports.push_back(
      {name, dir, (!type.empty() ? sanitize_type_name(type) : "any"), description});
  }
}

}  // namespace

std::vector<ManifestNode> ManifestConverter::parse_xml(const std::string & xml_content)
{
  tinyxml2::XMLDocument doc;
  const tinyxml2::XMLError err = doc.Parse(xml_content.c_str());
  if (err != tinyxml2::XML_SUCCESS) {
    throw std::runtime_error("Failed to parse XML: " + std::string(doc.ErrorStr()));
  }

  std::vector<ManifestNode> result;

  // Helper to process a "root" or "TreeNodesModel" element that contains node
  // definitions
  auto process_container = [&](const tinyxml2::XMLElement * container) {
    if (!container) return;

    const char * categories[] = {"Action", "Condition", "Control", "Decorator", "SubTree"};

    for (const char * cat : categories) {
      for (const auto * node = container->FirstChildElement(cat); node;
           node = node->NextSiblingElement(cat)) {
        const std::string id = get_string_attr(node, "ID");
        if (id.empty()) continue;

        ManifestNode manifest_node;
        manifest_node.category = cat;
        manifest_node.name = id;

        parse_ports(node, "input_port", ManifestPortDirection::In, manifest_node.ports);
        parse_ports(node, "output_port", ManifestPortDirection::Out, manifest_node.ports);
        parse_ports(node, "inout_port", ManifestPortDirection::InOut, manifest_node.ports);

        result.push_back(std::move(manifest_node));
      }
    }
  };

  const auto * root = doc.RootElement();
  if (!root) return result;  // Empty document

  // Check if root is TreeNodesModel/root or if it contains it
  // Some XMLs have <root><TreeNodesModel>...</TreeNodesModel></root>
  // Others might just be <TreeNodesModel>...</TreeNodesModel>

  const std::string root_name = root->Name();
  if (root_name == "TreeNodesModel") {
    process_container(root);
  } else {
    // Look for TreeNodesModel child
    const auto * model = root->FirstChildElement("TreeNodesModel");
    if (model) {
      process_container(model);
    } else {
      // If no TreeNodesModel, assume root might contain nodes directly
      // (fallback) or maybe it is <root ...> with nodes inside
      process_container(root);
    }
  }

  return result;
}

std::string ManifestConverter::render_bt(const std::vector<ManifestNode> & nodes)
{
  std::stringstream ss;
  ss << "//! Converted from TreeNodesModel XML\n";
  ss << "//! This file contains only `declare ...` statements.\n\n";

  for (const auto & n : nodes) {
    ss << "declare " << n.category << " " << n.name << "(";

    bool multiline = (n.ports.size() > 2);
    if (!multiline) {
      for (const auto & p : n.ports) {
        if (!p.description.empty()) {
          multiline = true;
          break;
        }
      }
    }

    if (!multiline) {
      for (size_t i = 0; i < n.ports.size(); ++i) {
        if (i > 0) ss << ", ";

        const auto & p = n.ports[i];
        const char * dir_str = "in";
        if (p.direction == ManifestPortDirection::Out)
          dir_str = "out";
        else if (p.direction == ManifestPortDirection::InOut)
          dir_str = "ref";

        ss << dir_str << " " << p.name << ": " << p.type_name;
      }
      ss << ")\n";
    } else {
      // Multiline
      ss << "\n";
      for (size_t i = 0; i < n.ports.size(); ++i) {
        const auto & p = n.ports[i];
        if (!p.description.empty()) {
          ss << "    /// " << p.description << "\n";
        }

        const char * dir_str = "in";
        if (p.direction == ManifestPortDirection::Out)
          dir_str = "out";
        else if (p.direction == ManifestPortDirection::InOut)
          dir_str = "ref";

        ss << "    " << dir_str << " " << p.name << ": " << p.type_name;
        if (i < n.ports.size() - 1) {
          ss << ",";
        }
        ss << "\n";
      }
      ss << ")\n";
    }
  }

  return ss.str();
}

ManifestConvertResult ManifestConverter::convert(const std::string & xml_content)
{
  auto nodes = parse_xml(xml_content);
  return {render_bt(nodes), static_cast<int>(nodes.size())};
}

}  // namespace bt_dsl
