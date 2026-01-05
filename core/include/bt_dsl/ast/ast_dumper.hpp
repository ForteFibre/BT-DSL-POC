// bt_dsl/ast/ast_dumper.hpp - Debug AST tree output
//
// This header provides utilities for dumping AST nodes in a human-readable
// tree format, useful for debugging and development.
//
#pragma once

#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/ast/visitor.hpp"

namespace bt_dsl
{

// ============================================================================
// AstDumper - Debug AST output
// ============================================================================

/**
 * Dumps AST nodes in a human-readable tree format.
 *
 * @code
 *   Program
 *   |-TreeDecl 'main'
 *   | |-ParamDecl 'target' in Pose?
 *   | `-NodeStmt 'MoveTo'
 *   |   `-Argument 'goal' = VarRefExpr 'target'
 *   `-GlobalConstDecl 'MAX_SPEED' = 10
 * @endcode
 *
 * Usage:
 * @code
 *   AstDumper dumper(std::cout);
 *   dumper.dump(program);
 * @endcode
 */
class AstDumper : public AstVisitor<AstDumper, void>
{
public:
  explicit AstDumper(std::ostream & os) : os_(os) {}

  /// Dump an AST node and its subtree
  void dump(AstNode * node)
  {
    visit(node);
    os_ << "\n";
  }

  // ===========================================================================
  // Generic tree printer
  // ===========================================================================

  /// Property for display: either {"key", "value"} or {"", "value"} for bare values
  struct Prop
  {
    std::string_view key;
    std::string value;

    // Key-value pair: key='value'
    Prop(std::string_view k, std::string_view v) : key(k), value(v) {}
    Prop(std::string_view k, std::string v) : key(k), value(std::move(v)) {}
    Prop(std::string_view k, const char * v) : key(k), value(v) {}

    // Bare value (no key)
    Prop(std::string_view v) : value(v) {}
    Prop(std::string v) : value(std::move(v)) {}
    Prop(const char * v) : value(v) {}

    // Numeric values
    Prop(std::string_view k, int64_t v) : key(k), value(std::to_string(v)) {}
    Prop(std::string_view k, uint64_t v) : key(k), value(std::to_string(v)) {}
    Prop(std::string_view k, double v) : key(k), value(std::to_string(v)) {}
    Prop(std::string_view k, bool v) : key(k), value(v ? "true" : "false") {}

    // Bare numeric values
    Prop(int64_t v) : value(std::to_string(v)) {}
    Prop(uint64_t v) : value(std::to_string(v)) {}
    Prop(double v) : value(std::to_string(v)) {}
  };

  /**
   * Generic dump function
   * @param label Display name (e.g., "BinaryExpr")
   * @param props Property list
   * @param childContainers Children (pointers, vectors, etc. as variadic args)
   */
  template <typename... Containers>
  void print_tree(
    std::string_view label, std::initializer_list<Prop> props,
    const Containers &... childContainers)
  {
    // 1. Print the node itself
    print_prefix();
    os_ << label;
    for (const auto & prop : props) {
      if (prop.key.empty()) {
        os_ << " " << prop.value;
      } else {
        os_ << " " << prop.key << "='" << prop.value << "'";
      }
    }
    os_ << "\n";

    // 2. Flatten all children into a single list
    std::vector<AstNode *> all_children;
    (collect_children(all_children, childContainers), ...);

    // 3. Render children with automatic indent management
    if (!all_children.empty()) {
      const IndentScope scope(*this);
      for (size_t i = 0; i < all_children.size(); ++i) {
        isLast_ = (i == all_children.size() - 1);
        visit(all_children[i]);
      }
    }
  }

  /**
   * Overload that accepts a dynamically built property vector.
   *
   * Many visitors conditionally add properties; using std::vector keeps that
   * code readable.
   */
  template <typename... Containers>
  void print_tree(
    std::string_view label, const std::vector<Prop> & props, const Containers &... childContainers)
  {
    print_prefix();
    os_ << label;
    for (const auto & prop : props) {
      if (prop.key.empty()) {
        os_ << " " << prop.value;
      } else {
        os_ << " " << prop.key << "='" << prop.value << "'";
      }
    }
    os_ << "\n";

    std::vector<AstNode *> all_children;
    (collect_children(all_children, childContainers), ...);

    if (!all_children.empty()) {
      const IndentScope scope(*this);
      for (size_t i = 0; i < all_children.size(); ++i) {
        isLast_ = (i == all_children.size() - 1);
        visit(all_children[i]);
      }
    }
  }

  // Overload without props
  template <typename... Containers>
  void print_tree(std::string_view label, const Containers &... childContainers)
  {
    print_tree(label, {}, childContainers...);
  }

