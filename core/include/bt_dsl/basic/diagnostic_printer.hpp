// bt_dsl/basic/diagnostic_printer.hpp
//
// Prints diagnostics with source context, line/column information,
// and position markers for easy error identification.
//
#pragma once

#include <iosfwd>
#include <string_view>

#include "bt_dsl/basic/diagnostic.hpp"
#include "bt_dsl/basic/source_manager.hpp"

namespace bt_dsl
{

/**
 *
 * Produces output like:
 *   src/main.bt:5:12: error: undefined variable 'foo'
 *       let x = foo + 1;
 *               ^~~
 */
class DiagnosticPrinter
{
public:
  /**
   * Create a diagnostic printer.
   *
   * @param os Output stream (typically std::cerr)
   * @param useColor Whether to use ANSI color codes
   */
  explicit DiagnosticPrinter(std::ostream & os, bool use_color = true);

  /**
   * Print a single diagnostic with source context.
   *
   * @param diag The diagnostic to print
   * @param source SourceManager for line/column lookup
   * @param filename Filename to display in output
   */
  void print(const Diagnostic & diag, const SourceManager & source, std::string_view filename);

  /**
   * Print all diagnostics from a DiagnosticBag.
   *
   * @param diags Collection of diagnostics
   * @param source SourceManager for line/column lookup
   * @param filename Filename to display in output
   */
  void print_all(
    const DiagnosticBag & diags, const SourceManager & source, std::string_view filename);

private:
  /// Print source line with position marker
  void print_source_line(
    const SourceManager & source, uint32_t line_index, uint32_t start_col, uint32_t end_col);

  /// Get severity string with optional coloring
  std::string format_severity(Severity severity);

  /// ANSI color codes
  [[nodiscard]] std::string color_red() const;
  [[nodiscard]] std::string color_magenta() const;
  [[nodiscard]] std::string color_cyan() const;
  [[nodiscard]] std::string color_yellow() const;
  [[nodiscard]] std::string color_green() const;
  [[nodiscard]] std::string color_bold() const;
  [[nodiscard]] std::string color_reset() const;

  std::ostream & os_;
  bool use_color_;
};

}  // namespace bt_dsl
