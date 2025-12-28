// bt_dsl/parser.hpp - Parser interface for BT-DSL
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bt_dsl/core/ast.hpp"

namespace bt_dsl
{

// ============================================================================
// Error Types
// ============================================================================

/**
 * Parse error information.
 */
struct ParseError
{
  std::string message;
  SourceRange range;

  // Error severity
  enum class Severity { Error, Warning, Info } severity = Severity::Error;
};

// ============================================================================
// Result Type (C++17 compatible)
// ============================================================================

/**
 * Parse result type using std::variant.
 * Holds either a success value T or an error vector.
 */
template <typename T>
class ParseResult
{
public:
  using ValueType = T;
  using ErrorType = std::vector<ParseError>;

  // Construct with success value
  ParseResult(T value) : data_(std::move(value)) {}

  // Construct with errors
  ParseResult(std::vector<ParseError> errors) : data_(std::move(errors)) {}

  // Check if result contains a value
  [[nodiscard]] bool has_value() const { return std::holds_alternative<T>(data_); }

  // Check if result contains errors
  [[nodiscard]] bool has_error() const { return std::holds_alternative<ErrorType>(data_); }

  // Conversion to bool (true if has value)
  explicit operator bool() const { return has_value(); }

  // Get the value (undefined behavior if has_error())
  T & value() & { return std::get<T>(data_); }
  [[nodiscard]] const T & value() const & { return std::get<T>(data_); }
  T && value() && { return std::get<T>(std::move(data_)); }

  // Get errors (undefined behavior if has_value())
  ErrorType & error() & { return std::get<ErrorType>(data_); }
  [[nodiscard]] const ErrorType & error() const & { return std::get<ErrorType>(data_); }
  ErrorType && error() && { return std::get<ErrorType>(std::move(data_)); }

  // Pointer-like access
  T * operator->() { return &value(); }
  const T * operator->() const { return &value(); }
  T & operator*() & { return value(); }
  const T & operator*() const & { return value(); }

private:
  std::variant<T, ErrorType> data_;
};

// ============================================================================
// Parser Class
// ============================================================================

/**
 * Parser for BT-DSL source code.
 *
 * Uses Tree-sitter to parse source code and convert CST to AST.
 *
 * Example usage:
 * @code
 *     bt_dsl::Parser parser;
 *     auto result = parser.parse(source_code);
 *     if (result) {
 *         // Use result.value() or *result
 *     } else {
 *         // Handle result.error()
 *     }
 * @endcode
 */
class Parser
{
public:
  Parser();
  ~Parser();

  // Non-copyable
  Parser(const Parser &) = delete;
  Parser & operator=(const Parser &) = delete;

  // Movable
  Parser(Parser &&) noexcept;
  Parser & operator=(Parser &&) noexcept;

  /**
   * Parse BT-DSL source code into an AST.
   *
   * @param source The source code to parse
   * @return ParseResult containing either the Program AST or parse errors
   */
  ParseResult<Program> parse(std::string_view source);

  /**
   * Parse BT-DSL source code, returning partial AST even with errors.
   *
   * This is useful for IDE features where partial results are better than none.
   *
   * @param source The source code to parse
   * @return Tuple of (partial Program, errors)
   */
  std::pair<Program, std::vector<ParseError>> parse_with_recovery(std::string_view source);

  /**
   * Reset parser state.
   */
  void reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bt_dsl
