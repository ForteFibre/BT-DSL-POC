#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bt_dsl
{

enum class ManifestPortDirection { In, Out, InOut };

struct ManifestPort
{
  std::string name;
  ManifestPortDirection direction;
  std::string type_name;
  std::string description;
};

struct ManifestNode
{
  std::string category;  // Action, Condition, Control, Decorator, SubTree
  std::string name;
  std::vector<ManifestPort> ports;
};

struct ManifestConvertResult
{
  std::string bt_text;
  int nodes_count;
};

class ManifestConverter
{
public:
  // Decodes XML content and returns a list of declared nodes.
  static std::vector<ManifestNode> parse_xml(const std::string & xml_content);

  // Renders the list of nodes into BT DSL text (declare ...).
  static std::string render_bt(const std::vector<ManifestNode> & nodes);

  // Convenience function: parse XML and render BT.
  static ManifestConvertResult convert(const std::string & xml_content);
};

}  // namespace bt_dsl
