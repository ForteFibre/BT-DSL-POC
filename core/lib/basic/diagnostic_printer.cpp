// bt_dsl/basic/diagnostic_printer.cpp - Rust-style diagnostic output
//
// Uses fmt for formatting and rang for terminal colors.
//
#include "bt_dsl/basic/diagnostic_printer.hpp"

#include <fmt/core.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <ostream>
#include <rang.hpp>
#include <string>
#include <vector>

namespace bt_dsl
{

DiagnosticPrinter::DiagnosticPrinter(std::ostream & os, bool use_color)
: os_(os), use_color_(use_color)
{
  // Configure rang based on use_color setting
  if (!use_color_) {
    rang::setControlMode(rang::control::Off);
  }
}

void DiagnosticPrinter::print(const Diagnostic & diag, const SourceRegistry & sources)
{
  const SourceRange primary_range = diag.primary_range();
  const FileId file_id = primary_range.file_id();

  // Convert to relative path for cleaner output
  std::string filename = "<unknown>";
  if (file_id.is_valid()) {
    const auto abs_path = sources.get_path(file_id);
    std::error_code ec;
    auto rel_path = std::filesystem::relative(abs_path, std::filesystem::current_path(), ec);
    filename = ec ? abs_path.string() : rel_path.string();
  }
  const FullSourceRange primary_fr = sources.get_full_range(primary_range);

  // === Header line: error[CODE]: message ===
  print_severity_header(diag);

  // === Location line: --> file:line:col ===
  if (primary_fr.is_valid()) {
    fmt::print(
      os_, "{} {}\n", gutter_arrow(),
      fmt::format("{}:{}:{}", filename, primary_fr.start_line, primary_fr.start_column));
  } else {
    fmt::print(os_, "{} {}\n", gutter_arrow(), filename);
  }

  // === Empty gutter line ===
  fmt::print(os_, "{}\n", gutter_pipe());

  // === Labels (source snippets) ===
  for (const auto & label : diag.labels) {
    print_label_context(label, sources);
  }

  // === Fix-its ===
  for (const auto & f : diag.fixits) {
    print_fixit(f, sources);
  }

  // === Help message ===
  if (diag.help_message) {
    print_help(*diag.help_message);
  }

  // === Trailing empty line for separation ===
  fmt::print(os_, "\n");
}

void DiagnosticPrinter::print_all(const DiagnosticBag & diags, const SourceRegistry & sources)
{
  std::vector<Diagnostic> sorted_diags;
  sorted_diags.reserve(diags.size());
  std::copy(diags.begin(), diags.end(), std::back_inserter(sorted_diags));

  // Sort by primary start location (stable)
  std::stable_sort(
    sorted_diags.begin(), sorted_diags.end(), [](const Diagnostic & a, const Diagnostic & b) {
      return a.primary_range().get_begin() < b.primary_range().get_begin();
    });

  for (const auto & d : sorted_diags) {
    print(d, sources);
  }
}

// =============================================================================
// Private helpers
// =============================================================================

void DiagnosticPrinter::print_severity_header(const Diagnostic & diag)
{
  if (use_color_) {
    os_ << rang::style::bold;
    switch (diag.severity) {
      case Severity::Error:
        os_ << rang::fg::red << "error";
        break;
      case Severity::Warning:
        os_ << rang::fg::yellow << "warning";
        break;
      case Severity::Info:
        os_ << rang::fg::cyan << "info";
        break;
      case Severity::Hint:
        os_ << rang::fg::green << "hint";
        break;
    }
    if (!diag.code.empty()) {
      os_ << "[" << diag.code << "]";
    }
    os_ << rang::fg::reset << ": " << diag.message << rang::style::reset << "\n";
  } else {
    std::string severity_str;
    switch (diag.severity) {
      case Severity::Error:
        severity_str = "error";
        break;
      case Severity::Warning:
        severity_str = "warning";
        break;
      case Severity::Info:
        severity_str = "info";
        break;
      case Severity::Hint:
        severity_str = "hint";
        break;
    }
    if (!diag.code.empty()) {
      fmt::print(os_, "{}[{}]: {}\n", severity_str, diag.code, diag.message);
    } else {
      fmt::print(os_, "{}: {}\n", severity_str, diag.message);
    }
  }
}

void DiagnosticPrinter::print_label_context(const Label & label, const SourceRegistry & sources)
{
  if (!label.range.is_valid()) {
    // No valid range - print as a note if message exists
    if (!label.message.empty()) {
      print_note(label.message);
    }
    return;
  }

  const FileId fid = label.range.file_id();
  if (!fid.is_valid()) {
    return;
  }

  const SourceFile * source = sources.get_file(fid);
  if (source == nullptr) {
    return;
  }

  const FullSourceRange fr = sources.get_full_range(label.range);
  if (!fr.is_valid()) {
    return;
  }

  const uint32_t end_col = (fr.end_line == fr.start_line && fr.end_column > fr.start_column)
                             ? fr.end_column
                             : (fr.start_column + 1);

  print_source_line(
    *source, fr.start_line - 1, fr.start_column, end_col, label.style, label.message);
}

void DiagnosticPrinter::print_source_line(
  const SourceFile & source, uint32_t line_index, uint32_t start_col, uint32_t end_col,
  LabelStyle style, std::string_view label_message)
{
  const std::string_view line = source.get_line(line_index);

  // Skip empty lines
  if (line.empty()) {
    return;
  }

  const uint32_t line_num = line_index + 1;

  // Build cleaned line (tabs -> spaces)
  std::string cleaned_line;
  cleaned_line.reserve(line.size());
  for (const char c : line) {
    if (c == '\t') {
      cleaned_line += "    ";  // 4 spaces per tab
    } else if (c != '\r' && c != '\n') {
      cleaned_line += c;
    }
  }

  // Print line number and source line
  if (use_color_) {
    os_ << rang::fg::cyan;
    fmt::print(os_, " {:>4} ", line_num);
    os_ << rang::fg::reset << rang::style::bold << "| " << rang::style::reset;
  } else {
    fmt::print(os_, " {:>4} | ", line_num);
  }
  fmt::print(os_, "{}\n", cleaned_line);

  // Print marker line
  fmt::print(os_, "      {} ", gutter_pipe_only());

  // Build marker string
  std::string marker_prefix;
  uint32_t visual_col = 1;
  size_t char_idx = 0;

  // Skip to start column (handle tabs)
  for (; visual_col < start_col && char_idx < line.size(); ++char_idx) {
    if (line[char_idx] == '\t') {
      marker_prefix += "    ";
      visual_col += 4;
    } else {
      marker_prefix += ' ';
      visual_col++;
    }
  }

  // Calculate marker length
  size_t marker_len = (end_col > start_col) ? (end_col - start_col) : 1;
  if (marker_len < 1) {
    marker_len = 1;
  }

  // Print markers
  fmt::print(os_, "{}", marker_prefix);

  char marker_char = (style == LabelStyle::Primary) ? '^' : '-';

  if (use_color_) {
    if (style == LabelStyle::Primary) {
      os_ << rang::fg::red << rang::style::bold;
    } else {
      os_ << rang::fg::cyan;
    }
    fmt::print(os_, "{}", std::string(marker_len, marker_char));
    if (!label_message.empty()) {
      fmt::print(os_, " {}", label_message);
    }
    os_ << rang::style::reset << rang::fg::reset;
  } else {
    fmt::print(os_, "{}", std::string(marker_len, marker_char));
    if (!label_message.empty()) {
      fmt::print(os_, " {}", label_message);
    }
  }
  fmt::print(os_, "\n");
}

void DiagnosticPrinter::print_fixit(const FixIt & fixit, const SourceRegistry & sources)
{
  const FullSourceRange fr = sources.get_full_range(fixit.range);

  if (!fr.is_valid()) {
    // Fallback for invalid ranges
    fmt::print(os_, "{}\n", gutter_pipe());
    if (use_color_) {
      os_ << rang::fg::green << rang::style::bold << "   = " << rang::style::reset
          << rang::fg::reset;
    } else {
      fmt::print(os_, "   = ");
    }
    fmt::print(os_, "fix: insert \"{}\"\n", fixit.replacement_text);
    return;
  }

  const FileId fid = fixit.range.file_id();
  const SourceFile * source = sources.get_file(fid);
  if (source == nullptr) {
    return;
  }

  // Print "help: add ';' here" header
  fmt::print(os_, "{}\n", gutter_pipe());
  if (use_color_) {
    os_ << rang::fg::cyan << rang::style::bold << "help" << rang::style::reset << rang::fg::reset;
    fmt::print(os_, ": add '{}' here\n", fixit.replacement_text);
  } else {
    fmt::print(os_, "help: add '{}' here\n", fixit.replacement_text);
  }

  // Print empty gutter
  fmt::print(os_, "{}\n", gutter_pipe());

  // Get the source line and apply the fix
  const std::string_view line = source->get_line(fr.start_line - 1);

  // Build the fixed line by inserting replacement at the end position
  std::string cleaned_line;
  cleaned_line.reserve(line.size() + fixit.replacement_text.size());
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '\t') {
      cleaned_line += "    ";
    } else if (c != '\r' && c != '\n') {
      cleaned_line += c;
    }
  }

  // Insert the replacement text at the end column position
  std::string fixed_line = cleaned_line + fixit.replacement_text;

  // Print line number and fixed line
  if (use_color_) {
    os_ << rang::fg::cyan;
    fmt::print(os_, " {:>4} ", fr.start_line);
    os_ << rang::fg::reset << rang::style::bold << "| " << rang::style::reset;
  } else {
    fmt::print(os_, " {:>4} | ", fr.start_line);
  }
  fmt::print(os_, "{}\n", fixed_line);

  // Print the marker line with green +
  fmt::print(os_, "      {} ", gutter_pipe_only());

  // Calculate visual position for the + marker
  std::string marker_prefix;
  for (size_t i = 0; i < cleaned_line.size(); ++i) {
    marker_prefix += ' ';
  }

  fmt::print(os_, "{}", marker_prefix);
  if (use_color_) {
    os_ << rang::fg::green << rang::style::bold << "+" << rang::style::reset << rang::fg::reset;
  } else {
    fmt::print(os_, "+");
  }
  fmt::print(os_, "\n");
}

