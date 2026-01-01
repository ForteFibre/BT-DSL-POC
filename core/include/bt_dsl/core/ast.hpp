// bt_dsl/ast.hpp - Modern C++23 AST definitions for BT-DSL
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bt_dsl
{

// ============================================================================
// Utility Types
// ============================================================================

/**
 * Wrapper for recursive types in std::variant.
 * Provides pointer semantics with value-like construction.
 */
template <typename T>
class Box
{
public:
  Box() : ptr_(std::make_unique<T>()) {}
  Box(T value) : ptr_(std::make_unique<T>(std::move(value))) {}
  Box(const Box & other) : ptr_(std::make_unique<T>(*other.ptr_)) {}
  Box(Box &&) noexcept = default;

  Box & operator=(const Box & other)
  {
    if (this != &other) {
      ptr_ = std::make_unique<T>(*other.ptr_);
    }
    return *this;
  }
  Box & operator=(Box &&) noexcept = default;

  T & operator*() { return *ptr_; }
  const T & operator*() const { return *ptr_; }
  T * operator->() { return ptr_.get(); }
  const T * operator->() const { return ptr_.get(); }
  T * get() { return ptr_.get(); }
  [[nodiscard]] const T * get() const { return ptr_.get(); }

private:
  std::unique_ptr<T> ptr_;
};

/**
 * Source location information for AST nodes.
 */
struct SourceRange
{
  uint32_t start_line = 0;
  uint32_t start_column = 0;
  uint32_t end_line = 0;
  uint32_t end_column = 0;
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;
};

// ============================================================================
// Enums
// ============================================================================

/**
 * Port/parameter direction.
 */
enum class PortDirection : uint8_t {
  In,   // Input (read-only)
  Out,  // Output (write-only)
  Ref,  // View (live read-only)
  Mut   // State (live read/write)
};

/**
 * Binary operators.
 */
enum class BinaryOp : uint8_t {
  // Arithmetic
  Add,  // +
  Sub,  // -
  Mul,  // *
  Div,  // /
  Mod,  // %
  // Comparison
  Eq,  // ==
  Ne,  // !=
  Lt,  // <
  Le,  // <=
  Gt,  // >
  Ge,  // >=
  // Logical
  And,  // &&
  Or,   // ||
  // Bitwise
  BitAnd,  // &
  BitOr,   // |
  BitXor,  // ^
};

/**
 * Unary operators.
 */
enum class UnaryOp : uint8_t {
  Not,  // !
  Neg,  // -
};

/**
 * Assignment operators.
 */
enum class AssignOp : uint8_t {
  Assign,     // =
  AddAssign,  // +=
  SubAssign,  // -=
  MulAssign,  // *=
  DivAssign,  // /=
  ModAssign,  // %=
};

// ============================================================================
// Literal Types
// ============================================================================

struct StringLiteral
{
  std::string value;
  SourceRange range;
};

struct IntLiteral
{
  int64_t value;
  SourceRange range;
};

struct FloatLiteral
{
  double value;
  SourceRange range;
};

struct BoolLiteral
{
  bool value;
  SourceRange range;
};

struct NullLiteral
{
  SourceRange range;
};

using Literal = std::variant<StringLiteral, IntLiteral, FloatLiteral, BoolLiteral, NullLiteral>;

// ============================================================================
// Expression Types
// ============================================================================

// Forward declarations for recursive types
struct BinaryExpr;
struct UnaryExpr;
struct CastExpr;
struct IndexExpr;
struct ArrayLiteralExpr;
struct VecMacroExpr;

/**
 * Placeholder expression used by parser recovery when a syntactically required
 * expression is missing.
 *
 * This should not appear in successfully parsed programs (i.e. when parse
 * diagnostics are empty).
 */
struct MissingExpr
{
  SourceRange range;
};

/**
 * Variable reference with optional direction.
 */
struct VarRef
{
  std::string name;
  // Optional direction marker used in argument expressions.
  // NOTE: This is *not* part of general expression syntax, but is used
  // by the DSL to disambiguate port intent in argument passing.
  std::optional<PortDirection> direction;
  SourceRange range;
};

/**
 * Expression node - can be a literal, variable reference, or compound
 * expression.
 */
using Expression = std::variant<
  Literal, VarRef, MissingExpr, Box<BinaryExpr>, Box<UnaryExpr>, Box<CastExpr>, Box<IndexExpr>,
  Box<ArrayLiteralExpr>, Box<VecMacroExpr>>;

/**
 * Binary expression: left op right.
 */
struct BinaryExpr
{
  Expression left;
  BinaryOp op;
  Expression right;
  SourceRange range;
};

/**
 * Unary expression: op operand.
 */
struct UnaryExpr
{
  UnaryOp op;
  Expression operand;
  SourceRange range;
};

/**
 * Cast expression: expr as type.
 * NOTE: type is currently stored as source text.
 */
struct CastExpr
{
  Expression expr;
  std::string type_name;
  SourceRange range;
};

/**
 * Index expression: base[index]
 */
struct IndexExpr
{
  Expression base;
  Expression index;
  SourceRange range;
};

/**
 * Array literal: [a, b, c] or repeat-init form [value; count]
 */
struct ArrayLiteralExpr
{
  // If repeat_init is used, elements is empty.
  std::vector<Expression> elements;

  // repeat_init := value ; count
  std::optional<Expression> repeat_value;
  std::optional<Expression> repeat_count;

  SourceRange range;
};

/**
 * vec! macro: vec![...]
 */
struct VecMacroExpr
{
  ArrayLiteralExpr value;
  SourceRange range;
};

// ============================================================================
// Statement Types
// ============================================================================

/**
 * Import statement.
 */
struct ImportStmt
{
  std::string path;
  SourceRange range;
};

/**
 * Port declaration in a declare statement.
 */
struct DeclarePort
{
  std::string name;
  std::optional<PortDirection> direction;
  std::string type_name;
  std::optional<Expression> default_value;
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * Declare statement for external nodes.
 * Example: declare Action MyAction(in target: string)
 */
struct DeclareStmt
{
  std::string category;  // "Action", "Condition", "Control", "Decorator", "SubTree"
  std::string name;
  std::vector<DeclarePort> ports;
  std::vector<std::string> docs;
  // Optional behavior attribute: #[behavior(DataPolicy[, FlowPolicy])]
  // When omitted, defaults are All + Chained.
  std::optional<std::string> data_policy;
  std::optional<std::string> flow_policy;
  SourceRange range;
};

/**
 * extern type statement.
 */
struct ExternTypeStmt
{
  std::string name;
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * type alias statement.
 * NOTE: The aliased type is currently stored as source text.
 */
struct TypeAliasStmt
{
  std::string name;
  std::string value;
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * Global variable declaration.
 */
struct GlobalVarDecl
{
  std::string name;
  std::optional<std::string> type_name;
  std::optional<Expression> initial_value;
  // Outer doc comments (///) attached to this declaration.
  // Reference: docs/reference/lexical-structure.md 1.2.3, docs/reference/syntax.md 2.6.2
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * Local variable declaration within a Tree.
 */
struct LocalVarDecl
{
  std::string name;
  std::optional<std::string> type_name;
  std::optional<Expression> initial_value;
  SourceRange range;
};

/**
 * Parameter declaration in a Tree definition.
 */
struct ParamDecl
{
  std::string name;
  std::optional<PortDirection> direction;
  std::string type_name;
  std::optional<Expression> default_value;
  SourceRange range;
};

// ============================================================================
// Value Expression (used in arguments)
// ============================================================================

/**
 * Inline Blackboard declaration used in argument_expr:
 *   out var identifier
 */
struct InlineBlackboardDecl
{
  std::string name;
  SourceRange range;
};

/**
 * Value used in node arguments.
 *
 * Spec:
 *   argument_expr := [port_direction] expression
 *                 |  'out' inline_blackboard_decl
 */
using ArgumentValue = std::variant<Expression, InlineBlackboardDecl>;

// ============================================================================
// Tree Structure
// ============================================================================

/**
 * Argument passed to a node call.
 */
struct Argument
{
  std::optional<std::string> name;  // Positional if nullopt
  // Optional direction prefix in argument_expr.
  // For inline decl form (out var x), this is always Out.
  std::optional<PortDirection> direction;

  ArgumentValue value;
  SourceRange range;
};

/**
 * Precondition attached to a node call.
 */
struct Precondition
{
  std::string kind;  // success_if | failure_if | skip_if | run_while | guard
  Expression condition;
  SourceRange range;
};

/**
 * Assignment statement within a children block.
 */
struct AssignmentStmt
{
  // Preconditions attached to this assignment statement.
  // Reference: docs/reference/execution-model.md 5.3.3 and syntax.md (statement forms)
  std::vector<Precondition> preconditions;

  // lvalue := identifier { index_suffix }
  std::string target;
  std::vector<Expression> indices;
  AssignOp op;
  Expression value;

  // Outer doc comments (///) attached to this statement.
  // Reference: docs/reference/lexical-structure.md 1.2.3
  std::vector<std::string> docs;

  SourceRange range;
};

/**
 * Blackboard declaration statement (`var`).
 */
struct BlackboardDeclStmt
{
  std::string name;
  std::optional<std::string> type_name;
  std::optional<Expression> initial_value;
  // Outer doc comments (///) attached to this declaration.
  // Reference: docs/reference/lexical-structure.md 1.2.3, docs/reference/syntax.md 2.6.3
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * Local const declaration statement (`const`).
 * NOTE: const_expr is currently represented as Expression.
 */
struct ConstDeclStmt
{
  std::string name;
  std::optional<std::string> type_name;
  Expression value;
  // Outer doc comments (///) attached to this declaration.
  // Reference: docs/reference/lexical-structure.md 1.2.3, docs/reference/syntax.md 2.6.2/2.6.3
  std::vector<std::string> docs;
  SourceRange range;
};

// Forward declaration
struct NodeStmt;

/**
 * Statement inside tree bodies and children blocks.
 */
using Statement = std::variant<Box<NodeStmt>, AssignmentStmt, BlackboardDeclStmt, ConstDeclStmt>;

/**
 * Node statement (tree node invocation).
 */
struct NodeStmt
{
  std::string node_name;
  std::vector<Precondition> preconditions;
  std::vector<Argument> args;

  // True if the source had an explicit property_block `(...)`.
  // Leaf node calls always have this, but compound calls may omit it.
  bool has_property_block = false;

  // True if the source had an explicit children block `{ ... }`, even if
  // it's empty. This allows semantic validation to distinguish `Node()` from
  // `Node {}`.
  bool has_children_block = false;

  std::vector<Statement> children;
  std::vector<std::string> docs;
  SourceRange range;
};

/**
 * Tree definition.
 */
struct TreeDef
{
  std::string name;
  std::vector<ParamDecl> params;
  std::vector<Statement> body;
  std::vector<std::string> docs;
  SourceRange range;
};

// ============================================================================
// Program (Root Node)
// ============================================================================

/**
 * Program (root AST node).
 */
struct Program
{
  std::vector<std::string> inner_docs;
  std::vector<ImportStmt> imports;

  std::vector<ExternTypeStmt> extern_types;
  std::vector<TypeAliasStmt> type_aliases;

  std::vector<DeclareStmt> declarations;

  // Global declarations
  std::vector<GlobalVarDecl> global_vars;
  std::vector<ConstDeclStmt> global_consts;

  std::vector<TreeDef> trees;
  SourceRange range;
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Convert PortDirection to string.
 */
constexpr std::string_view to_string(PortDirection dir)
{
  switch (dir) {
    case PortDirection::In:
      return "in";
    case PortDirection::Out:
      return "out";
    case PortDirection::Ref:
      return "ref";
    case PortDirection::Mut:
      return "mut";
  }
  return "";
}

/**
 * Convert BinaryOp to string.
 */
constexpr std::string_view to_string(BinaryOp op)
{
  switch (op) {
    case BinaryOp::Add:
      return "+";
    case BinaryOp::Sub:
      return "-";
    case BinaryOp::Mul:
      return "*";
    case BinaryOp::Div:
      return "/";
    case BinaryOp::Mod:
      return "%";
    case BinaryOp::Eq:
      return "==";
    case BinaryOp::Ne:
      return "!=";
    case BinaryOp::Lt:
      return "<";
    case BinaryOp::Le:
      return "<=";
    case BinaryOp::Gt:
      return ">";
    case BinaryOp::Ge:
      return ">=";
    case BinaryOp::And:
      return "&&";
    case BinaryOp::Or:
      return "||";
    case BinaryOp::BitAnd:
      return "&";
    case BinaryOp::BitOr:
      return "|";
    case BinaryOp::BitXor:
      return "^";
  }
  return "";
}

/**
 * Convert UnaryOp to string.
 */
constexpr std::string_view to_string(UnaryOp op)
{
  switch (op) {
    case UnaryOp::Not:
      return "!";
    case UnaryOp::Neg:
      return "-";
  }
  return "";
}

/**
 * Convert AssignOp to string.
 */
constexpr std::string_view to_string(AssignOp op)
{
  switch (op) {
    case AssignOp::Assign:
      return "=";
    case AssignOp::AddAssign:
      return "+=";
    case AssignOp::SubAssign:
      return "-=";
    case AssignOp::MulAssign:
      return "*=";
    case AssignOp::DivAssign:
      return "/=";
    case AssignOp::ModAssign:
      return "%=";
  }
  return "";
}

/**
 * Get the SourceRange of any expression.
 */
inline SourceRange get_range(const Expression & expr)
{
  return std::visit(
    [](const auto & e) -> SourceRange {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, Literal>) {
        return std::visit([](const auto & lit) { return lit.range; }, e);
      } else if constexpr (std::is_same_v<T, VarRef> || std::is_same_v<T, MissingExpr>) {
        return e.range;
      } else if constexpr (
        std::is_same_v<T, Box<BinaryExpr>> || std::is_same_v<T, Box<UnaryExpr>> ||
        std::is_same_v<T, Box<CastExpr>> || std::is_same_v<T, Box<IndexExpr>> ||
        std::is_same_v<T, Box<ArrayLiteralExpr>> || std::is_same_v<T, Box<VecMacroExpr>>) {
        return e->range;
      } else {
        return {};
      }
    },
    expr);
}

/**
 * Get the SourceRange of a literal.
 */
inline SourceRange get_range(const Literal & lit)
{
  return std::visit([](const auto & l) { return l.range; }, lit);
}

}  // namespace bt_dsl
