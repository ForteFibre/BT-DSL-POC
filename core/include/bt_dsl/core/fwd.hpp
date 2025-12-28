// bt_dsl/fwd.hpp - Forward declarations for BT-DSL
#pragma once

#include <cstdint>

namespace bt_dsl
{

// Utility types
template <typename T>
class Box;
struct SourceRange;

// Enums
enum class PortDirection : uint8_t;
enum class BinaryOp : uint8_t;
enum class UnaryOp : uint8_t;
enum class AssignOp : uint8_t;

// Literal types
struct StringLiteral;
struct IntLiteral;
struct FloatLiteral;
struct BoolLiteral;

// Expression types
struct VarRef;
struct BinaryExpr;
struct UnaryExpr;

// Statement types
struct ImportStmt;
struct DeclarePort;
struct DeclareStmt;
struct GlobalVarDecl;
struct LocalVarDecl;
struct ParamDecl;

// Value expression types
struct BlackboardRef;

// Tree structure types
struct Argument;
struct Decorator;
struct AssignmentStmt;
struct NodeStmt;
struct TreeDef;

// Root type
struct Program;

// Parser types
struct ParseError;
class Parser;

}  // namespace bt_dsl
