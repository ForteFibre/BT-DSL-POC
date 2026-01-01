// bt_dsl/xml_generator.hpp - Generate BehaviorTree.CPP XML from AST
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "bt_dsl/codegen/btcpp_model.hpp"
#include "bt_dsl/core/ast.hpp"
#include "bt_dsl/semantic/analyzer.hpp"

namespace bt_dsl
{

// Direct-import graph used for single-output XML generation.
// Key: a program/module. Value: the list of programs it directly imports.
using ImportGraph = std::unordered_map<const Program *, std::vector<const Program *>>;

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

  /**
   * Convert a main program (and its import closure) into a *single* BT.CPP document.
   *
   * Reference: docs/reference/xml-mapping.md §1.4 (Single Output) and §1.6 (Tree naming).
   *
   * @param main_program Entry-point module
   * @param main_analysis Semantic analysis of the entry-point module (should use its direct imports)
   * @param direct_imports Direct-import graph for all loaded modules (including main)
   */
  [[nodiscard]] btcpp::Document convert_single_output(
    const Program & main_program, const AnalysisResult & main_analysis,
    const ImportGraph & direct_imports) const;

private:
  struct CodegenContext;

  // Apply non-guard preconditions as BT.CPP attributes, and desugar @guard into
  // a wrapper Sequence as described in docs/reference/xml-mapping.md §5.1.
  [[nodiscard]] static btcpp::Node apply_preconditions_and_guard(
    btcpp::Node node, const std::vector<Precondition> & preconditions, CodegenContext & ctx);

  static std::string join_docs(const std::vector<std::string> & docs);

  // Value/Expression serialization for BT.CPP Script nodes
  static std::string serialize_expression(const Expression & expr);
  static std::string format_literal_for_script(const Literal & lit);

  // Preconditions are parsed by BT.CPP's Script engine; variable references
  // must use blackboard substitution syntax: {var}.
  static std::string serialize_expression_for_precondition(const Expression & expr);

  enum class ExprMode : uint8_t {
    Script,
    Precondition,
    AttributeValue,
  };

  // Context-aware serialization (xml-mapping.md requires scoping/mangling).
  static std::string serialize_expression(
    const Expression & expr, CodegenContext & ctx, ExprMode mode);
  static std::string serialize_assignment_stmt(const AssignmentStmt & stmt, CodegenContext & ctx);

  // Convert script-like statements (assignment/var/const desugarings) into
  // BT.CPP nodes, including preconditions and doc descriptions.
  [[nodiscard]] static btcpp::Node convert_script_stmt(
    std::string code, const std::vector<Precondition> & preconditions,
    const std::vector<std::string> & docs, CodegenContext & ctx);

  // Attribute values for node arguments
  static std::string serialize_argument_value_for_attribute(const ArgumentValue & value);

  static std::string serialize_argument_value_for_attribute(
    const ArgumentValue & value, CodegenContext & ctx);

  // Node conversion
  [[nodiscard]] btcpp::Node convert_node_stmt(
    const NodeStmt & node, const Program & program, const NodeRegistry & nodes,
    CodegenContext & ctx) const;

  static std::vector<btcpp::Attribute> convert_arguments_to_attributes(
    const std::vector<Argument> & args, std::string_view node_id, const NodeRegistry & nodes,
    CodegenContext & ctx);
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

  /**
   * Generate BehaviorTree.CPP XML as a single output file, including reachable imported trees.
   */
  [[nodiscard]] static std::string generate(
    const Program & main_program, const AnalysisResult & main_analysis,
    const ImportGraph & direct_imports);
};

}  // namespace bt_dsl
