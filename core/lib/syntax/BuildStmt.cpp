// bt_dsl/syntax/BuildStmt.cpp - CST -> AST for statements (tree bodies / children blocks)
#include <string>
#include <vector>

#include "bt_dsl/syntax/ast_builder.hpp"

namespace bt_dsl
{

static ts_ll::Node find_first_named_child(ts_ll::Node node, std::string_view kind)
{
  for (uint32_t i = 0; i < node.named_child_count(); ++i) {
    const ts_ll::Node c = node.named_child(i);
    if (c.kind() == kind) return c;
  }
  return ts_ll::Node{};
}

void AstBuilder::parse_children_block(ts_ll::Node children_block_node, std::vector<Stmt *> & out)
{
  if (children_block_node.is_null()) return;

  if (children_block_node.kind() != "children_block") {
    if (children_block_node.kind() == "ERROR" || children_block_node.is_missing()) {
      diags_.error(node_range(children_block_node), "Invalid children_block");
      return;
    }
    diags_.error(node_range(children_block_node), "Expected 'children_block'");
    return;
  }

  for (uint32_t i = 0; i < children_block_node.named_child_count(); ++i) {
    const ts_ll::Node st = children_block_node.named_child(i);
    if (st.kind() != "statement") continue;
    if (auto * s = build_statement(st)) {
      out.push_back(s);
    }
  }
}

Stmt * AstBuilder::build_statement(ts_ll::Node stmt_node)
{
  // grammar: statement = choice(seq(simple_stmt,';'), block_stmt)
  if (stmt_node.is_null()) {
    diags_.error({}, "Missing statement");
    return nullptr;
  }

  if (stmt_node.kind() != "statement") {
    if (stmt_node.kind() == "ERROR" || stmt_node.is_missing()) {
      diags_.error(node_range(stmt_node), "Invalid statement");
      return nullptr;
    }
    diags_.error(node_range(stmt_node), "Expected 'statement'");
    return nullptr;
  }

  if (stmt_node.named_child_count() == 0) {
    diags_.error(node_range(stmt_node), "Empty statement");
    return nullptr;
  }

  ts_ll::Node inner = stmt_node.named_child(0);

  // Peel wrappers.
  if (
    (inner.kind() == "simple_stmt" || inner.kind() == "block_stmt") &&
    inner.named_child_count() > 0) {
    inner = inner.named_child(0);
  }

  if (inner.is_null()) {
    diags_.error(node_range(stmt_node), "statement missing inner node");
    return nullptr;
  }

  if (inner.kind() == "leaf_node_call") {
    return build_leaf_node_call(inner);
  }
  if (inner.kind() == "compound_node_call") {
    return build_compound_node_call(inner);
  }
  if (inner.kind() == "assignment_stmt") {
    return build_assignment_stmt(inner);
  }
  if (inner.kind() == "blackboard_decl") {
    return build_blackboard_decl_stmt(inner);
  }
  if (inner.kind() == "local_const_decl") {
    return build_const_decl_stmt(inner);
  }

  if (inner.kind() == "ERROR" || inner.is_missing()) {
    diags_.error(node_range(inner), "Syntax error in statement");
    return nullptr;
  }

  diags_.error(
    node_range(inner), "Statement kind not implemented in core_v2 CST->AST builder yet: '" +
                         std::string(inner.kind()) + "'");
  return nullptr;
}

NodeStmt * AstBuilder::build_leaf_node_call(ts_ll::Node leaf_call_node)
{
  if (leaf_call_node.is_null()) {
    diags_.error({}, "Missing leaf_node_call");
    return nullptr;
  }
  if (leaf_call_node.kind() != "leaf_node_call") {
    if (leaf_call_node.kind() == "ERROR" || leaf_call_node.is_missing()) {
      diags_.error(node_range(leaf_call_node), "Invalid leaf_node_call");
    } else {
      diags_.error(node_range(leaf_call_node), "Expected 'leaf_node_call'");
    }
    return nullptr;
  }

  const auto docs_vec = collect_outer_docs(leaf_call_node);
  const ts_ll::Node pre_list = find_first_named_child(leaf_call_node, "precondition_list");
  const auto pre_vec = build_precondition_list(pre_list);

  const ts_ll::Node name_node = leaf_call_node.child_by_field("name");
  const ts_ll::Node args_node = leaf_call_node.child_by_field("args");

  std::string_view name = ast_.intern("<missing>");
  if (!name_node.is_null()) {
    name = intern_text(name_node);
  } else {
    diags_.error(node_range(leaf_call_node), "leaf_node_call missing name");
  }

  auto * stmt = ast_.create<NodeStmt>(name, node_range(leaf_call_node));
  stmt->hasPropertyBlock = true;

  stmt->docs = ast_.copy_to_arena(docs_vec);
  stmt->preconditions = ast_.copy_to_arena(pre_vec);

  // property_block -> optional argument_list
  std::vector<Argument *> args;
  if (!args_node.is_null() && args_node.kind() == "property_block") {
    const ts_ll::Node list = find_first_named_child(args_node, "argument_list");
    if (!list.is_null()) {
      for (uint32_t j = 0; j < list.named_child_count(); ++j) {
        const ts_ll::Node a = list.named_child(j);
        if (a.kind() != "argument") continue;
        if (auto * arg = build_argument(a)) args.push_back(arg);
      }
    }
  } else {
    // Grammar requires property_block; under error recovery we may not have it.
    diags_.error(node_range(leaf_call_node), "leaf_node_call missing property_block");
  }

  stmt->args = ast_.copy_to_arena(args);
  stmt->children = {};
  return stmt;
}

NodeStmt * AstBuilder::build_compound_node_call(ts_ll::Node compound_call_node)
{
  if (compound_call_node.is_null()) {
    diags_.error({}, "Missing compound_node_call");
    return nullptr;
  }
  if (compound_call_node.kind() != "compound_node_call") {
    if (compound_call_node.kind() == "ERROR" || compound_call_node.is_missing()) {
      diags_.error(node_range(compound_call_node), "Invalid compound_node_call");
    } else {
      diags_.error(node_range(compound_call_node), "Expected 'compound_node_call'");
    }
    return nullptr;
  }

  const auto docs_vec = collect_outer_docs(compound_call_node);
  const ts_ll::Node pre_list = find_first_named_child(compound_call_node, "precondition_list");
  const auto pre_vec = build_precondition_list(pre_list);

  const ts_ll::Node name_node = compound_call_node.child_by_field("name");
  const ts_ll::Node body_node = compound_call_node.child_by_field("body");

  std::string_view name = ast_.intern("<missing>");
  if (!name_node.is_null()) {
    name = intern_text(name_node);
  } else {
    diags_.error(node_range(compound_call_node), "compound_node_call missing name");
  }

  auto * stmt = ast_.create<NodeStmt>(name, node_range(compound_call_node));
  stmt->docs = ast_.copy_to_arena(docs_vec);
  stmt->preconditions = ast_.copy_to_arena(pre_vec);

  // node_body_with_children = (property_block children_block) | children_block
  ts_ll::Node prop_node;
  ts_ll::Node children_node;

  if (!body_node.is_null() && body_node.kind() == "node_body_with_children") {
    prop_node = find_first_named_child(body_node, "property_block");
    children_node = find_first_named_child(body_node, "children_block");
  } else {
    // Under error recovery, attempt to locate directly.
    prop_node = find_first_named_child(compound_call_node, "property_block");
    children_node = find_first_named_child(compound_call_node, "children_block");
    if (body_node.is_null()) {
      diags_.error(node_range(compound_call_node), "compound_node_call missing body");
    } else {
      diags_.error(node_range(body_node), "Expected 'node_body_with_children'");
    }
  }

  // Args
  std::vector<Argument *> args;
  if (!prop_node.is_null()) {
    stmt->hasPropertyBlock = true;
    const ts_ll::Node list = find_first_named_child(prop_node, "argument_list");
    if (!list.is_null()) {
      for (uint32_t j = 0; j < list.named_child_count(); ++j) {
        const ts_ll::Node a = list.named_child(j);
        if (a.kind() != "argument") continue;
        if (auto * arg = build_argument(a)) args.push_back(arg);
      }
    }
  } else {
    stmt->hasPropertyBlock = false;
  }
  stmt->args = ast_.copy_to_arena(args);

  // Children
  std::vector<Stmt *> children;
  if (!children_node.is_null()) {
    stmt->hasChildrenBlock = true;
    parse_children_block(children_node, children);
  } else {
    // Grammar requires children_block.
    stmt->hasChildrenBlock = false;
    diags_.error(node_range(compound_call_node), "compound_node_call missing children_block");
  }

  stmt->children = ast_.copy_to_arena(children);
  return stmt;
}

AssignmentStmt * AstBuilder::build_assignment_stmt(ts_ll::Node assignment_node)
{
  if (assignment_node.is_null()) {
    diags_.error({}, "Missing assignment_stmt");
    return nullptr;
  }
  if (assignment_node.kind() != "assignment_stmt") {
    if (assignment_node.kind() == "ERROR" || assignment_node.is_missing()) {
      diags_.error(node_range(assignment_node), "Invalid assignment_stmt");
    } else {
      diags_.error(node_range(assignment_node), "Expected 'assignment_stmt'");
    }
    return nullptr;
  }

  const auto docs_vec = collect_outer_docs(assignment_node);
  const ts_ll::Node pre_list = find_first_named_child(assignment_node, "precondition_list");
  const auto pre_vec = build_precondition_list(pre_list);

  const ts_ll::Node lvalue_node = assignment_node.child_by_field("target");
  const ts_ll::Node op_node = assignment_node.child_by_field("op");
  const ts_ll::Node value_node = assignment_node.child_by_field("value");

  std::string_view target = ast_.intern("<missing>");
  std::vector<Expr *> indices;

  if (!lvalue_node.is_null() && lvalue_node.kind() == "lvalue") {
    const ts_ll::Node base = lvalue_node.child_by_field("base");
    if (!base.is_null()) {
      target = intern_text(base);
    } else {
      diags_.error(node_range(lvalue_node), "lvalue missing base identifier");
    }

    for (uint32_t i = 0; i < lvalue_node.named_child_count(); ++i) {
      const ts_ll::Node c = lvalue_node.named_child(i);
      if (c.kind() != "index_suffix") continue;
      if (c.named_child_count() >= 1) {
        indices.push_back(build_expr(c.named_child(0)));
      } else {
        indices.push_back(ast_.create<MissingExpr>(node_range(c)));
        diags_.error(node_range(c), "index_suffix missing expression");
      }
    }
  } else {
    if (lvalue_node.is_null()) {
      diags_.error(node_range(assignment_node), "assignment_stmt missing lvalue");
    } else {
      diags_.error(node_range(lvalue_node), "Expected 'lvalue'");
    }
  }

  AssignOp op = AssignOp::Assign;
  if (op_node.is_null()) {
    diags_.error(node_range(assignment_node), "assignment_stmt missing assignment_op");
  } else if (auto parsed = parse_assign_op(op_node)) {
    op = *parsed;
  } else {
    diags_.error(node_range(op_node), "Unsupported assignment_op");
  }

  Expr * value = nullptr;
  if (!value_node.is_null()) {
    value = build_expr(value_node);
  } else {
    value = missing_expr(assignment_node, "assignment_stmt missing value expression");
  }

  auto * stmt = ast_.create<AssignmentStmt>(target, op, value, node_range(assignment_node));
  stmt->docs = ast_.copy_to_arena(docs_vec);
  stmt->preconditions = ast_.copy_to_arena(pre_vec);
  stmt->indices = ast_.copy_to_arena(indices);
  return stmt;
}

BlackboardDeclStmt * AstBuilder::build_blackboard_decl_stmt(ts_ll::Node decl_node)
{
  if (decl_node.is_null()) {
    diags_.error({}, "Missing blackboard_decl");
    return nullptr;
  }
  if (decl_node.kind() != "blackboard_decl") {
    if (decl_node.kind() == "ERROR" || decl_node.is_missing()) {
      diags_.error(node_range(decl_node), "Invalid blackboard_decl");
    } else {
      diags_.error(node_range(decl_node), "Expected 'blackboard_decl'");
    }
    return nullptr;
  }

  const auto docs_vec = collect_outer_docs(decl_node);

  const ts_ll::Node name_node = decl_node.child_by_field("name");
  const ts_ll::Node type_node = decl_node.child_by_field("type");
  const ts_ll::Node init_node = decl_node.child_by_field("init");

  std::string_view name = ast_.intern("<missing>");
  if (!name_node.is_null()) {
    name = intern_text(name_node);
  } else {
    diags_.error(node_range(decl_node), "blackboard_decl missing name");
  }

  auto * stmt = ast_.create<BlackboardDeclStmt>(name, node_range(decl_node));
  stmt->docs = ast_.copy_to_arena(docs_vec);

  if (!type_node.is_null()) {
    stmt->type = build_type(type_node);
  }

  if (!init_node.is_null()) {
    stmt->initialValue = build_expr(init_node);
  }

  return stmt;
}

ConstDeclStmt * AstBuilder::build_const_decl_stmt(ts_ll::Node decl_node)
{
  if (decl_node.is_null()) {
    diags_.error({}, "Missing local_const_decl");
    return nullptr;
  }
  if (decl_node.kind() != "local_const_decl") {
    if (decl_node.kind() == "ERROR" || decl_node.is_missing()) {
      diags_.error(node_range(decl_node), "Invalid local_const_decl");
    } else {
      diags_.error(node_range(decl_node), "Expected 'local_const_decl'");
    }
    return nullptr;
  }

  const auto docs_vec = collect_outer_docs(decl_node);

  const ts_ll::Node name_node = decl_node.child_by_field("name");
  const ts_ll::Node type_node = decl_node.child_by_field("type");
  const ts_ll::Node value_node = decl_node.child_by_field("value");

  std::string_view name = ast_.intern("<missing>");
  if (!name_node.is_null()) {
    name = intern_text(name_node);
  } else {
    diags_.error(node_range(decl_node), "local_const_decl missing name");
  }

  Expr * value = nullptr;
  if (!value_node.is_null()) {
    value = build_expr(value_node);
  } else {
    value = missing_expr(decl_node, "local_const_decl missing value expression");
  }

  auto * stmt = ast_.create<ConstDeclStmt>(name, value, node_range(decl_node));
  stmt->docs = ast_.copy_to_arena(docs_vec);

  if (!type_node.is_null()) {
    stmt->type = build_type(type_node);
  }

  return stmt;
}

}  // namespace bt_dsl
