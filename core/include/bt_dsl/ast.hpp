// bt_dsl/ast.hpp - Modern C++23 AST definitions for BT-DSL
#pragma once

#include <cstdint>
#include <expected>
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
  Ref   // Reference (read-write)
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

using Literal = std::variant<StringLiteral, IntLiteral, FloatLiteral, BoolLiteral>;

// ============================================================================
// Expression Types
// ============================================================================

// Forward declarations for recursive types
struct BinaryExpr;
struct UnaryExpr;

/**
 * Variable reference with optional direction.
 */
struct VarRef
{
  std::string name;
  std::optional<PortDirection> direction;
  SourceRange range;
};

/**
 * Expression node - can be a literal, variable reference, or compound
 * expression.
 */
using Expression = std::variant<Literal, VarRef, Box<BinaryExpr>, Box<UnaryExpr>>;

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
  SourceRange range;
};

/**
 * Global variable declaration.
 */
struct GlobalVarDecl
{
  std::string name;
  std::string type_name;
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
  std::optional<std::string> type_name;
  SourceRange range;
};

// ============================================================================
// Value Expression (used in arguments)
// ============================================================================

/**
 * Blackboard reference in an argument.
 */
struct BlackboardRef
{
  std::string name;
  std::optional<PortDirection> direction;
  SourceRange range;
};

/**
 * Value expression for node arguments - can be a literal or blackboard
 * reference.
 */
using ValueExpr = std::variant<Literal, BlackboardRef>;

// ============================================================================
// Tree Structure
// ============================================================================

/**
 * Argument passed to a node call.
 */
struct Argument
{
  std::optional<std::string> name;  // Positional if nullopt
  ValueExpr value;
  SourceRange range;
};

/**
 * Decorator attached to a node.
 */
struct Decorator
{
  std::string name;
  std::vector<Argument> args;
  SourceRange range;
};

/**
 * Assignment statement within a children block.
 */
struct AssignmentStmt
{
  std::string target;
  AssignOp op;
  Expression value;
  SourceRange range;
};

// Forward declaration
struct NodeStmt;

/**
 * Child element in a children block - can be a node or assignment.
 */
using ChildElement = std::variant<Box<NodeStmt>, AssignmentStmt>;

/**
 * Node statement (tree node invocation).
 */
struct NodeStmt
{
  std::string node_name;
  std::vector<Decorator> decorators;
  std::vector<Argument> args;
  // True if the source had an explicit children block `{ ... }`, even if
  // it's empty. This allows semantic validation to distinguish `Node()` from
  // `Node {}`.
  bool has_children_block = false;
  std::vector<ChildElement> children;
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
  std::vector<LocalVarDecl> local_vars;
  std::optional<NodeStmt> body;
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
  std::vector<DeclareStmt> declarations;
  std::vector<GlobalVarDecl> global_vars;
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
      } else if constexpr (std::is_same_v<T, VarRef>) {
        return e.range;
      } else if constexpr (
        std::is_same_v<T, Box<BinaryExpr>> || std::is_same_v<T, Box<UnaryExpr>>) {
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
