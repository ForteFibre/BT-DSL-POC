// bt_dsl/ast/visitor.hpp - CRTP Visitor pattern for AST traversal
//
// This header provides a visitor pattern implementation using CRTP
// (Curiously Recurring Template Pattern) for type-safe, efficient AST traversal.
//
#pragma once

#include <type_traits>

#include "bt_dsl/ast/ast.hpp"
#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/basic/casting.hpp"

namespace bt_dsl
{

// ============================================================================
// Type Traits for Const-Aware Node Pointer
// ============================================================================

namespace detail
{

/// Helper to propagate const from NodePtrT to derived node types
template <typename NodePtrT, typename DerivedNode>
struct PropagateConst
{
  using type = std::conditional_t<
    std::is_const_v<std::remove_pointer_t<NodePtrT>>, const DerivedNode *, DerivedNode *>;
};

template <typename NodePtrT, typename DerivedNode>
using propagate_const_t = typename PropagateConst<NodePtrT, DerivedNode>::type;

/// Base node type from pointer (strips const and pointer)
template <typename NodePtrT>
using base_node_t = std::remove_const_t<std::remove_pointer_t<NodePtrT>>;

}  // namespace detail

// ============================================================================
// AstVisitor - CRTP Base Class
// ============================================================================

/**
 * CRTP-based visitor for AST traversal.
 *
 * This provides a type-safe way to traverse the AST without needing
 * virtual dispatch overhead. The derived class implements visit methods
 * for specific node types.
 *
 * Usage:
 * @code
 *   class MyVisitor : public AstVisitor<MyVisitor, void> {
 *   public:
 *     void visitIntLiteralExpr(IntLiteralExpr* node) {
 *       std::cout << node->value << "\n";
 *     }
 *     // ... other visit methods
 *   };
 *
 *   MyVisitor v;
 *   v.visit(someExpr);
 * @endcode
 *
 * For const traversal:
 * @code
 *   class ConstVisitor : public ConstAstVisitor<ConstVisitor, void> {
 *   public:
 *     void visitIntLiteralExpr(const IntLiteralExpr* node) {
 *       std::cout << node->value << "\n";
 *     }
 *   };
 * @endcode
 *
 * @tparam Derived The derived visitor class
 * @tparam ReturnType The return type of visit methods (default: void)
 * @tparam NodePtrT The node pointer type (default: AstNode*, use const AstNode* for const traversal)
 */
template <typename Derived, typename ReturnType = void, typename NodePtrT = AstNode *>
class AstVisitor
{
public:
  /// The node pointer type used by this visitor
  using node_ptr_type = NodePtrT;

  /// Cast to derived type
  [[nodiscard]] Derived & get_derived() { return static_cast<Derived &>(*this); }
  [[nodiscard]] const Derived & get_derived() const { return static_cast<const Derived &>(*this); }

  // ===========================================================================
  // Main dispatch method
  // ===========================================================================

  /**
   * Visit an AST node, dispatching to the appropriate visit method.
   *
   * @param node The node to visit
   * @return The result of visiting the node
   */
  ReturnType visit(NodePtrT node)
  {
    if (!node) {
      return ReturnType();
    }

    switch (node->kind) {
      // Auto-generated dispatch cases from X-Macro
#define AST_NODE_EXPR(Class, Kind, Snake) \
  case NodeKind::Kind:                    \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"

#define AST_NODE_TYPE(Class, Kind, Snake) \
  case NodeKind::Kind:                    \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"

#define AST_NODE_STMT(Class, Kind, Snake) \
  case NodeKind::Kind:                    \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"

#define AST_NODE_DECL(Class, Kind, Snake) \
  case NodeKind::Kind:                    \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"

#define AST_NODE_SUPPORT(Class, Kind, Snake) \
  case NodeKind::Kind:                       \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"

#define AST_NODE_TOP(Class, Kind, Snake) \
  case NodeKind::Kind:                   \
    return get_derived().visit_##Snake(cast<Class>(node));
#include "bt_dsl/ast/ast_nodes.def"
    }

    // Unreachable, but silences compiler warning
    return ReturnType();
  }

  // ===========================================================================
  // Default visit methods (auto-generated from X-Macro)
  // ===========================================================================

