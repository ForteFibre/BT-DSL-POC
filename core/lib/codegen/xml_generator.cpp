#include "bt_dsl/codegen/xml_generator.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <gsl/span>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/sema/resolution/symbol_table.hpp"
#include "bt_dsl/sema/types/const_value.hpp"
#include "tinyxml2.h"

namespace bt_dsl
{

namespace
{

enum class ExprMode : std::uint8_t {
  Script,
  Precondition,
  AttributeValue,
};

[[nodiscard]] std::string to_string_lossless(double v)
{
  std::ostringstream oss;
  oss.precision(17);
  oss << v;
  return oss.str();
}

[[nodiscard]] std::string quote_script_string(std::string_view s)
{
  std::string escaped;
  escaped.reserve(s.size());
  for (const char c : s) {
    if (c == '\'') {
      escaped += "\\\'";
    } else {
      escaped.push_back(c);
    }
  }
  return "'" + escaped + "'";
}

[[nodiscard]] bool is_null_literal_expr(const Expr * expr)
{
  return expr != nullptr && expr->get_kind() == NodeKind::NullLiteral;
}

[[nodiscard]] bool is_simple_value_expr_for_in_port(const Expr * expr)
{
  if (!expr) {
    return false;
  }
  const NodeKind k = expr->get_kind();
  return k == NodeKind::IntLiteral || k == NodeKind::FloatLiteral || k == NodeKind::StringLiteral ||
         k == NodeKind::BoolLiteral || k == NodeKind::NullLiteral || k == NodeKind::VarRef;
}

[[nodiscard]] bool is_public_name(std::string_view name) { return ModuleInfo::is_public(name); }

[[nodiscard]] std::string choose_entry_tree_name(const Program & program)
{
  for (const auto * t : program.trees) {
    if (t && t->name == "Main") {
      return "Main";
    }
  }
  if (!program.trees.empty() && program.trees[0]) {
    return std::string(program.trees[0]->name);
  }
  return "Main";
}

[[nodiscard]] PortDirection port_direction_or_default(const std::optional<PortDirection> & dir)
{
  return dir.value_or(PortDirection::In);
}

[[nodiscard]] btcpp::PortKind to_btcpp_port_kind(PortDirection dir)
{
  switch (dir) {
    case PortDirection::Out:
      return btcpp::PortKind::Output;
    case PortDirection::Ref:
    case PortDirection::Mut:
      return btcpp::PortKind::InOut;
    case PortDirection::In:
      return btcpp::PortKind::Input;
  }
  return btcpp::PortKind::Input;
}

[[nodiscard]] std::string render_type_node(const TypeNode * node)
{
  if (!node) {
    return {};
  }

  switch (node->get_kind()) {
    case NodeKind::InferType:
      return "_";
    case NodeKind::PrimaryType: {
      const auto * pt = static_cast<const PrimaryType *>(node);
      if (pt->size.has_value()) {
        return std::string(pt->name) + "<" + std::string(*pt->size) + ">";
      }
      return std::string(pt->name);
    }
    case NodeKind::StaticArrayType: {
      const auto * at = static_cast<const StaticArrayType *>(node);
      std::string out = "[" + render_type_node(at->elementType) + "; ";
      if (at->isBounded) {
        out += "<=";
      }
      out += std::string(at->size);
      out += "]";
      return out;
    }
    case NodeKind::DynamicArrayType: {
      const auto * dt = static_cast<const DynamicArrayType *>(node);
      return "vec<" + render_type_node(dt->elementType) + ">";
    }
    case NodeKind::TypeExpr: {
      const auto * te = static_cast<const TypeExpr *>(node);
      std::string out = render_type_node(te->base);
      if (te->nullable) {
        out += "?";
      }
      return out;
    }

    default:
      break;
  }

  return {};
}

[[nodiscard]] std::string render_type_expr(const TypeExpr * type) { return render_type_node(type); }

[[nodiscard]] std::string default_init_for_type(const TypeExpr * type)
{
  // xml-mapping.md §6.3.2: initialize out var with a default value.
  // Best-effort: map common scalar types; fall back to 0.
  if (!type) {
    return "0";
  }

  const TypeNode * base = type->base;
  const auto * pt = (base && base->get_kind() == NodeKind::PrimaryType)
                      ? static_cast<const PrimaryType *>(base)
                      : nullptr;
  if (pt) {
    const std::string_view n = pt->name;
    if (n == "string") {
      return "''";
    }
    if (n.find("float") != std::string_view::npos || n.find("double") != std::string_view::npos) {
      return "0.0";
    }
    if (n == "bool") {
      return "false";
    }
  }

  return "0";
}

[[nodiscard]] std::string format_const_value_for_mode(const ConstValue & v, bool attribute_value)
{
  if (v.is_integer()) {
    return std::to_string(v.as_integer());
  }
  if (v.is_float()) {
    return to_string_lossless(v.as_float());
  }
  if (v.is_bool()) {
    return v.as_bool() ? "true" : "false";
  }
  if (v.is_string()) {
    if (attribute_value) {
      return std::string(v.as_string());
    }
    return quote_script_string(v.as_string());
  }
  if (v.is_null()) {
    return "null";
  }
  if (v.is_array()) {
    // Arrays are forbidden by xml-mapping.md output mode restrictions; keep best-effort.
    const auto arr = v.as_array();
    std::string out = "[";
    const std::size_t lim = (arr.size() > 16U) ? 16U : arr.size();
    for (std::size_t i = 0; i < lim; ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += format_const_value_for_mode(arr[i], attribute_value);
    }
    if (lim < arr.size()) {
      out += ", ...";
    }
    out += "]";
    return out;
  }

  return {};
}

[[nodiscard]] const ConstValue * try_get_const_value_from_symbol(const Symbol * sym)
{
  if (!sym || !sym->is_const()) {
    return nullptr;
  }
  if (!sym->astNode) {
    return nullptr;
  }

  if (sym->astNode->get_kind() == NodeKind::GlobalConstDecl) {
    const auto * decl = static_cast<const GlobalConstDecl *>(sym->astNode);
    return decl->evaluatedValue;
  }
  if (sym->astNode->get_kind() == NodeKind::ConstDeclStmt) {
    const auto * decl = static_cast<const ConstDeclStmt *>(sym->astNode);
    return decl->evaluatedValue;
  }

  return nullptr;
}

struct CodegenContext
{
  const ModuleInfo & module;

