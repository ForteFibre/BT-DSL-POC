// bt_dsl/ast/json_visitor.cpp - JSON serialization implementation
//
#include "bt_dsl/ast/json_visitor.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{
namespace
{

using nlohmann::json;

// ============================================================================
// Helper functions
// ============================================================================

uint32_t begin_off(SourceRange r) { return r.get_begin().get_offset(); }
uint32_t end_off(SourceRange r) { return r.get_end().get_offset(); }

json j_range(SourceRange r)
{
  if (r.is_invalid()) {
    return json{{"start", nullptr}, {"end", nullptr}};
  }
  return json{{"start", begin_off(r)}, {"end", end_off(r)}};
}

// Forward declarations
json j_type(const TypeNode * t);
json j_expr(const Expr * e);
json j_stmt(const Stmt * s);
json j_decl(const Decl * d);

// ============================================================================
// Type serialization
// ============================================================================

json j_type(const TypeNode * t)
{
  if (!t) return json{{"type", "MissingType"}, {"range", j_range({})}};

  if (isa<TypeExpr>(t)) {
    const auto * te = cast<TypeExpr>(t);
    return json{
      {"type", "TypeExpr"},
      {"range", j_range(te->get_range())},
      {"base", j_type(te->base)},
      {"nullable", te->nullable}};
  }

  if (isa<InferType>(t)) {
    return json{{"type", "InferType"}, {"range", j_range(t->get_range())}};
  }

  if (isa<PrimaryType>(t)) {
    const auto * pt = cast<PrimaryType>(t);
    json j{
      {"type", "PrimaryType"},
      {"range", j_range(pt->get_range())},
      {"name", std::string(pt->name)}};
    if (pt->size) {
      j["size"] = std::string(*pt->size);
    }
    return j;
  }

  if (isa<DynamicArrayType>(t)) {
    const auto * dt = cast<DynamicArrayType>(t);
    return json{
      {"type", "DynamicArrayType"},
      {"range", j_range(dt->get_range())},
      {"elementType", j_type(dt->elementType)}};
  }

  if (isa<StaticArrayType>(t)) {
    const auto * st = cast<StaticArrayType>(t);
    return json{
      {"type", "StaticArrayType"},
      {"range", j_range(st->get_range())},
      {"elementType", j_type(st->elementType)},
      {"size", std::string(st->size)},
      {"isBounded", st->isBounded}};
  }

  return json{{"type", "UnknownType"}, {"range", j_range(t->get_range())}};
}

// ============================================================================
// Expression serialization
// ============================================================================

json j_expr(const Expr * e)
{
  if (!e) return json{{"type", "MissingExpr"}, {"range", j_range({})}};

  if (isa<MissingExpr>(e)) {
    return json{{"type", "MissingExpr"}, {"range", j_range(e->get_range())}};
  }

  if (isa<IntLiteralExpr>(e)) {
    const auto * lit = cast<IntLiteralExpr>(e);
    return json{
      {"type", "IntLiteralExpr"}, {"range", j_range(lit->get_range())}, {"value", lit->value}};
  }

  if (isa<FloatLiteralExpr>(e)) {
    const auto * lit = cast<FloatLiteralExpr>(e);
    return json{
      {"type", "FloatLiteralExpr"}, {"range", j_range(lit->get_range())}, {"value", lit->value}};
  }

  if (isa<StringLiteralExpr>(e)) {
    const auto * lit = cast<StringLiteralExpr>(e);
    return json{
      {"type", "StringLiteralExpr"},
      {"range", j_range(lit->get_range())},
      {"value", std::string(lit->value)}};
  }

  if (isa<BoolLiteralExpr>(e)) {
    const auto * lit = cast<BoolLiteralExpr>(e);
    return json{
      {"type", "BoolLiteralExpr"}, {"range", j_range(lit->get_range())}, {"value", lit->value}};
  }

  if (isa<NullLiteralExpr>(e)) {
    return json{{"type", "NullLiteralExpr"}, {"range", j_range(e->get_range())}};
  }

  if (isa<VarRefExpr>(e)) {
    const auto * v = cast<VarRefExpr>(e);
    return json{
      {"type", "VarRefExpr"}, {"range", j_range(v->get_range())}, {"name", std::string(v->name)}};
  }

  if (isa<UnaryExpr>(e)) {
    const auto * u = cast<UnaryExpr>(e);
    return json{
      {"type", "UnaryExpr"},
      {"range", j_range(u->get_range())},
      {"op", std::string(to_string(u->op))},
      {"operand", j_expr(u->operand)}};
  }

  if (isa<BinaryExpr>(e)) {
    const auto * b = cast<BinaryExpr>(e);
    return json{
      {"type", "BinaryExpr"},
      {"range", j_range(b->get_range())},
      {"op", std::string(to_string(b->op))},
      {"lhs", j_expr(b->lhs)},
      {"rhs", j_expr(b->rhs)}};
  }

  if (isa<IndexExpr>(e)) {
    const auto * idx = cast<IndexExpr>(e);
    return json{
      {"type", "IndexExpr"},
      {"range", j_range(idx->get_range())},
      {"base", j_expr(idx->base)},
      {"index", j_expr(idx->index)}};
  }

  if (isa<CastExpr>(e)) {
    const auto * c = cast<CastExpr>(e);
    return json{
      {"type", "CastExpr"},
      {"range", j_range(c->get_range())},
      {"expr", j_expr(c->expr)},
      {"targetType", j_type(c->targetType)}};
  }

  if (isa<ArrayLiteralExpr>(e)) {
    const auto * a = cast<ArrayLiteralExpr>(e);
    json elems = json::array();
    for (auto * el : a->elements) {
      elems.push_back(j_expr(el));
    }
    return json{
      {"type", "ArrayLiteralExpr"}, {"range", j_range(a->get_range())}, {"elements", elems}};
  }

  if (isa<ArrayRepeatExpr>(e)) {
    const auto * a = cast<ArrayRepeatExpr>(e);
    return json{
      {"type", "ArrayRepeatExpr"},
      {"range", j_range(a->get_range())},
      {"value", j_expr(a->value)},
      {"count", j_expr(a->count)}};
  }

  if (isa<VecMacroExpr>(e)) {
    const auto * vm = cast<VecMacroExpr>(e);
    return json{
      {"type", "VecMacroExpr"}, {"range", j_range(vm->get_range())}, {"inner", j_expr(vm->inner)}};
  }

  return json{{"type", "UnknownExpr"}, {"range", j_range(e->get_range())}};
}

// ============================================================================
// Supporting node serialization
// ============================================================================

json j_precond(const Precondition * p)
{
  if (!p) return json{{"type", "Precondition"}, {"range", j_range({})}};

  std::string_view k = "guard";
  switch (p->kind) {
    case PreconditionKind::SuccessIf:
      k = "success_if";
      break;
    case PreconditionKind::FailureIf:
      k = "failure_if";
      break;
    case PreconditionKind::SkipIf:
      k = "skip_if";
      break;
    case PreconditionKind::RunWhile:
      k = "run_while";
      break;
    case PreconditionKind::Guard:
      k = "guard";
      break;
  }

  return json{
    {"type", "Precondition"},
    {"range", j_range(p->get_range())},
    {"kind", std::string(k)},
    {"condition", j_expr(p->condition)}};
}

json j_argument(const Argument * a)
{
  if (!a) return json{{"type", "Argument"}, {"range", j_range({})}};

  json j{{"type", "Argument"}, {"range", j_range(a->get_range())}};
  j["name"] = std::string(a->name);
  if (a->direction) {
    j["direction"] = std::string(to_string(*a->direction));
  }
  if (a->inlineDecl) {
    j["inlineDecl"] = json{
      {"type", "InlineBlackboardDecl"},
      {"range", j_range(a->inlineDecl->get_range())},
      {"name", std::string(a->inlineDecl->name)}};
  } else {
    j["valueExpr"] = j_expr(a->valueExpr);
  }
  return j;
}

json j_param(const ParamDecl * p)
{
  if (!p) return json{{"type", "ParamDecl"}, {"range", j_range({})}};
  json j{
    {"type", "ParamDecl"},
    {"range", j_range(p->get_range())},
    {"name", std::string(p->name)},
    {"typeExpr", j_type(p->type)}};
  if (p->direction) {
    j["direction"] = std::string(to_string(*p->direction));
  }
  if (p->defaultValue) {
    j["defaultValue"] = j_expr(p->defaultValue);
  }
  return j;
}

json j_extern_port(const ExternPort * p)
{
  if (!p) return json{{"type", "ExternPort"}, {"range", j_range({})}};
  json j{
    {"type", "ExternPort"},
    {"range", j_range(p->get_range())},
    {"name", std::string(p->name)},
    {"typeExpr", j_type(p->type)}};
  if (p->direction) {
    j["direction"] = std::string(to_string(*p->direction));
  }
  if (p->defaultValue) {
    j["defaultValue"] = j_expr(p->defaultValue);
  }
  return j;
}

// ============================================================================
// Statement serialization
// ============================================================================

json j_stmt(const Stmt * s)
{
  if (!s) return json{{"type", "MissingStmt"}, {"range", j_range({})}};

  if (isa<BlackboardDeclStmt>(s)) {
    const auto * v = cast<BlackboardDeclStmt>(s);
    json j{
      {"type", "BlackboardDeclStmt"},
      {"range", j_range(v->get_range())},
      {"name", std::string(v->name)}};
    if (v->type) j["typeExpr"] = j_type(v->type);
    if (v->initialValue) j["initialValue"] = j_expr(v->initialValue);
    return j;
  }

  if (isa<ConstDeclStmt>(s)) {
    const auto * c = cast<ConstDeclStmt>(s);
    json j{
      {"type", "ConstDeclStmt"},
      {"range", j_range(c->get_range())},
      {"name", std::string(c->name)},
      {"value", j_expr(c->value)}};
    if (c->type) j["typeExpr"] = j_type(c->type);
    return j;
  }

  if (isa<AssignmentStmt>(s)) {
    const auto * a = cast<AssignmentStmt>(s);
    json pre = json::array();
    for (auto * p : a->preconditions) pre.push_back(j_precond(p));
    json idx = json::array();
    for (auto * i : a->indices) idx.push_back(j_expr(i));
    return json{{"type", "AssignmentStmt"}, {"range", j_range(a->get_range())},
                {"preconditions", pre},     {"target", std::string(a->target)},
                {"indices", idx},           {"op", std::string(to_string(a->op))},
                {"value", j_expr(a->value)}};
  }

  if (isa<NodeStmt>(s)) {
    const auto * n = cast<NodeStmt>(s);
    json pre = json::array();
    for (auto * p : n->preconditions) pre.push_back(j_precond(p));
    json args = json::array();
    for (auto * a : n->args) args.push_back(j_argument(a));
    json children = json::array();
    for (auto * ch : n->children) children.push_back(j_stmt(ch));
    return json{
      {"type", "NodeStmt"},
      {"range", j_range(n->get_range())},
      {"nodeName", std::string(n->nodeName)},
      {"preconditions", pre},
      {"args", args},
      {"hasPropertyBlock", n->hasPropertyBlock},
      {"hasChildrenBlock", n->hasChildrenBlock},
      {"children", children}};
  }

  return json{{"type", "UnknownStmt"}, {"range", j_range(s->get_range())}};
}

// ============================================================================
// Declaration serialization
// ============================================================================

json j_decl(const Decl * d)
{
  if (!d) return json{{"type", "MissingDecl"}, {"range", j_range({})}};

  if (isa<ImportDecl>(d)) {
    const auto * imp = cast<ImportDecl>(d);
    return json{
      {"type", "ImportDecl"},
      {"range", j_range(imp->get_range())},
      {"path", std::string(imp->path)}};
  }

  if (isa<ExternTypeDecl>(d)) {
    const auto * et = cast<ExternTypeDecl>(d);
    return json{
      {"type", "ExternTypeDecl"},
      {"range", j_range(et->get_range())},
      {"name", std::string(et->name)}};
  }

  if (isa<TypeAliasDecl>(d)) {
    const auto * ta = cast<TypeAliasDecl>(d);
    return json{
      {"type", "TypeAliasDecl"},
      {"range", j_range(ta->get_range())},
      {"name", std::string(ta->name)},
      {"aliasedType", j_type(ta->aliasedType)}};
  }

  if (isa<ExternDecl>(d)) {
    const auto * ex = cast<ExternDecl>(d);
    json ports = json::array();
    for (auto * p : ex->ports) ports.push_back(j_extern_port(p));

    json j{
      {"type", "ExternDecl"},
      {"range", j_range(ex->get_range())},
      {"category", std::string(to_string(ex->category))},
      {"name", std::string(ex->name)},
      {"ports", ports}};

    if (ex->behaviorAttr) {
      json a{
        {"type", "BehaviorAttr"},
        {"range", j_range(ex->behaviorAttr->get_range())},
        {"dataPolicy", std::string(to_string(ex->behaviorAttr->dataPolicy))}};
      if (ex->behaviorAttr->flowPolicy) {
        a["flowPolicy"] = std::string(to_string(*ex->behaviorAttr->flowPolicy));
      }
      j["behaviorAttr"] = a;
    }

    return j;
  }

  if (isa<GlobalVarDecl>(d)) {
    const auto * gv = cast<GlobalVarDecl>(d);
    json j{
      {"type", "GlobalVarDecl"},
      {"range", j_range(gv->get_range())},
      {"name", std::string(gv->name)}};
    if (gv->type) j["typeExpr"] = j_type(gv->type);
    if (gv->initialValue) j["initialValue"] = j_expr(gv->initialValue);
    return j;
  }

  if (isa<GlobalConstDecl>(d)) {
    const auto * gc = cast<GlobalConstDecl>(d);
    json j{
      {"type", "GlobalConstDecl"},
      {"range", j_range(gc->get_range())},
      {"name", std::string(gc->name)},
      {"value", j_expr(gc->value)}};
    if (gc->type) j["typeExpr"] = j_type(gc->type);
    return j;
  }

  if (isa<TreeDecl>(d)) {
    const auto * t = cast<TreeDecl>(d);
    json params = json::array();
    for (auto * p : t->params) params.push_back(j_param(p));
    json body = json::array();
    for (auto * s : t->body) body.push_back(j_stmt(s));
    return json{
      {"type", "TreeDecl"},
      {"range", j_range(t->get_range())},
      {"name", std::string(t->name)},
      {"params", params},
      {"body", body}};
  }

  return json{{"type", "UnknownDecl"}, {"range", j_range(d->get_range())}};
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

nlohmann::json to_json(const AstNode * node)
{
  if (!node) return nlohmann::json{{"type", "null"}, {"range", j_range({})}};

  // Dispatch based on node category
  if (isa<Program>(node)) {
    return to_json(cast<Program>(node));
  }
  if (isa<Decl>(node)) {
    return j_decl(cast<Decl>(node));
  }
  if (isa<Stmt>(node)) {
    return j_stmt(cast<Stmt>(node));
  }
  if (isa<Expr>(node)) {
    return j_expr(cast<Expr>(node));
  }
  if (isa<TypeNode>(node)) {
    return j_type(cast<TypeNode>(node));
  }

  // Supporting nodes
  if (isa<Precondition>(node)) {
    return j_precond(cast<Precondition>(node));
  }
  if (isa<Argument>(node)) {
    return j_argument(cast<Argument>(node));
  }
  if (isa<ParamDecl>(node)) {
    return j_param(cast<ParamDecl>(node));
  }
  if (isa<ExternPort>(node)) {
    return j_extern_port(cast<ExternPort>(node));
  }

  return nlohmann::json{{"type", "UnknownNode"}, {"range", j_range(node->get_range())}};
}

nlohmann::json to_json(const Program * program)
{
  if (!program)
    return nlohmann::json{
      {"type", "Program"}, {"range", j_range({})}, {"decls", nlohmann::json::array()}};

  nlohmann::json decls = nlohmann::json::array();

  for (auto * d : program->decls) decls.push_back(j_decl(d));

  return nlohmann::json{
    {"type", "Program"}, {"range", j_range(program->get_range())}, {"decls", decls}};
}

}  // namespace bt_dsl