  // Expressions - default implementation calls visitExpr
#define AST_NODE_EXPR(Class, Kind, Snake)                                   \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_expr(node);                                                \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // Types - default implementation calls visitTypeNode
#define AST_NODE_TYPE(Class, Kind, Snake)                                   \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_type_node(node);                                           \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // Statements - default implementation calls visitStmt
#define AST_NODE_STMT(Class, Kind, Snake)                                   \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_stmt(node);                                                \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // Declarations - default implementation calls visitDecl
#define AST_NODE_DECL(Class, Kind, Snake)                                   \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_decl(node);                                                \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // Supporting nodes - default implementation calls visitNode
#define AST_NODE_SUPPORT(Class, Kind, Snake)                                \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_node(node);                                                \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // Top-level - default implementation calls visitNode
#define AST_NODE_TOP(Class, Kind, Snake)                                    \
  ReturnType visit_##Snake(detail::propagate_const_t<NodePtrT, Class> node) \
  {                                                                         \
    return visit_node(node);                                                \
  }
#include "bt_dsl/ast/ast_nodes.def"

  // ===========================================================================
  // Category-level visit methods (for grouping behavior)
  // ===========================================================================

  ReturnType visit_expr(detail::propagate_const_t<NodePtrT, Expr> node) { return visit_node(node); }
  ReturnType visit_type_node(detail::propagate_const_t<NodePtrT, TypeNode> node)
  {
    return visit_node(node);
  }
  ReturnType visit_stmt(detail::propagate_const_t<NodePtrT, Stmt> node) { return visit_node(node); }
  ReturnType visit_decl(detail::propagate_const_t<NodePtrT, Decl> node) { return visit_node(node); }

  /// Base case - does nothing by default
  ReturnType visit_node(NodePtrT /*node*/) { return ReturnType(); }
};

/// Alias for const AST traversal
template <typename Derived, typename ReturnType = void>
using ConstAstVisitor = AstVisitor<Derived, ReturnType, const AstNode *>;

// ============================================================================
// RecursiveAstVisitor - Traverses children automatically
// ============================================================================

/**
 * A visitor that automatically traverses child nodes.
 *
 * Override specific visit methods to customize behavior. Call the base
 * implementation to continue traversal, or skip it to prune the subtree.
 *
 * @tparam Derived The derived visitor class
 * @tparam NodePtrT The node pointer type (default: AstNode*)
 */
template <typename Derived, typename NodePtrT = AstNode *>
class RecursiveAstVisitor : public AstVisitor<Derived, bool, NodePtrT>
{
  using Base = AstVisitor<Derived, bool, NodePtrT>;

public:
  using Base::get_derived;

  // Helper type for propagating const
  template <typename T>
  using NodePtr = detail::propagate_const_t<NodePtrT, T>;

  // ===========================================================================
  // Traversal methods that recurse into children
  // ===========================================================================

  bool visit_binary_expr(NodePtr<BinaryExpr> node)
  {
    if (!get_derived().visit(node->lhs)) return false;
    if (!get_derived().visit(node->rhs)) return false;
    return true;
  }

  bool visit_unary_expr(NodePtr<UnaryExpr> node) { return get_derived().visit(node->operand); }

  bool visit_cast_expr(NodePtr<CastExpr> node)
  {
    if (!get_derived().visit(node->expr)) return false;
    if (!get_derived().visit(node->targetType)) return false;
    return true;
  }

  bool visit_index_expr(NodePtr<IndexExpr> node)
  {
    if (!get_derived().visit(node->base)) return false;
    if (!get_derived().visit(node->index)) return false;
    return true;
  }

  bool visit_array_literal_expr(NodePtr<ArrayLiteralExpr> node)
  {
    for (auto * elem : node->elements) {
      if (!get_derived().visit(elem)) return false;
    }
    return true;
  }

  bool visit_array_repeat_expr(NodePtr<ArrayRepeatExpr> node)
  {
    if (!get_derived().visit(node->value)) return false;
    if (!get_derived().visit(node->count)) return false;
    return true;
  }

  bool visit_vec_macro_expr(NodePtr<VecMacroExpr> node) { return get_derived().visit(node->inner); }

  bool visit_static_array_type(NodePtr<StaticArrayType> node)
  {
    return get_derived().visit(node->elementType);
  }

  bool visit_dynamic_array_type(NodePtr<DynamicArrayType> node)
  {
    return get_derived().visit(node->elementType);
  }

  bool visit_type_expr(NodePtr<TypeExpr> node) { return get_derived().visit(node->base); }

  bool visit_node_stmt(NodePtr<NodeStmt> node)
  {
    for (auto * pre : node->preconditions) {
      if (!get_derived().visit(pre)) return false;
    }
    for (auto * arg : node->args) {
      if (!get_derived().visit(arg)) return false;
    }
    for (auto * child : node->children) {
      if (!get_derived().visit(child)) return false;
    }
    return true;
  }

