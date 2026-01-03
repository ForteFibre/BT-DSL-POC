// bt_dsl/basic/diagnostic.hpp - Diagnostic types for parsing/sema
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

// ============================================================================
// Diagnostic Types
// ============================================================================

/**
 * Severity level for diagnostics.
 */
enum class Severity : uint8_t {
  Error,    // Fatal errors that prevent code generation
  Warning,  // Potential issues that don't prevent compilation
  Info,     // Informational messages
  Hint,     // Suggestions for improvement
};

/**
 * A single diagnostic message.
 */
struct Diagnostic
{
  std::string message;
  SourceRange range;
  Severity severity = Severity::Error;
  std::string code;  // Optional error code, e.g., "E001", "W002"

  // Factory methods for common severities
  static Diagnostic error(SourceRange range, std::string message, std::string code = "");
  static Diagnostic warning(SourceRange range, std::string message, std::string code = "");
  static Diagnostic info(SourceRange range, std::string message, std::string code = "");
  static Diagnostic hint(SourceRange range, std::string message, std::string code = "");
};

// ============================================================================
// Diagnostic Collection
// ============================================================================

/**
 * Collection of diagnostics with convenience methods.
 *
 * Used to accumulate errors and warnings during semantic analysis.
 */
class DiagnosticBag
{
public:
  DiagnosticBag() = default;

  // Add a diagnostic
  void add(Diagnostic diag);

  // Convenience methods to add diagnostics
  void error(SourceRange range, std::string_view message, std::string_view code = "");
  void warning(SourceRange range, std::string_view message, std::string_view code = "");
  void info(SourceRange range, std::string_view message, std::string_view code = "");
  void hint(SourceRange range, std::string_view message, std::string_view code = "");

  // Query methods
  [[nodiscard]] bool has_errors() const;
  [[nodiscard]] bool has_warnings() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] size_t size() const;

  // Access diagnostics
  [[nodiscard]] const std::vector<Diagnostic> & all() const;
  [[nodiscard]] std::vector<Diagnostic> errors() const;
  [[nodiscard]] std::vector<Diagnostic> warnings() const;

  // Merge another bag into this one
  void merge(const DiagnosticBag & other);
  void merge(DiagnosticBag && other);

  // Iteration support
  [[nodiscard]] auto begin() const { return diagnostics_.begin(); }
  [[nodiscard]] auto end() const { return diagnostics_.end(); }

private:
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace bt_dsl
