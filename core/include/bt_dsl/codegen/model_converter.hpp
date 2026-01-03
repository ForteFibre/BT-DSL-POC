// bt_dsl/codegen/model_converter.hpp - XML manifest to BT-DSL converter
//
// Converts BehaviorTree.CPP TreeNodesModel XML to BT-DSL extern declarations.
//
#pragma once

#include <string>
#include <vector>

namespace bt_dsl
{

// ============================================================================
// Manifest Types
// ============================================================================

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

struct ModelConvertResult
{
  std::string bt_text;
  int nodes_count;
};

// ============================================================================
// Model Converter
// ============================================================================

/**
 * Converts BehaviorTree.CPP XML manifests to BT-DSL extern declarations.
 *
 * This is useful for generating `extern node` declarations from existing
 * BehaviorTree.CPP node manifests (TreeNodesModel XML).
 */
class ModelConverter
{
public:
  /**
   * Parse XML content and extract node definitions.
   *
   * @param xml_content The XML content to parse
   * @return List of parsed nodes
   * @throws std::runtime_error if parsing fails
   */
  [[nodiscard]] static std::vector<ManifestNode> parse_xml(const std::string & xml_content);

  /**
   * Render nodes as BT-DSL extern declarations.
   *
   * @param nodes List of nodes to render
   * @return BT-DSL source code with extern declarations
   */
  [[nodiscard]] static std::string render_bt(const std::vector<ManifestNode> & nodes);

  /**
   * Convenience function: parse XML and render to BT-DSL.
   *
   * @param xml_content The XML content to convert
   * @return Result containing BT-DSL text and node count
   */
  [[nodiscard]] static ModelConvertResult convert(const std::string & xml_content);
};

}  // namespace bt_dsl
