// bt_dsl/ast/ast.hpp - AST node class definitions for BT-DSL
//
// This header contains all AST node class definitions following the
// LLVM/Clang style with classof() for RTTI support.
//
#pragma once

#include <gsl/span>
#include <optional>
#include <string_view>

#include "bt_dsl/ast/ast_enums.hpp"
#include "bt_dsl/basic/casting.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

// Forward declarations for sema symbol types
struct Symbol;      // Value-space symbol (var, const, param)
struct TypeSymbol;  // Type-space symbol (extern type, type alias, builtin)
struct NodeSymbol;  // Node-space symbol (extern node, tree)
class ConstValue;   // Compile-time constant value

// ============================================================================
// Base Classes
// ============================================================================

/**
 * Base class for all AST nodes.
 *
 * Every AST node has:
 * - A NodeKind for RTTI (using classof pattern)
 * - A SourceRange indicating its location in source
 *
 * Nodes are non-copyable and managed by AstContext.
 */
class AstNode
{
public:
  const NodeKind kind;
  SourceRange range_;  ///< Byte offsets only (8 bytes). Line/col computed via SourceManager.

  // Non-copyable, non-movable (managed by AstContext)
  AstNode(const AstNode &) = delete;
  AstNode & operator=(const AstNode &) = delete;
  AstNode(AstNode &&) = delete;
  AstNode & operator=(AstNode &&) = delete;

  /// Get the node kind
  [[nodiscard]] NodeKind get_kind() const noexcept { return kind; }

  /// Get the source range (byte offsets only)
  [[nodiscard]] SourceRange get_range() const noexcept { return range_; }

protected:
  explicit AstNode(NodeKind k, SourceRange r = {}) : kind(k), range_(r) {}
  ~AstNode() = default;  // Non-virtual, protected: prevents polymorphic delete
};

// ============================================================================
// CRTP Base for Automatic classof()
// ============================================================================

/**
 * CRTP base class that automatically implements classof().
 *
 * @tparam Derived The concrete node class
 * @tparam Base The base class to inherit from
 * @tparam K The NodeKind for this node type
 */
template <typename Derived, typename Base, NodeKind K>
class NodeBase : public Base
{
public:
  static constexpr NodeKind kind = K;

  static bool classof(const AstNode * node) { return node->get_kind() == K; }

protected:
  explicit NodeBase(SourceRange r = {}) : Base(K, r) {}
};

// ============================================================================
// Category Base Classes
// ============================================================================

/**
 * Base class for expressions.
 */
class Expr : public AstNode
{
public:
  /// Resolved semantic type (set during Sema phase, nullptr before resolution)
  const struct Type * resolvedType = nullptr;

  static bool classof(const AstNode * node) { return is_expr_kind(node->kind); }

protected:
  explicit Expr(NodeKind k, SourceRange r = {}) : AstNode(k, r) {}
};

/**
 * Base class for type expressions.
 */
class TypeNode : public AstNode
{
public:
  static bool classof(const AstNode * node) { return is_type_kind(node->kind); }

protected:
  explicit TypeNode(NodeKind k, SourceRange r = {}) : AstNode(k, r) {}
};

/**
 * Base class for statements.
 */
class Stmt : public AstNode
{
public:
  static bool classof(const AstNode * node) { return is_stmt_kind(node->kind); }

protected:
  explicit Stmt(NodeKind k, SourceRange r = {}) : AstNode(k, r) {}
};

/**
 * Base class for declarations.
 */
class Decl : public AstNode
{
public:
  static bool classof(const AstNode * node) { return is_decl_kind(node->kind); }

protected:
  explicit Decl(NodeKind k, SourceRange r = {}) : AstNode(k, r) {}
};

// ============================================================================
// Expression Nodes
// ============================================================================

/// Integer literal expression.
class IntLiteralExpr : public NodeBase<IntLiteralExpr, Expr, NodeKind::IntLiteral>
{
public:
  int64_t value;

  explicit IntLiteralExpr(int64_t v, SourceRange r = {}) : NodeBase(r), value(v) {}
};

/// Float literal expression.
class FloatLiteralExpr : public NodeBase<FloatLiteralExpr, Expr, NodeKind::FloatLiteral>
{
public:
  double value;

