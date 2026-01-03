// bt_dsl/syntax/AstBuilder.cpp - Mid-level dispatcher implementation (non-type/non-expr parts)
#include <cassert>
#include <optional>
#include <string>
#include <vector>

#include "bt_dsl/syntax/ast_builder.hpp"

namespace bt_dsl
{

static std::string_view strip_trailing_cr(std::string_view s)
{
  if (!s.empty() && s.back() == '\r') {
    return s.substr(0, s.size() - 1);
  }
  return s;
}

static bool is_hex_digit(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c)
{
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(10 + (c - 'a'));
  return static_cast<uint32_t>(10 + (c - 'A'));
}

static void append_utf8(uint32_t cp, std::string & out)
{
  // Encode a Unicode scalar value into UTF-8.
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

static bool unescape_string(
  std::string_view token_with_quotes, std::string & out, std::string & err)
{
  token_with_quotes = strip_trailing_cr(token_with_quotes);
  if (
    token_with_quotes.size() < 2 || token_with_quotes.front() != '"' ||
    token_with_quotes.back() != '"') {
    err = "Invalid string token";
    return false;
  }

  const std::string_view s = token_with_quotes.substr(1, token_with_quotes.size() - 2);
  out.clear();

  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }

    if (i + 1 >= s.size()) {
      err = "Unterminated escape sequence";
      return false;
    }

    const char e = s[++i];
    switch (e) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '0':
        out.push_back('\0');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'u': {
        // \u{XXXX} (1..6 hex)
        if (i + 1 >= s.size() || s[i + 1] != '{') {
          err = "Expected '{' after \\u";
          return false;
        }
        i += 2;  // skip '{'
        if (i >= s.size()) {
          err = "Unterminated \\u{...} escape";
          return false;
        }

        uint32_t cp = 0;
        int digits = 0;
        for (; i < s.size(); ++i) {
          const char h = s[i];
          if (h == '}') break;
          if (!is_hex_digit(h)) {
            err = "Non-hex digit in \\u{...} escape";
            return false;
          }
          if (digits >= 6) {
            err = "Too many hex digits in \\u{...} escape (max 6)";
            return false;
          }
          cp = (cp << 4) | hex_value(h);
          ++digits;
        }

        if (i >= s.size() || s[i] != '}') {
          err = "Unterminated \\u{...} escape";
          return false;
        }

        if (digits < 1) {
          err = "Empty \\u{...} escape";
          return false;
        }
        if (cp > 0x10FFFF) {
          err = "Unicode codepoint out of range in \\u{...} escape";
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
          err = "Surrogate codepoint is not a valid Unicode scalar value";
          return false;
        }

        append_utf8(cp, out);
        break;
      }
      default:
        err = "Unknown escape sequence";
        return false;
    }
  }

  return true;
}

std::string_view AstBuilder::node_text(ts_ll::Node n) const
{
  return strip_trailing_cr(n.text(sm_));
}

std::string_view AstBuilder::intern_text(ts_ll::Node n) { return ast_.intern(node_text(n)); }

SourceRange AstBuilder::node_range(ts_ll::Node n) noexcept { return n.range(); }

Expr * AstBuilder::missing_expr(ts_ll::Node at, std::string_view message)
{
  diags_.error(node_range(at), message);
  return ast_.create<MissingExpr>(node_range(at));
}

std::optional<PortDirection> AstBuilder::parse_port_direction(ts_ll::Node port_dir_node)
{
  if (port_dir_node.is_null() || port_dir_node.kind() != "port_direction") {
    return std::nullopt;
  }

  const std::string_view t = node_text(port_dir_node);
  if (t == "in") return PortDirection::In;
  if (t == "out") return PortDirection::Out;
  if (t == "ref") return PortDirection::Ref;
  if (t == "mut") return PortDirection::Mut;

  diags_.error(node_range(port_dir_node), "Unknown port direction: '" + std::string(t) + "'");
  return std::nullopt;
}

static std::optional<ExternNodeCategory> parse_extern_category_from_token(std::string_view kw)
{
  if (kw == "action") return ExternNodeCategory::Action;
  if (kw == "condition") return ExternNodeCategory::Condition;
  if (kw == "control") return ExternNodeCategory::Control;
  if (kw == "decorator") return ExternNodeCategory::Decorator;
  if (kw == "subtree") return ExternNodeCategory::Subtree;
  return std::nullopt;
}