  // Variable mangling for BT.CPP flat blackboard
  std::uint32_t next_id = 1;

  // Tree ID mapping for single-output mode (optional)
  std::function<std::string(const TreeDecl *)> subtree_id_resolver;

  struct Frame
  {
    Frame * parent = nullptr;
    std::unordered_map<std::string_view, std::string> var_keys;
  };

  std::vector<std::unique_ptr<Frame>> frames;
  Frame * current = nullptr;

  explicit CodegenContext(const ModuleInfo & m) : module(m)
  {
    auto root = std::make_unique<Frame>();
    root->parent = nullptr;
    root->var_keys.reserve(64);
    current = root.get();
    frames.push_back(std::move(root));
  }

  void push_block()
  {
    auto f = std::make_unique<Frame>();
    f->parent = current;
    f->var_keys.reserve(32);
    current = f.get();
    frames.push_back(std::move(f));
  }

  void pop_block()
  {
    if (current && current->parent) {
      current = current->parent;
    }
  }

  [[nodiscard]] std::string declare_var(std::string_view name)
  {
    const std::string key = std::string(name) + "#" + std::to_string(next_id++);
    current->var_keys.insert_or_assign(name, key);
    return key;
  }

  [[nodiscard]] std::optional<std::string_view> lookup_local_var_key(std::string_view name) const
  {
    for (const Frame * f = current; f; f = f->parent) {
      auto it = f->var_keys.find(name);
      if (it != f->var_keys.end()) {
        return std::string_view(it->second);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static bool is_global_var(const Symbol * sym)
  {
    return sym != nullptr && sym->kind == SymbolKind::GlobalVariable;
  }

  [[nodiscard]] std::string var_ref(std::string_view name, const Symbol * sym, ExprMode mode) const
  {
    if (sym && (sym->kind == SymbolKind::LocalVariable || sym->kind == SymbolKind::BlockVariable)) {
      if (auto local = lookup_local_var_key(name)) {
        if (mode == ExprMode::Script) {
          return std::string(*local);
        }
        return "{" + std::string(*local) + "}";
      }
      // Missing mapping (should not happen after successful sema)
      if (mode == ExprMode::Script) {
        return std::string(name);
      }
      return "{" + std::string(name) + "}";
    }

    if (is_global_var(sym)) {
      return "@{" + std::string(name) + "}";
    }

    // Parameters and unknowns: use plain name (with braces in attribute/precondition mode)
    if (mode == ExprMode::Script) {
      return std::string(name);
    }
    return "{" + std::string(name) + "}";
  }

  [[nodiscard]] std::string subtree_xml_id(const TreeDecl * tree) const
  {
    if (subtree_id_resolver) {
      return subtree_id_resolver(tree);
    }
    return tree ? std::string(tree->name) : std::string{};
  }
};

[[nodiscard]] std::string serialize_expression(
  const Expr * expr, CodegenContext & ctx, ExprMode mode);

[[nodiscard]] std::string serialize_expression(
  const Expr * expr, CodegenContext & ctx, ExprMode mode)
{
  if (!expr) {
    return {};
  }

  switch (expr->get_kind()) {
    case NodeKind::IntLiteral: {
      const auto * e = static_cast<const IntLiteralExpr *>(expr);
      return std::to_string(e->value);
    }
    case NodeKind::FloatLiteral: {
      const auto * e = static_cast<const FloatLiteralExpr *>(expr);
      return to_string_lossless(e->value);
    }
    case NodeKind::BoolLiteral: {
      const auto * e = static_cast<const BoolLiteralExpr *>(expr);
      return e->value ? "true" : "false";
    }
    case NodeKind::NullLiteral:
      return "null";
    case NodeKind::StringLiteral: {
      const auto * e = static_cast<const StringLiteralExpr *>(expr);
      if (mode == ExprMode::AttributeValue) {
        return std::string(e->value);
      }
      return quote_script_string(e->value);
    }
    case NodeKind::VarRef: {
      const auto * e = static_cast<const VarRefExpr *>(expr);
      if (const ConstValue * cv = try_get_const_value_from_symbol(e->resolvedSymbol)) {
        return format_const_value_for_mode(*cv, mode == ExprMode::AttributeValue);
      }
      return ctx.var_ref(e->name, e->resolvedSymbol, mode);
    }
    case NodeKind::BinaryExpr: {
      const auto * e = static_cast<const BinaryExpr *>(expr);
      const auto left = serialize_expression(e->lhs, ctx, mode);
      const auto right = serialize_expression(e->rhs, ctx, mode);
      return "(" + left + " " + std::string(to_string(e->op)) + " " + right + ")";
    }
    case NodeKind::UnaryExpr: {
      const auto * e = static_cast<const UnaryExpr *>(expr);
      const auto operand = serialize_expression(e->operand, ctx, mode);
      return std::string(to_string(e->op)) + operand;
    }
    case NodeKind::CastExpr: {
      // xml-mapping.md output mode forbids casts; best-effort: keep inner expression.
      const auto * e = static_cast<const CastExpr *>(expr);
      return serialize_expression(e->expr, ctx, mode);
    }
    case NodeKind::IndexExpr: {
      const auto * e = static_cast<const IndexExpr *>(expr);
      const auto base = serialize_expression(e->base, ctx, mode);
      const auto idx = serialize_expression(e->index, ctx, mode);
      return base + "[" + idx + "]";
    }
    case NodeKind::ArrayLiteralExpr: {
      const auto * e = static_cast<const ArrayLiteralExpr *>(expr);
      std::string out = "[";
      for (std::size_t i = 0; i < e->elements.size(); ++i) {
        if (i > 0) {
          out += ", ";
        }
        out += serialize_expression(e->elements[i], ctx, mode);
      }
      out += "]";
      return out;
    }
    case NodeKind::ArrayRepeatExpr: {
      const auto * e = static_cast<const ArrayRepeatExpr *>(expr);
      return "[" + serialize_expression(e->value, ctx, mode) + "; " +
             serialize_expression(e->count, ctx, mode) + "]";
    }
    case NodeKind::VecMacroExpr: {
      const auto * e = static_cast<const VecMacroExpr *>(expr);
      return "vec!" + serialize_expression(e->inner, ctx, mode);
    }
    case NodeKind::MissingExpr:
      return {};

    default:
      break;
  }

  return {};
}

[[nodiscard]] btcpp::Node make_plain_script_node(std::string code)
{
  btcpp::Node script;
  script.tag = "Script";
  script.attributes.push_back(btcpp::Attribute{"code", " " + std::move(code) + " "});
  return script;
}

[[nodiscard]] btcpp::Node make_assignment_script_node(std::string_view lhs, std::string_view rhs)
{
  std::string code;
  code.reserve(lhs.size() + rhs.size() + 4U);
  code.append(lhs);
  code.append(" := ");
  code.append(rhs);
  return make_plain_script_node(std::move(code));
}

[[nodiscard]] btcpp::Node apply_preconditions_and_guard(
  btcpp::Node node, gsl::span<Precondition * const> preconditions, CodegenContext & ctx)
{
  std::vector<const Expr *> guard_conditions;
  guard_conditions.reserve(preconditions.size());

  for (const auto * pc : preconditions) {
    if (!pc) {
      continue;
    }
    if (pc->kind == PreconditionKind::Guard) {
      guard_conditions.push_back(pc->condition);
      continue;
    }

    std::string attr_name;
    switch (pc->kind) {
      case PreconditionKind::SuccessIf:
        attr_name = "_successIf";
        break;
      case PreconditionKind::FailureIf:
        attr_name = "_failureIf";
        break;
      case PreconditionKind::SkipIf:
        attr_name = "_skipIf";
        break;
      case PreconditionKind::RunWhile:
        attr_name = "_while";
        break;
      case PreconditionKind::Guard:
        break;
    }

    if (!attr_name.empty()) {
      node.attributes.push_back(btcpp::Attribute{
        std::move(attr_name), serialize_expression(pc->condition, ctx, ExprMode::Precondition)});
    }
  }

  if (guard_conditions.empty()) {
    return node;
  }

  // xml-mapping.md §5.1: @guard(expr) ->
  // <Sequence>
  //   <Node _while="expr" ... />
  //   <AlwaysSuccess _failureIf="!(expr)" />
  // </Sequence>

  // Combine multiple guards with &&.
  const Expr * combined = guard_conditions.front();
  std::string expr_str;
  if (guard_conditions.size() == 1U) {
    expr_str = serialize_expression(combined, ctx, ExprMode::Precondition);
  } else {
    // Build textual "(a && b && c)" form.
    std::string out = "(";
    for (std::size_t i = 0; i < guard_conditions.size(); ++i) {
      if (i > 0) {
        out += " && ";
      }
      out += serialize_expression(guard_conditions[i], ctx, ExprMode::Precondition);
    }
    out += ")";
    expr_str = std::move(out);
  }

  node.attributes.push_back(btcpp::Attribute{"_while", expr_str});

  btcpp::Node always;
  always.tag = "AlwaysSuccess";
  always.attributes.push_back(btcpp::Attribute{"_failureIf", "!(" + expr_str + ")"});

  btcpp::Node seq;
  seq.tag = "Sequence";
  seq.children.push_back(std::move(node));
  seq.children.push_back(std::move(always));
  return seq;
}

[[nodiscard]] std::string serialize_assignment_stmt(
  const AssignmentStmt & stmt, CodegenContext & ctx)
{
  const std::string lhs = [&]() {
    std::string out = ctx.var_ref(stmt.target, stmt.resolvedTarget, ExprMode::Script);
    for (const auto * idx : stmt.indices) {
      out += "[";
      out += serialize_expression(idx, ctx, ExprMode::Script);
      out += "]";
    }
    return out;
  }();

  const std::string rhs = serialize_expression(stmt.value, ctx, ExprMode::Script);

  auto expanded_assign = [&](std::string_view op) -> std::string {
    return lhs + " = (" + lhs + " " + std::string(op) + " " + rhs + ")";
  };

  switch (stmt.op) {
    case AssignOp::Assign:
      return lhs + " = " + rhs;
    case AssignOp::AddAssign:
      return expanded_assign("+");
    case AssignOp::SubAssign:
      return expanded_assign("-");
    case AssignOp::MulAssign:
      return expanded_assign("*");
    case AssignOp::DivAssign:
      return expanded_assign("/");
    case AssignOp::ModAssign:
      return expanded_assign("%");
  }

  return lhs + " = " + rhs;
}

[[nodiscard]] const ExternDecl * as_extern_decl(const NodeSymbol * sym)
{
  if (!sym || !sym->decl || sym->decl->get_kind() != NodeKind::ExternDecl) {
    return nullptr;
  }
  return static_cast<const ExternDecl *>(sym->decl);
}

[[nodiscard]] const TreeDecl * as_tree_decl(const NodeSymbol * sym)
{
  if (!sym || !sym->decl || sym->decl->get_kind() != NodeKind::TreeDecl) {
    return nullptr;
  }
  return static_cast<const TreeDecl *>(sym->decl);
}

[[nodiscard]] btcpp::Node convert_node_stmt(const NodeStmt & node, CodegenContext & ctx)
{
  // Pre-scripts required by xml-mapping.md §6.3 (defaults / out var / in-expr).
  std::vector<btcpp::Node> pre_scripts;
  pre_scripts.reserve(4);

  btcpp::Node element;

  const NodeSymbol * ns = node.resolvedNode;
  const ExternDecl * ext = as_extern_decl(ns);
  const TreeDecl * tree = as_tree_decl(ns);

  const bool is_subtree_call =
    (tree != nullptr) || (ext && ext->category == ExternNodeCategory::Subtree);

  if (is_subtree_call) {
    element.tag = "SubTree";
    if (tree) {
      element.attributes.push_back(btcpp::Attribute{"ID", ctx.subtree_xml_id(tree)});
    } else {
      element.attributes.push_back(btcpp::Attribute{"ID", std::string(node.nodeName)});
    }
  } else {
    element.tag = std::string(node.nodeName);
  }

  // ------------------------------------------------------------------------
  // Arguments -> attributes, inserting required pre-Scripts
  // ------------------------------------------------------------------------
  std::unordered_set<std::string_view> provided_ports;
  provided_ports.reserve(node.args.size());

  struct PreparedAttr
  {
    std::string key;
    std::string value;
  };

  std::vector<PreparedAttr> prepared;
  prepared.reserve(node.args.size());

  // Helper: find extern port definition by name
  auto find_extern_port = [&](std::string_view port_name) -> const ExternPort * {
    if (!ext) {
      return nullptr;
    }
    for (const auto * p : ext->ports) {
      if (p && p->name == port_name) {
        return p;
      }
    }
    return nullptr;
  };

  for (const auto * arg : node.args) {
    if (!arg) {
      continue;
    }
    if (arg->name.empty()) {
      continue;
    }

    const std::string_view port_name = arg->name;
    provided_ports.insert(port_name);

    const ExternPort * port_def = find_extern_port(port_name);
    const PortDirection port_dir =
      port_def ? port_direction_or_default(port_def->direction) : PortDirection::In;

    std::string attr_value;

    if (arg->is_inline_decl()) {
      // xml-mapping.md §6.3.2: out var x -> pre-Script declaration.
      const InlineBlackboardDecl * decl = arg->inlineDecl;
      const std::string_view var_name = decl ? decl->name : std::string_view{};
      const std::string key = ctx.declare_var(var_name);
      const std::string init = default_init_for_type(port_def ? port_def->type : nullptr);
      pre_scripts.push_back(make_assignment_script_node(key, init));
      attr_value = "{" + key + "}";
    } else {
      const Expr * expr = arg->valueExpr;

      // xml-mapping.md §6.3.3: in port with expression -> pre-Script.
      const bool is_in_port = (port_dir == PortDirection::In);
      if (is_in_port && !is_simple_value_expr_for_in_port(expr)) {
        const std::string_view tmp_base = "_expr";
        const std::string key = ctx.declare_var(tmp_base);
        const std::string rhs = serialize_expression(expr, ctx, ExprMode::Script);
        pre_scripts.push_back(make_assignment_script_node(key, rhs));
        attr_value = "{" + key + "}";
      } else {
        attr_value = serialize_expression(expr, ctx, ExprMode::AttributeValue);
      }
    }

    prepared.push_back(PreparedAttr{std::string(port_name), std::move(attr_value)});
  }

  // Synthesize omitted defaults (extern nodes only).
  if (ext) {
    for (const auto * p : ext->ports) {
      if (!p) {
        continue;
      }
      const PortDirection dir = port_direction_or_default(p->direction);
      if (dir != PortDirection::In) {
        continue;
      }
      if (!p->defaultValue) {
        continue;
      }
      if (provided_ports.count(p->name) > 0U) {
        continue;
      }

      const std::string_view tmp_base = "_default";
      const std::string key = ctx.declare_var(tmp_base);
      const std::string rhs = serialize_expression(p->defaultValue, ctx, ExprMode::Script);
      pre_scripts.push_back(make_assignment_script_node(key, rhs));

      prepared.push_back(PreparedAttr{std::string(p->name), "{" + key + "}"});
    }
  }

  for (auto & pa : prepared) {
    element.attributes.push_back(btcpp::Attribute{std::move(pa.key), std::move(pa.value)});
  }

  // Children
  std::vector<btcpp::Node> converted_children;
  converted_children.reserve(node.children.size());

  ctx.push_block();

  for (const auto * child : node.children) {
    if (!child) {
      continue;
    }

    switch (child->get_kind()) {
      case NodeKind::NodeStmt: {
        const auto * st = static_cast<const NodeStmt *>(child);
        converted_children.push_back(convert_node_stmt(*st, ctx));
        break;
      }
      case NodeKind::AssignmentStmt: {
        const auto * st = static_cast<const AssignmentStmt *>(child);
        if (st->op == AssignOp::Assign && st->indices.empty() && is_null_literal_expr(st->value)) {
          // xml-mapping.md §7.2: null assignment -> UnsetBlackboard.
          btcpp::Node unset;
          unset.tag = "UnsetBlackboard";
          unset.attributes.push_back(
            btcpp::Attribute{"key", ctx.var_ref(st->target, st->resolvedTarget, ExprMode::Script)});
          converted_children.push_back(
            apply_preconditions_and_guard(std::move(unset), st->preconditions, ctx));
        } else {
          btcpp::Node script = make_plain_script_node(serialize_assignment_stmt(*st, ctx));
          converted_children.push_back(
            apply_preconditions_and_guard(std::move(script), st->preconditions, ctx));
        }
        break;
      }
      case NodeKind::BlackboardDeclStmt: {
        const auto * st = static_cast<const BlackboardDeclStmt *>(child);
        const std::string key = ctx.declare_var(st->name);
        if (st->initialValue) {
          const std::string rhs = serialize_expression(st->initialValue, ctx, ExprMode::Script);
          converted_children.push_back(make_assignment_script_node(key, rhs));
        }
        break;
      }
      case NodeKind::ConstDeclStmt:
      default:
        break;
    }
  }

  ctx.pop_block();

  // Decorator nodes may be written with multiple children, which must be implicitly wrapped.
  if (ext && ext->category == ExternNodeCategory::Decorator && converted_children.size() > 1U) {
    btcpp::Node seq;
    seq.tag = "Sequence";
    seq.children = std::move(converted_children);
    element.children.push_back(std::move(seq));
  } else {
    element.children = std::move(converted_children);
  }

  btcpp::Node with_preconds =
    apply_preconditions_and_guard(std::move(element), node.preconditions, ctx);

  if (!pre_scripts.empty()) {
    btcpp::Node seq;
    seq.tag = "Sequence";
    for (auto & s : pre_scripts) {
      seq.children.push_back(std::move(s));
    }
    seq.children.push_back(std::move(with_preconds));
    return seq;
  }

  return with_preconds;
}

[[nodiscard]] btcpp::NodeModelKind to_model_kind(ExternNodeCategory cat)
{
  switch (cat) {
    case ExternNodeCategory::Action:
      return btcpp::NodeModelKind::Action;
    case ExternNodeCategory::Condition:
      return btcpp::NodeModelKind::Condition;
    case ExternNodeCategory::Control:
      return btcpp::NodeModelKind::Control;
    case ExternNodeCategory::Decorator:
      return btcpp::NodeModelKind::Decorator;
    case ExternNodeCategory::Subtree:
      // Not represented as a node model; callers are emitted as <SubTree>.
      return btcpp::NodeModelKind::Action;
  }
  return btcpp::NodeModelKind::Action;
}

[[nodiscard]] bool is_manifest_model_eligible(const ExternDecl & ext)
{
  return ext.category != ExternNodeCategory::Subtree;
}

struct TreeKey
{
  const ModuleInfo * module = nullptr;
  const TreeDecl * tree = nullptr;

  bool operator==(const TreeKey & other) const noexcept
  {
    return module == other.module && tree == other.tree;
  }
};

struct TreeKeyHash
{
  std::size_t operator()(const TreeKey & k) const noexcept
  {
    const auto h1 = std::hash<const ModuleInfo *>{}(k.module);
    const auto h2 = std::hash<const TreeDecl *>{}(k.tree);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

[[nodiscard]] const TreeDecl * find_tree_decl_by_name(const ModuleInfo & m, std::string_view name)
{
  if (!m.program) {
    return nullptr;
  }
  for (const auto * t : m.program->trees) {
    if (t && t->name == name) {
      return t;
    }
  }
  return nullptr;
}

[[nodiscard]] const ModuleInfo * owner_module_for_tree_decl(
  const std::unordered_map<const TreeDecl *, const ModuleInfo *> & map, const TreeDecl * t)
{
  auto it = map.find(t);
  return (it != map.end()) ? it->second : nullptr;
}

void collect_tree_owner_map(
  const ModuleInfo & entry, std::unordered_map<const TreeDecl *, const ModuleInfo *> & out)
{
  std::vector<const ModuleInfo *> stack;
  stack.reserve(16);

  std::unordered_set<const ModuleInfo *> visited;
  visited.reserve(16);

  stack.push_back(&entry);

  while (!stack.empty()) {
    const ModuleInfo * m = stack.back();
    stack.pop_back();
    if (!m) {
      continue;
    }
    if (!visited.insert(m).second) {
      continue;
    }

    if (m->program) {
      for (const auto * t : m->program->trees) {
        if (t) {
          out.emplace(t, m);
        }
      }
    }

    for (const auto * imp : m->imports) {
      stack.push_back(imp);
    }
  }
}

void collect_used_externs_and_subtree_calls(
  const TreeDecl & tree, std::unordered_set<const ExternDecl *> & used_externs,
  std::unordered_set<const TreeDecl *> & called_trees)
{
  std::vector<const Stmt *> stack;
  stack.reserve(128);

  for (const auto * st : tree.body) {
    stack.push_back(st);
  }

  while (!stack.empty()) {
    const Stmt * st = stack.back();
    stack.pop_back();
    if (!st) {
      continue;
    }

    if (st->get_kind() == NodeKind::NodeStmt) {
      const auto * ns = static_cast<const NodeStmt *>(st);
      if (const auto * ext = as_extern_decl(ns->resolvedNode)) {
        used_externs.insert(ext);
      }
      if (const auto * td = as_tree_decl(ns->resolvedNode)) {
        called_trees.insert(td);
      }

      for (const auto * ch : ns->children) {
        stack.push_back(ch);
      }
    }
  }
}

void append_blackboard_exists_model(btcpp::Document & doc)
{
  btcpp::NodeModel nm;
  nm.kind = btcpp::NodeModelKind::Condition;
  nm.id = "BlackboardExists";

  btcpp::PortModel pm;
  pm.kind = btcpp::PortKind::Input;
  pm.name = "key";
  pm.type = std::string{"string"};
  nm.ports.push_back(std::move(pm));

  doc.node_models.push_back(std::move(nm));
}

[[nodiscard]] int model_kind_rank(btcpp::NodeModelKind k)
{
  // Keep a stable, human-friendly ordering.
  switch (k) {
    case btcpp::NodeModelKind::Action:
      return 0;
    case btcpp::NodeModelKind::Condition:
      return 1;
    case btcpp::NodeModelKind::Control:
      return 2;
    case btcpp::NodeModelKind::Decorator:
      return 3;
  }
  return 99;
}

[[nodiscard]] int port_kind_rank(btcpp::PortKind k)
{
  switch (k) {
    case btcpp::PortKind::Input:
      return 0;
    case btcpp::PortKind::Output:
      return 1;
    case btcpp::PortKind::InOut:
      return 2;
  }
  return 99;
}

void sort_models_for_deterministic_output(btcpp::Document & doc)
{
  for (auto & nm : doc.node_models) {
    std::sort(
      nm.ports.begin(), nm.ports.end(), [](const btcpp::PortModel & a, const btcpp::PortModel & b) {
        const int ak = port_kind_rank(a.kind);
        const int bk = port_kind_rank(b.kind);
        if (ak != bk) return ak < bk;
        return a.name < b.name;
      });
  }

  std::sort(
    doc.node_models.begin(), doc.node_models.end(),
    [](const btcpp::NodeModel & a, const btcpp::NodeModel & b) {
      const int ak = model_kind_rank(a.kind);
      const int bk = model_kind_rank(b.kind);
      if (ak != bk) return ak < bk;
      return a.id < b.id;
    });

  for (auto & st : doc.subtree_models) {
    std::sort(
      st.ports.begin(), st.ports.end(), [](const btcpp::PortModel & a, const btcpp::PortModel & b) {
        const int ak = port_kind_rank(a.kind);
        const int bk = port_kind_rank(b.kind);
        if (ak != bk) return ak < bk;
        return a.name < b.name;
      });
  }

  std::sort(
    doc.subtree_models.begin(), doc.subtree_models.end(),
    [](const btcpp::SubTreeModel & a, const btcpp::SubTreeModel & b) { return a.id < b.id; });
}

}  // namespace

btcpp::Document AstToBtCppModelConverter::convert(const ModuleInfo & module)
{
  btcpp::Document doc;
  if (!module.program) {
    return doc;
  }

  doc.main_tree_to_execute = choose_entry_tree_name(*module.program);

  // TreeNodesModel manifest
  {
    std::unordered_set<const ExternDecl *> used_externs;
    used_externs.reserve(64);

    for (const auto * t : module.program->trees) {
      if (!t) {
        continue;
      }
      std::unordered_set<const TreeDecl *> called;
      collect_used_externs_and_subtree_calls(*t, used_externs, called);
    }

    for (const auto * ext : used_externs) {
      if (!ext || !is_manifest_model_eligible(*ext)) {
        continue;
      }

      btcpp::NodeModel nm;
      nm.kind = to_model_kind(ext->category);
      nm.id = std::string(ext->name);

      for (const auto * p : ext->ports) {
        if (!p) {
          continue;
        }
        btcpp::PortModel pm;
        pm.kind = to_btcpp_port_kind(port_direction_or_default(p->direction));
        pm.name = std::string(p->name);
        pm.type = render_type_expr(p->type);
        nm.ports.push_back(std::move(pm));
      }

      doc.node_models.push_back(std::move(nm));
    }

    append_blackboard_exists_model(doc);

    // SubTree models (only for trees with params)
    for (const auto * t : module.program->trees) {
      if (!t || t->params.empty()) {
        continue;
      }

      btcpp::SubTreeModel st;
      st.id = std::string(t->name);

      for (const auto * p : t->params) {
        if (!p) {
          continue;
        }
        btcpp::PortModel pm;
        pm.kind = to_btcpp_port_kind(port_direction_or_default(p->direction));
        pm.name = std::string(p->name);
        pm.type = render_type_expr(p->type);
        st.ports.push_back(std::move(pm));
      }

      doc.subtree_models.push_back(std::move(st));
    }
  }

  // BehaviorTrees
  for (const auto * tree : module.program->trees) {
    if (!tree) {
      continue;
    }

    btcpp::BehaviorTreeModel tm;
    tm.id = std::string(tree->name);

    CodegenContext ctx(module);

    std::vector<btcpp::Node> roots;
    roots.reserve(tree->body.size());

    for (const auto * st : tree->body) {
      if (!st) {
        continue;
      }

      switch (st->get_kind()) {
        case NodeKind::NodeStmt:
          roots.push_back(convert_node_stmt(*static_cast<const NodeStmt *>(st), ctx));
          break;

        case NodeKind::AssignmentStmt: {
          const auto * as = static_cast<const AssignmentStmt *>(st);
          if (
            as->op == AssignOp::Assign && as->indices.empty() && is_null_literal_expr(as->value)) {
            btcpp::Node unset;
            unset.tag = "UnsetBlackboard";
            unset.attributes.push_back(btcpp::Attribute{
              "key", ctx.var_ref(as->target, as->resolvedTarget, ExprMode::Script)});
            roots.push_back(
              apply_preconditions_and_guard(std::move(unset), as->preconditions, ctx));
          } else {
            btcpp::Node script = make_plain_script_node(serialize_assignment_stmt(*as, ctx));
            roots.push_back(
              apply_preconditions_and_guard(std::move(script), as->preconditions, ctx));
          }
          break;
        }

        case NodeKind::BlackboardDeclStmt: {
          const auto * vd = static_cast<const BlackboardDeclStmt *>(st);
          const std::string key = ctx.declare_var(vd->name);
          if (vd->initialValue) {
            const std::string rhs = serialize_expression(vd->initialValue, ctx, ExprMode::Script);
            roots.push_back(make_assignment_script_node(key, rhs));
          }
          break;
        }

        case NodeKind::ConstDeclStmt:
        default:
          break;
      }
    }

    if (roots.size() == 1U) {
      tm.root = std::move(roots.front());
    } else if (!roots.empty()) {
      btcpp::Node seq;
      seq.tag = "Sequence";
      seq.children = std::move(roots);
      tm.root = std::move(seq);
    }

    doc.behavior_trees.push_back(std::move(tm));
  }

  sort_models_for_deterministic_output(doc);

  return doc;
}

btcpp::Document AstToBtCppModelConverter::convert_single_output(const ModuleInfo & entry)
{
  btcpp::Document doc;
  if (!entry.program) {
    return doc;
  }

  const std::string entry_tree_name = choose_entry_tree_name(*entry.program);
  doc.main_tree_to_execute = entry_tree_name;

  // Map TreeDecl* -> owner ModuleInfo*
  std::unordered_map<const TreeDecl *, const ModuleInfo *> tree_owner;
  tree_owner.reserve(128);
  collect_tree_owner_map(entry, tree_owner);

  const TreeDecl * entry_tree = find_tree_decl_by_name(entry, entry_tree_name);
  if (!entry_tree) {
    // Best-effort: no trees.
    return doc;
  }

  // Discover reachable trees by following resolved subtree calls.
  std::vector<TreeKey> ordered;
  ordered.reserve(32);

  std::unordered_set<TreeKey, TreeKeyHash> seen;
  seen.reserve(64);

  std::vector<TreeKey> queue;
  queue.reserve(32);

  queue.push_back(TreeKey{&entry, entry_tree});

  std::unordered_set<const ExternDecl *> used_externs;
  used_externs.reserve(128);

  while (!queue.empty()) {
    const TreeKey current = queue.back();
    queue.pop_back();

    if (!current.module || !current.tree) {
      continue;
    }

    if (!seen.insert(current).second) {
      continue;
    }

    ordered.push_back(current);

    std::unordered_set<const TreeDecl *> called;
    collect_used_externs_and_subtree_calls(*current.tree, used_externs, called);

    for (const auto * callee : called) {
      const ModuleInfo * owner = owner_module_for_tree_decl(tree_owner, callee);
      if (!owner) {
        continue;
      }

      // Enforce visibility rule for cross-module traversal.
      if (owner != &entry && !is_public_name(callee->name)) {
        continue;
      }

      queue.push_back(TreeKey{owner, callee});
    }
  }

  // Assign mangled XML IDs.
  std::unordered_map<const ModuleInfo *, std::uint32_t> module_ids;
  module_ids.reserve(ordered.size());

  std::unordered_set<std::string> used_tree_ids;
  used_tree_ids.reserve(ordered.size() * 2U);

  std::unordered_map<TreeKey, std::string, TreeKeyHash> tree_xml_ids;
  tree_xml_ids.reserve(ordered.size());

  std::uint32_t next_module_id = 1;

  auto ensure_unique = [&](std::string id) -> std::string {
    if (used_tree_ids.insert(id).second) {
      return id;
    }
    for (std::uint32_t n = 2;; ++n) {
      std::string candidate = id + "_" + std::to_string(n);
      if (used_tree_ids.insert(candidate).second) {
        return candidate;
      }
    }
  };

  for (const auto & k : ordered) {
    if (k.module == &entry) {
      tree_xml_ids.emplace(k, ensure_unique(std::string(k.tree->name)));
      continue;
    }

    auto it = module_ids.find(k.module);
    if (it == module_ids.end()) {
      it = module_ids.emplace(k.module, next_module_id++).first;
    }

    const std::string mangled =
      std::string{"_SubTree_"} + std::to_string(it->second) + "_" + std::string(k.tree->name);
    tree_xml_ids.emplace(k, ensure_unique(mangled));
  }

  // TreeNodesModel
  {
    for (const auto * ext : used_externs) {
      if (!ext || !is_manifest_model_eligible(*ext)) {
        continue;
      }

      btcpp::NodeModel nm;
      nm.kind = to_model_kind(ext->category);
      nm.id = std::string(ext->name);

      for (const auto * p : ext->ports) {
        if (!p) {
          continue;
        }
        btcpp::PortModel pm;
        pm.kind = to_btcpp_port_kind(port_direction_or_default(p->direction));
        pm.name = std::string(p->name);
        pm.type = render_type_expr(p->type);
        nm.ports.push_back(std::move(pm));
      }

      doc.node_models.push_back(std::move(nm));
    }

    append_blackboard_exists_model(doc);

    // SubTree models (only for included trees with params)
    for (const auto & k : ordered) {
      if (!k.tree || k.tree->params.empty()) {
        continue;
      }

      btcpp::SubTreeModel st;
      st.id = tree_xml_ids.at(k);

      for (const auto * p : k.tree->params) {
        if (!p) {
          continue;
        }
        btcpp::PortModel pm;
        pm.kind = to_btcpp_port_kind(port_direction_or_default(p->direction));
        pm.name = std::string(p->name);
        pm.type = render_type_expr(p->type);
        st.ports.push_back(std::move(pm));
      }

      doc.subtree_models.push_back(std::move(st));
    }
  }

  // BehaviorTrees
  for (const auto & k : ordered) {
    btcpp::BehaviorTreeModel tm;
    tm.id = tree_xml_ids.at(k);

    CodegenContext ctx(*k.module);
    ctx.subtree_id_resolver = [&](const TreeDecl * t) -> std::string {
      if (!t) {
        return {};
      }
      const ModuleInfo * owner = owner_module_for_tree_decl(tree_owner, t);
      if (!owner) {
        return std::string(t->name);
      }

      const TreeKey tk{owner, t};
      auto it = tree_xml_ids.find(tk);
      if (it != tree_xml_ids.end()) {
        return it->second;
      }
      return std::string(t->name);
    };

    std::vector<btcpp::Node> roots;
    roots.reserve(k.tree->body.size());

    for (const auto * st : k.tree->body) {
      if (!st) {
        continue;
      }

      switch (st->get_kind()) {
        case NodeKind::NodeStmt:
          roots.push_back(convert_node_stmt(*static_cast<const NodeStmt *>(st), ctx));
          break;

        case NodeKind::AssignmentStmt: {
          const auto * as = static_cast<const AssignmentStmt *>(st);
          if (
            as->op == AssignOp::Assign && as->indices.empty() && is_null_literal_expr(as->value)) {
            btcpp::Node unset;
            unset.tag = "UnsetBlackboard";
            unset.attributes.push_back(btcpp::Attribute{
              "key", ctx.var_ref(as->target, as->resolvedTarget, ExprMode::Script)});
            roots.push_back(
              apply_preconditions_and_guard(std::move(unset), as->preconditions, ctx));
          } else {
            btcpp::Node script = make_plain_script_node(serialize_assignment_stmt(*as, ctx));
            roots.push_back(
              apply_preconditions_and_guard(std::move(script), as->preconditions, ctx));
          }
          break;
        }

        case NodeKind::BlackboardDeclStmt: {
          const auto * vd = static_cast<const BlackboardDeclStmt *>(st);
          const std::string key = ctx.declare_var(vd->name);
          if (vd->initialValue) {
            const std::string rhs = serialize_expression(vd->initialValue, ctx, ExprMode::Script);
            roots.push_back(make_assignment_script_node(key, rhs));
          }
          break;
        }

        case NodeKind::ConstDeclStmt:
        default:
          break;
      }
    }

    if (roots.size() == 1U) {
      tm.root = std::move(roots.front());
    } else if (!roots.empty()) {
      btcpp::Node seq;
      seq.tag = "Sequence";
      seq.children = std::move(roots);
      tm.root = std::move(seq);
    }

    doc.behavior_trees.push_back(std::move(tm));
  }

  sort_models_for_deterministic_output(doc);

  return doc;
}

// ============================================================================
// BtCppXmlSerializer (tinyxml2)
// ============================================================================

namespace
{

tinyxml2::XMLElement * append_node_impl(
  tinyxml2::XMLDocument & doc, tinyxml2::XMLElement * parent, const btcpp::Node & node)
{
  auto * elem = doc.NewElement(node.tag.c_str());

  for (const auto & a : node.attributes) {
    elem->SetAttribute(a.key.c_str(), a.value.c_str());
  }

  if (node.text.has_value()) {
    elem->SetText(node.text->c_str());
  }

  for (const auto & ch : node.children) {
    append_node_impl(doc, elem, ch);
  }

  parent->InsertEndChild(elem);
  return elem;
}

}  // namespace

std::string BtCppXmlSerializer::serialize(const btcpp::Document & doc_model)
{
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild(doc.NewDeclaration(R"(xml version="1.0" encoding="UTF-8")"));

  auto * root = doc.NewElement("root");
  root->SetAttribute("BTCPP_format", "4");
  root->SetAttribute("main_tree_to_execute", doc_model.main_tree_to_execute.c_str());
  doc.InsertEndChild(root);

  for (const auto & tree : doc_model.behavior_trees) {
    auto * bt = doc.NewElement("BehaviorTree");
    bt->SetAttribute("ID", tree.id.c_str());
    root->InsertEndChild(bt);

    if (tree.root.has_value()) {
      append_node_impl(doc, bt, *tree.root);
    }
  }

  // TreeNodesModel (manifest) should appear under <root>.
  if (!doc_model.node_models.empty() || !doc_model.subtree_models.empty()) {
    auto * tnm = doc.NewElement("TreeNodesModel");
    root->InsertEndChild(tnm);

    for (const auto & nm : doc_model.node_models) {
      const char * tag = nullptr;
      switch (nm.kind) {
        case btcpp::NodeModelKind::Action:
          tag = "Action";
          break;
        case btcpp::NodeModelKind::Condition:
          tag = "Condition";
          break;
        case btcpp::NodeModelKind::Control:
          tag = "Control";
          break;
        case btcpp::NodeModelKind::Decorator:
          tag = "Decorator";
          break;
      }

      auto * ne = doc.NewElement(tag);
      ne->SetAttribute("ID", nm.id.c_str());
      tnm->InsertEndChild(ne);

      for (const auto & p : nm.ports) {
        const char * port_tag = nullptr;
        switch (p.kind) {
          case btcpp::PortKind::Input:
            port_tag = "input_port";
            break;
          case btcpp::PortKind::Output:
            port_tag = "output_port";
            break;
          case btcpp::PortKind::InOut:
            port_tag = "inout_port";
            break;
        }

        auto * pe = doc.NewElement(port_tag);
        pe->SetAttribute("name", p.name.c_str());
        if (p.type.has_value()) {
          pe->SetAttribute("type", p.type->c_str());
        }
        ne->InsertEndChild(pe);
      }
    }

    for (const auto & st : doc_model.subtree_models) {
      auto * sub = doc.NewElement("SubTree");
      sub->SetAttribute("ID", st.id.c_str());
      tnm->InsertEndChild(sub);

      for (const auto & p : st.ports) {
        const char * port_tag = nullptr;
        switch (p.kind) {
          case btcpp::PortKind::Input:
            port_tag = "input_port";
            break;
          case btcpp::PortKind::Output:
            port_tag = "output_port";
            break;
          case btcpp::PortKind::InOut:
            port_tag = "inout_port";
            break;
        }

        auto * pe = doc.NewElement(port_tag);
        pe->SetAttribute("name", p.name.c_str());
        if (p.type.has_value()) {
          pe->SetAttribute("type", p.type->c_str());
        }
        sub->InsertEndChild(pe);
      }
    }
  }

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return {printer.CStr()};
}

// ============================================================================
// XmlGenerator facade
// ============================================================================

std::string XmlGenerator::generate(const ModuleInfo & module)
{
  const auto model = AstToBtCppModelConverter::convert(module);
  return BtCppXmlSerializer::serialize(model);
}

std::string XmlGenerator::generate_single_output(const ModuleInfo & entry)
{
  const auto model = AstToBtCppModelConverter::convert_single_output(entry);
  return BtCppXmlSerializer::serialize(model);
}

}  // namespace bt_dsl
