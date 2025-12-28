// bt_dsl/xml_generator.cpp - BehaviorTree.CPP XML generation
#include "bt_dsl/codegen/xml_generator.hpp"

#include <sstream>
#include <string_view>
#include <type_traits>

#include "tinyxml2.h"

namespace bt_dsl
{

// ============================================================================
// AstToBtCppModelConverter
// ============================================================================

std::string AstToBtCppModelConverter::join_docs(const std::vector<std::string> & docs)
{
  std::string out;
  for (const auto & d : docs) {
    std::string_view sv(d);
    // Parser already removed leading "///", but keep trimming.
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
      sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
      sv.remove_suffix(1);
    }
    if (sv.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(sv);
  }
  return out;
}

static std::string to_string_lossless(double v)
{
  // Keep tinyxml2 output stable and avoid scientific notation for common
  // values.
  std::ostringstream oss;
  oss.precision(17);
  oss << v;
  return oss.str();
}

std::string AstToBtCppModelConverter::format_literal_for_script(const Literal & lit)
{
  return std::visit(
    [](const auto & l) -> std::string {
      using T = std::decay_t<decltype(l)>;
      if constexpr (std::is_same_v<T, StringLiteral>) {
        // BT.CPP Script uses single quotes.
        const std::string s = l.value;
        // Escape single quotes in script strings.
        std::string escaped;
        escaped.reserve(s.size());
        for (const char c : s) {
          if (c == '\'') {
            escaped += "\\'";
          } else {
            escaped += c;
          }
        }
        return "'" + escaped + "'";
      } else if constexpr (std::is_same_v<T, IntLiteral>) {
        return std::to_string(l.value);
      } else if constexpr (std::is_same_v<T, FloatLiteral>) {
        return to_string_lossless(l.value);
      } else if constexpr (std::is_same_v<T, BoolLiteral>) {
        return l.value ? "true" : "false";
      } else {
        return std::string{};
      }
    },
    lit);
}

std::string AstToBtCppModelConverter::serialize_expression(const Expression & expr)
{
  return std::visit(
    [&](const auto & e) -> std::string {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        return format_literal_for_script(e);
      } else if constexpr (std::is_same_v<T, VarRef>) {
        return e.name;
      } else if constexpr (std::is_same_v<T, Box<BinaryExpr>>) {
        const auto & b = *e;
        const auto left = serialize_expression(b.left);
        const auto right = serialize_expression(b.right);
        return "(" + left + " " + std::string(to_string(b.op)) + " " + right + ")";
      } else if constexpr (std::is_same_v<T, Box<UnaryExpr>>) {
        const auto & u = *e;
        const auto operand = serialize_expression(u.operand);
        return std::string(to_string(u.op)) + operand;
      } else {
        return std::string{};
      }
    },
    expr);
}

std::string AstToBtCppModelConverter::serialize_assignment_stmt(const AssignmentStmt & stmt)
{
  return stmt.target + " " + std::string(to_string(stmt.op)) + " " +
         serialize_expression(stmt.value);
}

std::string AstToBtCppModelConverter::serialize_value_expr_for_attribute(const ValueExpr & value)
{
  return std::visit(
    [](const auto & v) -> std::string {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, Literal>) {
        return std::visit(
          [](const auto & lit) -> std::string {
            using L = std::decay_t<decltype(lit)>;
            if constexpr (std::is_same_v<L, StringLiteral>) {
              return lit.value;
            } else if constexpr (std::is_same_v<L, IntLiteral>) {
              return std::to_string(lit.value);
            } else if constexpr (std::is_same_v<L, FloatLiteral>) {
              return to_string_lossless(lit.value);
            } else if constexpr (std::is_same_v<L, BoolLiteral>) {
              return lit.value ? "true" : "false";
            } else {
              return std::string{};
            }
          },
          v);
      } else if constexpr (std::is_same_v<T, BlackboardRef>) {
        return "{" + v.name + "}";
      } else {
        return std::string{};
      }
    },
    value);
}