ExternDecl * AstBuilder::build_extern_decl(ts_ll::Node extern_stmt_node)
{
  if (extern_stmt_node.is_null() || extern_stmt_node.kind() != "extern_stmt") {
    diags_.error(node_range(extern_stmt_node), "Expected 'extern_stmt'");
    return nullptr;
  }

  // extern_stmt = { outer_doc } , [ behavior_attr ] , "extern" , extern_def ;
  const ts_ll::Node def_node = extern_stmt_node.child_by_field("def");
  if (def_node.is_null()) {
    diags_.error(node_range(extern_stmt_node), "extern_stmt missing extern_def");
    return nullptr;
  }

  // Determine category by scanning unnamed keyword tokens in extern_def.
  std::optional<ExternNodeCategory> cat;
  for (uint32_t i = 0; i < def_node.child_count(); ++i) {
    const ts_ll::Node ch = def_node.child(i);
    if (ch.is_null()) continue;
    if (ts_node_is_named(ch.raw())) continue;
    const auto kw = node_text(ch);
    cat = parse_extern_category_from_token(kw);
    if (cat) break;
  }
  if (!cat) {
    diags_.error(node_range(def_node), "extern_def missing/unknown category keyword");
    // Default to action to keep building.
    cat = ExternNodeCategory::Action;
  }

  const ts_ll::Node name_node = def_node.child_by_field("name");
  if (name_node.is_null()) {
    diags_.error(node_range(def_node), "extern_def missing name");
    return nullptr;
  }

  auto * decl = ast_.create<ExternDecl>(*cat, intern_text(name_node), node_range(extern_stmt_node));
  decl->docs = ast_.copy_to_arena(collect_outer_docs(extern_stmt_node));

  // behavior_attr
  for (uint32_t i = 0; i < extern_stmt_node.named_child_count(); ++i) {
    const ts_ll::Node c = extern_stmt_node.named_child(i);
    if (c.kind() != "behavior_attr") continue;
    decl->behaviorAttr = build_behavior_attr(c);
    break;
  }

  // Ports
  std::vector<ExternPort *> ports;
  for (uint32_t i = 0; i < def_node.named_child_count(); ++i) {
    const ts_ll::Node c = def_node.named_child(i);
    if (c.kind() != "extern_port_list") continue;
    for (uint32_t j = 0; j < c.named_child_count(); ++j) {
      const ts_ll::Node p = c.named_child(j);
      if (p.kind() == "extern_port") {
        if (auto * ep = build_extern_port(p)) ports.push_back(ep);
      }
    }
  }
  decl->ports = ast_.copy_to_arena(ports);

  // Signature-only step: ignore docs/behavior_attr for now.
  return decl;
}

ExternPort * AstBuilder::build_extern_port(ts_ll::Node extern_port_node)
{
  if (extern_port_node.is_null() || extern_port_node.kind() != "extern_port") {
    diags_.error(node_range(extern_port_node), "Expected 'extern_port'");
    return nullptr;
  }

  std::optional<PortDirection> dir;
  for (uint32_t i = 0; i < extern_port_node.named_child_count(); ++i) {
    const ts_ll::Node c = extern_port_node.named_child(i);
    if (c.kind() == "port_direction") {
      dir = parse_port_direction(c);
      break;
    }
  }

  const ts_ll::Node name_node = extern_port_node.child_by_field("name");
  const ts_ll::Node type_node = extern_port_node.child_by_field("type");
  if (name_node.is_null() || type_node.is_null()) {
    diags_.error(node_range(extern_port_node), "extern_port missing name/type");
    return nullptr;
  }

  TypeExpr * ty = build_type(type_node);

  Expr * def = nullptr;
  const ts_ll::Node default_node = extern_port_node.child_by_field("default");
  if (!default_node.is_null()) {
    const PortDirection eff_dir = dir.value_or(PortDirection::In);
    // Reference: static-analysis-and-safety.md §6.4.6
    // ref/mut/out ports cannot have default values (MUST FAIL at parse).
    if (
      eff_dir == PortDirection::Ref || eff_dir == PortDirection::Mut ||
      eff_dir == PortDirection::Out) {
      diags_.error(node_range(default_node), "Default value is not allowed for ref/mut/out ports");
    }
    // Current phase: const_expr literal-only; BuildExpr already enforces that and
    // will emit MissingExpr+diag for unsupported expressions.
    def = build_expr(default_node);
  }

  auto * port =
    ast_.create<ExternPort>(intern_text(name_node), dir, ty, def, node_range(extern_port_node));
  port->docs = ast_.copy_to_arena(collect_outer_docs(extern_port_node));
  return port;
}