  explicit FloatLiteralExpr(double v, SourceRange r = {}) : NodeBase(r), value(v) {}
};

/// String literal expression.
class StringLiteralExpr : public NodeBase<StringLiteralExpr, Expr, NodeKind::StringLiteral>
{
public:
  std::string_view value;

  explicit StringLiteralExpr(std::string_view v, SourceRange r = {}) : NodeBase(r), value(v) {}
};

/// Boolean literal expression.
class BoolLiteralExpr : public NodeBase<BoolLiteralExpr, Expr, NodeKind::BoolLiteral>
{
public:
  bool value;

  explicit BoolLiteralExpr(bool v, SourceRange r = {}) : NodeBase(r), value(v) {}
};

/// Null literal expression.
class NullLiteralExpr : public NodeBase<NullLiteralExpr, Expr, NodeKind::NullLiteral>
{
public:
  explicit NullLiteralExpr(SourceRange r = {}) : NodeBase(r) {}
};

/// Variable reference expression.
class VarRefExpr : public NodeBase<VarRefExpr, Expr, NodeKind::VarRef>
{
public:
  std::string_view name;

  /// Resolved symbol (set during NameResolver phase, nullptr before resolution)
  const Symbol * resolvedSymbol = nullptr;

  explicit VarRefExpr(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}
};

/// Missing expression (parser recovery placeholder).
class MissingExpr : public NodeBase<MissingExpr, Expr, NodeKind::MissingExpr>
{
public:
  explicit MissingExpr(SourceRange r = {}) : NodeBase(r) {}
};

/// Binary expression.
class BinaryExpr : public NodeBase<BinaryExpr, Expr, NodeKind::BinaryExpr>
{
public:
  Expr * lhs;
  BinaryOp op;
  Expr * rhs;

  BinaryExpr(Expr * l, BinaryOp o, Expr * r, SourceRange range = {})
  : NodeBase(range), lhs(l), op(o), rhs(r)
  {
  }
};

/// Unary expression.
class UnaryExpr : public NodeBase<UnaryExpr, Expr, NodeKind::UnaryExpr>
{
public:
  UnaryOp op;
  Expr * operand;

  UnaryExpr(UnaryOp o, Expr * e, SourceRange r = {}) : NodeBase(r), op(o), operand(e) {}
};

/// Cast expression: expr as type.
class CastExpr : public NodeBase<CastExpr, Expr, NodeKind::CastExpr>
{
public:
  Expr * expr;
  TypeNode * targetType;

  CastExpr(Expr * e, TypeNode * t, SourceRange r = {}) : NodeBase(r), expr(e), targetType(t) {}
};

/// Index expression: base[index].
class IndexExpr : public NodeBase<IndexExpr, Expr, NodeKind::IndexExpr>
{
public:
  Expr * base;
  Expr * index;

  IndexExpr(Expr * b, Expr * i, SourceRange r = {}) : NodeBase(r), base(b), index(i) {}
};

/// Array literal expression: [a, b, c]
class ArrayLiteralExpr : public NodeBase<ArrayLiteralExpr, Expr, NodeKind::ArrayLiteralExpr>
{
public:
  gsl::span<Expr *> elements;

  explicit ArrayLiteralExpr(SourceRange r = {}) : NodeBase(r) {}

  explicit ArrayLiteralExpr(gsl::span<Expr *> elems, SourceRange r = {})
  : NodeBase(r), elements(elems)
  {
  }
};

/// Array repeat expression: [value; count]
class ArrayRepeatExpr : public NodeBase<ArrayRepeatExpr, Expr, NodeKind::ArrayRepeatExpr>
{
public:
  Expr * value;
  Expr * count;

  ArrayRepeatExpr(Expr * val, Expr * cnt, SourceRange r = {}) : NodeBase(r), value(val), count(cnt)
  {
  }
};

/// Vec macro expression: vec![...]
class VecMacroExpr : public NodeBase<VecMacroExpr, Expr, NodeKind::VecMacroExpr>
{
public:
  Expr * inner;  ///< Either ArrayLiteralExpr or ArrayRepeatExpr

  explicit VecMacroExpr(Expr * arr, SourceRange r = {}) : NodeBase(r), inner(arr) {}
};

// ============================================================================
// Type Nodes
// ============================================================================