std::vector<btcpp::Attribute> AstToBtCppModelConverter::convert_arguments_to_attributes(
  const std::vector<Argument> & args, std::string_view node_id, const NodeRegistry & nodes)
{
  std::vector<btcpp::Attribute> out;
  out.reserve(args.size());

  for (const auto & arg : args) {
    std::optional<std::string> port_name = arg.name;
    if (!port_name) {
      port_name = nodes.get_single_port_name(node_id);
    }
    if (!port_name) {
      continue;  // unknown positional mapping
    }
    out.push_back(btcpp::Attribute{*port_name, serialize_value_expr_for_attribute(arg.value)});
  }

  return out;
}

btcpp::Node AstToBtCppModelConverter::wrap_with_decorators(
  btcpp::Node inner, const std::vector<Decorator> & decorators, const NodeRegistry & nodes)
{
  // Decorators are wrappers.
  // DSL order: @A @B Node() => <B><A><Node/></A></B>
  // (i.e. the last decorator becomes the outermost wrapper).
  btcpp::Node current = std::move(inner);

  for (const auto & dec : decorators) {
    btcpp::Node wrapper;
    wrapper.tag = dec.name;
    wrapper.attributes = convert_arguments_to_attributes(dec.args, dec.name, nodes);
    wrapper.children.push_back(std::move(current));
    current = std::move(wrapper);
  }

  return current;
}

btcpp::Node AstToBtCppModelConverter::convert_node_stmt(
  const NodeStmt & node, const Program & program, const NodeRegistry & nodes) const
{
  (void)program;  // reserved for future (e.g., scope-aware formatting)

  btcpp::Node element;
  element.tag = node.node_name;

  // Attributes from args
  element.attributes = convert_arguments_to_attributes(node.args, node.node_name, nodes);

  // Description from docs
  const auto desc = join_docs(node.docs);
  if (!desc.empty()) {
    element.attributes.push_back(btcpp::Attribute{"_description", desc});
  }

  // Children
  for (const auto & child : node.children) {
    std::visit(
      [&](const auto & ch) {
        using T = std::decay_t<decltype(ch)>;
        if constexpr (std::is_same_v<T, Box<NodeStmt>>) {
          element.children.push_back(convert_node_stmt(*ch, program, nodes));
        } else if constexpr (std::is_same_v<T, AssignmentStmt>) {
          btcpp::Node script;
          script.tag = "Script";
          script.attributes.push_back(
            btcpp::Attribute{"code", " " + serialize_assignment_stmt(ch) + " "});
          element.children.push_back(std::move(script));
        }
      },
      child);
  }

  // Apply decorators as wrappers
  if (!node.decorators.empty()) {
    return wrap_with_decorators(std::move(element), node.decorators, nodes);
  }

  return element;
}

btcpp::Document AstToBtCppModelConverter::convert(
  const Program & program, const AnalysisResult & analysis) const
{
  btcpp::Document doc;

  const std::string main_tree = !program.trees.empty() ? program.trees.front().name : "Main";
  doc.main_tree_to_execute = main_tree;

  // TreeNodesModel: only subtrees with params
  for (const auto & tree : program.trees) {
    if (tree.params.empty()) {
      continue;
    }

    btcpp::SubTreeModel st;
    st.id = tree.name;

    const TypeContext * ctx = analysis.get_tree_context(tree.name);

    for (const auto & p : tree.params) {
      btcpp::PortModel pm;
      const auto dir = p.direction.value_or(PortDirection::In);
      if (dir == PortDirection::Out) {
        pm.kind = btcpp::PortKind::Output;
      } else if (dir == PortDirection::Ref) {
        pm.kind = btcpp::PortKind::InOut;
      } else {
        pm.kind = btcpp::PortKind::Input;
      }
      pm.name = p.name;

      if (p.type_name) {
        pm.type = *p.type_name;
      } else if (ctx) {
        if (const Type * t = ctx->get_type(p.name)) {
          if (!t->is_unknown()) {
            pm.type = t->to_string();
          }
        }
      }

      st.ports.push_back(std::move(pm));
    }

    doc.tree_nodes_model.push_back(std::move(st));
  }

  // BehaviorTrees
  for (const auto & tree : program.trees) {
    btcpp::BehaviorTreeModel tm;
    tm.id = tree.name;

    const auto tree_desc = join_docs(tree.docs);
    if (!tree_desc.empty()) {
      tm.description = tree_desc;
    }

    if (tree.body) {
      // Local var initialization: create Script node and wrap with Sequence
      std::vector<const LocalVarDecl *> vars_with_init;
      for (const auto & lv : tree.local_vars) {
        if (lv.initial_value) {
          vars_with_init.push_back(&lv);
        }
      }

      if (!vars_with_init.empty()) {
        btcpp::Node seq;
        seq.tag = "Sequence";

        btcpp::Node script;
        script.tag = "Script";

        std::string code;
        for (const auto * v : vars_with_init) {
          if (!v || !v->initial_value) {
            continue;
          }
          if (!code.empty()) {
            code += "; ";
          }
          code += v->name;
          code += ":=";
          code += serialize_expression(*v->initial_value);
        }

        script.attributes.push_back(btcpp::Attribute{"code", " " + code + " "});
        seq.children.push_back(std::move(script));

        seq.children.push_back(convert_node_stmt(*tree.body, program, analysis.nodes));
        tm.root = std::move(seq);
      } else {
        tm.root = convert_node_stmt(*tree.body, program, analysis.nodes);
      }
    }

    doc.behavior_trees.push_back(std::move(tm));
  }

  return doc;
}