  // ===========================================================================
  // Visit methods
  // ===========================================================================

  void visit_program(Program * node)
  {
    // Program is special: we print without prefix at root level
    os_ << "Program";
    if (!node->innerDocs.empty()) {
      os_ << " [" << node->innerDocs.size() << " docs]";
    }
    os_ << "\n";

    // Collect all children
    std::vector<AstNode *> all_children;
    collect_children(all_children, node->decls);

    if (!all_children.empty()) {
      // Root's direct children should start at column 0 with the tree markers.
      // Indentation begins from the second level.
      for (size_t i = 0; i < all_children.size(); ++i) {
        isLast_ = (i == all_children.size() - 1);
        visit(all_children[i]);
      }
    }
  }

  // --- Declarations ---
  void visit_tree_decl(TreeDecl * node)
  {
    print_tree("TreeDecl", {{"name", node->name}}, node->params, node->body);
  }
  void visit_import_decl(ImportDecl * node)
  {
    print_tree("ImportDecl", {{"path", node->path_string()}});
  }
  void visit_extern_decl(ExternDecl * node)
  {
    print_tree(
      "ExternDecl", {{to_string(node->category)}, {"name", node->name}}, node->ports,
      node->behaviorAttr);
  }
  void visit_extern_type_decl(ExternTypeDecl * node)
  {
    print_tree("ExternTypeDecl", {{"name", node->name}});
  }
  void visit_type_alias_decl(TypeAliasDecl * node)
  {
    print_tree("TypeAliasDecl", {{"name", node->name}}, node->aliasedType);
  }
  void visit_global_var_decl(GlobalVarDecl * node)
  {
    print_tree("GlobalVarDecl", {{"name", node->name}}, node->type, node->initialValue);
  }
  void visit_global_const_decl(GlobalConstDecl * node)
  {
    print_tree("GlobalConstDecl", {{"name", node->name}}, node->type, node->value);
  }

  // --- Statements ---
  void visit_node_stmt(NodeStmt * node)
  {
    std::vector<Prop> props = {{"name", node->nodeName}};
    if (node->hasPropertyBlock) props.emplace_back("[props]");
    if (node->hasChildrenBlock) props.emplace_back("[children]");
    print_tree("NodeStmt", props, node->preconditions, node->args, node->children);
  }
  void visit_assignment_stmt(AssignmentStmt * node)
  {
    print_tree(
      "AssignmentStmt", {{"target", node->target}, {to_string(node->op)}}, node->preconditions,
      node->indices, node->value);
  }
  void visit_blackboard_decl_stmt(BlackboardDeclStmt * node)
  {
    print_tree("BlackboardDeclStmt", {{"name", node->name}}, node->type, node->initialValue);
  }
  void visit_const_decl_stmt(ConstDeclStmt * node)
  {
    print_tree("ConstDeclStmt", {{"name", node->name}}, node->type, node->value);
  }

  // --- Supporting nodes ---
  void visit_param_decl(ParamDecl * node)
  {
    std::vector<Prop> props = {{"name", node->name}};
    if (node->direction) props.emplace_back(to_string(*node->direction));
    print_tree("ParamDecl", props, node->type, node->defaultValue);
  }
  void visit_argument(Argument * node)
  {
    std::vector<Prop> props = {{"name", node->name}};
    if (node->direction) props.emplace_back(to_string(*node->direction));
    if (node->is_inline_decl()) props.emplace_back("[inline]");
    print_tree("Argument", props, node->valueExpr, node->inlineDecl);
  }
  void visit_precondition(Precondition * node)
  {
    print_tree("Precondition", {{"@" + std::string(to_string(node->kind))}}, node->condition);
  }
  void visit_extern_port(ExternPort * node)
  {
    std::vector<Prop> props = {{"name", node->name}};
    if (node->direction) props.emplace_back(to_string(*node->direction));
    print_tree("ExternPort", props, node->type, node->defaultValue);
  }
  void visit_behavior_attr(BehaviorAttr * node)
  {
    std::vector<Prop> props = {{"data", to_string(node->dataPolicy)}};
    if (node->flowPolicy) props.emplace_back("flow", to_string(*node->flowPolicy));
    print_tree("BehaviorAttr", props);
  }
  void visit_inline_blackboard_decl(InlineBlackboardDecl * node)
  {
    print_tree("InlineBlackboardDecl", {{"name", node->name}});
  }