/// Type inference wildcard: _.
class InferType : public NodeBase<InferType, TypeNode, NodeKind::InferType>
{
public:
  explicit InferType(SourceRange r = {}) : NodeBase(r) {}
};

/// Primary type: identifier or string<N>.
class PrimaryType : public NodeBase<PrimaryType, TypeNode, NodeKind::PrimaryType>
{
public:
  std::string_view name;
  std::optional<std::string_view> size;  ///< For bounded string: string<N>

  /// Resolved type symbol (set during NameResolver phase, nullptr before resolution)
  const TypeSymbol * resolvedType = nullptr;

  explicit PrimaryType(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}

  PrimaryType(std::string_view n, std::string_view s, SourceRange r = {})
  : NodeBase(r), name(n), size(s)
  {
  }
};

/// Static array type: [T; N] or [T; <=N].
class StaticArrayType : public NodeBase<StaticArrayType, TypeNode, NodeKind::StaticArrayType>
{
public:
  TypeNode * elementType;
  std::string_view size;
  bool isBounded;  ///< true for [T; <=N]

  StaticArrayType(TypeNode * elem, std::string_view s, bool bounded, SourceRange r = {})
  : NodeBase(r), elementType(elem), size(s), isBounded(bounded)
  {
  }
};

/// Dynamic array type: vec<T>.
class DynamicArrayType : public NodeBase<DynamicArrayType, TypeNode, NodeKind::DynamicArrayType>
{
public:
  TypeNode * elementType;

  explicit DynamicArrayType(TypeNode * elem, SourceRange r = {}) : NodeBase(r), elementType(elem) {}
};

/// Complete type expression (base type with optional nullable suffix).
class TypeExpr : public NodeBase<TypeExpr, TypeNode, NodeKind::TypeExpr>
{
public:
  TypeNode * base;
  bool nullable = false;

  explicit TypeExpr(TypeNode * b, bool n = false, SourceRange r = {})
  : NodeBase(r), base(b), nullable(n)
  {
  }
};

// ============================================================================
// Supporting Nodes
// ============================================================================

/// Inline blackboard declaration: out var identifier.
class InlineBlackboardDecl
: public NodeBase<InlineBlackboardDecl, AstNode, NodeKind::InlineBlackboardDecl>
{
public:
  std::string_view name;

  explicit InlineBlackboardDecl(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}
};

/// Argument passed to a node call.
class Argument : public NodeBase<Argument, AstNode, NodeKind::Argument>
{
public:
  std::string_view name;
  std::optional<PortDirection> direction;
  // Either an expression or inline blackboard decl
  Expr * valueExpr = nullptr;
  InlineBlackboardDecl * inlineDecl = nullptr;

  Argument(std::string_view n, Expr * v, SourceRange r = {}) : NodeBase(r), name(n), valueExpr(v) {}

  Argument(std::string_view n, std::optional<PortDirection> dir, Expr * v, SourceRange r = {})
  : NodeBase(r), name(n), direction(dir), valueExpr(v)
  {
  }

  Argument(std::string_view n, InlineBlackboardDecl * decl, SourceRange r = {})
  : NodeBase(r), name(n), direction(PortDirection::Out), inlineDecl(decl)
  {
  }

  [[nodiscard]] bool is_inline_decl() const noexcept { return inlineDecl != nullptr; }
};

/// Precondition attached to a node call.
class Precondition : public NodeBase<Precondition, AstNode, NodeKind::Precondition>
{
public:
  PreconditionKind kind;
  Expr * condition;

  Precondition(PreconditionKind k, Expr * c, SourceRange r = {})
  : NodeBase(r), kind(k), condition(c)
  {
  }
};

/// Parameter declaration in a Tree definition.
class ParamDecl : public NodeBase<ParamDecl, AstNode, NodeKind::ParamDecl>
{
public:
  std::string_view name;
  std::optional<PortDirection> direction;
  TypeExpr * type;
  Expr * defaultValue = nullptr;

  ParamDecl(std::string_view n, TypeExpr * t, SourceRange r = {}) : NodeBase(r), name(n), type(t) {}

  ParamDecl(
    std::string_view n, std::optional<PortDirection> dir, TypeExpr * t, Expr * def,
    SourceRange r = {})
  : NodeBase(r), name(n), direction(dir), type(t), defaultValue(def)
  {
  }
};