// ============================================================================
// BtCppXmlSerializer (tinyxml2)
// ============================================================================

static tinyxml2::XMLElement * append_node_impl(
  tinyxml2::XMLDocument & doc, tinyxml2::XMLElement * parent, const btcpp::Node & node)
{
  auto * elem = doc.NewElement(node.tag.c_str());

  for (const auto & a : node.attributes) {
    elem->SetAttribute(a.key.c_str(), a.value.c_str());
  }

  if (node.text) {
    elem->SetText(node.text->c_str());
  }

  for (const auto & ch : node.children) {
    append_node_impl(doc, elem, ch);
  }

  parent->InsertEndChild(elem);
  return elem;
}

std::string BtCppXmlSerializer::serialize(const btcpp::Document & doc_model)
{
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild(doc.NewDeclaration(R"(xml version="1.0" encoding="UTF-8")"));

  auto * root = doc.NewElement("root");
  root->SetAttribute("BTCPP_format", "4");
  root->SetAttribute("main_tree_to_execute", doc_model.main_tree_to_execute.c_str());
  doc.InsertEndChild(root);

  if (!doc_model.tree_nodes_model.empty()) {
    auto * tnm = doc.NewElement("TreeNodesModel");
    root->InsertEndChild(tnm);

    for (const auto & st : doc_model.tree_nodes_model) {
      auto * sub = doc.NewElement("SubTree");
      sub->SetAttribute("ID", st.id.c_str());
      tnm->InsertEndChild(sub);

      for (const auto & p : st.ports) {
        const char * tag = nullptr;
        switch (p.kind) {
          case btcpp::PortKind::Input:
            tag = "input_port";
            break;
          case btcpp::PortKind::Output:
            tag = "output_port";
            break;
          case btcpp::PortKind::InOut:
            tag = "inout_port";
            break;
        }
        auto * pe = doc.NewElement(tag);
        pe->SetAttribute("name", p.name.c_str());
        if (p.type) {
          pe->SetAttribute("type", p.type->c_str());
        }
        sub->InsertEndChild(pe);
      }
    }
  }

  for (const auto & tree : doc_model.behavior_trees) {
    auto * bt = doc.NewElement("BehaviorTree");
    bt->SetAttribute("ID", tree.id.c_str());
    root->InsertEndChild(bt);

    if (tree.description && !tree.description->empty()) {
      auto * meta = doc.NewElement("Metadata");
      bt->InsertEndChild(meta);

      auto * item = doc.NewElement("item");
      item->SetAttribute("key", "description");
      item->SetAttribute("value", tree.description->c_str());
      meta->InsertEndChild(item);
    }

    if (tree.root) {
      append_node_impl(doc, bt, *tree.root);
    }
  }

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return {printer.CStr()};
}

// ============================================================================
// XmlGenerator facade
// ============================================================================

std::string XmlGenerator::generate(const Program & program, const AnalysisResult & analysis)
{
  const AstToBtCppModelConverter converter;
  const auto model = converter.convert(program, analysis);
  return BtCppXmlSerializer::serialize(model);
}

}  // namespace bt_dsl
