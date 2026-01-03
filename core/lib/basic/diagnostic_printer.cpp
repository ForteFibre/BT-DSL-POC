// bt_dsl/basic/diagnostic_printer.cpp - Clang-like diagnostic output
//
#include "bt_dsl/basic/diagnostic_printer.hpp"

#include <cmath>
#include <iomanip>
#include <ostream>

namespace bt_dsl
{

DiagnosticPrinter::DiagnosticPrinter(std::ostream & os, bool use_color)
: os_(os), use_color_(use_color)
{
}

void DiagnosticPrinter::print(
  const Diagnostic & diag, const SourceManager & source, std::string_view filename)
{
  // Get line/column information
  LineColumn start_lc{0, 0};
  LineColumn end_lc{0, 0};

  if (diag.range.is_valid()) {
    start_lc = source.get_line_column(diag.range.get_begin());
    end_lc = source.get_line_column(diag.range.get_end());
  }

  // Print location: filename:line:column:
  os_ << color_bold() << filename;
  if (start_lc.is_valid()) {
    os_ << ":" << start_lc.line << ":" << start_lc.column;
  }
  os_ << ": " << color_reset();

  // Print severity with color
  os_ << format_severity(diag.severity) << ": ";

  // Print message
  os_ << color_bold() << diag.message << color_reset();

  // Print error code if present
  if (!diag.code.empty()) {
    os_ << " [" << diag.code << "]";
  }
  os_ << "\n";

  // Print source line with position marker if we have valid location
  if (start_lc.is_valid() && start_lc.line > 0) {
    const uint32_t end_col =
      (end_lc.is_valid() && end_lc.line == start_lc.line) ? end_lc.column : start_lc.column + 1;
    print_source_line(source, start_lc.line - 1, start_lc.column, end_col);
  }
}

void DiagnosticPrinter::print_all(
  const DiagnosticBag & diags, const SourceManager & source, std::string_view filename)
{
  for (const auto & diag : diags) {
    print(diag, source, filename);
  }
}

void DiagnosticPrinter::print_source_line(
  const SourceManager & source, uint32_t line_index, uint32_t start_col, uint32_t end_col)
{
  const std::string_view line = source.get_line(line_index);

  // Skip empty lines
  if (line.empty()) {
    return;
  }

  // Calculate line number width for padding
  const uint32_t line_num = line_index + 1;
  int line_num_width = 0;
  for (uint32_t n = line_num; n > 0; n /= 10) {
    line_num_width++;
  }
  line_num_width = std::max(line_num_width, 4);

  // Print line number and source line
  os_ << "  " << std::setw(line_num_width) << line_num << " | ";

  // Print source line, replacing tabs with spaces for alignment
  for (const char c : line) {
    if (c == '\t') {
      os_ << "    ";  // 4 spaces per tab
    } else if (c != '\r' && c != '\n') {
      os_ << c;
    }
  }
  os_ << "\n";

  // Print position marker line
  os_ << "  " << std::string(static_cast<size_t>(line_num_width), ' ') << " | ";

  // Calculate position marker with tab handling
  uint32_t visual_col = 1;
  size_t char_idx = 0;

  // Skip to start column
  for (; visual_col < start_col && char_idx < line.size(); ++char_idx) {
    if (line[char_idx] == '\t') {
      os_ << "    ";
      visual_col += 4;
    } else {
      os_ << " ";
      visual_col++;
    }
  }

  // Print marker
  os_ << color_green() << "^";

  // Print tilde for the rest of the range
  if (end_col > start_col) {
    for (uint32_t i = start_col + 1; i < end_col && (char_idx + i - start_col) < line.size(); ++i) {
      os_ << "~";
    }
  }

  os_ << color_reset() << "\n";
}

std::string DiagnosticPrinter::format_severity(Severity severity)
{
  switch (severity) {
    case Severity::Error:
      return color_red() + "error" + color_reset();
    case Severity::Warning:
      return color_magenta() + "warning" + color_reset();
    case Severity::Info:
      return color_cyan() + "info" + color_reset();
    case Severity::Hint:
      return color_green() + "hint" + color_reset();
  }
  return "unknown";
}

std::string DiagnosticPrinter::color_red() const { return use_color_ ? "\033[1;31m" : ""; }

std::string DiagnosticPrinter::color_magenta() const { return use_color_ ? "\033[1;35m" : ""; }

std::string DiagnosticPrinter::color_cyan() const { return use_color_ ? "\033[1;36m" : ""; }

std::string DiagnosticPrinter::color_yellow() const { return use_color_ ? "\033[1;33m" : ""; }

std::string DiagnosticPrinter::color_green() const { return use_color_ ? "\033[1;32m" : ""; }

std::string DiagnosticPrinter::color_bold() const { return use_color_ ? "\033[1m" : ""; }

std::string DiagnosticPrinter::color_reset() const { return use_color_ ? "\033[0m" : ""; }

}  // namespace bt_dsl