/// Port declaration in an extern statement.
class ExternPort : public NodeBase<ExternPort, AstNode, NodeKind::ExternPort>
{
public:
  std::string_view name;
  std::optional<PortDirection> direction;
  TypeExpr * type;
  Expr * defaultValue = nullptr;
  gsl::span<std::string_view> docs;

  ExternPort(std::string_view n, TypeExpr * t, SourceRange r = {}) : NodeBase(r), name(n), type(t)
  {
  }

  ExternPort(
    std::string_view n, std::optional<PortDirection> dir, TypeExpr * t, Expr * def,
    SourceRange r = {})
  : NodeBase(r), name(n), direction(dir), type(t), defaultValue(def)
  {
  }
};

/// Behavior attribute for extern declarations.
class BehaviorAttr : public NodeBase<BehaviorAttr, AstNode, NodeKind::BehaviorAttr>
{
public:
  DataPolicy dataPolicy;
  std::optional<FlowPolicy> flowPolicy;

  explicit BehaviorAttr(DataPolicy dp, SourceRange r = {}) : NodeBase(r), dataPolicy(dp) {}

  BehaviorAttr(DataPolicy dp, std::optional<FlowPolicy> fp, SourceRange r = {})
  : NodeBase(r), dataPolicy(dp), flowPolicy(fp)
  {
  }
};

// ============================================================================
// Statement Nodes
// ============================================================================

/// Node statement (tree node invocation).
class NodeStmt : public NodeBase<NodeStmt, Stmt, NodeKind::NodeStmt>
{
public:
  std::string_view nodeName;
  gsl::span<Precondition *> preconditions;
  gsl::span<Argument *> args;
  bool hasPropertyBlock = false;
  bool hasChildrenBlock = false;
  gsl::span<Stmt *> children;
  gsl::span<std::string_view> docs;

  /// Resolved node symbol (set during NameResolver phase, nullptr before resolution)
  const NodeSymbol * resolvedNode = nullptr;

  /// Resolved block scope for children_block (set by SymbolTableBuilder, nullptr if no children)
  class Scope * resolvedBlockScope = nullptr;

  explicit NodeStmt(std::string_view name, SourceRange r = {}) : NodeBase(r), nodeName(name) {}
};

/// Assignment statement.
class AssignmentStmt : public NodeBase<AssignmentStmt, Stmt, NodeKind::AssignmentStmt>
{
public:
  gsl::span<Precondition *> preconditions;
  std::string_view target;
  gsl::span<Expr *> indices;
  AssignOp op;
  Expr * value;
  gsl::span<std::string_view> docs;

  /// Resolved symbol for assignment target (set during NameResolver phase)
  const Symbol * resolvedTarget = nullptr;

  AssignmentStmt(std::string_view t, AssignOp o, Expr * v, SourceRange r = {})
  : NodeBase(r), target(t), op(o), value(v)
  {
  }
};

/// Blackboard declaration statement (var).
class BlackboardDeclStmt : public NodeBase<BlackboardDeclStmt, Stmt, NodeKind::BlackboardDeclStmt>
{
public:
  std::string_view name;
  TypeExpr * type = nullptr;
  Expr * initialValue = nullptr;
  gsl::span<std::string_view> docs;

  explicit BlackboardDeclStmt(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}

  BlackboardDeclStmt(std::string_view n, TypeExpr * t, Expr * init, SourceRange r = {})
  : NodeBase(r), name(n), type(t), initialValue(init)
  {
  }
};

/// Const declaration statement.
class ConstDeclStmt : public NodeBase<ConstDeclStmt, Stmt, NodeKind::ConstDeclStmt>
{
public:
  std::string_view name;
  TypeExpr * type = nullptr;
  Expr * value;
  gsl::span<std::string_view> docs;

  /// Evaluated constant value (set by ConstEvaluator, nullptr before)
  const ConstValue * evaluatedValue = nullptr;

  ConstDeclStmt(std::string_view n, Expr * v, SourceRange r = {}) : NodeBase(r), name(n), value(v)
  {
  }

  ConstDeclStmt(std::string_view n, TypeExpr * t, Expr * v, SourceRange r = {})
  : NodeBase(r), name(n), type(t), value(v)
  {
  }
};

// ============================================================================
// Declaration Nodes
// ============================================================================

