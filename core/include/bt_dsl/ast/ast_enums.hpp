// bt_dsl/ast/ast_enums.hpp - AST enumeration definitions
//
// This header contains all enumeration types used in the BT-DSL AST,
// including node kinds, operators, and semantic attributes.
//
#pragma once

#include <cstdint>
#include <string_view>

namespace bt_dsl
{

// ============================================================================
// NodeKind - Identifies all AST node types
// ============================================================================

/**
 * Node kind enumeration for LLVM-style RTTI.
 * Nodes are grouped by category for efficient range-based classof checks.
 * Auto-generated from ast_nodes.def.
 */
enum class NodeKind : uint8_t {
// === Expressions ===
#define AST_NODE_EXPR(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"

// === Types ===
#define AST_NODE_TYPE(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"

// === Statements ===
#define AST_NODE_STMT(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"

// === Declarations ===
#define AST_NODE_DECL(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"

// === Supporting nodes ===
#define AST_NODE_SUPPORT(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"

// === Top-level ===
#define AST_NODE_TOP(Class, Kind, Snake) Kind,
#include "bt_dsl/ast/ast_nodes.def"
};

// ============================================================================
// PortDirection - Parameter/port direction specifiers
// ============================================================================

/**
 * Port/parameter direction.
 * Reference: syntax.md 2.6.1 port_direction
 */
enum class PortDirection : uint8_t {
  In,   ///< Input (read-only, snapshot semantics)
  Out,  ///< Output (write-only)
  Ref,  ///< View (live read-only reference)
  Mut   ///< State (live read/write reference)
};

// ============================================================================
// Operators
// ============================================================================

/**
 * Binary operators.
 * Reference: syntax.md 2.4.2
 */
enum class BinaryOp : uint8_t {
  // Arithmetic
  Add,  ///< +
  Sub,  ///< -
  Mul,  ///< *
  Div,  ///< /
  Mod,  ///< %
  // Comparison
  Eq,  ///< ==
  Ne,  ///< !=
  Lt,  ///< <
  Le,  ///< <=
  Gt,  ///< >
  Ge,  ///< >=
  // Logical
  And,  ///< &&
  Or,   ///< ||
  // Bitwise
  BitAnd,  ///< &
  BitXor,  ///< ^
  BitOr,   ///< |
};

/**
 * Unary operators.
 * Reference: syntax.md 2.4.1
 */
enum class UnaryOp : uint8_t {
  Not,  ///< !
  Neg,  ///< -
};

/**
 * Assignment operators.
 * Reference: syntax.md 2.5 assignment_op
 */
enum class AssignOp : uint8_t {
  Assign,     ///< =
  AddAssign,  ///< +=
  SubAssign,  ///< -=
  MulAssign,  ///< *=
  DivAssign,  ///< /=
  ModAssign,  ///< %=
};

// ============================================================================
// Node Categories
// ============================================================================

/**
 * Extern node category.
 * Reference: syntax.md 2.6.1 extern_def
 */
enum class ExternNodeCategory : uint8_t { Action, Condition, Control, Decorator, Subtree };

/**
 * Precondition kind.
 * Reference: syntax.md 2.6.4 precond_kind
 */
enum class PreconditionKind : uint8_t {
  SuccessIf,  ///< @success_if
  FailureIf,  ///< @failure_if
  SkipIf,     ///< @skip_if
  RunWhile,   ///< @run_while
  Guard       ///< @guard
};

// ============================================================================
// Behavior Attributes
// ============================================================================

/**
 * Data policy for behavior attribute.
 * Reference: syntax.md 2.6.1 data_policy
 */
enum class DataPolicy : uint8_t {
  All,  ///< Requires all ports to be connected
  Any,  ///< Allows partial port connection
  None  ///< No data requirements
};

/**
 * Flow policy for behavior attribute.
 * Reference: syntax.md 2.6.1 flow_policy
 */
enum class FlowPolicy : uint8_t {
  Chained,  ///< Sequential execution flow
  Isolated  ///< Independent execution
};

// ============================================================================
// to_string() Helper Functions
// ============================================================================

[[nodiscard]] constexpr std::string_view to_string(PortDirection dir) noexcept
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

[[nodiscard]] constexpr std::string_view to_string(BinaryOp op) noexcept
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
    case BinaryOp::BitXor:
      return "^";
    case BinaryOp::BitOr:
      return "|";
  }
  return "";
}