BehaviorAttr * AstBuilder::build_behavior_attr(ts_ll::Node behavior_attr_node)
{
  if (behavior_attr_node.is_null()) return nullptr;
  if (behavior_attr_node.kind() != "behavior_attr") {
    diags_.error(node_range(behavior_attr_node), "Expected 'behavior_attr'");
    return nullptr;
  }

  // behavior_attr = #[behavior(data_policy[, flow_policy])]
  const ts_ll::Node data_node = behavior_attr_node.child_by_field("data");
  const ts_ll::Node flow_node = behavior_attr_node.child_by_field("flow");

  DataPolicy data_policy = DataPolicy::All;
  if (!data_node.is_null()) {
    const std::string_view t = node_text(data_node);
    if (t == "All")
      data_policy = DataPolicy::All;
    else if (t == "Any")
      data_policy = DataPolicy::Any;
    else if (t == "None")
      data_policy = DataPolicy::None;
    else
      diags_.error(node_range(data_node), "Unknown data_policy: '" + std::string(t) + "'");
  } else {
    diags_.error(node_range(behavior_attr_node), "behavior_attr missing data_policy");
  }

  std::optional<FlowPolicy> flow_policy;
  if (!flow_node.is_null()) {
    const std::string_view t = node_text(flow_node);
    if (t == "Chained")
      flow_policy = FlowPolicy::Chained;
    else if (t == "Isolated")
      flow_policy = FlowPolicy::Isolated;
    else
      diags_.error(node_range(flow_node), "Unknown flow_policy: '" + std::string(t) + "'");
  }

  return ast_.create<BehaviorAttr>(data_policy, flow_policy, node_range(behavior_attr_node));
}

ImportDecl * AstBuilder::build_import_decl(ts_ll::Node import_stmt_node)
{
  if (import_stmt_node.is_null() || import_stmt_node.kind() != "import_stmt") {
    diags_.error(node_range(import_stmt_node), "Expected 'import_stmt'");
    return nullptr;
  }

  const ts_ll::Node path_node = import_stmt_node.child_by_field("path");
  if (path_node.is_null() || path_node.kind() != "string") {
    diags_.error(node_range(import_stmt_node), "import_stmt missing string path");
    return nullptr;
  }

  std::string unescaped;
  std::string err;
  const auto txt = node_text(path_node);
  if (!unescape_string(txt, unescaped, err)) {
    diags_.error(node_range(path_node), "Invalid import path string: " + err);
    return ast_.create<ImportDecl>(ast_.intern("<invalid>"), node_range(import_stmt_node));
  }

  return ast_.create<ImportDecl>(ast_.intern(unescaped), node_range(import_stmt_node));
}

ExternTypeDecl * AstBuilder::build_extern_type_decl(ts_ll::Node extern_type_stmt_node)
{
  if (extern_type_stmt_node.is_null() || extern_type_stmt_node.kind() != "extern_type_stmt") {
    diags_.error(node_range(extern_type_stmt_node), "Expected 'extern_type_stmt'");
    return nullptr;
  }

  const ts_ll::Node name_node = extern_type_stmt_node.child_by_field("name");
  if (name_node.is_null()) {
    diags_.error(node_range(extern_type_stmt_node), "extern_type_stmt missing name");
    return nullptr;
  }

  auto * decl =
    ast_.create<ExternTypeDecl>(intern_text(name_node), node_range(extern_type_stmt_node));
  decl->docs = ast_.copy_to_arena(collect_outer_docs(extern_type_stmt_node));
  return decl;
}

TypeAliasDecl * AstBuilder::build_type_alias_decl(ts_ll::Node type_alias_stmt_node)
{
  if (type_alias_stmt_node.is_null() || type_alias_stmt_node.kind() != "type_alias_stmt") {
    diags_.error(node_range(type_alias_stmt_node), "Expected 'type_alias_stmt'");
    return nullptr;
  }

  const ts_ll::Node name_node = type_alias_stmt_node.child_by_field("name");
  const ts_ll::Node value_node = type_alias_stmt_node.child_by_field("value");
  if (name_node.is_null() || value_node.is_null()) {
    diags_.error(node_range(type_alias_stmt_node), "type_alias_stmt missing name/value");
    return nullptr;
  }

  auto * decl = ast_.create<TypeAliasDecl>(
    intern_text(name_node), build_type(value_node), node_range(type_alias_stmt_node));
  decl->docs = ast_.copy_to_arena(collect_outer_docs(type_alias_stmt_node));
  return decl;
}

