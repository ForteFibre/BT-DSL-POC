// bt_dsl/xml_generator.hpp - Generate BehaviorTree.CPP XML from AST
#pragma once

#include <string>

#include "bt_dsl/codegen/btcpp_model.hpp"
#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/semantic/analyzer.hpp"

namespace bt_dsl
{

/**
 * Converts BT-DSL AST to a BehaviorTree.CPP intermediate model.
 *
 * Keeping this conversion separate makes it easier to test semantics
 * independently from XML serialization (and keeps tinyxml2 out of most of the
 * codebase).
 */
class AstToBtCppModelConverter
{
public:
  AstToBtCppModelConverter() = default;

  /**
   * Convert a program to BT.CPP model.
   * @param program Parsed AST
   * @param analysis Semantic analysis result (ports, categories, inferred
   * types)
   */
  [[nodiscard]] btcpp::Document convert(
    const Program & program, const AnalysisResult & analysis) const;

private:
  static std::string join_docs(const std::vector<std::string> & docs);

  // Value/Expression serialization for BT.CPP Script nodes
  static std::string serialize_expression(const Expression & expr);
  static std::string format_literal_for_script(const Literal & lit);

  static std::string serialize_assignment_stmt(const AssignmentStmt & stmt);

  // Attribute values for node arguments
  static std::string serialize_value_expr_for_attribute(const ValueExpr & value);

  // Node conversion
  [[nodiscard]] btcpp::Node convert_node_stmt(
    const NodeStmt & node, const Program & program, const NodeRegistry & nodes) const;

  [[nodiscard]] static btcpp::Node wrap_with_decorators(
    btcpp::Node inner, const std::vector<Decorator> & decorators, const NodeRegistry & nodes);

  static std::vector<btcpp::Attribute> convert_arguments_to_attributes(
    const std::vector<Argument> & args, std::string_view node_id, const NodeRegistry & nodes);
};

/**
 * Serialize a BT.CPP intermediate model to XML using tinyxml2.
 */
class BtCppXmlSerializer
{
public:
  BtCppXmlSerializer() = default;

  /**
   * Serialize a document model to a UTF-8 XML string.
   */
  [[nodiscard]] static std::string serialize(const btcpp::Document & doc);
};

/**
 * High-level XML generator facade.
 */
class XmlGenerator
{
public:
  XmlGenerator() = default;

  /**
   * Generate BehaviorTree.CPP XML.
   * This uses: AST -> BT.CPP model -> tinyxml2 XML string.
   */
  [[nodiscard]] static std::string generate(
    const Program & program, const AnalysisResult & analysis);
};

}  // namespace bt_dsl
