// bt_dsl/basic/diagnostic_printer.hpp
//
// Prints diagnostics with source context, line/column information,
// and position markers in Rust-style format.
//
#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

/**
 * Prints diagnostics in Rust-style format.
 *
 * Produces output like:
 *   error[E0001]: undefined variable 'foo'
 *     --> src/main.bt:5:12
 *      |
 *    5 | let x = foo + 1;
 *      |         ^^^ not found in this scope
 *      |
 *      = help: consider declaring 'foo' before use
 */
class DiagnosticPrinter
{
public:
  /**
   * Create a diagnostic printer.
   *
   * @param os Output stream (typically std::cerr)
   * @param use_color Whether to use terminal colors
   */
  explicit DiagnosticPrinter(std::ostream & os, bool use_color = true);

  /**
   * Print a single diagnostic.
   *
   * Source context is resolved via SourceRegistry and the diagnostic labels.
   */
  void print(const Diagnostic & diag, const SourceRegistry & sources);

  /**
   * Print all diagnostics from a DiagnosticBag.
   */
  void print_all(const DiagnosticBag & diags, const SourceRegistry & sources);

private:
  // Rust-style formatting helpers
  void print_severity_header(const Diagnostic & diag);

  void print_label_context(const Label & label, const SourceRegistry & sources);

  void print_source_line(
    const SourceFile & source, uint32_t line_index, uint32_t start_col, uint32_t end_col,
    LabelStyle style, std::string_view label_message);

  void print_fixit(const FixIt & fixit, const SourceRegistry & sources);
  void print_help(std::string_view message);
  void print_note(std::string_view message);

  // Gutter elements for Rust-style output
  [[nodiscard]] std::string gutter_arrow() const;
  [[nodiscard]] std::string gutter_pipe() const;
  [[nodiscard]] std::string gutter_pipe_only() const;

  std::ostream & os_;
  bool use_color_;
};

}  // namespace bt_dsl