GlobalVarDecl * AstBuilder::build_global_var_decl(ts_ll::Node global_var_node)
{
  if (global_var_node.is_null() || global_var_node.kind() != "global_blackboard_decl") {
    diags_.error(node_range(global_var_node), "Expected 'global_blackboard_decl'");
    return nullptr;
  }

  const ts_ll::Node name_node = global_var_node.child_by_field("name");
  if (name_node.is_null()) {
    diags_.error(node_range(global_var_node), "global_blackboard_decl missing name");
    return nullptr;
  }

  const ts_ll::Node type_node = global_var_node.child_by_field("type");
  const ts_ll::Node init_node = global_var_node.child_by_field("init");

  TypeExpr * type = nullptr;
  if (!type_node.is_null()) type = build_type(type_node);

  Expr * init = nullptr;
  if (!init_node.is_null()) init = build_expr(init_node);

  auto * decl =
    ast_.create<GlobalVarDecl>(intern_text(name_node), type, init, node_range(global_var_node));
  decl->docs = ast_.copy_to_arena(collect_outer_docs(global_var_node));
  return decl;
}

GlobalConstDecl * AstBuilder::build_global_const_decl(ts_ll::Node global_const_node)
{
  if (global_const_node.is_null() || global_const_node.kind() != "global_const_decl") {
    diags_.error(node_range(global_const_node), "Expected 'global_const_decl'");
    return nullptr;
  }

  const ts_ll::Node name_node = global_const_node.child_by_field("name");
  const ts_ll::Node value_node = global_const_node.child_by_field("value");
  if (name_node.is_null() || value_node.is_null()) {
    diags_.error(node_range(global_const_node), "global_const_decl missing name/value");
    return nullptr;
  }

  const ts_ll::Node type_node = global_const_node.child_by_field("type");
  TypeExpr * type = nullptr;
  if (!type_node.is_null()) type = build_type(type_node);

  Expr * value = build_expr(value_node);

  auto * decl = ast_.create<GlobalConstDecl>(
    intern_text(name_node), type, value, node_range(global_const_node));
  decl->docs = ast_.copy_to_arena(collect_outer_docs(global_const_node));
  return decl;
}

Program * AstBuilder::build_program(ts_ll::Node program_node)
{
  // grammar: program = repeat(choice(...))
  if (program_node.is_null() || program_node.kind() != "program") {
    diags_.error(node_range(program_node), "Expected root node 'program'");
    return ast_.create<Program>();
  }

  std::vector<std::string_view> inner_docs;
  std::vector<ImportDecl *> imports;
  std::vector<ExternTypeDecl *> extern_types;
  std::vector<TypeAliasDecl *> type_aliases;
  std::vector<ExternDecl *> externs;
  std::vector<GlobalVarDecl *> global_vars;
  std::vector<GlobalConstDecl *> global_consts;
  std::vector<TreeDecl *> trees;

  for (uint32_t i = 0; i < program_node.named_child_count(); ++i) {
    const ts_ll::Node item = program_node.named_child(i);

    if (
      item.kind() == "comment" || item.kind() == "line_comment" || item.kind() == "block_comment") {
      continue;
    }

    if (item.kind() == "inner_doc") {
      std::string_view raw = node_text(item);
      if (raw.rfind("//!", 0) == 0) raw.remove_prefix(3);
      inner_docs.push_back(ast_.intern(std::string(raw)));
      continue;
    }

    if (item.kind() == "import_stmt") {
      if (auto * d = build_import_decl(item)) imports.push_back(d);
      continue;
    }
    if (item.kind() == "extern_type_stmt") {
      if (auto * d = build_extern_type_decl(item)) extern_types.push_back(d);
      continue;
    }
    if (item.kind() == "type_alias_stmt") {
      if (auto * d = build_type_alias_decl(item)) type_aliases.push_back(d);
      continue;
    }
    if (item.kind() == "extern_stmt") {
      if (auto * ed = build_extern_decl(item)) externs.push_back(ed);
      continue;
    }
    if (item.kind() == "global_blackboard_decl") {
      if (auto * d = build_global_var_decl(item)) global_vars.push_back(d);
      continue;
    }
    if (item.kind() == "global_const_decl") {
      if (auto * d = build_global_const_decl(item)) global_consts.push_back(d);
      continue;
    }
    if (item.kind() == "tree_def") {
      if (auto * td = build_tree_decl(item)) trees.push_back(td);
      continue;
    }

    if (item.kind() == "ERROR" || item.is_missing()) {
      diags_.error(node_range(item), "Syntax error in program item");
      continue;
    }
    diags_.error(
      node_range(item), "Top-level item not implemented in core CST->AST builder yet: '" +
                          std::string(item.kind()) + "'");
  }

  auto * p = ast_.create<Program>(node_range(program_node));
  p->innerDocs = ast_.copy_to_arena(inner_docs);
  p->imports = ast_.copy_to_arena(imports);
  p->externTypes = ast_.copy_to_arena(extern_types);
  p->typeAliases = ast_.copy_to_arena(type_aliases);
  p->externs = ast_.copy_to_arena(externs);
  p->globalVars = ast_.copy_to_arena(global_vars);
  p->globalConsts = ast_.copy_to_arena(global_consts);
  p->trees = ast_.copy_to_arena(trees);
  return p;
}