[[nodiscard]] constexpr std::string_view to_string(UnaryOp op) noexcept
{
  switch (op) {
    case UnaryOp::Not:
      return "!";
    case UnaryOp::Neg:
      return "-";
  }
  return "";
}

[[nodiscard]] constexpr std::string_view to_string(AssignOp op) noexcept
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

[[nodiscard]] constexpr std::string_view to_string(ExternNodeCategory category) noexcept
{
  switch (category) {
    case ExternNodeCategory::Action:
      return "action";
    case ExternNodeCategory::Condition:
      return "condition";
    case ExternNodeCategory::Control:
      return "control";
    case ExternNodeCategory::Decorator:
      return "decorator";
    case ExternNodeCategory::Subtree:
      return "subtree";
  }
  return "";
}

[[nodiscard]] constexpr std::string_view to_string(PreconditionKind kind) noexcept
{
  switch (kind) {
    case PreconditionKind::SuccessIf:
      return "success_if";
    case PreconditionKind::FailureIf:
      return "failure_if";
    case PreconditionKind::SkipIf:
      return "skip_if";
    case PreconditionKind::RunWhile:
      return "run_while";
    case PreconditionKind::Guard:
      return "guard";
  }
  return "";
}

[[nodiscard]] constexpr std::string_view to_string(DataPolicy policy) noexcept
{
  switch (policy) {
    case DataPolicy::All:
      return "All";
    case DataPolicy::Any:
      return "Any";
    case DataPolicy::None:
      return "None";
  }
  return "";
}

[[nodiscard]] constexpr std::string_view to_string(FlowPolicy policy) noexcept
{
  switch (policy) {
    case FlowPolicy::Chained:
      return "Chained";
    case FlowPolicy::Isolated:
      return "Isolated";
  }
  return "";
}

// ============================================================================
// NodeKind Range Helpers
// ============================================================================

namespace detail
{

/// First expression node kind
inline constexpr NodeKind k_first_expr_kind = NodeKind::IntLiteral;
/// Last expression node kind
inline constexpr NodeKind k_last_expr_kind = NodeKind::VecMacroExpr;

/// First type node kind
inline constexpr NodeKind k_first_type_kind = NodeKind::InferType;
/// Last type node kind
inline constexpr NodeKind k_last_type_kind = NodeKind::TypeExpr;

/// First statement node kind
inline constexpr NodeKind k_first_stmt_kind = NodeKind::NodeStmt;
/// Last statement node kind
inline constexpr NodeKind k_last_stmt_kind = NodeKind::ConstDeclStmt;

/// First declaration node kind
inline constexpr NodeKind k_first_decl_kind = NodeKind::ImportDecl;
/// Last declaration node kind
inline constexpr NodeKind k_last_decl_kind = NodeKind::TreeDecl;

}  // namespace detail

/// Check if a NodeKind is an expression
[[nodiscard]] constexpr bool is_expr_kind(NodeKind kind) noexcept
{
  return kind >= detail::k_first_expr_kind && kind <= detail::k_last_expr_kind;
}

/// Check if a NodeKind is a type
[[nodiscard]] constexpr bool is_type_kind(NodeKind kind) noexcept
{
  return kind >= detail::k_first_type_kind && kind <= detail::k_last_type_kind;
}

/// Check if a NodeKind is a statement
[[nodiscard]] constexpr bool is_stmt_kind(NodeKind kind) noexcept
{
  return kind >= detail::k_first_stmt_kind && kind <= detail::k_last_stmt_kind;
}

/// Check if a NodeKind is a declaration
[[nodiscard]] constexpr bool is_decl_kind(NodeKind kind) noexcept
{
  return kind >= detail::k_first_decl_kind && kind <= detail::k_last_decl_kind;
}

}  // namespace bt_dsl
