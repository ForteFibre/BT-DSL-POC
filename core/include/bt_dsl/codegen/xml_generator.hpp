#pragma once

#include <string>

#include "bt_dsl/codegen/btcpp_model.hpp"
#include "bt_dsl/sema/resolution/module_graph.hpp"

namespace bt_dsl
{

/**
 * Converts core AST to a BehaviorTree.CPP intermediate model.
 *
 * Keeping conversion separate from XML serialization makes it easier
 * to test semantics independently from tinyxml2 output.
 */
class AstToBtCppModelConverter
{
public:
  AstToBtCppModelConverter() = default;

  /// Convert a single module into a BT.CPP model (no import mangling).
  [[nodiscard]] static btcpp::Document convert(const ModuleInfo & module);

  /// Convert an entry module (and its reachable imported trees) into a single BT.CPP model.
  [[nodiscard]] static btcpp::Document convert_single_output(const ModuleInfo & entry);
};

/**
 * Serialize a BT.CPP intermediate model to XML using tinyxml2.
 */
class BtCppXmlSerializer
{
public:
  BtCppXmlSerializer() = default;

  /// Serialize a document model to a UTF-8 XML string.
  [[nodiscard]] static std::string serialize(const btcpp::Document & doc);
};

/**
 * High-level XML generator facade.
 */
class XmlGenerator
{
public:
  XmlGenerator() = default;

  /// Generate BehaviorTree.CPP XML for a single module.
  [[nodiscard]] static std::string generate(const ModuleInfo & module);

  /// Generate a single-output BehaviorTree.CPP XML, including reachable imported trees.
  [[nodiscard]] static std::string generate_single_output(const ModuleInfo & entry);
};

}  // namespace bt_dsl