  bool visit_assignment_stmt(NodePtr<AssignmentStmt> node)
  {
    for (auto * pre : node->preconditions) {
      if (!get_derived().visit(pre)) return false;
    }
    for (auto * idx : node->indices) {
      if (!get_derived().visit(idx)) return false;
    }
    return get_derived().visit(node->value);
  }

  bool visit_blackboard_decl_stmt(NodePtr<BlackboardDeclStmt> node)
  {
    if (node->type && !get_derived().visit(node->type)) return false;
    if (node->initialValue && !get_derived().visit(node->initialValue)) return false;
    return true;
  }

  bool visit_const_decl_stmt(NodePtr<ConstDeclStmt> node)
  {
    if (node->type && !get_derived().visit(node->type)) return false;
    if (!get_derived().visit(node->value)) return false;
    return true;
  }

  bool visit_extern_decl(NodePtr<ExternDecl> node)
  {
    for (auto * port : node->ports) {
      if (!get_derived().visit(port)) return false;
    }
    return !node->behaviorAttr || get_derived().visit(node->behaviorAttr);
  }

  bool visit_type_alias_decl(NodePtr<TypeAliasDecl> node)
  {
    return get_derived().visit(node->aliasedType);
  }

  bool visit_global_var_decl(NodePtr<GlobalVarDecl> node)
  {
    if (node->type && !get_derived().visit(node->type)) return false;
    if (node->initialValue && !get_derived().visit(node->initialValue)) return false;
    return true;
  }

  bool visit_global_const_decl(NodePtr<GlobalConstDecl> node)
  {
    if (node->type && !get_derived().visit(node->type)) return false;
    if (!get_derived().visit(node->value)) return false;
    return true;
  }

  bool visit_tree_decl(NodePtr<TreeDecl> node)
  {
    for (auto * param : node->params) {
      if (!get_derived().visit(param)) return false;
    }
    for (auto * stmt : node->body) {
      if (!get_derived().visit(stmt)) return false;
    }
    return true;
  }

  bool visit_argument(NodePtr<Argument> node)
  {
    if (node->valueExpr && !get_derived().visit(node->valueExpr)) return false;
    if (node->inlineDecl && !get_derived().visit(node->inlineDecl)) return false;
    return true;
  }

  bool visit_precondition(NodePtr<Precondition> node)
  {
    return get_derived().visit(node->condition);
  }

  bool visit_param_decl(NodePtr<ParamDecl> node)
  {
    if (!get_derived().visit(node->type)) return false;
    if (node->defaultValue && !get_derived().visit(node->defaultValue)) return false;
    return true;
  }

  bool visit_extern_port(NodePtr<ExternPort> node)
  {
    if (!get_derived().visit(node->type)) return false;
    if (node->defaultValue && !get_derived().visit(node->defaultValue)) return false;
    return true;
  }

  bool visit_program(NodePtr<Program> node)
  {
    for (auto * d : node->decls) {
      if (!get_derived().visit(d)) return false;
    }
    return true;
  }

  // Leaf nodes that don't have children
  bool visit_int_literal_expr(NodePtr<IntLiteralExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_float_literal_expr(NodePtr<FloatLiteralExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_string_literal_expr(NodePtr<StringLiteralExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_bool_literal_expr(NodePtr<BoolLiteralExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_null_literal_expr(NodePtr<NullLiteralExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_var_ref_expr(NodePtr<VarRefExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_missing_expr(NodePtr<MissingExpr> node)
  {
    (void)node;
    return true;
  }
  bool visit_infer_type(NodePtr<InferType> node)
  {
    (void)node;
    return true;
  }
  bool visit_primary_type(NodePtr<PrimaryType> node)
  {
    (void)node;
    return true;
  }
  bool visit_import_decl(NodePtr<ImportDecl> node)
  {
    (void)node;
    return true;
  }
  bool visit_extern_type_decl(NodePtr<ExternTypeDecl> node)
  {
    (void)node;
    return true;
  }
  bool visit_behavior_attr(NodePtr<BehaviorAttr> node)
  {
    (void)node;
    return true;
  }
  bool visit_inline_blackboard_decl(NodePtr<InlineBlackboardDecl> node)
  {
    (void)node;
    return true;
  }
};

/// Alias for const recursive AST traversal
template <typename Derived>
using ConstRecursiveAstVisitor = RecursiveAstVisitor<Derived, const AstNode *>;

}  // namespace bt_dsl
