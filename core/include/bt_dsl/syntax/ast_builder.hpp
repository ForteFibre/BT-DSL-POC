// bt_dsl/syntax/ast_builder.hpp - Mid-level CST -> AST builder dispatcher
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"
#include "bt_dsl/syntax/ts_ll.hpp"

namespace bt_dsl
{

// Mid-level builder: owns no memory; writes into AstContext and DiagnosticBag.
class AstBuilder
{
public:
  AstBuilder(AstContext & ast, const SourceManager & sm, DiagnosticBag & diags)
  : ast_(ast), sm_(sm), diags_(diags)
  {
  }

  [[nodiscard]] Program * build_program(ts_ll::Node program_node);

  // Types
  [[nodiscard]] TypeExpr * build_type(ts_ll::Node type_node);

  // Expressions (subset for this task: literals)
  [[nodiscard]] Expr * build_expr(ts_ll::Node expr_node);

private:
  // High-level-ish helpers for scaffolding enough structure for tests.
  [[nodiscard]] ImportDecl * build_import_decl(ts_ll::Node import_stmt_node);
  [[nodiscard]] ExternTypeDecl * build_extern_type_decl(ts_ll::Node extern_type_stmt_node);
  [[nodiscard]] TypeAliasDecl * build_type_alias_decl(ts_ll::Node type_alias_stmt_node);
  [[nodiscard]] ExternDecl * build_extern_decl(ts_ll::Node extern_stmt_node);
  [[nodiscard]] ExternPort * build_extern_port(ts_ll::Node extern_port_node);
  [[nodiscard]] BehaviorAttr * build_behavior_attr(ts_ll::Node behavior_attr_node);
  [[nodiscard]] GlobalVarDecl * build_global_var_decl(ts_ll::Node global_var_node);
  [[nodiscard]] GlobalConstDecl * build_global_const_decl(ts_ll::Node global_const_node);
  [[nodiscard]] TreeDecl * build_tree_decl(ts_ll::Node tree_def_node);
  [[nodiscard]] ParamDecl * build_param_decl(ts_ll::Node param_decl_node);

  // Statements
  [[nodiscard]] Stmt * build_statement(ts_ll::Node stmt_node);
  [[nodiscard]] NodeStmt * build_leaf_node_call(ts_ll::Node leaf_call_node);
  [[nodiscard]] NodeStmt * build_compound_node_call(ts_ll::Node compound_call_node);
  [[nodiscard]] AssignmentStmt * build_assignment_stmt(ts_ll::Node assignment_node);
  [[nodiscard]] BlackboardDeclStmt * build_blackboard_decl_stmt(ts_ll::Node decl_node);
  [[nodiscard]] ConstDeclStmt * build_const_decl_stmt(ts_ll::Node decl_node);

  // Supporting nodes
  [[nodiscard]] Argument * build_argument(ts_ll::Node arg_node);
  [[nodiscard]] InlineBlackboardDecl * build_inline_blackboard_decl(ts_ll::Node inline_decl_node);
  [[nodiscard]] Precondition * build_precondition(ts_ll::Node precond_node);
  [[nodiscard]] std::vector<Precondition *> build_precondition_list(ts_ll::Node precond_list_node);

  [[nodiscard]] std::optional<PortDirection> parse_port_direction(ts_ll::Node port_dir_node);
  [[nodiscard]] std::optional<PreconditionKind> parse_precondition_kind(ts_ll::Node precond_kind_node);
  [[nodiscard]] std::optional<AssignOp> parse_assign_op(ts_ll::Node assign_op_node);

  // Blocks
  void parse_children_block(ts_ll::Node children_block_node, std::vector<Stmt *> & out);

  // Docs
  [[nodiscard]] std::vector<std::string_view> collect_outer_docs(ts_ll::Node node);

  // Utility
  [[nodiscard]] std::string_view node_text(ts_ll::Node n) const;
  [[nodiscard]] std::string_view intern_text(ts_ll::Node n);
  [[nodiscard]] static SourceRange node_range(ts_ll::Node n) noexcept;

  [[nodiscard]] Expr * missing_expr(ts_ll::Node at, std::string_view message);

  AstContext & ast_;
  const SourceManager & sm_;
  DiagnosticBag & diags_;
};

}  // namespace bt_dsl