  // --- Expressions ---
  void visit_int_literal_expr(IntLiteralExpr * node)
  {
    print_tree("IntLiteralExpr", {Prop(node->value)});
  }
  void visit_float_literal_expr(FloatLiteralExpr * node)
  {
    print_tree("FloatLiteralExpr", {Prop(node->value)});
  }
  void visit_string_literal_expr(StringLiteralExpr * node)
  {
    print_tree("StringLiteralExpr", {{"\"" + std::string(node->value) + "\""}});
  }
  void visit_bool_literal_expr(BoolLiteralExpr * node)
  {
    print_tree("BoolLiteralExpr", {Prop(node->value ? "true" : "false")});
  }
  void visit_null_literal_expr([[maybe_unused]] NullLiteralExpr * node)
  {
    print_tree("NullLiteralExpr", {});
  }
  void visit_missing_expr([[maybe_unused]] MissingExpr * node) { print_tree("MissingExpr", {}); }

  void visit_var_ref_expr(VarRefExpr * node) { print_tree("VarRefExpr", {{"name", node->name}}); }
  void visit_binary_expr(BinaryExpr * node)
  {
    print_tree("BinaryExpr", {{"op", to_string(node->op)}}, node->lhs, node->rhs);
  }
  void visit_unary_expr(UnaryExpr * node)
  {
    print_tree("UnaryExpr", {{"op", to_string(node->op)}}, node->operand);
  }
  void visit_cast_expr(CastExpr * node)
  {
    print_tree("CastExpr", {}, node->expr, node->targetType);
  }
  void visit_index_expr(IndexExpr * node) { print_tree("IndexExpr", {}, node->base, node->index); }
  void visit_vec_macro_expr(VecMacroExpr * node) { print_tree("VecMacroExpr", {}, node->inner); }

  void visit_array_literal_expr(ArrayLiteralExpr * node)
  {
    print_tree("ArrayLiteralExpr", {}, node->elements);
  }

  void visit_array_repeat_expr(ArrayRepeatExpr * node)
  {
    print_tree("ArrayRepeatExpr", {}, node->value, node->count);
  }

  // --- Types ---
  void visit_infer_type([[maybe_unused]] InferType * node) { print_tree("InferType", {}); }
  void visit_dynamic_array_type(DynamicArrayType * node)
  {
    print_tree("DynamicArrayType", {}, node->elementType);
  }

  void visit_primary_type(PrimaryType * node)
  {
    std::vector<Prop> props = {{"name", node->name}};
    if (node->size) props.emplace_back("size", std::string(*node->size));
    print_tree("PrimaryType", props);
  }
  void visit_static_array_type(StaticArrayType * node)
  {
    std::string size_str = (node->isBounded ? "<=" : "") + std::string(node->size);
    print_tree("StaticArrayType", {{"size", size_str}}, node->elementType);
  }
  void visit_type_expr(TypeExpr * node)
  {
    std::vector<Prop> props;
    if (node->nullable) props.emplace_back("nullable");
    print_tree("TypeExpr", props, node->base);
  }

private:
  std::ostream & os_;
  std::string prefix_;
  bool isLast_ = true;

  // --- Helper: extract children based on type ---

  // Single pointer
  template <typename T>
  void collect_children(std::vector<AstNode *> & out, T * ptr)
  {
    if (ptr) out.push_back(ptr);
  }

  // std::vector<T*>
  template <typename T>
  void collect_children(std::vector<AstNode *> & out, const std::vector<T *> & vec)
  {
    for (auto * ptr : vec) {
      if (ptr) out.push_back(ptr);
    }
  }

  // Note: std::vector<Prop> is handled as props by the print_tree overload above.

  // gsl::span<T*> (for AST nodes using span instead of vector)
  template <typename T>
  void collect_children(std::vector<AstNode *> & out, gsl::span<T *> span)
  {
    for (auto * ptr : span) {
      if (ptr) out.push_back(ptr);
    }
  }

  // --- Rendering ---

  void print_prefix()
  {
    os_ << prefix_;
    os_ << (isLast_ ? "`-" : "|-");
  }

  struct IndentScope
  {
    AstDumper & d;
    std::string saved;

    explicit IndentScope(AstDumper & dumper) : d(dumper), saved(d.prefix_)
    {
      d.prefix_ += d.isLast_ ? "  " : "| ";
    }

    ~IndentScope() { d.prefix_ = saved; }
  };
};

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Dump an AST node to the given output stream.
 */
inline void dump(AstNode * node, std::ostream & os)
{
  AstDumper dumper(os);
  dumper.dump(node);
}

/**
 * Dump an AST node to a string.
 */
inline std::string dump_to_string(AstNode * node)
{
  std::ostringstream ss;
  dump(node, ss);
  return ss.str();
}

}  // namespace bt_dsl