void DiagnosticPrinter::print_help(std::string_view message)
{
  fmt::print(os_, "{}\n", gutter_pipe());

  if (use_color_) {
    os_ << rang::fg::cyan << rang::style::bold << "   = " << rang::style::reset << rang::fg::reset;
    fmt::print(os_, "help: {}\n", message);
  } else {
    fmt::print(os_, "   = help: {}\n", message);
  }
}

void DiagnosticPrinter::print_note(std::string_view message)
{
  fmt::print(os_, "{}\n", gutter_pipe());

  if (use_color_) {
    os_ << rang::fg::cyan << rang::style::bold << "   = " << rang::style::reset << rang::fg::reset;
    fmt::print(os_, "note: {}\n", message);
  } else {
    fmt::print(os_, "   = note: {}\n", message);
  }
}

// =============================================================================
// Gutter helpers (Rust-style)
// =============================================================================

std::string DiagnosticPrinter::gutter_arrow() const
{
  if (use_color_) {
    return fmt::format("{}{} -->{}", "\033[1;36m", " ", "\033[0m");
  }
  return "  -->";
}

std::string DiagnosticPrinter::gutter_pipe() const
{
  if (use_color_) {
    return fmt::format("{}      |{}", "\033[1;36m", "\033[0m");
  }
  return "      |";
}

std::string DiagnosticPrinter::gutter_pipe_only() const
{
  if (use_color_) {
    return fmt::format("{}|{}", "\033[1;36m", "\033[0m");
  }
  return "|";
}

}  // namespace bt_dsl
