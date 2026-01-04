#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_context.hpp"
#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"
#include "bt_dsl/syntax/token.hpp"

namespace bt_dsl::syntax
{

class Parser
{
public:
  Parser(AstContext & ast, SourceManager & source, DiagnosticBag & diags, std::vector<Token> tokens)
  : ast_(ast), source_(source), diags_(diags), tokens_(std::move(tokens))
  {
  }

  [[nodiscard]] Program * parse_program();

private:
  // Token helpers
  [[nodiscard]] const Token & cur(size_t lookahead = 0) const;
  [[nodiscard]] bool at(TokenKind k) const;
  [[nodiscard]] bool at_eof() const;

  const Token & advance();
  bool match(TokenKind k);
  bool expect(TokenKind k, std::string_view what);

  void error_at(const Token & t, std::string_view msg);
  void synchronize_to_stmt();

  // Small scanners
  [[nodiscard]] static bool is_kw(std::string_view kw, const Token & t);
  [[nodiscard]] static bool is_reserved_ident(std::string_view ident);
  bool expect_identifier_not_reserved(std::string_view what);
  [[nodiscard]] std::optional<PortDirection> parse_port_direction_opt();

  // Docs / preconditions
  [[nodiscard]] std::vector<std::string_view> collect_module_docs();
  [[nodiscard]] std::vector<std::string_view> collect_line_docs();
  [[nodiscard]] std::vector<Precondition *> collect_preconditions();

  // Top-level
  [[nodiscard]] ImportDecl * parse_import_decl(const std::vector<std::string_view> & docs);
  [[nodiscard]] ExternTypeDecl * parse_extern_type_decl(const std::vector<std::string_view> & docs);
  [[nodiscard]] TypeAliasDecl * parse_type_alias_decl(const std::vector<std::string_view> & docs);
  [[nodiscard]] GlobalVarDecl * parse_global_var_decl(const std::vector<std::string_view> & docs);
  [[nodiscard]] GlobalConstDecl * parse_global_const_decl(
    const std::vector<std::string_view> & docs);
  [[nodiscard]] ExternDecl * parse_extern_decl(const std::vector<std::string_view> & docs);
  [[nodiscard]] BehaviorAttr * parse_behavior_attr_opt();
  [[nodiscard]] TreeDecl * parse_tree_decl(const std::vector<std::string_view> & docs);

  // Statements
  [[nodiscard]] Stmt * parse_stmt();
  [[nodiscard]] BlackboardDeclStmt * parse_var_stmt(
    const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds);
  [[nodiscard]] ConstDeclStmt * parse_const_stmt(
    const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds);
  [[nodiscard]] AssignmentStmt * parse_assignment_stmt(
    const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds);
  [[nodiscard]] NodeStmt * parse_node_stmt(
    const std::vector<std::string_view> & docs, const std::vector<Precondition *> & preconds);

  [[nodiscard]] gsl::span<Stmt *> parse_block_body();

  // Supporting nodes
  [[nodiscard]] Argument * parse_argument();
  [[nodiscard]] InlineBlackboardDecl * parse_inline_blackboard_decl();
  [[nodiscard]] ParamDecl * parse_param_decl();
  [[nodiscard]] ExternPort * parse_extern_port();

  // Types
  [[nodiscard]] TypeExpr * parse_type_expr();
  [[nodiscard]] TypeNode * parse_type_base();

  // Expressions
  [[nodiscard]] Expr * parse_expr();
  [[nodiscard]] Expr * parse_or();
  [[nodiscard]] Expr * parse_and();
  [[nodiscard]] Expr * parse_bitor();
  [[nodiscard]] Expr * parse_bitxor();
  [[nodiscard]] Expr * parse_bitand();
  [[nodiscard]] Expr * parse_equality();
  [[nodiscard]] Expr * parse_comparison();
  [[nodiscard]] Expr * parse_add();
  [[nodiscard]] Expr * parse_mul();
  [[nodiscard]] Expr * parse_unary();
  [[nodiscard]] Expr * parse_postfix();
  [[nodiscard]] Expr * parse_primary();

  [[nodiscard]] Expr * make_missing_expr_at(const Token & t);

  [[nodiscard]] std::string unescape_string(std::string_view raw, const Token & tok_for_diag);

  AstContext & ast_;
  SourceManager & source_;
  DiagnosticBag & diags_;
  std::vector<Token> tokens_;
  size_t idx_ = 0;
};

}  // namespace bt_dsl::syntax