TreeDecl * AstBuilder::build_tree_decl(ts_ll::Node tree_def_node)
{
  if (tree_def_node.is_null() || tree_def_node.kind() != "tree_def") {
    diags_.error(node_range(tree_def_node), "Expected 'tree_def'");
    return nullptr;
  }

  const ts_ll::Node name_node = tree_def_node.child_by_field("name");
  if (name_node.is_null()) {
    diags_.error(node_range(tree_def_node), "tree_def missing name");
    return nullptr;
  }

  auto * tree = ast_.create<TreeDecl>(intern_text(name_node), node_range(tree_def_node));
  tree->docs = ast_.copy_to_arena(collect_outer_docs(tree_def_node));

  // Params
  std::vector<ParamDecl *> params;
  for (uint32_t i = 0; i < tree_def_node.named_child_count(); ++i) {
    const ts_ll::Node c = tree_def_node.named_child(i);
    if (c.kind() == "param_list") {
      for (uint32_t j = 0; j < c.named_child_count(); ++j) {
        const ts_ll::Node pd = c.named_child(j);
        if (pd.kind() == "param_decl") {
          if (auto * p = build_param_decl(pd)) params.push_back(p);
        }
      }
    }
  }
  tree->params = ast_.copy_to_arena(params);

  // Body
  std::vector<Stmt *> stmts;
  const ts_ll::Node body_node = tree_def_node.child_by_field("body");
  if (!body_node.is_null() && body_node.kind() == "tree_body") {
    for (uint32_t i = 0; i < body_node.named_child_count(); ++i) {
      const ts_ll::Node st = body_node.named_child(i);
      if (st.kind() == "statement") {
        if (auto * s = build_statement(st)) stmts.push_back(s);
      }
    }
  }
  tree->body = ast_.copy_to_arena(stmts);

  return tree;
}

ParamDecl * AstBuilder::build_param_decl(ts_ll::Node param_decl_node)
{
  if (param_decl_node.is_null() || param_decl_node.kind() != "param_decl") {
    diags_.error(node_range(param_decl_node), "Expected 'param_decl'");
    return nullptr;
  }

  std::optional<PortDirection> dir;
  for (uint32_t i = 0; i < param_decl_node.named_child_count(); ++i) {
    const ts_ll::Node c = param_decl_node.named_child(i);
    if (c.kind() == "port_direction") {
      dir = parse_port_direction(c);
      break;
    }
  }

  const ts_ll::Node name_node = param_decl_node.child_by_field("name");
  const ts_ll::Node type_node = param_decl_node.child_by_field("type");
  if (name_node.is_null() || type_node.is_null()) {
    diags_.error(node_range(param_decl_node), "param_decl missing name/type");
    return nullptr;
  }

  TypeExpr * type = build_type(type_node);

  Expr * def = nullptr;
  const ts_ll::Node default_node = param_decl_node.child_by_field("default");
  if (!default_node.is_null()) {
    const PortDirection eff_dir = dir.value_or(PortDirection::In);
    // Reference: static-analysis-and-safety.md §6.4.6
    // ref/mut/out params cannot have default values.
    if (
      eff_dir == PortDirection::Ref || eff_dir == PortDirection::Mut ||
      eff_dir == PortDirection::Out) {
      diags_.error(
        node_range(default_node), "Default value is not allowed for ref/mut/out parameters");
    }
    def = build_expr(default_node);
  }

  return ast_.create<ParamDecl>(
    intern_text(name_node), dir, type, def, node_range(param_decl_node));
}

// Statement / argument building lives in BuildStmt.cpp / BuildSupport.cpp.

}  // namespace bt_dsl
