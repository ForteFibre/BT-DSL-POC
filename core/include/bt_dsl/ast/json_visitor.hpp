// bt_dsl/ast/json_visitor.hpp - JSON serialization for AST nodes
//
// Provides a visitor-based JSON serialization for the AST,
// returning nlohmann::json objects for any AST node.
//
#pragma once

#include <nlohmann/json.hpp>

#include "bt_dsl/ast/ast.hpp"

namespace bt_dsl
{

/**
 * Serialize an AST node to JSON.
 *
 * This is the primary entry point for JSON serialization.
 * It handles all node types and returns a structured JSON object.
 *
 * @param node The AST node to serialize (can be any node type)
 * @return JSON representation of the node
 */
[[nodiscard]] nlohmann::json to_json(const AstNode * node);

/**
 * Serialize a Program node including all its declarations.
 *
 * @param program The program to serialize
 * @return JSON representation with all declarations
 */
[[nodiscard]] nlohmann::json to_json(const Program * program);

}  // namespace bt_dsl