/// Import statement.
class ImportDecl : public NodeBase<ImportDecl, Decl, NodeKind::ImportDecl>
{
public:
  std::string_view path;  ///< Import path (arena-interned string)

  explicit ImportDecl(std::string_view p, SourceRange r = {}) : NodeBase(r), path(p) {}

  /// Get the path as a string_view
  [[nodiscard]] std::string_view path_string() const noexcept { return path; }
};

/// Extern node declaration.
class ExternDecl : public NodeBase<ExternDecl, Decl, NodeKind::ExternDecl>
{
public:
  ExternNodeCategory category;
  std::string_view name;
  gsl::span<ExternPort *> ports;
  gsl::span<std::string_view> docs;
  BehaviorAttr * behaviorAttr = nullptr;

  ExternDecl(ExternNodeCategory cat, std::string_view n, SourceRange r = {})
  : NodeBase(r), category(cat), name(n)
  {
  }
};

/// Extern type declaration.
class ExternTypeDecl : public NodeBase<ExternTypeDecl, Decl, NodeKind::ExternTypeDecl>
{
public:
  std::string_view name;
  gsl::span<std::string_view> docs;

  explicit ExternTypeDecl(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}
};

/// Type alias declaration.
class TypeAliasDecl : public NodeBase<TypeAliasDecl, Decl, NodeKind::TypeAliasDecl>
{
public:
  std::string_view name;
  TypeExpr * aliasedType;
  gsl::span<std::string_view> docs;

  TypeAliasDecl(std::string_view n, TypeExpr * t, SourceRange r = {})
  : NodeBase(r), name(n), aliasedType(t)
  {
  }
};

/// Global variable declaration.
class GlobalVarDecl : public NodeBase<GlobalVarDecl, Decl, NodeKind::GlobalVarDecl>
{
public:
  std::string_view name;
  TypeExpr * type = nullptr;
  Expr * initialValue = nullptr;
  gsl::span<std::string_view> docs;

  explicit GlobalVarDecl(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}

  GlobalVarDecl(std::string_view n, TypeExpr * t, Expr * init, SourceRange r = {})
  : NodeBase(r), name(n), type(t), initialValue(init)
  {
  }
};

/// Global const declaration.
class GlobalConstDecl : public NodeBase<GlobalConstDecl, Decl, NodeKind::GlobalConstDecl>
{
public:
  std::string_view name;
  TypeExpr * type = nullptr;
  Expr * value;
  gsl::span<std::string_view> docs;

  /// Evaluated constant value (set by ConstEvaluator, nullptr before)
  const ConstValue * evaluatedValue = nullptr;

  GlobalConstDecl(std::string_view n, Expr * v, SourceRange r = {}) : NodeBase(r), name(n), value(v)
  {
  }

  GlobalConstDecl(std::string_view n, TypeExpr * t, Expr * v, SourceRange r = {})
  : NodeBase(r), name(n), type(t), value(v)
  {
  }
};

/// Tree definition.
class TreeDecl : public NodeBase<TreeDecl, Decl, NodeKind::TreeDecl>
{
public:
  std::string_view name;
  gsl::span<ParamDecl *> params;
  gsl::span<Stmt *> body;
  gsl::span<std::string_view> docs;

  explicit TreeDecl(std::string_view n, SourceRange r = {}) : NodeBase(r), name(n) {}
};

// ============================================================================
// Program (Root Node)
// ============================================================================

/// Program (root AST node).
class Program : public NodeBase<Program, AstNode, NodeKind::Program>
{
public:
  gsl::span<std::string_view> innerDocs;
  gsl::span<ImportDecl *> imports;
  gsl::span<ExternTypeDecl *> externTypes;
  gsl::span<TypeAliasDecl *> typeAliases;
  gsl::span<ExternDecl *> externs;
  gsl::span<GlobalVarDecl *> globalVars;
  gsl::span<GlobalConstDecl *> globalConsts;
  gsl::span<TreeDecl *> trees;

  explicit Program(SourceRange r = {}) : NodeBase(r) {}
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Get the SourceRange from any AST node.
 */
[[nodiscard]] inline SourceRange get_range(const AstNode * node) noexcept
{
  return node ? node->get_range() : SourceRange{};
}

}  // namespace bt_dsl
