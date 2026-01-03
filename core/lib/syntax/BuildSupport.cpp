// bt_dsl/syntax/BuildSupport.cpp - CST -> AST for supporting nodes (docs/arguments/preconditions)
#include <string>

#include "bt_dsl/syntax/ast_builder.hpp"

namespace bt_dsl
{

static bool starts_with(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::vector<std::string_view> AstBuilder::collect_outer_docs(ts_ll::Node node)
{
  std::vector<std::string_view> docs;

  // outer_doc is a named token in the grammar.
  for (uint32_t i = 0; i < node.named_child_count(); ++i) {
    const ts_ll::Node c = node.named_child(i);
    if (c.kind() != "outer_doc") continue;

    std::string_view raw = node_text(c);
    if (starts_with(raw, "///")) {
      raw.remove_prefix(3);
    }

    // Intern the doc text (without the leading ///). The spec doesn't prescribe
    // trimming spaces; keep them as-authored.
    docs.push_back(ast_.intern(std::string(raw)));
  }

  return docs;
}

std::optional<PreconditionKind> AstBuilder::parse_precondition_kind(ts_ll::Node precond_kind_node)
{
  if (precond_kind_node.is_null() || precond_kind_node.kind() != "precond_kind") {
    return std::nullopt;
  }

  const std::string_view t = node_text(precond_kind_node);
  if (t == "success_if") return PreconditionKind::SuccessIf;
  if (t == "failure_if") return PreconditionKind::FailureIf;
  if (t == "skip_if") return PreconditionKind::SkipIf;
  if (t == "run_while") return PreconditionKind::RunWhile;
  if (t == "guard") return PreconditionKind::Guard;

  diags_.error(
    node_range(precond_kind_node), "Unknown precondition kind: '" + std::string(t) + "'");
  return std::nullopt;
}

std::optional<AssignOp> AstBuilder::parse_assign_op(ts_ll::Node assign_op_node)
{
  if (assign_op_node.is_null() || assign_op_node.kind() != "assignment_op") {
    return std::nullopt;
  }

  const std::string_view t = node_text(assign_op_node);
  if (t == "=") return AssignOp::Assign;
  if (t == "+=") return AssignOp::AddAssign;
  if (t == "-=") return AssignOp::SubAssign;
  if (t == "*=") return AssignOp::MulAssign;
  if (t == "/=") return AssignOp::DivAssign;
  if (t == "%=") return AssignOp::ModAssign;

  diags_.error(node_range(assign_op_node), "Unknown assignment operator: '" + std::string(t) + "'");
  return std::nullopt;
}

InlineBlackboardDecl * AstBuilder::build_inline_blackboard_decl(ts_ll::Node inline_decl_node)
{
  if (inline_decl_node.is_null()) {
    diags_.error({}, "Missing inline_blackboard_decl");
    return ast_.create<InlineBlackboardDecl>(ast_.intern("<missing>"));
  }

  if (inline_decl_node.kind() != "inline_blackboard_decl") {
    if (inline_decl_node.kind() == "ERROR" || inline_decl_node.is_missing()) {
      diags_.error(node_range(inline_decl_node), "Invalid inline blackboard declaration");
      return ast_.create<InlineBlackboardDecl>(
        ast_.intern("<missing>"), node_range(inline_decl_node));
    }
    diags_.error(node_range(inline_decl_node), "Expected 'inline_blackboard_decl'");
    return ast_.create<InlineBlackboardDecl>(
      ast_.intern("<missing>"), node_range(inline_decl_node));
  }

  const ts_ll::Node name_node = inline_decl_node.child_by_field("name");
  if (name_node.is_null()) {
    diags_.error(node_range(inline_decl_node), "inline_blackboard_decl missing name");
    return ast_.create<InlineBlackboardDecl>(
      ast_.intern("<missing>"), node_range(inline_decl_node));
  }

  return ast_.create<InlineBlackboardDecl>(intern_text(name_node), node_range(inline_decl_node));
}

Precondition * AstBuilder::build_precondition(ts_ll::Node precond_node)
{
  if (precond_node.is_null()) {
    diags_.error({}, "Missing precondition");
    return ast_.create<Precondition>(PreconditionKind::Guard, ast_.create<MissingExpr>());
  }

  if (precond_node.kind() != "precondition") {
    if (precond_node.kind() == "ERROR" || precond_node.is_missing()) {
      diags_.error(node_range(precond_node), "Invalid precondition");
      return ast_.create<Precondition>(
        PreconditionKind::Guard, ast_.create<MissingExpr>(node_range(precond_node)),
        node_range(precond_node));
    }
    diags_.error(node_range(precond_node), "Expected 'precondition'");
    return ast_.create<Precondition>(
      PreconditionKind::Guard, ast_.create<MissingExpr>(node_range(precond_node)),
      node_range(precond_node));
  }

  const ts_ll::Node kind_node = precond_node.child_by_field("kind");
  const ts_ll::Node cond_node = precond_node.child_by_field("cond");

  PreconditionKind kind = PreconditionKind::Guard;
  if (!kind_node.is_null()) {
    if (auto k = parse_precondition_kind(kind_node)) {
      kind = *k;
    }
  } else {
    diags_.error(node_range(precond_node), "precondition missing kind");
  }

  Expr * cond = nullptr;
  if (!cond_node.is_null()) {
    cond = build_expr(cond_node);
  } else {
    cond = missing_expr(precond_node, "precondition missing condition expression");
  }

  return ast_.create<Precondition>(kind, cond, node_range(precond_node));
}

std::vector<Precondition *> AstBuilder::build_precondition_list(ts_ll::Node precond_list_node)
{
  std::vector<Precondition *> out;
  if (precond_list_node.is_null()) return out;

  if (precond_list_node.kind() != "precondition_list") {
    if (precond_list_node.kind() == "ERROR" || precond_list_node.is_missing()) {
      diags_.error(node_range(precond_list_node), "Invalid precondition_list");
      return out;
    }
    diags_.error(node_range(precond_list_node), "Expected 'precondition_list'");
    return out;
  }

  for (uint32_t i = 0; i < precond_list_node.named_child_count(); ++i) {
    const ts_ll::Node c = precond_list_node.named_child(i);
    if (c.kind() != "precondition") continue;
    out.push_back(build_precondition(c));
  }

  return out;
}

Argument * AstBuilder::build_argument(ts_ll::Node arg_node)
{
  if (arg_node.is_null()) {
    diags_.error({}, "Missing argument");
    auto * miss = ast_.create<MissingExpr>();
    return ast_.create<Argument>(ast_.intern("<missing>"), miss);
  }

  if (arg_node.kind() != "argument") {
    if (arg_node.kind() == "ERROR" || arg_node.is_missing()) {
      diags_.error(node_range(arg_node), "Invalid argument");
      auto * miss = ast_.create<MissingExpr>(node_range(arg_node));
      return ast_.create<Argument>(ast_.intern("<missing>"), miss, node_range(arg_node));
    }
    diags_.error(node_range(arg_node), "Expected 'argument'");
    return nullptr;
  }

  const ts_ll::Node name_node = arg_node.child_by_field("name");
  const ts_ll::Node value_node = arg_node.child_by_field("value");

  std::string_view name = ast_.intern("<missing>");
  if (!name_node.is_null()) {
    name = intern_text(name_node);
  } else {
    diags_.error(node_range(arg_node), "argument missing name");
  }

  if (value_node.is_null()) {
    diags_.error(node_range(arg_node), "argument missing value");
    auto * miss = ast_.create<MissingExpr>(node_range(arg_node));
    return ast_.create<Argument>(name, miss, node_range(arg_node));
  }

  // argument_expr:
  //   1) 'out' inline_blackboard_decl
  //   2) [port_direction] expression
  const ts_ll::Node inline_decl_node = value_node.child_by_field("inline_decl");
  if (!inline_decl_node.is_null()) {
    InlineBlackboardDecl * decl = build_inline_blackboard_decl(inline_decl_node);
    return ast_.create<Argument>(name, decl, node_range(arg_node));
  }

  // Expression form.
  std::optional<PortDirection> dir;
  for (uint32_t i = 0; i < value_node.named_child_count(); ++i) {
    const ts_ll::Node c = value_node.named_child(i);
    if (c.kind() == "port_direction") {
      dir = parse_port_direction(c);
      break;
    }
  }

  const ts_ll::Node expr_node = value_node.child_by_field("value");
  Expr * expr = nullptr;
  if (!expr_node.is_null()) {
    expr = build_expr(expr_node);
  } else {
    expr = missing_expr(value_node, "argument_expr missing expression");
  }

  return ast_.create<Argument>(name, dir, expr, node_range(arg_node));
}

}  // namespace bt_dsl
